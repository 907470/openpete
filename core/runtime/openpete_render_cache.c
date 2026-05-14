/* openpete_render_cache.c — host-side world-space cache, 3-generation
 * ring. See openpete_render_cache.h and
 * notes/plans/openpete-renderer-foundation.md.
 *
 * Generation model:
 *   gen[current]  — receiving GTE writes this yield.
 *   gen[prev]     — last GAME FRAME's GTE writes. This is the cache the
 *                   sim-due DrawOTag AND intermediate renders look up.
 *                   The OT being walked at sim-due was built by last
 *                   game frame's GTE → its SXY values were emitted into
 *                   THIS buffer.
 *   gen[prev2]    — two game frames ago. Held only so the recycle path
 *                   has somewhere to land without overwriting prev while
 *                   intermediate renders are still hitting it.
 *
 * Rotation policy:
 *   Host loop calls openpete_render_cache_arm_rotate() before each
 *   yield. The arm is one-shot: it sets a pending flag, and the FIRST
 *   emit_vertex of the next yield consumes it (rotate-on-first-emit).
 *   PadVSync-only yields don't emit, so the pending flag carries over —
 *   no data destruction on no-GTE yields, no rotate per VBL on a 30 Hz
 *   game.
 *
 * Why first-emit (not pre-co_switch):
 *   - Pre-co_switch unconditional rotate destroyed data on PadVSync-only
 *     yields (see soak f3112-f3140 vs f1-f3000: 1540 hits vs 9234
 *     misses).
 *   - Post-DrawOTag rotate forces sim-due and intermediate to read from
 *     different generations (prev vs prev2), needing API divergence.
 *   - First-emit-per-yield is the unique timing where the SAME lookup
 *     API (lookup_prev) serves both sim-due AND intermediate without
 *     losing alignment with the saved OT.
 *
 * Linear-probe open addressing, key = packed int16 SXY (matches what
 * the game mfc2's into prim packets). seq counter is monotonic; logical
 * clear of recycled gen happens by bumping its seq, which makes every
 * slot read as "stale".
 */

#include "openpete_render_cache.h"
#include "openpete_time.h"

#include <string.h>

typedef struct {
    uint32_t                       seq;
    openpete_render_cache_entry_t  slots[OPENPETE_RENDER_CACHE_SLOTS];
} openpete_render_cache_frame_t;

#define NUM_GENS 3

/* seq starts at 1 so BSS-zeroed slots (seq=0) read as empty on the
 * first frame's lookups. */
static openpete_render_cache_frame_t s_gens[NUM_GENS] = {
    { .seq = 1 }, { .seq = 1 }, { .seq = 1 },
};
static int s_current_idx = 0;

/* Rotate is one-shot: armed by host before yield, consumed by first
 * emit_vertex of the yield. PadVSync-only yields don't emit so the
 * arm carries over harmlessly. */
static int s_rotate_pending = 0;

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
 * See note in the file's prior incarnation — matcher.c.o lives in a
 * separate static archive that GNU ld won't revisit; this symbol
 * reference drags it along as a fellow-traveler. */
extern void openpete_render_matcher_finalize_frame(void);
__attribute__((used))
static void (*const s_openpete_render_matcher_force_link)(void) =
    openpete_render_matcher_finalize_frame;

static inline int gen_idx_prev(void) {
    return (s_current_idx + NUM_GENS - 1) % NUM_GENS;
}
/* Generation two game frames back; reserved for the recycle path. Not
 * read by sim-due or intermediate, but accessible for future plans
 * (e.g. quadratic-velocity prediction). */
static inline int gen_idx_prev2(void) {
    return (s_current_idx + NUM_GENS - 2) % NUM_GENS;
}

static inline openpete_render_cache_frame_t* gen_current(void) {
    return &s_gens[s_current_idx];
}
static inline openpete_render_cache_frame_t* gen_prev(void) {
    return &s_gens[gen_idx_prev()];
}

static void consume_pending_rotate(void) {
    if (!s_rotate_pending) return;
    s_rotate_pending = 0;
    s_current_idx = (s_current_idx + 1) % NUM_GENS;
    /* Logical clear of the newly-rotated-in slot (which holds gen[F-2]
     * data and is about to be overwritten with gen[F] writes). The
     * other two generations keep their seqs intact so prev and prev2
     * lookups stay valid. */
    s_gens[s_current_idx].seq++;
    /* Matrix-span detector resets so the first emit of the new game
     * frame always registers as a fresh span. */
    s_matrix_state_initialized = 0;
}

static inline uint32_t hash_sxy(uint32_t key) {
    uint32_t h = key ^ (key >> 16);
    h *= 0x9E3779B1u;
    return h & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
}

void openpete_render_cache_emit_vertex(const openpete_render_cache_entry_t* src) {
    /* First-emit-per-yield rotate. No-op if not armed. */
    consume_pending_rotate();

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

    openpete_render_cache_frame_t* curr = gen_current();
    const uint32_t key = src->sxy_key;
    uint32_t idx = hash_sxy(key);
    const uint32_t curr_seq = curr->seq;

    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_PROBE_MAX; ++i) {
        openpete_render_cache_entry_t* slot = &curr->slots[idx];
        /* Empty (stale seq) OR same-key overwrite. Most-recent-writer
         * wins (matches the FIFO-top semantics of push_sxy). */
        if (slot->seq != curr_seq || slot->sxy_key == key) {
            *slot = *src;
            slot->seq = curr_seq;
            slot->matrix_span_id = g_openpete_render_cache_matrix_span_id;
            g_openpete_render_cache_emit_count++;
            return;
        }
        idx = (idx + 1u) & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
    }
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
        if (slot->seq != frame_seq) return 0;
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
    return lookup_frame(gen_current(), sxy_key, out);
}

int openpete_render_cache_lookup_prev(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out) {
    return lookup_frame(gen_prev(), sxy_key, out);
}

/* OT-walker entry point. Target = gen[prev] = last game frame's GTE
 * writes, which is the source of the SXY values in the OT being
 * walked. 98% hit rate empirically (level_10 active gameplay). */
int openpete_render_cache_lookup_curr_raw(uint32_t sxy_key) {
    return lookup_frame(gen_prev(), sxy_key, NULL);
}

void openpete_render_cache_for_each_curr_entry(
        openpete_render_cache_iter_fn cb, void* userdata) {
    const openpete_render_cache_frame_t* frame = gen_current();
    const uint32_t seq = frame->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_SLOTS; ++i) {
        const openpete_render_cache_entry_t* slot = &frame->slots[i];
        if (slot->seq == seq) cb(slot, userdata);
    }
}

void openpete_render_cache_for_each_prev_entry(
        openpete_render_cache_iter_fn cb, void* userdata) {
    const openpete_render_cache_frame_t* frame = gen_prev();
    const uint32_t seq = frame->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_SLOTS; ++i) {
        const openpete_render_cache_entry_t* slot = &frame->slots[i];
        if (slot->seq == seq) cb(slot, userdata);
    }
}

void openpete_render_cache_arm_rotate(void) {
    s_rotate_pending = 1;
}

/* Legacy no-deferred rotate. Retained for tests that pre-date the
 * arm_rotate split; advances the ring immediately. */
void openpete_render_cache_begin_frame(void) {
    s_rotate_pending = 1;
    consume_pending_rotate();
}

/* Phase 2.1 — sim-tick boundary state. */
static openpete_op_time_t s_last_sim_tick_ns   = 0;
static openpete_op_time_t s_last_sim_period_ns = 0;

void openpete_render_sim_tick_advance_time(openpete_op_time_t sim_tick_ns,
                                           openpete_op_time_t sim_period_ns) {
    s_last_sim_tick_ns   = sim_tick_ns;
    s_last_sim_period_ns = sim_period_ns;
}

void openpete_render_sim_tick_rotate_cache(void) {
    /* Compat shim — arm the deferred rotate so the next yield's first
     * emit consumes it. Old callers that expected immediate rotation
     * are gone (run.c calls arm_rotate directly now). */
    openpete_render_cache_arm_rotate();
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
