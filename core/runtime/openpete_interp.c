/* core/runtime/openpete_interp.c — render-side snapshot ring.
 *
 * See openpete_interp.h for the API contract. v1 scaffolding: capture
 * + dedupe predicate only; lerped re-emission lands in #58.
 *
 * Determinism notes (this TU is read-only against PSX RAM):
 *   - openpete_interp_capture() runs after ticked_this_iter; the game
 *     fiber is yielded inside VSync(0), so RAM is consistent.
 *   - No write path touches PSX RAM. Phase 2a will add a bracket
 *     (save-canonical → write-lerped → render → restore-canonical)
 *     gated entirely on the host fiber.
 */

#include "openpete_interp.h"
#include "openpete_psx.h"
#include "openpete_log.h"

#include <stdint.h>
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

static openpete_interp_snapshot_t s_snap[2];
static int      s_head        = 0;   /* index of the most-recently-written slot */
static int      s_initialized = 0;
static int      s_enabled     = 0;

static uint64_t s_total_captures     = 0;
static uint64_t s_total_dedupe_skips = 0;

void openpete_interp_register_regions(const openpete_interp_region_t* regions,
                                      size_t count) {
    s_regions = regions;
    s_region_count = (regions ? count : 0);
}

int openpete_interp_init(void) {
    if (s_initialized) return 0;

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
    for (int i = 0; i < 2; ++i) {
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

void openpete_interp_set_enabled(int on) {
    s_enabled = on ? 1 : 0;
}

int openpete_interp_enabled(void) {
    return s_enabled;
}

void openpete_interp_capture(uint32_t tick, op_time_t tick_ns) {
    if (!s_initialized || !s_enabled) return;
    if (!s_regions || s_region_count == 0) return;

    /* Advance the head, then fill. snap[s_head] becomes "latest tick";
     * snap[s_head ^ 1] holds "previous tick". */
    s_head ^= 1;
    openpete_interp_snapshot_t* slot = &s_snap[s_head];
    slot->tick    = tick;
    slot->tick_ns = tick_ns;

    for (size_t r = 0; r < s_region_count; ++r) {
        const openpete_interp_region_t* reg = &s_regions[r];
        void* host = openpete_psx_to_host_ptr(reg->psx_addr);
        uint32_t off = slot->region_offsets[r];
        if (host) {
            memcpy(slot->data + off, host, reg->length);
        } else {
            /* Unmapped address — zero the slot so dedupe behavior is
             * deterministic. Should not happen for KSEG0 RAM ranges. */
            memset(slot->data + off, 0, reg->length);
        }
    }

    slot->valid = 1;
    s_total_captures++;
}

int openpete_interp_query(op_time_t now_ns,
                          op_time_t sim_period_ns,
                          double*   out_alpha,
                          const openpete_interp_snapshot_t** out_prev,
                          const openpete_interp_snapshot_t** out_curr) {
    if (!s_initialized || !s_enabled) return 0;

    const openpete_interp_snapshot_t* curr = &s_snap[s_head];
    const openpete_interp_snapshot_t* prev = &s_snap[s_head ^ 1];
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

uint64_t openpete_interp_total_captures(void)     { return s_total_captures;     }
uint64_t openpete_interp_total_dedupe_skips(void) { return s_total_dedupe_skips; }
