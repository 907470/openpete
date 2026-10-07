/* core/renderer/openpete_interp.c - render-side snapshot ring.
 *
 * See openpete_interp.h for the API contract. This TU provides capture +
 * the dedupe predicate + the prev/curr query; the lerped re-emission bracket
 * is built game-side (games/spyro-1/renderer/reextract.c), not here.
 *
 * Determinism notes (this TU is read-only against PSX RAM):
 *   - openpete_interp_capture() runs after ticked_this_iter; the game
 *     fiber is yielded inside VSync(0), so RAM is consistent.
 *   - No write path in THIS TU touches PSX RAM. The lerped-write bracket
 *     (save-canonical -> write-lerped -> render -> restore-canonical) lives
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
 * cross-toolchain weak-symbol semantics - MinGW's PE/COFF linker mis-
 * resolves the strong override in some configurations, leaving the
 * weak NULL in place at runtime even though the strong def exists. */
static const openpete_interp_region_t* s_regions = NULL;
static size_t                          s_region_count = 0;

/* 3-deep ring: keeps the last THREE distinct ticks (prev2, prev, curr) so the
 * render side can build a cubic (Hermite/pred1) interpolant with a centered
 * start-tangent, not just the linear prev->curr chord. s_head = newest. */
static openpete_interp_snapshot_t s_snap[3];
static int      s_head        = 0;   /* index of the most-recently-written slot */
static int      s_initialized = 0;
static int      s_enabled     = 0;

static uint64_t s_total_captures     = 0;
static uint64_t s_total_dedupe_skips = 0;

/* The newest tick capture has SEEN, stored whether or not the ring rolled.
 * The ring itself holds only distinct states, so the head's `tick` is when the
 * state last changed; the difference between the two is how long the present
 * has been running past a state that stopped moving. */
static uint32_t s_last_tick      = 0;
static int      s_last_tick_seen = 0;

/* Scratch for the capture-time dedupe compare (built before committing to a
 * ring slot, so an unchanged tick doesn't clobber the previous DISTINCT one). */
static uint8_t s_cap_scratch[OPENPETE_INTERP_MAX_SNAPSHOT_BYTES];

/* Ticks between the last two distinct captures, the cadence at which the
 * game's logic actually advances state (2 for 30 Hz logic on a 60 Hz capture).
 * It is the hold the dedupe tolerates: a candidate identical to the head is
 * skipped while the head is younger than this, and committed once the state
 * has held this long, which is when the state is static rather than
 * mid-cadence.
 * Capped so a long pause (a load) cannot stretch the hold without bound. */
#define INTERP_MAX_DISTINCT_GAP 4u
/* Floor on the hold, in captures. Capture runs once per VBL and the game's
 * logic advances once per sim tick (two VBLs), so the shortest hold that can
 * distinguish a static state from the gap between two logic steps is two
 * captures. A measured gap of one comes from a stretch that advanced per VBL
 * (a load, a fade) and outlives it; honouring it commits a duplicate at the
 * mid-cadence capture of ordinary 30 Hz motion, which replaces the motion pair
 * with an identity pair; the distinct commits are then one VBL apart, which
 * pins the measurement at one for the rest of the run. */
#define INTERP_MIN_DISTINCT_GAP 2u
static uint32_t s_distinct_gap = INTERP_MIN_DISTINCT_GAP;
/* The head slot was committed as a duplicate of the slot before it, so the
 * tick distance to it measures the static hold, not the logic's cadence. */
static int      s_head_is_dup  = 0;

void openpete_interp_register_regions(const openpete_interp_region_t* regions,
                                      size_t count) {
    s_regions = regions;
    s_region_count = (regions ? count : 0);
}

/** @brief Drop the snapshot ring and the object table on a guest-state discontinuity.
 *
 * Registered by openpete_interp_init with the guest-discontinuity
 * broadcast (savestate load, rewind restore). Every snapshot in both
 * interp stores describes the old timeline; dropping them makes the next
 * present snap to the restored state instead of lerping a full-scene smear
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
        /* %u + cast rather than %zu - MinGW's default MSVCRT printf
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

static uint32_t s_reset_gen = 0;
void openpete_interp_reset(void) {
    /* Invalidate the ring but keep the region-table config (total_bytes,
     * region_offsets) computed at init - only the SNAPSHOTS are stale. */
    for (int i = 0; i < 3; ++i) s_snap[i].valid = 0;
    s_head = 0;
    s_last_tick_seen = 0;
    s_distinct_gap = INTERP_MIN_DISTINCT_GAP;
    s_head_is_dup = 0;
    s_reset_gen++;
}
uint32_t openpete_interp_reset_generation(void) { return s_reset_gen; }

void openpete_interp_set_enabled(int on) {
    s_enabled = on ? 1 : 0;
}

int openpete_interp_enabled(void) {
    return s_enabled;
}

/* Virtual present clock - see the header note. */
static op_time_t s_present_target = 0;
void openpete_interp_set_present_target(op_time_t t) { s_present_target = t; }
op_time_t openpete_interp_present_now(void) {
    return s_present_target ? s_present_target : op_now(); /* clock-guard: wall */ /* inline presents: display instant == now */
}

/* Deterministic capture - see the header note. */
static int       s_det_mode    = 0;
static op_time_t s_det_period  = 0;
void openpete_interp_set_deterministic(int on, op_time_t period_ns) {
    s_det_mode = on ? 1 : 0;
    s_det_period = period_ns;
}
int openpete_interp_deterministic(void) { return s_det_mode; }
op_time_t openpete_interp_det_period(void) { return s_det_period; }

/** @brief The deterministic-capture stamp of a roll at VBL @p tick.
 *
 * The VBL clock, tick * P/2 (P is the tick period, two VBLs), so the stamp is
 * a function of the capture's VBL alone (see the header note). Two distinct
 * states one logic step (two VBLs) apart are exactly P apart, a duplicate
 * committed at the cadence lands where the next logic step would, and no roll
 * is stamped at or before the head it follows: the object ring takes this
 * stamp, and a per-object pair whose curr stamp does not exceed its prev reads
 * as a stale pair. The stamp does not depend on how many duplicates were
 * committed, so the duplicate policy cannot move anything keyed on the
 * absolute stamp (the animated-texture phase the present clock is derived
 * from). The fallback past the head covers a roll at the head's own VBL.
 * @param tick The VBL counter of the roll.
 * @param curr The head snapshot the roll follows.
 * @return The stamp in nanoseconds. */
static op_time_t interp_det_stamp(uint32_t tick, const openpete_interp_snapshot_t* curr) {
    op_time_t t = (op_time_t)tick * s_det_period / 2;
    if (curr->valid && t <= curr->tick_ns) t = curr->tick_ns + s_det_period;
    return t;
}
op_time_t openpete_interp_det_vbl_stamp(uint32_t tick) {
    const op_time_t head = openpete_interp_curr_tick_ns();
    const op_time_t t = (op_time_t)tick * s_det_period / 2;
    return t > head ? t : head;
}
op_time_t openpete_interp_curr_tick_ns(void) {
    return (s_initialized && s_snap[s_head].valid) ? s_snap[s_head].tick_ns : 0;
}
const openpete_interp_snapshot_t* openpete_interp_curr_snapshot(void) {
    return (s_initialized && s_snap[s_head].valid) ? &s_snap[s_head] : NULL;
}
int openpete_interp_paired(void) {
    if (!s_initialized || !s_enabled) return 0;
    const openpete_interp_snapshot_t* curr = &s_snap[s_head];
    const openpete_interp_snapshot_t* prev = &s_snap[(s_head + 2) % 3];
    return (curr->valid && prev->valid && curr->total_bytes == prev->total_bytes);
}

void openpete_interp_capture(uint32_t tick, op_time_t tick_ns) {
    if (!s_initialized || !s_enabled) return;
    if (!s_regions || s_region_count == 0) return;
    s_last_tick = tick;
    s_last_tick_seen = 1;

    /* Build the candidate snapshot into scratch FIRST, so we can compare it to
     * the current head before deciding whether to advance the ring. */
    const openpete_interp_snapshot_t* curr = &s_snap[s_head];
    uint32_t total = curr->total_bytes;
    for (size_t r = 0; r < s_region_count; ++r) {
        const openpete_interp_region_t* reg = &s_regions[r];
        void* host = openpete_psx_to_host_span(reg->psx_addr, reg->length);
        uint32_t off = curr->region_offsets[r];
        if (host) memcpy(s_cap_scratch + off, host, reg->length);
        else      memset(s_cap_scratch + off, 0, reg->length);   /* unmapped -> deterministic */
    }
    s_total_captures++;

    /* Dedupe AT CAPTURE: Spyro's logic runs at 30 Hz on a 60 Hz VBL fiber, so
     * the transform-bearing state advances only every OTHER tick - yet this
     * capture fires every tick. A naive 2-slot ring (advance + fill every time)
     * then holds two SAME-frame duplicates whenever the camera updated on the
     * other VBL, so every sub-tick query sees prev==curr and dedupes -> the
     * interpolation bracket NEVER engages (paired=0 forever, which presents
     * as flicker and judder). By skipping the advance when the candidate is
     * byte-identical to the current head, the ring always holds the two most-
     * recent DISTINCT states, and curr->tick_ns stays pinned to when that state
     * FIRST appeared - exactly the [prev,curr] bracket alpha spans. A state
     * held past the cadence is committed as a duplicate (below). */
    const int identical = (curr->valid && total > 0 &&
                           memcmp(s_cap_scratch, curr->data, total) == 0);
    int dup_roll = 0;
    if (identical) {
        /* A state that has held for the logic's own cadence is static, not
         * mid-cadence: commit the duplicate so the ring holds a pair whose
         * endpoints are equal in content and distinct in tick. Interpolating
         * that pair is the identity on the registered regions and leaves the
         * bracket installed, so everything keyed on it - the world-animation
         * feeders, the record replays - keeps running on a screen whose camera
         * and player are held (the debug freecam holds both). Without it the
         * ring keeps the last pair that carried motion, which at a freecam
         * pose is the jump onto the pose: a cut, refused by every consumer,
         * leaving the sub-tick present a copy of the canonical one.
         *
         * The commit lands AT the cadence, not past it: a duplicate committed
         * one tick late lands every gap+1 ticks, so the ring's stamps advance
         * on a (gap+1)-tick grid while presents keep running per tick, and
         * every alpha-driven animation cycles against a period that is not the
         * tick's - the whole-scene judder at a held camera. */
        uint32_t held = (tick > curr->tick) ? tick - curr->tick : 0;
        const uint32_t cadence = (s_distinct_gap < INTERP_MIN_DISTINCT_GAP)
                               ? INTERP_MIN_DISTINCT_GAP : s_distinct_gap;
        if (held < cadence) {
            s_total_dedupe_skips++;
            return;
        }
        dup_roll = 1;
    } else if (curr->valid && !s_head_is_dup && tick > curr->tick) {
        /* Measured between distinct states only: a duplicate roll's own gap is
         * the cadence it was committed at, and feeding those back would let a
         * measurement error in one static episode persist through the next. */
        uint32_t g = tick - curr->tick;
        s_distinct_gap = (g > INTERP_MAX_DISTINCT_GAP) ? INTERP_MAX_DISTINCT_GAP : g;
    }

    /* OPENPETE_CAP_ROLL_DBG: one line per ROLL - which frame, and the first
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

    if (s_det_mode && s_det_period > 0) tick_ns = interp_det_stamp(tick, curr);

    /* State advanced - commit to the next ring slot. snap[s_head] becomes the
     * "latest distinct tick"; the two prior slots hold prev and prev2. */
    s_head = (s_head + 1) % 3;
    s_head_is_dup = dup_roll;
    openpete_interp_snapshot_t* slot = &s_snap[s_head];
    slot->tick    = tick;
    slot->tick_ns = tick_ns;
    memcpy(slot->data, s_cap_scratch, total);
    slot->valid = 1;
}

/** @brief Report whether the ra_commit region path is enabled by the environment.
 *
 * OPENPETE_RA_CAM_COMMIT enables it unless set to a value starting with
 * '0'; unset and empty count as enabled. The environment is read once, on
 * the first call, and latched.
 * @return 1 when enabled, else 0. */
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
    /* Overwrite flagged regions of the NEWEST slot in place - even when this
     * tick's capture deduped (no roll): the slot then still holds the last
     * distinct state, and refreshing its committed regions keeps them the
     * freshest committed bytes known. tick/tick_ns stay untouched; the
     * committed region shares the led regions' alpha mapping, it just pairs
     * one branch-free timeline instead of the led one. */
    for (size_t r = 0; r < s_region_count; ++r) {
        if (!s_regions[r].ra_commit) continue;
        void* host = openpete_psx_to_host_span(s_regions[r].psx_addr, s_regions[r].length);
        if (host) memcpy(curr->data + curr->region_offsets[r], host,
                         s_regions[r].length);
    }
}

/* Staleness of the head against the latest capture; tolerance = the pair's gap. */
int openpete_interp_curr_is_stale(void) {
    if (!s_initialized || !s_enabled || !s_last_tick_seen) return 0;
    const openpete_interp_snapshot_t* curr = &s_snap[s_head];
    const openpete_interp_snapshot_t* prev = &s_snap[(s_head + 2) % 3];
    if (!curr->valid) return 0;
    if (s_last_tick <= curr->tick) return 0;
    uint32_t gap = (prev->valid && prev->tick < curr->tick) ? curr->tick - prev->tick : 1u;
    return (s_last_tick - curr->tick) > gap;
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

    /* Byte-identical endpoints still pair. The ring only commits such a pair
     * once the state has held past the logic's cadence (see capture), so the
     * pair describes a static screen rather than a tick the logic skipped, and
     * the lerp across it is the identity. The frozen-pair case the query used
     * to reject here - a pair that carried motion which has since ended - is
     * held by openpete_interp_curr_is_stale(), which forces alpha 1. */
    /* Alpha is measured from prev->tick_ns. Render shows state at
     * `prev->tick_ns + alpha * sim_period`, so alpha in [0, 1] maps to
     * [prev, curr]; the host loop runs between curr and the next,
     * yet-uncaptured tick, so the present lerps one tick in the past. */
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
         * or when the query itself didn't pair - callers fall back to linear. */
        const openpete_interp_snapshot_t* p2 = &s_snap[(s_head + 1) % 3];
        *out_prev2 = (r && p2->valid) ? p2 : NULL;
    }
    return r;
}

uint64_t openpete_interp_total_captures(void)     { return s_total_captures;     }
uint64_t openpete_interp_total_dedupe_skips(void) { return s_total_dedupe_skips; }
