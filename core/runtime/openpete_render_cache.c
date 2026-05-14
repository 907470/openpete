/* openpete_render_cache.c — host-side world-space cache, double-buffered
 * by render frame. See openpete_render_cache.h and
 * notes/plans/openpete-renderer-foundation.md Phase 0.
 *
 * Implementation notes:
 * - Two parallel arrays of OPENPETE_RENDER_CACHE_SLOTS entries; pointer-
 *   swap on begin_frame so consumers always see the previous render
 *   frame's data while producers write the current one.
 * - Linear-probe open addressing keyed by sxy_key, with a probe cap to
 *   bound worst-case insertion cost. Probe overflows are counted but not
 *   fatal — Phase 1's matcher will use (matrix_span_id, vertex_idx) as a
 *   collision-free fallback key when needed.
 * - "Clearing" the current frame is logical only: we bump frame->seq and
 *   treat any slot whose stored seq doesn't match as empty. Wraparound at
 *   2^32 frames is impractical (~2 years at 60 Hz).
 * - Producer auto-bumps matrix_span_id when RT/TR change vs the last
 *   emit. Same detector pattern as the existing render-probe in
 *   gte.cpp:1228, just promoted to first-class cache state.
 */

#include "openpete_render_cache.h"
#include "openpete_time.h"

#include <string.h>

typedef struct {
    uint32_t                       seq;
    openpete_render_cache_entry_t  slots[OPENPETE_RENDER_CACHE_SLOTS];
} openpete_render_cache_frame_t;

/* seq starts at 1 so that BSS-zeroed slot.seq=0 reliably reads as "empty"
 * on the very first frame's lookups (before the first rotate). The seq
 * counter is monotonic-mod-2^32 from there. */
static openpete_render_cache_frame_t s_frame_a = { .seq = 1 };
static openpete_render_cache_frame_t s_frame_b = { .seq = 1 };
static openpete_render_cache_frame_t* s_curr = &s_frame_a;
static openpete_render_cache_frame_t* s_prev = &s_frame_b;

/* Matrix-span detector state, scoped to the producer side. */
static int16_t  s_last_rt[3][3];
static int32_t  s_last_tr[3];
static int      s_matrix_state_initialized = 0;

unsigned g_openpete_render_cache_emit_count            = 0u;
unsigned g_openpete_render_cache_lookup_count          = 0u;
unsigned g_openpete_render_cache_hit_count             = 0u;
unsigned g_openpete_render_cache_probe_overflow_count  = 0u;
unsigned g_openpete_render_cache_matrix_span_id        = 0u;

/* Linker force-pull anchor for the renderer-foundation matcher.
 *
 * The matcher TU (openpete_render_matcher.c) is referenced only by
 * PsyCross's PsyX_GPU.cpp and PsyX_main.cpp, both of which compile
 * into libpsycross.a — a separate archive linked AFTER
 * libopenpete_core.a. GNU ld's default left-to-right archive pass
 * won't revisit openpete_core.a once it has moved on to psycross, so
 * matcher.c.o would be left out of the final binary.
 *
 * This file (cache.c) IS pulled into the link by gte.cpp's reference
 * to openpete_render_cache_emit_vertex, so referencing a matcher
 * symbol here drags the matcher TU along as a fellow-traveler. */
extern void openpete_render_matcher_finalize_frame(void);
__attribute__((used))
static void (*const s_openpete_render_matcher_force_link)(void) =
    openpete_render_matcher_finalize_frame;

static inline uint32_t hash_sxy(uint32_t key) {
    /* Fibonacci-style mix: cheap, distributes both halves over the slot
     * range so neighbours-on-screen don't always collide. */
    uint32_t h = key ^ (key >> 16);
    h *= 0x9E3779B1u;
    return h & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
}

void openpete_render_cache_emit_vertex(const openpete_render_cache_entry_t* src) {
    /* Detect matrix change against last-emitted RT/TR. */
    if (!s_matrix_state_initialized ||
        memcmp(s_last_rt, src->rt, sizeof(s_last_rt)) != 0 ||
        s_last_tr[0] != src->tr[0] ||
        s_last_tr[1] != src->tr[1] ||
        s_last_tr[2] != src->tr[2]) {
        g_openpete_render_cache_matrix_span_id++;
        memcpy(s_last_rt, src->rt, sizeof(s_last_rt));
        s_last_tr[0] = src->tr[0];
        s_last_tr[1] = src->tr[1];
        s_last_tr[2] = src->tr[2];
        s_matrix_state_initialized = 1;
    }

    const uint32_t key = src->sxy_key;
    uint32_t idx = hash_sxy(key);
    const uint32_t curr_seq = s_curr->seq;

    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_PROBE_MAX; ++i) {
        openpete_render_cache_entry_t* slot = &s_curr->slots[idx];
        /* Empty (stale seq) OR same-key overwrite. Same-key overwrite is
         * deliberate: the most recent emission wins, since the most recent
         * SXY is the one the game will push into the prim packet (FIFO
         * top). For Spyro the producer order matches the consumer order
         * within a matrix span, so collisions across spans naturally
         * resolve to "last writer". */
        if (slot->seq != curr_seq || slot->sxy_key == key) {
            *slot = *src;
            slot->seq = curr_seq;
            slot->matrix_span_id = g_openpete_render_cache_matrix_span_id;
            g_openpete_render_cache_emit_count++;
            return;
        }
        idx = (idx + 1u) & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
    }
    /* Probe-cap exceeded — drop. Counted so we can tune slot count if it
     * ever exceeds a handful per frame. */
    g_openpete_render_cache_probe_overflow_count++;
}

static int lookup_frame(const openpete_render_cache_frame_t* frame,
                        uint32_t sxy_key,
                        openpete_render_cache_entry_t* out) {
    g_openpete_render_cache_lookup_count++;
    uint32_t idx = hash_sxy(sxy_key);
    const uint32_t frame_seq = frame->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_PROBE_MAX; ++i) {
        const openpete_render_cache_entry_t* slot = &frame->slots[idx];
        if (slot->seq != frame_seq) return 0;        /* empty → key absent */
        if (slot->sxy_key == sxy_key) {
            if (out) *out = *slot;
            g_openpete_render_cache_hit_count++;
            return 1;
        }
        idx = (idx + 1u) & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
    }
    return 0;
}

int openpete_render_cache_lookup_curr(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out) {
    return lookup_frame(s_curr, sxy_key, out);
}

int openpete_render_cache_lookup_prev(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out) {
    return lookup_frame(s_prev, sxy_key, out);
}

/* Header-free entry point for PsyCross callers that only want the
 * hit/miss counter side-effect (Phase 0 probe). Avoids dragging
 * openpete_render_cache.h through PsyX's include surface. */
/* OT-walker entry point.
 *
 * Spyro's render-emit pipeline runs ONE GAME FRAME AHEAD of DrawOTag: the
 * game fiber's GTE work in frame N populates an OT that DrawOTag walks
 * during frame N+1's render pass. Empirically verified at level_10 f2200:
 * looking up SXY keys in s_curr (just-emitted) misses 100 % of the time,
 * while looking up in s_prev (last-render-frame's emits) hits 98 %.
 *
 * So this entry point — used by ParsePrimitive → MakeVertexTriangle /
 * MakeVertexQuad — targets s_prev. Phase 1's matcher will want a third
 * buffer (frame-N-2) to compare "now-visible W" against
 * "one-game-frame-ago-visible W" for inter-frame interpolation. */
int openpete_render_cache_lookup_curr_raw(uint32_t sxy_key) {
    return lookup_frame(s_prev, sxy_key, NULL);
}

void openpete_render_cache_for_each_curr_entry(
        openpete_render_cache_iter_fn cb, void* userdata) {
    const uint32_t curr_seq = s_curr->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_SLOTS; ++i) {
        const openpete_render_cache_entry_t* slot = &s_curr->slots[i];
        if (slot->seq == curr_seq) cb(slot, userdata);
    }
}

void openpete_render_cache_for_each_prev_entry(
        openpete_render_cache_iter_fn cb, void* userdata) {
    const uint32_t prev_seq = s_prev->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_SLOTS; ++i) {
        const openpete_render_cache_entry_t* slot = &s_prev->slots[i];
        if (slot->seq == prev_seq) cb(slot, userdata);
    }
}

void openpete_render_cache_begin_frame(void) {
    openpete_render_cache_frame_t* tmp = s_prev;
    s_prev = s_curr;
    s_curr = tmp;
    /* Logical clear via seq bump. Wrap-safe (seq is monotonic mod 2^32). */
    s_curr->seq++;
    /* Reset the matrix-span detector so the first emit of the new frame
     * always registers as a fresh span (avoids cross-frame span IDs
     * spanning a DrawOTag boundary). */
    s_matrix_state_initialized = 0;
}

/* Phase 2.1 — sim-tick boundary state. Tracks the timestamp of the
 * most recent sim_tick_advanced call so substitute can compute alpha
 * relative to it. sim_period_ns is the inter-tick interval (= Spyro
 * game-frame period ≈ 33.36 ms). */
static openpete_op_time_t s_last_sim_tick_ns   = 0;
static openpete_op_time_t s_last_sim_period_ns = 0;

void openpete_render_sim_tick_advance_time(openpete_op_time_t sim_tick_ns,
                                           openpete_op_time_t sim_period_ns) {
    s_last_sim_tick_ns   = sim_tick_ns;
    s_last_sim_period_ns = sim_period_ns;
}

void openpete_render_sim_tick_rotate_cache(void) {
    openpete_render_cache_begin_frame();
}

float g_openpete_substitute_alpha_snapshot = 0.0f;

void openpete_render_sim_tick_mark_now(void) {
    s_last_sim_tick_ns = op_now();
    g_openpete_substitute_alpha_snapshot = 0.0f;
}

void openpete_render_substitute_alpha_snapshot_intermediate(void) {
    g_openpete_substitute_alpha_snapshot =
        openpete_render_compute_alpha(op_now());
}

float openpete_render_compute_alpha(openpete_op_time_t now_ns) {
    if (s_last_sim_period_ns <= 0) return 0.0f;
    openpete_op_time_t delta = now_ns - s_last_sim_tick_ns;
    if (delta <= 0) return 0.0f;
    if (delta >= s_last_sim_period_ns) return 1.0f;
    return (float)((double)delta / (double)s_last_sim_period_ns);
}
