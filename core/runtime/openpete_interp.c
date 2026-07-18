/* core/runtime/openpete_interp.c — render-side snapshot ring.
 *
 * See openpete_interp.h for the API contract. This TU provides capture +
 * the dedupe predicate + the prev/curr query; the lerped re-emission bracket
 * is built game-side (e.g. spyro_native_reextract.c), not here.
 *
 * Determinism notes (this TU is read-only against PSX RAM):
 *   - openpete_interp_capture() runs after ticked_this_iter; the game
 *     fiber is yielded inside VSync(0), so RAM is consistent.
 *   - No write path in THIS TU touches PSX RAM. The lerped-write bracket
 *     (save-canonical → write-lerped → render → restore-canonical) lives
 *     game-side and runs entirely on the host fiber, restoring canonical RAM
 *     before the game fiber resumes; core capture stays read-only.
 */

#include "openpete_interp.h"
#include "openpete_interp_objs.h"     /* reset on guest discontinuity */
#include "openpete_discontinuity.h"
#include "openpete_psx.h"
#include "openpete_log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Game-registered region table. NULL/0 until the game's startup calls
 * openpete_interp_register_regions(); init() short-circuits to no-op
 * capture in that case. Explicit registration avoids relying on
 * cross-toolchain weak-symbol semantics — MinGW's PE/COFF linker mis-
 * resolves the strong override in some configurations, leaving the
 * weak NULL in place at runtime even though the strong def exists. */
static const openpete_interp_region_t* s_regions = NULL;
static size_t                          s_region_count = 0;

/* 3-deep ring: keeps the last THREE distinct ticks (prev2, prev, curr) so the
 * render side can build a cubic (Hermite/pred1) interpolant with a centered
 * start-tangent, not just the linear prev→curr chord. s_head = newest. */
static openpete_interp_snapshot_t s_snap[3];
static int      s_head        = 0;   /* index of the most-recently-written slot */
static int      s_initialized = 0;
static int      s_enabled     = 0;

static uint64_t s_total_captures     = 0;
static uint64_t s_total_dedupe_skips = 0;

/* Scratch for the capture-time dedupe compare (built before committing to a
 * ring slot, so an unchanged tick doesn't clobber the previous DISTINCT one). */
static uint8_t s_cap_scratch[OPENPETE_INTERP_MAX_SNAPSHOT_BYTES];

void openpete_interp_register_regions(const openpete_interp_region_t* regions,
                                      size_t count) {
    s_regions = regions;
    s_region_count = (regions ? count : 0);
}

/* Guest state was replaced (savestate load / rewind restore): every snapshot
 * in both interp stores describes the OLD timeline. Drop them so the next
 * present snaps to the restored state instead of lerping a full-scene smear
 * across the discontinuity (prev = pre-load, curr = post-load). */
static void interp_on_guest_discontinuity(void) {
    openpete_interp_reset();
    openpete_interp_objs_reset();
}

int openpete_interp_init(void) {
    if (s_initialized) return 0;

    /* Register even when capture stays disabled (no regions): the objs
     * table can still be driven by a registered capture callback. */
    (void)openpete_on_guest_discontinuity(interp_on_guest_discontinuity);

    if (!s_regions || s_region_count == 0) {
        OPL_INFO_F("interp", "no game regions registered; capture disabled");
        s_initialized = 1;
        return 0;
    }
    if (s_region_count > OPENPETE_INTERP_MAX_REGIONS) {
        /* %u + cast rather than %zu — MinGW's default MSVCRT printf
         * doesn't reliably understand C99 %zu. */
        OPL_ERROR_F("interp",
                    "region table has %u entries (max %u); bump OPENPETE_INTERP_MAX_REGIONS",
                    (unsigned)s_region_count,
                    (unsigned)OPENPETE_INTERP_MAX_REGIONS);
        return 1;
    }

    /* Compute per-region offsets; reject tables that exceed the snapshot cap. */
    uint32_t total = 0;
    uint32_t offsets[OPENPETE_INTERP_MAX_REGIONS] = {0};
    for (size_t r = 0; r < s_region_count; ++r) {
        offsets[r] = total;
        total += s_regions[r].length;
    }
    if (total > OPENPETE_INTERP_MAX_SNAPSHOT_BYTES) {
        OPL_ERROR_F("interp",
                    "region total %u bytes exceeds snapshot cap %u; bump OPENPETE_INTERP_MAX_SNAPSHOT_BYTES",
                    (unsigned)total, (unsigned)OPENPETE_INTERP_MAX_SNAPSHOT_BYTES);
        return 1;
    }

    memset(s_snap, 0, sizeof(s_snap));
    for (int i = 0; i < 3; ++i) {
        s_snap[i].total_bytes = total;
        for (size_t r = 0; r < s_region_count; ++r) {
            s_snap[i].region_offsets[r] = offsets[r];
        }
    }
    s_head = 0;
    s_initialized = 1;

    OPL_INFO_F("interp",
               "initialised: %u regions, %u bytes/snapshot",
               (unsigned)s_region_count, (unsigned)total);
    return 0;
}

void openpete_interp_shutdown(void) {
    s_initialized = 0;
    s_enabled = 0;
}

void openpete_interp_reset(void) {
    /* Invalidate the ring but keep the region-table config (total_bytes,
     * region_offsets) computed at init — only the SNAPSHOTS are stale. */
    for (int i = 0; i < 3; ++i) s_snap[i].valid = 0;
    s_head = 0;
}

void openpete_interp_set_enabled(int on) {
    s_enabled = on ? 1 : 0;
}

int openpete_interp_enabled(void) {
    return s_enabled;
}

/* Virtual present clock — see the header note. */
static op_time_t s_present_target = 0;
void openpete_interp_set_present_target(op_time_t t) { s_present_target = t; }
op_time_t openpete_interp_present_now(void) {
    return s_present_target ? s_present_target : op_now();
}

/* Deterministic capture — see the header note. */
static int       s_det_mode    = 0;
static op_time_t s_det_period  = 0;
static uint64_t  s_det_index   = 0;
void openpete_interp_set_deterministic(int on, op_time_t period_ns) {
    s_det_mode = on ? 1 : 0;
    s_det_period = period_ns;
    if (on) s_det_index = 0;
}
op_time_t openpete_interp_curr_tick_ns(void) {
    return (s_initialized && s_snap[s_head].valid) ? s_snap[s_head].tick_ns : 0;
}
int openpete_interp_paired(void) {
    if (!s_initialized || !s_enabled) return 0;
    const openpete_interp_snapshot_t* curr = &s_snap[s_head];
    const openpete_interp_snapshot_t* prev = &s_snap[(s_head + 2) % 3];
    if (!curr->valid || !prev->valid || curr->total_bytes != prev->total_bytes)
        return 0;
    return memcmp(curr->data, prev->data, curr->total_bytes) != 0;
}

void openpete_interp_capture(uint32_t tick, op_time_t tick_ns) {
    if (!s_initialized || !s_enabled) return;
    if (!s_regions || s_region_count == 0) return;

    /* Build the candidate snapshot into scratch FIRST, so we can compare it to
     * the current head before deciding whether to advance the ring. */
    const openpete_interp_snapshot_t* curr = &s_snap[s_head];
    uint32_t total = curr->total_bytes;
    for (size_t r = 0; r < s_region_count; ++r) {
        const openpete_interp_region_t* reg = &s_regions[r];
        void* host = openpete_psx_to_host_ptr(reg->psx_addr);
        uint32_t off = curr->region_offsets[r];
        if (host) memcpy(s_cap_scratch + off, host, reg->length);
        else      memset(s_cap_scratch + off, 0, reg->length);   /* unmapped → deterministic */
    }
    s_total_captures++;

    /* Dedupe AT CAPTURE: Spyro's logic runs at 30 Hz on a 60 Hz VBL fiber, so
     * the transform-bearing state advances only every OTHER tick — yet this
     * capture fires every tick. A naive 2-slot ring (advance + fill every time)
     * then holds two SAME-frame duplicates whenever the camera updated on the
     * other VBL, so every sub-tick query sees prev==curr and dedupes → the
     * interpolation bracket NEVER engages (paired=0 forever; the flicker/judder
     * the user reported). By skipping the advance when the candidate is
     * byte-identical to the current head, the ring always holds the two most-
     * recent DISTINCT states, and curr->tick_ns stays pinned to when that state
     * FIRST appeared — exactly the [prev,curr] bracket alpha spans. (A truly
     * static screen keeps the last two motion frames; the query's alpha clamps
     * to 1 → shows curr, i.e. frozen, which is correct.) */
    if (curr->valid && total > 0 && memcmp(s_cap_scratch, curr->data, total) == 0) {
        s_total_dedupe_skips++;
        return;
    }

    /* OPENPETE_CAP_ROLL_DBG: one line per ROLL — which frame, and the first
     * words of region 0 (the game's camera) before/after. Forensics for
     * degenerate pairs (prev==curr in one region while another rolled). */
    {
        static int s_crd = -1;
        if (s_crd < 0) { const char* e = getenv("OPENPETE_CAP_ROLL_DBG"); s_crd = (e && *e && *e != '0'); }
        if (s_crd) {
            uint32_t oldw = 0, neww = 0;
            memcpy(&oldw, curr->data + 0x28, 4);      /* region0 + CAM pos.x (Spyro 1) */
            memcpy(&neww, s_cap_scratch + 0x28, 4);
            fprintf(stderr, "[cap-roll f=%u tick=%u] r0X %d -> %d%s\n",
                    tick, tick, (int32_t)oldw, (int32_t)neww,
                    (int32_t)oldw == (int32_t)neww ? "  (CAM-DEGENERATE)" : "");
        }
    }

    /* Deterministic capture: override the wall-clock stamp with a fixed grid
     * point (0, P, 2P, …) advanced only on an actual store, so consecutive
     * distinct ticks are exactly `period` apart (see the header note). */
    if (s_det_mode && s_det_period > 0)
        tick_ns = (op_time_t)(s_det_index++) * s_det_period;

    /* State advanced — commit to the next ring slot. snap[s_head] becomes the
     * "latest distinct tick"; the two prior slots hold prev and prev2. */
    s_head = (s_head + 1) % 3;
    openpete_interp_snapshot_t* slot = &s_snap[s_head];
    slot->tick    = tick;
    slot->tick_ns = tick_ns;
    memcpy(slot->data, s_cap_scratch, total);
    slot->valid = 1;
}

static int ra_commit_env_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_RA_CAM_COMMIT"); v = !(e && *e == '0'); }
    return v;
}

int openpete_interp_ra_commit_active(void) {
    if (!s_initialized || !s_enabled || !s_regions) return 0;
    if (!ra_commit_env_on()) return 0;
    for (size_t r = 0; r < s_region_count; ++r)
        if (s_regions[r].ra_commit) return 1;
    return 0;
}

void openpete_interp_recapture_committed(void) {
    if (!openpete_interp_ra_commit_active()) return;
    openpete_interp_snapshot_t* curr = &s_snap[s_head];
    if (!curr->valid) return;
    /* Overwrite flagged regions of the NEWEST slot in place — even when this
     * tick's capture deduped (no roll): the slot then still holds the last
     * distinct state, and refreshing its committed regions keeps them the
     * freshest committed bytes known. tick/tick_ns stay untouched; the
     * committed region shares the led regions' alpha mapping, it just pairs
     * one branch-free timeline instead of the led one. */
    for (size_t r = 0; r < s_region_count; ++r) {
        if (!s_regions[r].ra_commit) continue;
        void* host = openpete_psx_to_host_ptr(s_regions[r].psx_addr);
        if (host) memcpy(curr->data + curr->region_offsets[r], host,
                         s_regions[r].length);
    }
}

int openpete_interp_query(op_time_t now_ns,
                          op_time_t sim_period_ns,
                          double*   out_alpha,
                          const openpete_interp_snapshot_t** out_prev,
                          const openpete_interp_snapshot_t** out_curr) {
    if (!s_initialized || !s_enabled) return 0;

    const openpete_interp_snapshot_t* curr = &s_snap[s_head];
    const openpete_interp_snapshot_t* prev = &s_snap[(s_head + 2) % 3];
    if (!curr->valid || !prev->valid) return 0;
    if (curr->total_bytes != prev->total_bytes) return 0;

    /* Dedupe: byte-identical means the game didn't actually move
     * (vbl busy-wait, paused, loading screen). Caller should
     * PsyX_SwapWindowOnly() instead of re-emitting. */
    if (memcmp(curr->data, prev->data, curr->total_bytes) == 0) {
        s_total_dedupe_skips++;
        return 0;
    }

    /* Compute alpha against the LATEST tick's timestamp. Render shows
     * state at `prev->tick_ns + alpha * sim_period`, so alpha in
     * [0, 1] maps to [prev, curr]. We measure relative to curr->tick_ns
     * because that's the most recent reference point; the host loop is
     * always "between curr and the next, yet-uncaptured tick" — we
     * accept a 1-tick latency by lerping into the past. */
    op_time_t delta = now_ns - prev->tick_ns;
    if (delta < 0) delta = 0;
    if (sim_period_ns <= 0) return 0;

    double alpha = (double)delta / (double)sim_period_ns;
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;

    if (out_alpha) *out_alpha = alpha;
    if (out_prev)  *out_prev  = prev;
    if (out_curr)  *out_curr  = curr;
    return 1;
}

int openpete_interp_query3(op_time_t now_ns,
                           op_time_t sim_period_ns,
                           double*   out_alpha,
                           const openpete_interp_snapshot_t** out_prev2,
                           const openpete_interp_snapshot_t** out_prev,
                           const openpete_interp_snapshot_t** out_curr) {
    int r = openpete_interp_query(now_ns, sim_period_ns, out_alpha, out_prev, out_curr);
    if (out_prev2) {
        /* Two ticks back from curr. NULL until the ring has 3 distinct ticks,
         * or when the query itself didn't pair — callers fall back to linear. */
        const openpete_interp_snapshot_t* p2 = &s_snap[(s_head + 1) % 3];
        *out_prev2 = (r && p2->valid) ? p2 : NULL;
    }
    return r;
}

uint64_t openpete_interp_total_captures(void)     { return s_total_captures;     }
uint64_t openpete_interp_total_dedupe_skips(void) { return s_total_dedupe_skips; }
