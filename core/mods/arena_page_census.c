// core/mods/arena_page_census.c - per-owner changed-page census of the guest arena.
//
// Diagnostic, off unless OPENPETE_ARENA_PAGE_CENSUS is set (1, or a path the
// report is appended to). At every final yield (each snapshot opportunity)
// it compares the arena's used prefix with a shadow copy in 4 KiB pages and
// copies the changed pages into the shadow. A game tick is closed when
// openpete_canon_tick advances: its changed pages (the union over the
// tick's final yields) are attributed to every ledger owner with a live
// allocation row overlapping them. A second count takes the union of
// changed pages over each window of CENSUS_WINDOW ticks, about the
// keyframe interval of rewind (60 VBLs). Owners come from the arena's file
// state section (version 3), re-read whenever its size changes. At exit the
// report gives, per owner and for the whole arena, the mutable and
// IMMUTABLE bytes, the mean, p99 and max pages changed per tick and per
// window, and the ticks observed (the engagement count). Statistics cover
// two spans: every observed tick, and the steady span after the last
// ledger change.

#include "openpete_mods.h"
#include "openpete_fiber.h"
#include "openpete_mod_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_PAGE       4096u
#define CENSUS_WINDOW 30u
#define PC_OWNERS_MAX 64u

typedef struct {
    char     owner[OPENPETE_ARENA_OWNER_MAX];
    uint32_t kind, call_idx, size, align, flags, off, superseded;
} pc_rec_t;

typedef struct {
    char      name[OPENPETE_ARENA_OWNER_MAX];
    uint64_t  mut_bytes, imm_bytes;
    uint8_t*  pages;      /* 1 when a live row of this owner overlaps the page */
    uint32_t* tick_n;     /* changed pages per observed tick */
    uint32_t* win_n;      /* changed-page union per window */
    uint32_t  tick_cap, win_cap;
} pc_owner_t;

static int        s_on = -1;
static uint8_t*   s_shadow, *s_changed, *s_win;
static uint32_t   s_cap_pages;
static uint32_t   s_state_size;
static pc_owner_t s_own[PC_OWNERS_MAX + 1]; /* last slot = whole arena */
static uint32_t   s_nown;
static uint32_t   s_ticks, s_wins, s_win_fill;
static uint32_t   s_steady_tick, s_steady_win; /* first tick / window after the last ledger change */
static uint32_t   s_rows_live, s_used_last;
static uint32_t   s_samples, s_npg;
static unsigned   s_last_canon;

/** @brief Store @p v at index @p i of a per-sample array, growing it with zeroed entries.
 *
 * @param a the array, reallocated in place.
 * @param cap the array's capacity, updated.
 * @param i the index.
 * @param v the value. */
static void pc_put(uint32_t** a, uint32_t* cap, uint32_t i, uint32_t v) {
    if (i >= *cap) {
        uint32_t nc = *cap ? *cap : 1024u;
        while (nc <= i) nc *= 2u;
        uint32_t* n = realloc(*a, (size_t)nc * sizeof **a);
        if (!n) return;
        memset(n + *cap, 0, (size_t)(nc - *cap) * sizeof **a);
        *a = n;
        *cap = nc;
    }
    (*a)[i] = v;
}

/** @brief Find or add the census slot for @p name.
 *
 * @param name the owner id.
 * @return the slot, or NULL when the owner table is full. */
static pc_owner_t* pc_owner(const char* name) {
    for (uint32_t i = 0; i < s_nown; i++)
        if (strncmp(s_own[i].name, name, OPENPETE_ARENA_OWNER_MAX) == 0) return &s_own[i];
    if (s_nown >= PC_OWNERS_MAX) return NULL;
    pc_owner_t* o = &s_own[s_nown++];
    memset(o, 0, sizeof *o);
    memcpy(o->name, name, OPENPETE_ARENA_OWNER_MAX);
    o->name[OPENPETE_ARENA_OWNER_MAX - 1] = 0;
    return o;
}

/** @brief Rebuild every owner's page map and byte totals from the arena state section.
 *
 * Reads the version-3 section through openpete_mods_arena_state_save; a
 * section of another version leaves the maps empty and logs once. */
static void pc_reload_rows(void) {
    uint32_t sz = openpete_mods_arena_state_size();
    uint8_t* buf = malloc(sz);
    if (!buf) return;
    openpete_mods_arena_state_save(buf);
    uint32_t hdr[3];
    memcpy(hdr, buf, sizeof hdr);
    if (hdr[0] != 3u) {
        static int warned;
        if (!warned) fprintf(stderr, "[arena-pages] state section version %u, expected 3: no owners\n", hdr[0]);
        warned = 1;
        free(buf);
        return;
    }
    for (uint32_t i = 0; i < s_nown; i++) {
        s_own[i].mut_bytes = s_own[i].imm_bytes = 0;
        if (s_own[i].pages) memset(s_own[i].pages, 0, s_cap_pages);
    }
    s_rows_live = 0;
    const pc_rec_t* rec = (const pc_rec_t*)(buf + sizeof hdr);
    for (uint32_t r = 0; r < hdr[1]; r++) {
        pc_rec_t rr;
        memcpy(&rr, rec + r, sizeof rr);
        if (rr.kind != 0u || rr.superseded || rr.size == 0u) continue;
        pc_owner_t* o = pc_owner(rr.owner);
        if (!o) continue;
        if (!o->pages) o->pages = calloc(s_cap_pages, 1);
        if (!o->pages) continue;
        if (rr.flags & OP_GALLOC_IMMUTABLE) o->imm_bytes += rr.size; else o->mut_bytes += rr.size;
        uint32_t p0 = rr.off / PC_PAGE, p1 = (rr.off + rr.size - 1u) / PC_PAGE;
        for (uint32_t p = p0; p <= p1 && p < s_cap_pages; p++) o->pages[p] = 1;
        s_rows_live++;
    }
    free(buf);
}

/** @brief Allocate the shadow and page bitmaps for the arena's capacity.
 *
 * @return 1 when ready, 0 when an allocation fails (the census then turns off). */
static int pc_init_buffers(void) {
    s_cap_pages = (8u * 1024u * 1024u) / PC_PAGE;
    s_shadow = calloc(s_cap_pages, PC_PAGE);
    s_changed = calloc(s_cap_pages, 1);
    s_win = calloc(s_cap_pages, 1);
    return s_shadow && s_changed && s_win;
}

/** @brief Write one statistics line for a series of per-sample counts.
 *
 * @param f the destination.
 * @param tag the series label.
 * @param a the samples.
 * @param n the sample count; 0 prints n=0. */
static void pc_stats(FILE* f, const char* tag, const uint32_t* a, uint32_t n) {
    if (n == 0u) { fprintf(f, " %s n=0", tag); return; }
    uint32_t* c = malloc((size_t)n * sizeof *c);
    if (!c) return;
    memcpy(c, a, (size_t)n * sizeof *c);
    uint64_t sum = 0;
    for (uint32_t i = 1; i < n; i++) {        /* insertion sort: n is a few thousand per owner */
        uint32_t v = c[i], j = i;
        while (j > 0 && c[j - 1] > v) { c[j] = c[j - 1]; j--; }
        c[j] = v;
    }
    for (uint32_t i = 0; i < n; i++) sum += c[i];
    uint32_t p99 = c[(uint32_t)((uint64_t)(n - 1u) * 99u / 100u)];
    fprintf(f, " %s n=%u mean=%.2f p99=%u max=%u", tag, n, (double)sum / n, p99, c[n - 1]);
    free(c);
}

/** @brief Write the census report to @p f. */
static void pc_emit(FILE* f) {
    fprintf(f, "[arena-pages] samples=%u ticks=%u windows=%u window_ticks=%u steady_from_tick=%u used=%u live_rows=%u owners=%u\n",
            s_samples, s_ticks, s_wins, CENSUS_WINDOW, s_steady_tick, s_used_last, s_rows_live, s_nown);
    pc_owner_t* all = &s_own[PC_OWNERS_MAX];
    all->mut_bytes = all->imm_bytes = 0;
    for (uint32_t i = 0; i < s_nown; i++) {
        all->mut_bytes += s_own[i].mut_bytes;
        all->imm_bytes += s_own[i].imm_bytes;
    }
    for (uint32_t i = 0; i <= s_nown; i++) {
        const pc_owner_t* o = i < s_nown ? &s_own[i] : &s_own[PC_OWNERS_MAX];
        uint32_t own_pages = 0;
        if (o->pages) for (uint32_t p = 0; p < s_cap_pages; p++) own_pages += o->pages[p];
        if (i == s_nown) own_pages = (s_used_last + PC_PAGE - 1u) / PC_PAGE;
        uint64_t engaged = 0;
        if (o->tick_cap >= s_ticks)
            for (uint32_t t = 0; t < s_ticks; t++) engaged += o->tick_n[t];
        fprintf(f, "[arena-pages] owner=%s mut_bytes=%llu imm_bytes=%llu pages=%u changed_page_ticks=%llu",
                i < s_nown ? o->name : "*all*", (unsigned long long)o->mut_bytes,
                (unsigned long long)o->imm_bytes, own_pages, (unsigned long long)engaged);
        const uint32_t nt = o->tick_cap >= s_ticks ? s_ticks : 0u;
        const uint32_t nw = o->win_cap >= s_wins ? s_wins : 0u;
        pc_stats(f, "tick", o->tick_n, nt);
        pc_stats(f, "steady_tick", o->tick_n + (nt > s_steady_tick ? s_steady_tick : 0u),
                 nt > s_steady_tick ? nt - s_steady_tick : 0u);
        pc_stats(f, "steady_window", o->win_n + (nw > s_steady_win ? s_steady_win : 0u),
                 nw > s_steady_win ? nw - s_steady_win : 0u);
        fputc('\n', f);
    }
}

/** @brief Report the census at exit to stderr and, when the variable names a path, to that file. */
static void pc_report(void) {
    pc_emit(stderr);
    const char* e = getenv("OPENPETE_ARENA_PAGE_CENSUS");
    if (e && strcmp(e, "1") != 0) {
        FILE* f = fopen(e, "a");
        if (f) { pc_emit(f); fclose(f); }
    }
}

/** @brief Read OPENPETE_ARENA_PAGE_CENSUS once and arm the census and its exit report.
 *
 * @return 1 when the census is on. */
static int pc_armed(void) {
    if (s_on >= 0) return s_on;
    const char* e = getenv("OPENPETE_ARENA_PAGE_CENSUS");
    s_on = e && *e && *e != '0' && pc_init_buffers();
    if (s_on) {
        memset(&s_own[PC_OWNERS_MAX], 0, sizeof s_own[PC_OWNERS_MAX]);
        atexit(pc_report);
    }
    return s_on;
}

/** @brief Copy each page of the used prefix that differs from the shadow into it, marking it in the tick and window sets.
 *
 * @param used the arena's used bytes. */
static void pc_compare_pages(uint32_t used) {
    const uint8_t* a = openpete_mods_arena_bytes();
    const uint32_t npg = (used + PC_PAGE - 1u) / PC_PAGE;
    for (uint32_t p = 0; p < npg; p++) {
        const uint32_t len = p + 1u < npg ? PC_PAGE : used - p * PC_PAGE;
        uint8_t* dst = s_shadow + (size_t)p * PC_PAGE;
        if (memcmp(a + (size_t)p * PC_PAGE, dst, len) == 0) continue;
        memcpy(dst, a + (size_t)p * PC_PAGE, len);
        s_changed[p] = 1;
        s_win[p] = 1;
    }
}

/** @brief Count the pages of @p set that lie in @p o's live rows.
 *
 * @param o the owner.
 * @param set a page bitmap (one byte per page).
 * @param npg the pages to scan.
 * @return the count. */
static uint32_t pc_owner_count(const pc_owner_t* o, const uint8_t* set, uint32_t npg) {
    uint32_t n = 0;
    if (!o->pages) return 0;
    for (uint32_t p = 0; p < npg; p++) n += set[p] & o->pages[p];
    return n;
}

/** @brief Close the current window: store each owner's and the arena's changed-page union, then clear it.
 *
 * @param npg the pages of the used prefix. */
static void pc_close_window(uint32_t npg) {
    pc_owner_t* all = &s_own[PC_OWNERS_MAX];
    uint32_t n = 0;
    for (uint32_t p = 0; p < npg; p++) n += s_win[p];
    pc_put(&all->win_n, &all->win_cap, s_wins, n);
    for (uint32_t i = 0; i < s_nown; i++)
        pc_put(&s_own[i].win_n, &s_own[i].win_cap, s_wins, pc_owner_count(&s_own[i], s_win, npg));
    memset(s_win, 0, s_cap_pages);
    s_wins++;
}

/** @brief Close the current tick: store each owner's and the arena's changed pages, then clear the tick set. */
static void pc_close_tick(void) {
    pc_owner_t* all = &s_own[PC_OWNERS_MAX];
    uint32_t n = 0;
    for (uint32_t p = 0; p < s_npg; p++) n += s_changed[p];
    pc_put(&all->tick_n, &all->tick_cap, s_ticks, n);
    for (uint32_t i = 0; i < s_nown; i++)
        pc_put(&s_own[i].tick_n, &s_own[i].tick_cap, s_ticks, pc_owner_count(&s_own[i], s_changed, s_npg));
    memset(s_changed, 0, s_cap_pages);
    s_ticks++;
    if (++s_win_fill < CENSUS_WINDOW) return;
    s_win_fill = 0;
    pc_close_window(s_npg);
}

void openpete_mods_arena_page_census_tick(void) {
    if (!pc_armed()) return;
    const uint32_t used = openpete_mods_arena_used();
    if (used == 0u) return;
    if (s_samples > 0u && openpete_canon_tick != s_last_canon) pc_close_tick();
    s_last_canon = openpete_canon_tick;
    const uint32_t sz = openpete_mods_arena_state_size();
    if (sz != s_state_size) {
        s_state_size = sz;
        pc_reload_rows();
        s_steady_tick = s_ticks + 1u;  /* the tick that allocated is not steady */
        s_steady_win = s_wins + 1u;
    }
    s_used_last = used;
    s_npg = (used + PC_PAGE - 1u) / PC_PAGE;
    pc_compare_pages(used);
    s_samples++;
}
