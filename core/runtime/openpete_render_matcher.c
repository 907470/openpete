/* openpete_render_matcher.c — draw-call matcher (renderer-foundation
 * Phase 1). See openpete_render_matcher.h for the contract and
 * notes/plans/openpete-renderer-foundation.md for the architecture.
 *
 * Per-frame flow:
 *
 *   PsyCross OT walker (per prim):
 *     openpete_render_matcher_observe_{tri,quad}(p0, p1, ...)
 *       └── for each vertex: lookup s_prev → recover matrix_span_id +
 *           world-space MAC + ...
 *       └── find-or-create batch keyed by matrix_span_id
 *       └── accumulate bbox + centroid sum into batch
 *
 *   PsyCross PsyX_EndScene:
 *     openpete_render_matcher_finalize_frame()
 *       └── for each batch: centroid = sum/count; bbox_size_log2;
 *           hash = fnv1a(vertex_count, size_log2, centroid_quantized)
 *       └── build hash table over prev frame's batches
 *       └── for each curr batch: probe prev → if match: inherit
 *           object_id, compute velocity = curr.centroid - prev.centroid;
 *           else: assign fresh object_id
 *       └── count prev batches not consumed = dropped
 *       └── swap curr/prev
 *
 * Match-rate diagnostics flow through five counters that PsyX_EndScene
 * dumps once per frame when OPENPETE_RENDER_PROBE=1.
 */

#include "openpete_render_matcher.h"
#include "openpete_render_cache.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MATCHER_MAX_BATCHES 256u
#define MATCHER_HASH_SLOTS  512u  /* power-of-two */
#define MATCHER_PROBE_MAX   32u

typedef struct matcher_batch {
    uint64_t hash;
    uint32_t matrix_span_id;
    uint32_t object_id;
    uint32_t prim_count;
    uint32_t vertex_count;
    int32_t  bbox_min[3];
    int32_t  bbox_max[3];
    int64_t  centroid_sum[3];
    int32_t  centroid[3];
    int32_t  velocity[3];
    int      consumed;       /* used by finalize to count dropped prev batches */

    /* Phase 2 — screen-space centroid for SXY substitution. Accumulated
     * from each observed prim's vertex SXY values (which the cache key
     * stores in packed form). */
    int64_t  sxy_sum[2];
    int32_t  sxy_centroid[2];

    /* Phase 2 — FRAME-N future anchor. Populated by the
     * build_future_anchors pass after the OT-walker observe pass
     * completes, when a future batch hashes to the same bucket as
     * this batch. Lerp anchors are (centroid → next_centroid) and
     * (sxy_centroid → next_sxy_centroid). */
    int      has_future_anchor;
    int32_t  next_centroid[3];
    int32_t  next_sxy_centroid[2];
} matcher_batch_t;

typedef struct matcher_frame {
    matcher_batch_t batches[MATCHER_MAX_BATCHES];
    uint32_t        count;
    /* hash_table[slot] = batch_index + 1 (0 = empty). Open-addressed
     * with linear probe. Rebuilt over prev frame at finalize. */
    uint16_t        hash_table[MATCHER_HASH_SLOTS];
} matcher_frame_t;

static matcher_frame_t  s_frame_a;
static matcher_frame_t  s_frame_b;
static matcher_frame_t* s_curr = &s_frame_a;
static matcher_frame_t* s_prev = &s_frame_b;

static uint32_t s_next_object_id = 1u;

unsigned g_openpete_render_matcher_batch_count             = 0u;
unsigned g_openpete_render_matcher_matched_count           = 0u;
unsigned g_openpete_render_matcher_unmatched_new_count     = 0u;
unsigned g_openpete_render_matcher_unmatched_dropped_count = 0u;
unsigned g_openpete_render_matcher_no_cache_count          = 0u;
unsigned g_openpete_render_matcher_future_anchored_count   = 0u;

/* Phase 2 substitute pipeline stats. */
unsigned g_openpete_render_substitute_pair_count           = 0u;
unsigned g_openpete_render_substitute_vert_hit_count       = 0u;
unsigned g_openpete_render_substitute_vert_miss_count      = 0u;

/* Accumulators reset on every observe call's caller-decided frame
 * boundary — we use s_curr->count == 0 as the "fresh frame" signal.
 * find_or_create_batch initializes a slot with bbox sentinels. */
static matcher_batch_t* find_or_create_batch(uint32_t matrix_span_id) {
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        if (s_curr->batches[i].matrix_span_id == matrix_span_id)
            return &s_curr->batches[i];
    }
    if (s_curr->count >= MATCHER_MAX_BATCHES) return NULL;
    matcher_batch_t* b = &s_curr->batches[s_curr->count++];
    memset(b, 0, sizeof(*b));
    b->matrix_span_id = matrix_span_id;
    b->bbox_min[0] = b->bbox_min[1] = b->bbox_min[2] = INT32_MAX;
    b->bbox_max[0] = b->bbox_max[1] = b->bbox_max[2] = INT32_MIN;
    return b;
}

static inline uint32_t sxy_key_from_pair(const int16_t* p) {
    return ((uint32_t)(uint16_t)p[0]) | (((uint32_t)(uint16_t)p[1]) << 16);
}

static inline void accumulate_vertex(matcher_batch_t* b,
                                     const openpete_render_cache_entry_t* e) {
    for (int axis = 0; axis < 3; ++axis) {
        int32_t v = e->mac[axis];
        if (v < b->bbox_min[axis]) b->bbox_min[axis] = v;
        if (v > b->bbox_max[axis]) b->bbox_max[axis] = v;
        b->centroid_sum[axis] += (int64_t)v;
    }
    /* Screen-space centroid accumulator. sxy_key packs sx in low half,
     * sy in high half (matching cache.s_curr/s_prev's storage). */
    int16_t sx = (int16_t)(uint16_t)(e->sxy_key & 0xFFFFu);
    int16_t sy = (int16_t)(uint16_t)(e->sxy_key >> 16);
    b->sxy_sum[0] += (int64_t)sx;
    b->sxy_sum[1] += (int64_t)sy;
    b->vertex_count++;
}

/* Observe one vertex: lookup s_prev → return entry by value. On miss
 * (vertex not in cache, e.g. line or sprite not produced by GTE),
 * bumps the no_cache counter and returns 0. */
static int observe_vertex(const int16_t* p, openpete_render_cache_entry_t* out) {
    uint32_t key = sxy_key_from_pair(p);
    if (openpete_render_cache_lookup_prev(key, out)) return 1;
    g_openpete_render_matcher_no_cache_count++;
    return 0;
}

/* Observe hooks are RETAINED in the API for source compatibility with
 * Phase 1's plumbing in PsyX_GPU.cpp, but no longer drive matcher state
 * — Phase 2 builds matcher.curr eagerly via cache scan in
 * openpete_render_matcher_prepare_substitute(). The observe hooks
 * only bump prim_count on the batch (used for per-batch stats), since
 * eager scan is vertex-level and doesn't know prim boundaries. */
static matcher_batch_t* find_batch_by_span(uint32_t matrix_span_id) {
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        if (s_curr->batches[i].matrix_span_id == matrix_span_id)
            return &s_curr->batches[i];
    }
    return NULL;
}

void openpete_render_matcher_observe_tri(const int16_t* p0,
                                          const int16_t* p1,
                                          const int16_t* p2) {
    (void)p1; (void)p2;
    openpete_render_cache_entry_t e;
    uint32_t key = ((uint32_t)(uint16_t)p0[0]) |
                   (((uint32_t)(uint16_t)p0[1]) << 16);
    if (!openpete_render_cache_lookup_prev(key, &e)) {
        g_openpete_render_matcher_no_cache_count++;
        return;
    }
    matcher_batch_t* b = find_batch_by_span(e.matrix_span_id);
    if (b) b->prim_count++;
}

void openpete_render_matcher_observe_quad(const int16_t* p0,
                                           const int16_t* p1,
                                           const int16_t* p2,
                                           const int16_t* p3) {
    (void)p1; (void)p2; (void)p3;
    openpete_render_cache_entry_t e;
    uint32_t key = ((uint32_t)(uint16_t)p0[0]) |
                   (((uint32_t)(uint16_t)p0[1]) << 16);
    if (!openpete_render_cache_lookup_prev(key, &e)) {
        g_openpete_render_matcher_no_cache_count++;
        return;
    }
    matcher_batch_t* b = find_batch_by_span(e.matrix_span_id);
    if (b) b->prim_count++;
}

static inline int ilog2_ceil_i64(int64_t v_signed) {
    uint64_t v = (v_signed < 0) ? (uint64_t)-v_signed : (uint64_t)v_signed;
    if (v <= 1) return 0;
    int r = 0;
    v -= 1;
    while (v) { v >>= 1; r++; }
    return r;
}

static uint64_t fnv1a64(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Centroid quantization shift. MAC is q12 (world * 4096). Shifting
 * right 18 = world / 64, so each bucket is 64 world units across —
 * big enough that single-frame motion stays in the same bucket
 * (~3-30 world units of motion at 30 Hz), small enough that distinct
 * objects in different parts of the level don't collide.
 *
 * Tuning history (level_10 f2200):
 *   shift=16 (16-unit buckets) + bbox_size_log2 + vc → 40 % match
 *   shift=18 (64-unit buckets), vc only             → see below */
#define MATCHER_CENTROID_SHIFT 18

/* Shared hash function so the future-anchor scan produces identical
 * hashes for the same logical batch as the OT-walker observe pass. */
static uint64_t batch_centroid_hash(int32_t cx, int32_t cy, int32_t cz) {
    int32_t cq[3] = {
        cx >> MATCHER_CENTROID_SHIFT,
        cy >> MATCHER_CENTROID_SHIFT,
        cz >> MATCHER_CENTROID_SHIFT,
    };
    /* See "Phase 1 hash" rationale in finalize_frame: centroid-only. */
    return fnv1a64(cq, sizeof(cq));
}

/* ----------------------------------------------------------------------
 * Phase 2 — substitute pipeline (velocity-extrapolation model).
 *
 * Empirically (2026-05-14), Spyro does DrawOTag BEFORE GTE within a
 * single game frame: the OT walked at frame N's render contains prims
 * from frame N-1's GTE, and frame N's GTE happens AFTER DrawOTag
 * returns. This means at substitute time, cache.s_curr is empty (no
 * "future" anchor available) — only cache.s_prev (the just-completed
 * GTE work that produced the OT being walked) has data.
 *
 * Without a known future anchor we extrapolate: compute velocity from
 * (last render's matcher batches → this render's matcher batches),
 * then at substitute time apply `delta_sxy = alpha * sxy_velocity` to
 * predict where each matched batch will be at alpha within the current
 * display period. For stationary play velocity≈0 → no-op. For constant
 * motion, prediction is exact. For accel/decel, prediction
 * over/undershoots but stays smooth.
 *
 * Pipeline:
 *   psyq_DrawOTag → openpete_render_matcher_prepare_substitute():
 *     - Scan cache.s_prev → build matcher.curr batches.
 *     - Match against matcher.prev (= last frame's matcher.curr) by
 *       centroid hash → compute sxy_velocity per matched batch.
 *
 *   MakeVertexTriangle/Quad → openpete_render_matcher_substitute_query:
 *     - Lookup cache.s_prev[p's sxy] → matrix_span_id.
 *     - Find matcher.curr batch with that span_id.
 *     - Return alpha * batch.sxy_velocity as delta.
 *
 *   PsyX_EndScene → openpete_render_matcher_finalize_frame():
 *     - Swap matcher.curr ↔ matcher.prev (preserves N-1 batches as
 *       N-2 for the next render's velocity computation).
 * ---------------------------------------------------------------------- */

/* Scratch frame state for the substitute pipeline. We use the existing
 * matcher_frame_t struct so the (curr, prev) swap mechanism is the
 * same, but build matcher.curr eagerly via cache scan instead of via
 * per-prim observe. Observe still runs (for Phase 1 diagnostic match-
 * rate metric); its incremental matcher.curr build is redundant with
 * the eager scan, but harmless — it writes to the same matcher state
 * after prepare ran, so its accumulators add on top. */

static void sub_scan_cb_curr(const openpete_render_cache_entry_t* e, void* ud) {
    (void)ud;
    matcher_batch_t* b = find_or_create_batch(e->matrix_span_id);
    if (!b) return;
    /* Don't bump prim_count here — eager scan is vertex-level. We just
     * accumulate position data; matcher.curr is otherwise built the
     * usual way via per-vertex accumulate. */
    accumulate_vertex(b, e);
}

void openpete_render_matcher_prepare_substitute(void) {
    /* Reset matcher.curr to a clean state, then rebuild from cache scan. */
    s_curr->count = 0u;
    g_openpete_render_substitute_vert_hit_count  = 0u;
    g_openpete_render_substitute_vert_miss_count = 0u;
    g_openpete_render_matcher_no_cache_count     = 0u;

    /* Pass 1: scan cache.s_prev (= world data for OT about to be walked)
     * to build matcher.curr batches keyed by matrix_span_id. */
    openpete_render_cache_for_each_prev_entry(sub_scan_cb_curr, NULL);

    /* Pass 2: finalize each batch (centroid, sxy_centroid, hash). */
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* b = &s_curr->batches[i];
        if (b->vertex_count == 0) continue;
        for (int axis = 0; axis < 3; ++axis) {
            b->centroid[axis] =
                (int32_t)(b->centroid_sum[axis] / (int64_t)b->vertex_count);
        }
        b->sxy_centroid[0] = (int32_t)(b->sxy_sum[0] / (int64_t)b->vertex_count);
        b->sxy_centroid[1] = (int32_t)(b->sxy_sum[1] / (int64_t)b->vertex_count);
        b->hash = batch_centroid_hash(b->centroid[0],
                                      b->centroid[1],
                                      b->centroid[2]);
        b->has_future_anchor = 0;
        b->velocity[0] = b->velocity[1] = b->velocity[2] = 0;
    }

    /* Pass 3: match matcher.curr against matcher.prev (= last render's
     * batches). On match, compute sxy_velocity = curr.sxy_centroid -
     * prev.sxy_centroid for the substitute step. */
    memset(s_prev->hash_table, 0, sizeof(s_prev->hash_table));
    for (uint32_t i = 0; i < s_prev->count; ++i) {
        s_prev->batches[i].consumed = 0;
        uint64_t h = s_prev->batches[i].hash;
        uint32_t slot = (uint32_t)(h & (MATCHER_HASH_SLOTS - 1u));
        for (uint32_t p = 0; p < MATCHER_PROBE_MAX; ++p) {
            if (s_prev->hash_table[slot] == 0u) {
                s_prev->hash_table[slot] = (uint16_t)(i + 1u);
                break;
            }
            slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
        }
    }

    unsigned paired = 0u;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* cb = &s_curr->batches[i];
        uint64_t h = cb->hash;
        uint32_t slot = (uint32_t)(h & (MATCHER_HASH_SLOTS - 1u));
        for (uint32_t p = 0; p < MATCHER_PROBE_MAX; ++p) {
            uint16_t entry = s_prev->hash_table[slot];
            if (entry == 0u) break;
            matcher_batch_t* pb = &s_prev->batches[entry - 1u];
            if (pb->hash == h && !pb->consumed) {
                pb->consumed = 1;
                cb->object_id   = pb->object_id;
                /* Velocity in world (q12 MAC) and screen-space pixels. */
                cb->velocity[0] = cb->centroid[0] - pb->centroid[0];
                cb->velocity[1] = cb->centroid[1] - pb->centroid[1];
                cb->velocity[2] = cb->centroid[2] - pb->centroid[2];
                cb->next_sxy_centroid[0] = cb->sxy_centroid[0] +
                    (cb->sxy_centroid[0] - pb->sxy_centroid[0]);
                cb->next_sxy_centroid[1] = cb->sxy_centroid[1] +
                    (cb->sxy_centroid[1] - pb->sxy_centroid[1]);
                cb->has_future_anchor = 1;
                paired++;
                break;
            }
            slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
        }
        if (!cb->has_future_anchor) {
            cb->object_id = s_next_object_id++;
        }
    }
    g_openpete_render_substitute_pair_count = paired;
}

openpete_substitute_delta_t openpete_render_matcher_substitute_query(
        const int16_t* p, float alpha) {
    openpete_substitute_delta_t out = { 0.0f, 0.0f };
    if (alpha <= 0.0f) return out;

    uint32_t key = ((uint32_t)(uint16_t)p[0]) |
                   (((uint32_t)(uint16_t)p[1]) << 16);
    openpete_render_cache_entry_t entry;
    if (!openpete_render_cache_lookup_prev(key, &entry)) {
        g_openpete_render_substitute_vert_miss_count++;
        return out;
    }
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        if (s_curr->batches[i].matrix_span_id != entry.matrix_span_id) continue;
        if (!s_curr->batches[i].has_future_anchor) {
            g_openpete_render_substitute_vert_miss_count++;
            return out;
        }
        /* Screen-space velocity = curr - prev. Predict next = curr + vel.
         * delta from curr-position by alpha = alpha * (next - curr) = alpha * vel. */
        float vx = (float)(s_curr->batches[i].next_sxy_centroid[0] -
                           s_curr->batches[i].sxy_centroid[0]);
        float vy = (float)(s_curr->batches[i].next_sxy_centroid[1] -
                           s_curr->batches[i].sxy_centroid[1]);
        out.dx = vx * alpha;
        out.dy = vy * alpha;
        g_openpete_render_substitute_vert_hit_count++;
        return out;
    }
    g_openpete_render_substitute_vert_miss_count++;
    return out;
}

/* Phase 2 finalize: matcher state was fully built + matched in
 * prepare_substitute (at DrawOTag-prepare time). Finalize only needs
 * to publish remaining stats and swap curr↔prev so next render's
 * prepare can compute velocity vs THIS render's batches. */
void openpete_render_matcher_finalize_frame(void) {
    /* Stats: how many of this frame's curr batches got paired (matched
     * via velocity-extrapolation against prev). Also count prev batches
     * not consumed (dropped this frame). */
    unsigned matched_count = 0u, unmatched_new = 0u, unmatched_dropped = 0u;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        if (s_curr->batches[i].has_future_anchor) matched_count++;
        else unmatched_new++;
    }
    for (uint32_t i = 0; i < s_prev->count; ++i) {
        if (!s_prev->batches[i].consumed) unmatched_dropped++;
    }
    g_openpete_render_matcher_batch_count             = s_curr->count;
    g_openpete_render_matcher_matched_count           = matched_count;
    g_openpete_render_matcher_unmatched_new_count     = unmatched_new;
    g_openpete_render_matcher_unmatched_dropped_count = unmatched_dropped;
    g_openpete_render_matcher_future_anchored_count   = matched_count;

    /* Swap. Next render's prepare will build a fresh curr (via cache
     * scan) and match against this frame's batches sitting in prev. */
    matcher_frame_t* tmp = s_prev;
    s_prev = s_curr;
    s_curr = tmp;
    s_curr->count = 0u;
}

