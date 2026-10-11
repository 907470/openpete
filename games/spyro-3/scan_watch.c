// games/spyro-3/scan_watch.c - see scan_watch.h.

#include "scan_watch.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_mods.h"
#include "openpete_psx.h"
#include "openpete_time.h"
#include "spyro_overlay_table.h"
#include "features/antipiracy_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EXE .text: main_TEXT_START..main_TEXT_END in the decomp's psx.map. */
#define EXE_LO  0x8001200Cu
#define EXE_HI  0x80064F9Cu
#define EXE_LEN (EXE_HI - EXE_LO)

/* The overlay header: the id word at the overlay base, and at +4 the end of
 * the encrypted span, which is also the XOR key (func_8002CA50's decryption
 * loop at 0x8002CF78 walks words from base + 0x1000 up to it). */
#define OVL_BASE        0x800742D0u
#define OVL_DECRYPT_OFS 0x1000u

/* func_8002AB38, whose word a failed loading walk zeroes (the store at
 * 0x8002CD68 in func_8002CA50). */
#define SABOTAGE_LO 0x8002AB38u
#define SABOTAGE_HI 0x8002AE00u

/* The overlay window's base, recomp.conf's OVERLAY_RANGE low bound; the
 * shadow mirrors guest RAM from here to the highest scanned overlay byte. */
#define WIN_LO OVL_BASE

/* Report lines per class before the watch only counts. */
#define REPORT_LINES_MAX 32u

typedef struct {
    const char* name;
    uint32_t    lo, hi; /* [lo, hi) */
} scan_range_t;

/* Each scanned overlay's text: <name>_text_start up to <name>_data_start
 * (the level overlays) or <name>_text_end (title, loading) in psx.map, the
 * bounds build.py passes to Dragonbreath's -O. The atlas, credits, options
 * and cutscene overlays carry no scan. */
static const scan_range_t k_overlay_ranges[] = {
    { "title",    0x80074DECu, 0x8007EC9Cu },
    { "loading",  0x8007494Cu, 0x8007B6E4u },
    { "level_10", 0x80074C50u, 0x8008B688u },
    { "level_11", 0x800751C0u, 0x80091ED4u },
    { "level_12", 0x80074F50u, 0x8008BD18u },
    { "level_13", 0x8007507Cu, 0x8008C5CCu },
    { "level_14", 0x80075454u, 0x800996ECu },
    { "level_15", 0x800749ACu, 0x80089D24u },
    { "level_16", 0x80074AB4u, 0x800866F8u },
    { "level_17", 0x80074CF8u, 0x80083EECu },
    { "level_18", 0x80074A54u, 0x80088184u },
    { "level_20", 0x80074E2Cu, 0x8008C550u },
    { "level_21", 0x80075060u, 0x8008F6A0u },
    { "level_22", 0x8007574Cu, 0x8009B090u },
    { "level_23", 0x80075578u, 0x800963E4u },
    { "level_24", 0x8007572Cu, 0x80096B68u },
    { "level_25", 0x80074CE0u, 0x800898D4u },
    { "level_26", 0x80074DC8u, 0x8008ADD8u },
    { "level_27", 0x80074DECu, 0x80089BE0u },
    { "level_28", 0x80074A0Cu, 0x800862D4u },
    { "level_30", 0x8007521Cu, 0x8008D2D8u },
    { "level_31", 0x800757BCu, 0x8008FAF8u },
    { "level_32", 0x80075B98u, 0x8009D030u },
    { "level_33", 0x80075D1Cu, 0x8009F264u },
    { "level_34", 0x80075618u, 0x8009886Cu },
    { "level_35", 0x80074EE0u, 0x8008C4CCu },
    { "level_36", 0x80074AD0u, 0x800867F4u },
    { "level_37", 0x80074D90u, 0x8008BA38u },
    { "level_38", 0x80074A10u, 0x80088504u },
    { "level_40", 0x800749B0u, 0x800872B4u },
    { "level_41", 0x80075544u, 0x80096324u },
    { "level_42", 0x80074F78u, 0x8008FEBCu },
    { "level_43", 0x8007584Cu, 0x80096464u },
    { "level_44", 0x800755ACu, 0x80093E5Cu },
    { "level_45", 0x80074C28u, 0x80089AB8u },
    { "level_46", 0x80074E40u, 0x80091B40u },
    { "level_47", 0x80074D7Cu, 0x8008DF5Cu },
    { "level_48", 0x80074A34u, 0x8008753Cu },
    { "level_50", 0x800759B4u, 0x800A0654u },
};
static const scan_range_t k_exe_range = { "exe", EXE_LO, EXE_HI };

static void (*s_orig_write_word)(uint32_t, uint32_t) = NULL;
static void (*s_orig_write_half)(uint32_t, uint16_t) = NULL;
static void (*s_orig_write_byte)(uint32_t, uint8_t)  = NULL;
static void (*s_orig_swl)(uint32_t, uint32_t)        = NULL;
static void (*s_orig_swr)(uint32_t, uint32_t)        = NULL;
static CPUState* s_cpu = NULL;

static int      s_armed = 0;        /* store hooks wrapped */
static int      s_booted = 0;       /* spyro3_scan_watch_boot ran: the overlay table is bound */
static uint8_t* s_shadow_exe = NULL; /* EXE .text as last adopted */
static uint8_t* s_shadow_ovl = NULL; /* the overlay window as last adopted */
static uint32_t s_win_lo = 0, s_win_len = 0;
static int      s_shadow_failed = 0;

/* Resident-overlay range cache, keyed by the dispatcher's overlay id. */
static uint32_t            s_res_magic = 0;
static const scan_range_t* s_res_range = NULL;

/* Watch counters. */
static unsigned s_seen;           /* stores into a scanned range, every class */
static unsigned s_hits_rec, s_hits_host, s_hits_state;
static unsigned s_ex_decrypt, s_ex_copy, s_ex_verdict;
static unsigned s_lines;

/* Overlay-window recompiler stores held until the tick ends: a guest copy
 * that brings in another overlay replaces the id word, and its stores are
 * then the load. */
static unsigned    s_pend_n;
static uint32_t    s_pend_addr, s_pend_ra, s_pend_magic;
static unsigned    s_pend_size;
static const char* s_pend_name;

/* The failed walk's own zeroing store is let through once per failure. */
static int s_verdict_store_open = 0;

/* Verdicts. */
static unsigned s_pass, s_fail;
static int      s_level_seen = 0;

/* Guard. A stretch is one begin; a compare is one pass over the scanned
 * ranges, at begin (adopt) or at end (restore). */
static unsigned s_guard_stretches, s_guard_compares, s_guard_restores, s_guard_bytes;
static op_time_t s_guard_ns;
static const scan_range_t* s_guard_ovl = NULL;

/* Host-poke injector. */
static int      s_poke_armed = 0;
static unsigned s_poke_frame;
static uint32_t s_poke_addr, s_poke_value;

/** @brief Map a guest address in any segment to its kseg0 alias. */
static inline uint32_t kseg0(uint32_t a) { return (a & 0x1FFFFFFFu) | 0x80000000u; }

/** @brief Return a host pointer to the guest RAM byte at a kseg0 address. */
static uint8_t* ram_at(uint32_t k) { return openpete_psx_get_ram() + (k & 0x1FFFFFu); }

/** @brief Read the guest RAM word at a kseg0 address without going through a hook. */
static uint32_t ram_word(uint32_t k) {
    uint32_t w;
    memcpy(&w, ram_at(k), 4);
    return w;
}

/** @brief Return the scanned text range of the resident overlay, or NULL when the resident overlay carries no scan or the overlay table is not bound yet. */
static const scan_range_t* resident_range(void) {
    if (!s_booted) return NULL;
    const uint32_t magic = spyro_get_active_overlay_magic();
    if (magic == s_res_magic) return s_res_range;
    s_res_magic = magic;
    s_res_range = NULL;
    const spyro_overlay_entry_t* e = magic ? spyro_overlay_find(magic) : NULL;
    if (e)
        for (size_t i = 0; i < sizeof k_overlay_ranges / sizeof k_overlay_ranges[0]; ++i)
            if (strcmp(k_overlay_ranges[i].name, e->name) == 0) {
                s_res_range = &k_overlay_ranges[i];
                break;
            }
    return s_res_range;
}

/** @brief Allocate the shadows on first use and fill them from guest RAM, which holds the loaded EXE by then.
 *
 * @return 1 when the shadows exist, 0 after a failed allocation. */
static int shadows_ready(void) {
    if (s_shadow_exe) return 1;
    if (s_shadow_failed) return 0;
    uint32_t hi = 0;
    for (size_t i = 0; i < sizeof k_overlay_ranges / sizeof k_overlay_ranges[0]; ++i)
        if (k_overlay_ranges[i].hi > hi) hi = k_overlay_ranges[i].hi;
    s_win_lo = WIN_LO;
    s_win_len = hi - WIN_LO;
    s_shadow_exe = (uint8_t*)malloc(EXE_LEN);
    s_shadow_ovl = (uint8_t*)malloc(s_win_len);
    if (!s_shadow_exe || !s_shadow_ovl) {
        free(s_shadow_exe);
        free(s_shadow_ovl);
        s_shadow_exe = s_shadow_ovl = NULL;
        s_shadow_failed = 1;
        OPL_ERROR_F("s3-scan", "shadow allocation failed; watch and guard off");
        return 0;
    }
    memcpy(s_shadow_exe, ram_at(EXE_LO), EXE_LEN);
    memcpy(s_shadow_ovl, ram_at(s_win_lo), s_win_len);
    return 1;
}

/** @brief Return the shadow byte that mirrors a kseg0 address of a range. */
static uint8_t* shadow_at(const scan_range_t* r, uint32_t k) {
    return r == &k_exe_range ? s_shadow_exe + (k - EXE_LO) : s_shadow_ovl + (k - s_win_lo);
}

/** @brief Copy a range's guest bytes into its shadow when they differ, returning 1 when they did. */
static int adopt(const scan_range_t* r) {
    uint8_t* sh = shadow_at(r, r->lo);
    const uint8_t* live = ram_at(r->lo);
    if (memcmp(sh, live, r->hi - r->lo) == 0) return 0;
    memcpy(sh, live, r->hi - r->lo);
    return 1;
}

/** @brief Report one watch hit as an anti-piracy failure, up to the line cap.
 *
 * Goes through spyro3_antipiracy_report_watch_hit, whose `FAIL watch:`
 * line fails the oracle and determinism gates. Hits past the cap are only
 * counted.
 * @param writer The writer class.
 * @param r The scanned range.
 * @param k The first byte's kseg0 address.
 * @param size The bytes the first store (or the restore's diff) covers.
 * @param count The stores the hit stands for.
 * @param ra The guest link register at the store. */
static void report(const char* writer, const scan_range_t* r, uint32_t k,
                   unsigned size, unsigned count, uint32_t ra) {
    if (s_lines++ >= REPORT_LINES_MAX) return;
    char who[128];
    snprintf(who, sizeof who, "%s into scanned range %s (%u store(s), ra 0x%08X, vbl %d)",
             writer, r->name, count, ra, SDL_GetAtomicInt(&openpete_vbl));
    spyro3_antipiracy_report_watch_hit(k, size, who);
}

/** @brief Resolve the held overlay-window stores: the load when the id word now names another overlay, else recompiler-store hits. */
static void flush_pending(void) {
    if (!s_pend_n) return;
    if (ram_word(OVL_BASE) != s_pend_magic) {
        s_ex_copy += s_pend_n;
    } else {
        s_hits_rec += s_pend_n;
        const scan_range_t r = { s_pend_name, 0, 0 };
        report("recompiler store", &r, s_pend_addr, s_pend_size, s_pend_n, s_pend_ra);
    }
    s_pend_n = 0;
}

/** @brief Classify one store that may land in a scanned range, before it is performed.
 *
 * An swl or swr merges part of the register into the word, so its value is
 * not the word written and it is never taken for the decryption pass or the
 * failed walk's zeroing store.
 * @param k The kseg0 address of its first byte.
 * @param size The store width in bytes.
 * @param v The stored value.
 * @param exact 0 for an swl or swr, else 1. */
static void watch_store(uint32_t k, unsigned size, uint32_t v, int exact) {
    const scan_range_t* r;
    if (k - EXE_LO < EXE_LEN) {
        r = &k_exe_range;
    } else {
        r = resident_range();
        if (!r || k + size <= r->lo || k >= r->hi) return;
    }
    s_seen++;
    if (r != &k_exe_range) {
        const uint32_t key = ram_word(OVL_BASE + 4u);
        if (exact && size == 4u && k >= OVL_BASE + OVL_DECRYPT_OFS && k < key &&
            v == (ram_word(k) ^ key)) {
            s_ex_decrypt++;
            return;
        }
    } else if (exact && size == 4u && s_verdict_store_open && v == 0u && k >= SABOTAGE_LO && k < SABOTAGE_HI) {
        s_verdict_store_open = 0;
        s_ex_verdict++;
        return;
    }
    const uint32_t ra = s_cpu ? s_cpu->ra : 0u;
    if (!openpete_fiber_in_game()) {
        s_hits_host++;
        report("host poke", r, k, size, 1u, ra);
        return;
    }
    if (r != &k_exe_range) {
        if (!s_pend_n) {
            s_pend_addr = k;
            s_pend_size = size;
            s_pend_ra = ra;
            s_pend_magic = spyro_get_active_overlay_magic();
            s_pend_name = r->name;
        }
        s_pend_n++;
        return;
    }
    s_hits_rec++;
    report("recompiler store", r, k, size, 1u, ra);
}

/** @brief Report whether a kseg0 address can fall in a scanned range: EXE .text or the overlay window. */
static inline int maybe_scanned(uint32_t k) {
    return k - EXE_LO < EXE_LEN || k - s_win_lo < s_win_len;
}

/// @brief Store hook: classify a word store into a scanned range, then perform it.
static void sw_word(uint32_t a, uint32_t v) {
    const uint32_t k = kseg0(a);
    if (maybe_scanned(k)) watch_store(k, 4u, v, 1);
    s_orig_write_word(a, v);
}
/// @brief Store hook: classify a halfword store into a scanned range, then perform it.
static void sw_half(uint32_t a, uint16_t v) {
    const uint32_t k = kseg0(a);
    if (maybe_scanned(k)) watch_store(k, 2u, v, 1);
    s_orig_write_half(a, v);
}
/// @brief Store hook: classify a byte store into a scanned range, then perform it.
static void sw_byte(uint32_t a, uint8_t v) {
    const uint32_t k = kseg0(a);
    if (maybe_scanned(k)) watch_store(k, 1u, v, 1);
    s_orig_write_byte(a, v);
}
/// @brief Store hook: classify an swl on the aligned word it touches, then perform it.
static void sw_swl(uint32_t a, uint32_t v) {
    const uint32_t k = kseg0(a & ~3u);
    if (maybe_scanned(k)) watch_store(k, 4u, v, 0);
    s_orig_swl(a, v);
}
/// @brief Store hook: classify an swr on the aligned word it touches, then perform it.
static void sw_swr(uint32_t a, uint32_t v) {
    const uint32_t k = kseg0(a & ~3u);
    if (maybe_scanned(k)) watch_store(k, 4u, v, 0);
    s_orig_swr(a, v);
}

/** @brief Adopt the EXE range and the resident overlay's range into the shadow, returning the overlay range adopted (or NULL). */
static const scan_range_t* adopt_all(void) {
    (void)adopt(&k_exe_range);
    const scan_range_t* r = resident_range();
    if (r) (void)adopt(r);
    return r;
}

/** @brief Guard begin: adopt the scanned ranges as the state a mod's code must leave unchanged.
 *
 * Under --scan-guard 0 it only counts the stretch. */
static void guard_begin(void) {
    const op_time_t t0 = op_now(); /* clock-guard: wall */ /* guard cost span */
    s_guard_stretches++;
    if (openpete_mods_guest_guard_compares() && shadows_ready()) {
        s_guard_ovl = adopt_all();
        s_guard_compares++;
    }
    s_guard_ns += op_now() - t0; /* clock-guard: wall */
}

/** @brief Restore every byte of a range that differs from its shadow, logging the count against a mod.
 *
 * @param r The range.
 * @param mod_id The mod whose code ran. */
static void restore_range(const scan_range_t* r, const char* mod_id) {
    uint8_t* sh = shadow_at(r, r->lo);
    uint8_t* live = ram_at(r->lo);
    const uint32_t len = r->hi - r->lo;
    if (memcmp(sh, live, len) == 0) return;
    unsigned n = 0;
    uint32_t first = 0;
    for (uint32_t i = 0; i < len; ++i) {
        if (sh[i] == live[i]) continue;
        if (!n) first = r->lo + i;
        live[i] = sh[i];
        n++;
    }
    s_guard_restores++;
    s_guard_bytes += n;
    OPL_WARN_F("s3-scan", "guard: mod '%s' stored into scanned range %s: %u byte(s) from 0x%08X restored (vbl %d)",
               mod_id ? mod_id : "?", r->name, n, first, SDL_GetAtomicInt(&openpete_vbl));
}

/** @brief Guard end: restore and attribute every scanned byte a mod's code changed since guard_begin.
 *
 * @param mod_id The mod whose code ran. */
static void guard_end(const char* mod_id) {
    const op_time_t t0 = op_now(); /* clock-guard: wall */ /* guard cost span */
    if (openpete_mods_guest_guard_compares() && shadows_ready()) {
        restore_range(&k_exe_range, mod_id);
        if (s_guard_ovl) restore_range(s_guard_ovl, mod_id);
        s_guard_compares++;
    }
    s_guard_ns += op_now() - t0; /* clock-guard: wall */
}

const openpete_mods_guest_guard_t spyro3_scan_guard = { guard_begin, guard_end };

/// @brief Print the verdict counters, the watch counters and the guard counters at exit.
static void exit_report(void) {
    fprintf(stderr, "[s3-scan] checksum verdicts: pass=%u fail=%u%s\n", s_pass, s_fail,
            (s_level_seen && s_pass + s_fail == 0u)
                ? " VACUOUS: a level loaded and no loading-walk verdict ran" : "");
    if (s_armed) {
        flush_pending();
        fprintf(stderr, "[s3-scan] watch: in-range stores=%u hits=%u (recompiler=%u host=%u savestate=%u) "
                        "load excluded: decrypt=%u overlay-copy=%u verdict-store=%u\n",
                s_seen, s_hits_rec + s_hits_host + s_hits_state, s_hits_rec, s_hits_host,
                s_hits_state, s_ex_decrypt, s_ex_copy, s_ex_verdict);
    }
    if (openpete_mods_count() > 0)
        fprintf(stderr, "[s3-scan] guard: stretches=%u compares=%u restores=%u bytes=%u "
                        "cost=%.3f us/stretch%s\n",
                s_guard_stretches, s_guard_compares, s_guard_restores, s_guard_bytes,
                s_guard_stretches ? (double)s_guard_ns / 1000.0 / (double)s_guard_stretches : 0.0,
                openpete_mods_guest_guard_compares() ? "" : " (scan guard off)");
}

void spyro3_scan_watch_boot(CPUState* cpu) {
    atexit(exit_report);
    const char* poke = getenv("OPENPETE_S3_SCAN_POKE");
    if (poke && *poke &&
        sscanf(poke, "%u:%x:%x", &s_poke_frame, &s_poke_addr, &s_poke_value) == 3)
        s_poke_armed = 1;
    s_booted = 1;
    if (!openpete_mods_guest_guard_compares())
        OPL_INFO_F("s3-scan", "scan guard off (--scan-guard 0): a mod's stores into the "
                              "scanned code ranges persist");
    const char* env = getenv("OPENPETE_S3_SCAN_WATCH");
    const int watch = env && *env && *env != '0';
    if (!cpu || !watch || !shadows_ready()) return;
    OPL_INFO_F("s3-scan", "store watch armed");
    s_cpu = cpu;
    s_orig_write_word = cpu->write_word;
    s_orig_write_half = cpu->write_half;
    s_orig_write_byte = cpu->write_byte;
    s_orig_swl        = cpu->swl;
    s_orig_swr        = cpu->swr;
    cpu->write_word = sw_word;
    cpu->write_half = sw_half;
    cpu->write_byte = sw_byte;
    cpu->swl        = sw_swl;
    cpu->swr        = sw_swr;
    s_armed = 1;
}

void spyro3_scan_watch_tick(CPUState* cpu) {
    if (!s_level_seen) {
        const uint32_t magic = spyro_get_active_overlay_magic();
        const spyro_overlay_entry_t* e = magic ? spyro_overlay_find(magic) : NULL;
        if (e && strncmp(e->name, "level_", 6) == 0) s_level_seen = 1;
    }
    if (!s_armed) return;
    flush_pending();
    (void)adopt_all();
    if (s_poke_armed && openpete_frame == s_poke_frame) {
        s_poke_armed = 0;
        OPL_INFO_F("s3-scan", "poke: host store 0x%08X -> 0x%08X at frame %u",
                   s_poke_value, s_poke_addr, s_poke_frame);
        cpu->write_word(s_poke_addr, s_poke_value);
    }
}

/** @brief Count and report the bytes of a range that differ from its shadow as savestate-load hits. */
static void state_diff(const scan_range_t* r) {
    const uint8_t* sh = shadow_at(r, r->lo);
    const uint8_t* live = ram_at(r->lo);
    unsigned n = 0;
    uint32_t first = 0;
    for (uint32_t i = 0; i < r->hi - r->lo; ++i)
        if (sh[i] != live[i] && n++ == 0u) first = r->lo + i;
    if (!n) return;
    s_hits_state++;
    report("savestate load", r, first, n, 1u, 0u);
}

void spyro3_scan_watch_state_loaded(void) {
    if (!s_armed) return;
    s_pend_n = 0; /* stores of the abandoned timeline */
    state_diff(&k_exe_range);
    /* A restore that brings in another overlay is an overlay load. */
    uint32_t shadow_magic;
    memcpy(&shadow_magic, s_shadow_ovl + (OVL_BASE - s_win_lo), 4);
    const scan_range_t* r = resident_range();
    if (r && shadow_magic == ram_word(OVL_BASE)) state_diff(r);
    (void)adopt_all();
}

void spyro3_scan_watch_verdict(int passed) {
    if (passed) {
        s_pass++;
        return;
    }
    s_fail++;
    s_verdict_store_open = 1;
    char mods[512];
    size_t used = 0;
    mods[0] = '\0';
    for (int i = 0; i < openpete_mods_count() && used < sizeof mods; ++i) {
        const char* id = openpete_mods_id(i);
        if (!id || !openpete_mods_is_enabled(id)) continue;
        int w = snprintf(mods + used, sizeof mods - used, "%s%s", used ? ", " : "", id);
        if (w > 0) used += (size_t)w;
    }
    OPL_WARN_F("s3-scan", "loading walk failed with mods active: %s", used ? mods : "none");
}
