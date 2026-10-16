/* core/record/rewind_pages.c - see openpete_rewind_pages.h. */

#include "openpete_rewind_pages.h"
#include "openpete_log.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define SLAB_PAGES 512u                       /* 2 MiB per slab */
#define SLAB_BYTES ((size_t)SLAB_PAGES * RWP_PAGE_BYTES)

static uint8_t** s_slab;      /* slab host bases */
static uint32_t* s_ref;       /* per page id: reference count */
static uint32_t  s_nslabs;
static uint32_t  s_slab_cap;
static uint32_t  s_free;      /* pages with a zero count */
static uint32_t  s_scan;      /* next-fit cursor */

static uint32_t* s_flat;      /* the pending table's page ids */
static uint32_t  s_flat_cap;

/* The running compare: pending table k against target t, k's pages
 * [0, cursor) done. t's run cursor only moves forward. */
static struct {
    rwp_table_t*       k;
    const rwp_table_t* t;
    uint32_t           cursor;
    uint32_t           t_run;
    uint32_t           t_base;  /* first page index of t's run t_run */
} s_cmp;

static rwp_table_t* s_pending;
static rwp_stats_t  s_st;

/** @brief Return a page id's host bytes.
 * @param id A page id of a mapped slab.
 * @return The page's first byte. */
static uint8_t* page_host(uint32_t id) {
    return s_slab[id / SLAB_PAGES] + (size_t)(id % SLAB_PAGES) * RWP_PAGE_BYTES;
}

/** @brief Map one more slab and grow the count array to cover it.
 * @return 0 on success, -1 when the mapping or an array allocation fails. */
static int store_grow(void) {
    if (s_nslabs == s_slab_cap) {
        const uint32_t cap = s_slab_cap ? s_slab_cap * 2u : 16u;
        uint8_t** sl = (uint8_t**)realloc(s_slab, cap * sizeof *sl);
        if (!sl) return -1;
        s_slab = sl;
        uint32_t* rf = (uint32_t*)realloc(s_ref, (size_t)cap * SLAB_PAGES * sizeof *rf);
        if (!rf) return -1;
        s_ref = rf;
        s_slab_cap = cap;
    }
#ifdef _WIN32
    void* p = VirtualAlloc(NULL, SLAB_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return -1;
#else
    void* p = mmap(NULL, SLAB_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
#endif
    s_slab[s_nslabs] = (uint8_t*)p;
    memset(s_ref + (size_t)s_nslabs * SLAB_PAGES, 0, SLAB_PAGES * sizeof *s_ref);
    s_scan = s_nslabs * SLAB_PAGES;
    s_nslabs++;
    s_free += SLAB_PAGES;
    s_st.slabs = s_nslabs;
    if (s_nslabs > s_st.slabs_peak) s_st.slabs_peak = s_nslabs;
    return 0;
}

/** @brief Take a free page with a count of one, next-fit from the scan cursor, so a capture's pages run in consecutive ids where the store allows.
 * @param out Receives the page id.
 * @return 0 on success, -1 when the store cannot grow. */
static int page_alloc(uint32_t* out) {
    if (!s_free && store_grow() != 0) return -1;
    const uint32_t n = s_nslabs * SLAB_PAGES;
    uint32_t id = s_scan < n ? s_scan : 0u;
    while (s_ref[id]) id = id + 1u < n ? id + 1u : 0u;
    s_ref[id] = 1u;
    s_free--;
    s_scan = id + 1u;
    if (++s_st.pages_live > s_st.pages_live_peak) s_st.pages_live_peak = s_st.pages_live;
    *out = id;
    return 0;
}

/** @brief Add a reference to a live page. */
static void page_ref(uint32_t id) {
    s_ref[id]++;
}

/** @brief Drop a reference; a page whose count reaches zero returns to the store. */
static void page_unref(uint32_t id) {
    if (--s_ref[id] == 0u) {
        s_free++;
        s_st.pages_live--;
    }
}

/** @brief Return the page id of an encoded table's page, walking its runs forward from a cursor.
 *
 * @param t An encoded table.
 * @param g Page index, not below the index of the previous call on the same cursor.
 * @param run In/out: the cursor's run.
 * @param base In/out: the first page index of that run.
 * @return The page id. */
static uint32_t runs_at(const rwp_table_t* t, uint32_t g, uint32_t* run, uint32_t* base) {
    while (g >= *base + t->runs[*run].count) {
        *base += t->runs[*run].count;
        (*run)++;
    }
    return t->runs[*run].src + (g - *base);
}

/** @brief Encode the pending table as runs, shrinking a runs array far larger than needed, and end its pending state. */
static void encode_pending(void) {
    rwp_table_t* k = s_pending;
    if (!k) return;
    uint32_t nr = 0;
    for (uint32_t g = 0; g < k->npages; g++) {
        const uint32_t id = k->flat[g];
        if (nr && k->runs[nr - 1].src + k->runs[nr - 1].count == id) {
            k->runs[nr - 1].count++;
        } else {
            k->runs[nr].src = id;
            k->runs[nr].count = 1u;
            nr++;
        }
    }
    k->nruns = nr;
    k->flat = NULL;
    if (k->runs_cap > 2u * nr + 64u) {
        rwp_run_t* r = (rwp_run_t*)realloc(k->runs, ((size_t)nr + 64u) * sizeof *r);
        if (r) { k->runs = r; k->runs_cap = nr + 64u; }
    }
    s_pending = NULL;
    memset(&s_cmp, 0, sizeof s_cmp);
}

int rwp_capture(rwp_table_t* t) {
    if (s_pending) {
        (void)rwp_compare_step(UINT32_MAX);
        encode_pending();
    }
    uint32_t used[OPENPETE_ARENA_RANGES];
    (void)openpete_mods_arena_used(used);
    uint32_t total = 0;
    for (unsigned r = 0; r < OPENPETE_ARENA_RANGES; r++) {
        t->used[r] = used[r];
        t->pages[r] = (used[r] + RWP_PAGE_BYTES - 1u) / RWP_PAGE_BYTES;
        total += t->pages[r];
    }
    t->npages = 0;
    t->nruns = 0;
    if (total > s_flat_cap) {
        uint32_t* f = (uint32_t*)realloc(s_flat, (size_t)total * sizeof *f);
        if (!f) goto fail;
        s_flat = f;
        s_flat_cap = total;
    }
    if (total > t->runs_cap) {
        rwp_run_t* r = (rwp_run_t*)realloc(t->runs, (size_t)total * sizeof *r);
        if (!r) goto fail;
        t->runs = r;
        t->runs_cap = total;
    }
    t->flat = s_flat;
    for (unsigned r = 0; r < OPENPETE_ARENA_RANGES; r++) {
        const uint8_t* src = openpete_mods_arena_range_bytes(r);
        for (uint32_t i = 0; i < t->pages[r]; i++) {
            uint32_t id;
            if (page_alloc(&id) != 0) goto fail;
            const uint32_t off = i * RWP_PAGE_BYTES;
            const uint32_t n = used[r] - off < RWP_PAGE_BYTES ? used[r] - off : RWP_PAGE_BYTES;
            uint8_t* dst = page_host(id);
            memcpy(dst, src + off, n);
            if (n < RWP_PAGE_BYTES) memset(dst + n, 0, RWP_PAGE_BYTES - n);
            t->flat[t->npages++] = id;
        }
    }
    s_pending = t;
    s_st.captures++;
    s_st.pages_copied += total;
    return 0;
fail:
    for (uint32_t g = 0; g < t->npages; g++) page_unref(t->flat[g]);
    memset(t->used, 0, sizeof t->used);
    memset(t->pages, 0, sizeof t->pages);
    t->npages = 0;
    t->flat = NULL;
    s_st.capture_fails++;
    return -1;
}

void rwp_compare_begin(rwp_table_t* k, const rwp_table_t* target) {
    if (!s_pending || k != s_pending) return;
    if (!target || target == k || target->flat || !target->npages) {
        s_st.compares_no_target++;
        encode_pending();
        return;
    }
    s_cmp.k = k;
    s_cmp.t = target;
    s_cmp.cursor = 0;
    s_cmp.t_run = 0;
    s_cmp.t_base = 0;
    s_st.compares_begun++;
}

uint32_t rwp_compare_remaining(void) {
    return s_cmp.k ? s_cmp.k->npages - s_cmp.cursor : 0u;
}

/** @brief Compare one page of the running compare, sharing it when equal.
 * @param g Page index into the pending table, in range order.
 * @param r The page's range.
 * @param i The page's index within its range. */
static void compare_page(uint32_t g, unsigned r, uint32_t i) {
    const rwp_table_t* t = s_cmp.t;
    if (i >= t->pages[r]) return;
    uint32_t tg = i;
    for (unsigned q = 0; q < r; q++) tg += t->pages[q];
    const uint32_t tid = runs_at(t, tg, &s_cmp.t_run, &s_cmp.t_base);
    const uint32_t kid = s_cmp.k->flat[g];
    s_st.pages_compared++;
    if (tid == kid || memcmp(page_host(kid), page_host(tid), RWP_PAGE_BYTES) != 0) return;
    page_ref(tid);
    page_unref(kid);
    s_cmp.k->flat[g] = tid;
    s_st.pages_shared++;
}

uint32_t rwp_compare_step(uint32_t max_pages) {
    rwp_table_t* k = s_cmp.k;
    if (!k) return 0;
    uint32_t done = 0;
    unsigned r = 0;
    uint32_t start = 0;   /* first page index of range r */
    while (done < max_pages && s_cmp.cursor < k->npages) {
        const uint32_t g = s_cmp.cursor;
        while (g >= start + k->pages[r]) start += k->pages[r++];
        compare_page(g, r, g - start);
        s_cmp.cursor++;
        done++;
    }
    if (done) s_st.slices++;
    if (s_cmp.cursor >= k->npages) {
        s_st.compares_done++;
        encode_pending();
    }
    return done;
}

void rwp_release(rwp_table_t* t) {
    if (!t) return;
    if (s_cmp.k && (s_cmp.k == t || s_cmp.t == t)) {
        s_st.discards++;
        encode_pending();
    } else if (s_pending == t) {
        encode_pending();
    }
    uint32_t run = 0, base = 0;
    for (uint32_t g = 0; g < t->npages; g++) page_unref(runs_at(t, g, &run, &base));
    memset(t->used, 0, sizeof t->used);
    memset(t->pages, 0, sizeof t->pages);
    t->npages = 0;
    t->nruns = 0;
}

void rwp_table_free(rwp_table_t* t) {
    if (!t) return;
    free(t->runs);
    t->runs = NULL;
    t->runs_cap = 0;
}

void rwp_restore(const rwp_table_t* t) {
    if (!t) return;
    s_st.restores++;
    if (t->flat) s_st.restores_pending++;
    uint32_t g = 0, run = 0, base = 0;
    for (unsigned r = 0; r < OPENPETE_ARENA_RANGES; r++) {
        uint32_t n = t->used[r];
        uint8_t* dst = openpete_mods_arena_range_reanchor(r, &n);
        for (uint32_t i = 0; i < t->pages[r]; i++, g++) {
            const uint32_t off = i * RWP_PAGE_BYTES;
            if (!dst || off >= n) continue;
            const uint32_t id = t->flat ? t->flat[g] : runs_at(t, g, &run, &base);
            const uint32_t len = n - off < RWP_PAGE_BYTES ? n - off : RWP_PAGE_BYTES;
            memcpy(dst + off, page_host(id), len);
        }
    }
}

uint32_t rwp_store_trim(void) {
    if (s_st.pages_live || !s_nslabs) return 0;
    const uint32_t n = s_nslabs;
    for (uint32_t i = 0; i < n; i++) {
#ifdef _WIN32
        VirtualFree(s_slab[i], 0, MEM_RELEASE);
#else
        munmap(s_slab[i], SLAB_BYTES);
#endif
    }
    free(s_slab);
    free(s_ref);
    free(s_flat);
    s_slab = NULL; s_ref = NULL; s_flat = NULL;
    s_nslabs = s_slab_cap = s_free = s_scan = s_flat_cap = 0;
    s_st.slabs = 0;
    s_st.trims++;
    return n;
}

rwp_stats_t rwp_stats(void) { return s_st; }

const rwp_table_t* rwp_pending(void) { return s_pending; }
