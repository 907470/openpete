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
#include <stdlib.h>
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

    /* Phase 3 — HUD/particle opt-out. Captured from the first cache
     * entry of the batch (all entries in a span share the same
     * matrix). If the rotation matrix is near-identity, the batch is
     * almost certainly UI/HUD drawn in screen-space — opt out of lerp
     * so 2D overlays don't smear under interpolation. */
    int16_t  rt[3][3];
    int      is_ui_opt_out;
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
/* Phase 3 HUD opt-out stat. */
unsigned g_openpete_render_substitute_ui_optout_count      = 0u;
/* Velocity sanity cap rejects (mispair detector — should be small in
 * healthy gameplay, large during scene transitions). */
unsigned g_openpete_render_substitute_velocity_capped_count = 0u;

/* Phase 4 — OPENPETE_PREV_TRANSFORM escape hatch state. One-shot
 * override consumed by the next prepare_substitute call. See header
 * for usage and rationale. */
static int     s_prev_xform_active = 0;
static int16_t s_prev_xform_curr_rt[3][3];
static int32_t s_prev_xform_curr_tr[3];
static int16_t s_prev_xform_prev_rt[3][3];
static int32_t s_prev_xform_prev_tr[3];

void openpete_render_matcher_set_prev_transform(
        const int16_t curr_rt[3][3], const int32_t curr_tr[3],
        const int16_t prev_rt[3][3], const int32_t prev_tr[3]) {
    memcpy(s_prev_xform_curr_rt, curr_rt, sizeof(s_prev_xform_curr_rt));
    memcpy(s_prev_xform_curr_tr, curr_tr, sizeof(s_prev_xform_curr_tr));
    memcpy(s_prev_xform_prev_rt, prev_rt, sizeof(s_prev_xform_prev_rt));
    memcpy(s_prev_xform_prev_tr, prev_tr, sizeof(s_prev_xform_prev_tr));
    s_prev_xform_active = 1;
}

/* Apply any pending Phase 4 override to matcher.curr. Called near the
 * end of prepare_substitute, after match-against-prev produced default
 * velocities. The override replaces the matched batch's
 * sxy_velocity-derived `next_sxy_centroid` with one computed from the
 * declared prev_rt/tr.
 *
 * The current implementation patches the FIRST batch whose RT/TR match
 * the override's curr identifier. For accurate screen-space delta from
 * a world-space prev matrix, we'd need to re-project the batch's
 * vertices through the override matrix — full T/R/S+slerp territory.
 * Phase 4 minimal landing assumes a small RT/TR change between frames
 * so screen-space delta ≈ world-space delta projection of the centroid
 * via the current OFX/OFY/H (captured per cache entry).
 *
 * Override is consumed (s_prev_xform_active reset to 0). */
static void apply_prev_transform_override(void) {
    if (!s_prev_xform_active) return;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* b = &s_curr->batches[i];
        if (memcmp(b->rt, s_prev_xform_curr_rt, sizeof(b->rt)) != 0) continue;
        /* Recompute velocity using the override's prev_tr → curr_tr
         * translation delta. (Rotation lerp deferred to full T/R/S.) */
        int32_t delta_tr[3] = {
            s_prev_xform_curr_tr[0] - s_prev_xform_prev_tr[0],
            s_prev_xform_curr_tr[1] - s_prev_xform_prev_tr[1],
            s_prev_xform_curr_tr[2] - s_prev_xform_prev_tr[2],
        };
        b->velocity[0] = delta_tr[0];
        b->velocity[1] = delta_tr[1];
        b->velocity[2] = delta_tr[2];
        b->has_future_anchor = 1;
        /* Projection of world-space TR delta to screen-space isn't
         * exact without OFX/OFY/H + perspective division — Phase 4
         * minimal uses an approximation: the centroid's empirical
         * world→screen ratio. For small deltas this is acceptable. */
        if (b->vertex_count > 0 && b->centroid[2] != 0) {
            float ratio_x = (float)b->sxy_centroid[0] / (float)b->centroid[0];
            float ratio_y = (float)b->sxy_centroid[1] / (float)b->centroid[1];
            b->next_sxy_centroid[0] = b->sxy_centroid[0] +
                (int32_t)((float)delta_tr[0] * ratio_x);
            b->next_sxy_centroid[1] = b->sxy_centroid[1] +
                (int32_t)((float)delta_tr[1] * ratio_y);
        }
        break;
    }
    s_prev_xform_active = 0;
}

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
 *   shift=18 (64-unit buckets), centroid only        → 100 % match
 *     in active gameplay, but moving mobys can collide with unrelated
 *     stationary batches sharing the bucket → nonsense velocity →
 *     visible flicker. Mitigated by adding bbox-size and vertex-count
 *     to the hash (this file) AND a per-batch velocity sanity cap. */
#define MATCHER_CENTROID_SHIFT 18

/* Velocity sanity cap (screen-space pixels, Manhattan). A legitimate
 * moby moves at most ~30 px between game frames; anything beyond this
 * is almost certainly a mispair via hash collision. Capped matches
 * are released (pb->consumed=0) so a legitimate curr batch can still
 * pair with that prev, and the curr batch probes the next hash slot
 * for a better candidate. */
#define MATCHER_VELOCITY_CAP_PX 128

/* Shared hash function so the future-anchor scan produces identical
 * hashes for the same logical batch as the OT-walker observe pass.
 *
 * Centroid-only hashing was retained after a brief experiment with
 * (centroid + bbox-log2 + vert-count): adding bbox-log2 caused ~50 %
 * of legitimate matches to fail because the log2-ceil bucket flipped
 * across power-of-two boundaries under small frame-to-frame noise.
 * Hash collisions between unrelated batches in the same centroid
 * bucket are now caught downstream by the velocity sanity cap — see
 * MATCHER_VELOCITY_CAP_PX. */
static uint64_t batch_centroid_hash(int32_t cx, int32_t cy, int32_t cz) {
    int32_t cq[3] = {
        cx >> MATCHER_CENTROID_SHIFT,
        cy >> MATCHER_CENTROID_SHIFT,
        cz >> MATCHER_CENTROID_SHIFT,
    };
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
    /* find_or_create returns the existing batch OR creates a fresh one
     * with bbox sentinels. We need to detect "fresh" vs "existing" to
     * only capture RT on the first vertex of a span. Cheap heuristic:
     * vertex_count==0 means just-created (accumulate_vertex hasn't run). */
    matcher_batch_t* b = find_or_create_batch(e->matrix_span_id);
    if (!b) return;
    if (b->vertex_count == 0) {
        /* First vertex of this batch — capture RT for Phase 3 UI detect. */
        memcpy(b->rt, e->rt, sizeof(b->rt));
    }
    accumulate_vertex(b, e);
}

/* Phase 3 — UI/HUD detection. PSX matrices are s3.12 fixed (1.0 =
 * 0x1000 = 4096). Identity is diag=4096 with off-diag=0. Real HUD
 * prims occasionally have small rotations from animation, so accept a
 * generous tolerance (~3 degrees worth of off-diag). Batches that
 * pass this test (rotation ≈ identity) are 2D overlays in camera-
 * local coords; lerping them would smear pixels under motion. */
static int matrix_is_near_identity(const int16_t rt[3][3]) {
    const int16_t one = 0x1000;
    const int16_t diag_tol     = 0x80;  /* ±128 / 4096 ≈ ±3 % per axis */
    const int16_t offdiag_tol  = 0x200; /* ±512 / 4096 ≈ ±12 % per cell */
    if (abs((int)rt[0][0] - one) > diag_tol) return 0;
    if (abs((int)rt[1][1] - one) > diag_tol) return 0;
    if (abs((int)rt[2][2] - one) > diag_tol) return 0;
    if (abs((int)rt[0][1]) > offdiag_tol) return 0;
    if (abs((int)rt[0][2]) > offdiag_tol) return 0;
    if (abs((int)rt[1][0]) > offdiag_tol) return 0;
    if (abs((int)rt[1][2]) > offdiag_tol) return 0;
    if (abs((int)rt[2][0]) > offdiag_tol) return 0;
    if (abs((int)rt[2][1]) > offdiag_tol) return 0;
    return 1;
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
        b->is_ui_opt_out = matrix_is_near_identity(b->rt);
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
    unsigned velocity_capped = 0u;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* cb = &s_curr->batches[i];
        uint64_t h = cb->hash;
        uint32_t slot = (uint32_t)(h & (MATCHER_HASH_SLOTS - 1u));
        for (uint32_t p = 0; p < MATCHER_PROBE_MAX; ++p) {
            uint16_t entry = s_prev->hash_table[slot];
            if (entry == 0u) break;
            matcher_batch_t* pb = &s_prev->batches[entry - 1u];
            if (pb->hash == h && !pb->consumed) {
                /* Velocity sanity cap. Even with multi-feature hashing
                 * (centroid+bbox+vc), occasional false-positive matches
                 * survive — typically a moving moby colliding with a
                 * shape-similar stationary batch. Drop the match when
                 * the resulting screen-space velocity is implausibly
                 * large; release the prev slot so a legitimate curr
                 * batch can still pair with it. */
                const int32_t vx = cb->sxy_centroid[0] - pb->sxy_centroid[0];
                const int32_t vy = cb->sxy_centroid[1] - pb->sxy_centroid[1];
                if (abs(vx) + abs(vy) > MATCHER_VELOCITY_CAP_PX) {
                    velocity_capped++;
                    /* Don't consume pb; try next slot (may have a
                     * legitimate hash neighbor with sane velocity). */
                    slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
                    continue;
                }
                pb->consumed = 1;
                cb->object_id   = pb->object_id;
                /* Velocity in world (q12 MAC) and screen-space pixels. */
                cb->velocity[0] = cb->centroid[0] - pb->centroid[0];
                cb->velocity[1] = cb->centroid[1] - pb->centroid[1];
                cb->velocity[2] = cb->centroid[2] - pb->centroid[2];
                cb->next_sxy_centroid[0] = cb->sxy_centroid[0] + vx;
                cb->next_sxy_centroid[1] = cb->sxy_centroid[1] + vy;
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
    g_openpete_render_substitute_velocity_capped_count = velocity_capped;

    /* Phase 4: consume any pending OPENPETE_PREV_TRANSFORM override
     * (no-op if no override set). */
    apply_prev_transform_override();
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
        if (s_curr->batches[i].is_ui_opt_out) {
            /* Phase 3: HUD/UI batches must not lerp — they live in
             * screen-space with near-identity world matrix; any motion
             * delta would smear 2D overlays. */
            return out;
        }
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
    unsigned ui_optout = 0u;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        if (s_curr->batches[i].is_ui_opt_out) ui_optout++;
        if (s_curr->batches[i].has_future_anchor) matched_count++;
        else unmatched_new++;
    }
    g_openpete_render_substitute_ui_optout_count = ui_optout;
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

