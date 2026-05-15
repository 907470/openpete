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
#include "openpete_vertex_trace.h"

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

    /* Interpolation span. `has_future_anchor` means this "from"
     * (gen_prev2 = G(N-1)) batch paired with a "to" (gen_prev = G(N))
     * batch by centroid hash, so `delta_sxy` = to.sxy_centroid -
     * from.sxy_centroid is the KNOWN screen-space displacement across
     * this display cycle. substitute lerps base + alpha*delta_sxy;
     * alpha=0 → lagged frame, alpha=1 → next frame. Unpaired batches
     * keep delta_sxy = {0,0} (render frozen at the lagged position —
     * see plan "Spawn / despawn handling"). */
    int      has_future_anchor;
    int      vtrace_velocity_capped; /* 1 if cap engaged at least once during this batch's match probe — trace-only */
    int32_t  delta_sxy[2];

    /* Order-independent fingerprint of the batch's model-local v_local
     * set. RIGID geometry (world mesh, static props, anything moved
     * only by camera RT/TR) keeps an identical v_local set frame-to-
     * frame → vfp stable. ANIMATED meshes (Spyro's pre-baked keyframe
     * deformation) get a fresh v_local set every game frame → vfp
     * churns. We only apply the interpolation delta when from.vfp ==
     * to.vfp; a churning batch is the SAME object animating, so we
     * freeze it at its lagged position (= the intended 30 Hz cinematic
     * fallback for animated mobys, plan goal #2) instead of shoving it
     * by a pose-contaminated centroid delta (the "neck stretches 3×"
     * artifact). */
    uint64_t vfp;

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
static matcher_frame_t* s_curr = &s_frame_a;   /* "from" = gen_prev2 (G(N-1)) batches; swapped each call for object_id continuity */
static matcher_frame_t* s_prev = &s_frame_b;   /* last call's "from" batches — object_id source */

/* Transient "to" = gen_prev (G(N)) batches. Built fresh every
 * prepare_substitute, never swapped: both interpolation endpoints are
 * simultaneously alive in the 3-gen cache ring during the whole
 * display cycle, so the span is computed intra-call. */
static matcher_frame_t  s_to;

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
         * world→screen ratio. For small deltas this is acceptable.
         * Under the interpolation model the override declares the
         * KNOWN [from→to] span directly as delta_sxy. */
        if (b->vertex_count > 0 && b->centroid[2] != 0) {
            float ratio_x = (float)b->sxy_centroid[0] / (float)b->centroid[0];
            float ratio_y = (float)b->sxy_centroid[1] / (float)b->centroid[1];
            b->delta_sxy[0] = (int32_t)((float)delta_tr[0] * ratio_x);
            b->delta_sxy[1] = (int32_t)((float)delta_tr[1] * ratio_y);
        }
        break;
    }
    s_prev_xform_active = 0;
}

/* Accumulators reset on every observe call's caller-decided frame
 * boundary — we use s_curr->count == 0 as the "fresh frame" signal.
 * find_or_create_batch initializes a slot with bbox sentinels. */
static matcher_batch_t* find_or_create_batch_in(matcher_frame_t* f,
                                                uint32_t matrix_span_id) {
    for (uint32_t i = 0; i < f->count; ++i) {
        if (f->batches[i].matrix_span_id == matrix_span_id)
            return &f->batches[i];
    }
    if (f->count >= MATCHER_MAX_BATCHES) return NULL;
    matcher_batch_t* b = &f->batches[f->count++];
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

    /* Order-independent v_local fingerprint (sum-combine so emission
     * order can't false-trigger the rigid/animated test). */
    uint32_t vp = ((uint32_t)(uint16_t)e->v_local[0]) ^
                  (((uint32_t)(uint16_t)e->v_local[1]) << 11) ^
                  (((uint32_t)(uint16_t)e->v_local[2]) << 22);
    b->vfp += (uint64_t)(vp * 0x9E3779B1u) + 0x100000001b3ULL;
}

/* Observe one vertex: lookup gen_prev2 (the lagged OT's generation) →
 * return entry by value. On miss (vertex not in cache, e.g. line or
 * sprite not produced by GTE), bumps the no_cache counter, returns 0. */
static int observe_vertex(const int16_t* p, openpete_render_cache_entry_t* out) {
    uint32_t key = sxy_key_from_pair(p);
    if (openpete_render_cache_lookup_prev2(key, out)) return 1;
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
    if (!openpete_render_cache_lookup_prev2(key, &e)) {
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
    if (!openpete_render_cache_lookup_prev2(key, &e)) {
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

/* Applied-delta clamp (per-axis, per-vertex, per-render) — now an
 * emergency bound / kill switch, not the primary safety mechanism.
 *
 * Under the OLD extrapolation model this clamp was load-bearing: it
 * fought constant-velocity overshoot of objects that stopped or
 * reversed (11.1% of transitions jumped >64 px), and no setting was
 * good — clamp=0 disabled it, clamp=8 still left 1.56% nausea-inducing
 * flicker. The INTERPOLATION model removed the structural error: the
 * applied delta is alpha·(to−from) with alpha∈[0,1], so motion is
 * bounded by the KNOWN single-game-frame span itself — it physically
 * cannot overshoot the next frame's position.
 *
 * So the default is wide-open (substitute ON, no artificial clamp).
 * --substitute-clamp-px N (or OPENPETE_SUBSTITUTE_CLAMP_PX env) still
 * works for A/B testing; the special value 0 is the kill switch
 * (substitute fully off → intermediate renders identical to the last
 * sim-due, the "30 Hz look at high refresh"). */
float g_openpete_substitute_clamp_px = 4096.0f;

/* Debug viz toggle. Set by --debug-highlight-smoothed /
 * OPENPETE_DEBUG_HIGHLIGHT_SMOOTHED (seeded in run.c) and live-toggled
 * by F9. PsyX_GPU reads this directly — a real global, not a
 * getenv(), so it works across the WSL2→Win32 boundary where bare
 * env vars don't reach the .exe's environment. */
int g_openpete_dbg_highlight_smoothed = 0;

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
 * Substitute pipeline — 1-frame-lag INTERPOLATION model.
 * See notes/plans/openpete-interpolation-frame-gen.md.
 *
 * We display a LAGGED OT (OT#(N-1), built by GTE G(N-1)). Its vertices'
 * SXY keys resolve in the cache generation G(N-1) wrote — which, at
 * DrawOTag#N time, sits at gen_prev2 (because Spyro does DrawOTag
 * BEFORE that frame's GTE, so gen_prev = G(N) is the just-completed
 * pass and gen_prev2 = G(N-1) is the one that built the lagged OT;
 * gen_current is empty until frame N's GTE runs after DrawOTag).
 *
 * Both interpolation endpoints are therefore simultaneously alive in
 * the 3-gen ring for the WHOLE display cycle (no GTE/rotate happens
 * during intermediate renders):
 *
 *   "from" = gen_prev2 = G(N-1)  — the lagged OT's own positions
 *   "to"   = gen_prev  = G(N)    — where those objects ended up
 *
 * We pair from↔to batches by centroid hash WITHIN one call (no
 * cross-call swap needed for the span). delta_sxy = to - from is the
 * KNOWN displacement; substitute returns alpha*delta_sxy, so
 * alpha=0 → exactly the lagged frame, alpha=1 → exactly G(N). No
 * extrapolation, no overshoot — bounded by the [from,to] segment.
 *
 * Pipeline:
 *   prepare_substitute():
 *     - Scan gen_prev2 → s_curr ("from"); scan gen_prev → s_to ("to").
 *     - Object-id continuity: hash-match s_curr vs s_prev (last call's
 *       "from") to keep object_id stable across renders/frames (the
 *       trace harness groups trajectories by object_id).
 *     - Span: hash-match s_curr(from) vs s_to → delta_sxy per pair.
 *   substitute_query():
 *     - Lookup gen_prev2[p's sxy] → matrix_span_id → s_curr batch →
 *       return alpha * batch.delta_sxy.
 *   finalize_frame():
 *     - Swap s_curr ↔ s_prev for next call's object-id continuity.
 * ---------------------------------------------------------------------- */

/* Scan one cache generation into a matcher frame, one batch per
 * matrix_span_id. `ud` is the target matcher_frame_t*. */
static void sub_scan_cb(const openpete_render_cache_entry_t* e, void* ud) {
    matcher_frame_t* f = (matcher_frame_t*)ud;
    matcher_batch_t* b = find_or_create_batch_in(f, e->matrix_span_id);
    if (!b) return;
    if (b->vertex_count == 0) {
        /* First vertex of this batch — capture RT for UI-opt-out test. */
        memcpy(b->rt, e->rt, sizeof(b->rt));
    }
    accumulate_vertex(b, e);
}

static int matrix_is_near_identity(const int16_t rt[3][3]);

/* Finalize a freshly-scanned frame: per-batch centroid, sxy_centroid,
 * geometric hash, reset span/match state. */
static void finalize_scanned_frame(matcher_frame_t* f) {
    for (uint32_t i = 0; i < f->count; ++i) {
        matcher_batch_t* b = &f->batches[i];
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
        b->delta_sxy[0] = b->delta_sxy[1] = 0;
        b->velocity[0] = b->velocity[1] = b->velocity[2] = 0;
        b->is_ui_opt_out = matrix_is_near_identity(b->rt);
        b->vtrace_velocity_capped = 0;
        b->consumed = 0;
    }
}

/* Open-addressed hash table over a frame's batches (slot = batch_idx+1,
 * 0 = empty). Used for both object-id continuity (vs s_prev) and span
 * pairing (vs s_to). */
static void build_frame_hash_table(matcher_frame_t* f) {
    memset(f->hash_table, 0, sizeof(f->hash_table));
    for (uint32_t i = 0; i < f->count; ++i) {
        uint64_t h = f->batches[i].hash;
        uint32_t slot = (uint32_t)(h & (MATCHER_HASH_SLOTS - 1u));
        for (uint32_t p = 0; p < MATCHER_PROBE_MAX; ++p) {
            if (f->hash_table[slot] == 0u) {
                f->hash_table[slot] = (uint16_t)(i + 1u);
                break;
            }
            slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
        }
    }
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
    s_curr->count = 0u;
    s_to.count    = 0u;
    g_openpete_render_substitute_vert_hit_count  = 0u;
    g_openpete_render_substitute_vert_miss_count = 0u;
    g_openpete_render_matcher_no_cache_count     = 0u;

    /* Scan both interpolation endpoints. "from" = gen_prev2 (G(N-1),
     * the lagged OT we display); "to" = gen_prev (G(N)). */
    openpete_render_cache_for_each_prev2_entry(sub_scan_cb, s_curr);
    openpete_render_cache_for_each_prev_entry (sub_scan_cb, &s_to);
    finalize_scanned_frame(s_curr);
    finalize_scanned_frame(&s_to);

    /* --- Object-id continuity (s_curr "from" vs s_prev = last call's
     * "from"). Identity only — no velocity cap; within a display cycle
     * s_curr content is identical call-to-call so a batch matches its
     * own prior incarnation, and across cycles the centroid bucket
     * persists for objects that stayed in roughly the same place. The
     * trace harness groups trajectories by object_id. */
    build_frame_hash_table(s_prev);
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* cb = &s_curr->batches[i];
        uint64_t h = cb->hash;
        uint32_t slot = (uint32_t)(h & (MATCHER_HASH_SLOTS - 1u));
        int got_id = 0;
        for (uint32_t p = 0; p < MATCHER_PROBE_MAX; ++p) {
            uint16_t entry = s_prev->hash_table[slot];
            if (entry == 0u) break;
            matcher_batch_t* pb = &s_prev->batches[entry - 1u];
            if (pb->hash == h && !pb->consumed) {
                /* Same velocity sanity cap as the span pass. The
                 * centroid bucket (64-unit) collides unrelated batches;
                 * without this, a moving moby inherits a stationary
                 * batch's object_id and the trace harness reads a giant
                 * phantom "jump" between two physically different
                 * batches sharing an id. Within a display cycle the
                 * scan content is identical (delta 0 → always passes);
                 * across a cycle a real same-object move is ≤ ~30 px. */
                const int32_t idx = cb->sxy_centroid[0] - pb->sxy_centroid[0];
                const int32_t idy = cb->sxy_centroid[1] - pb->sxy_centroid[1];
                if (abs(idx) + abs(idy) > MATCHER_VELOCITY_CAP_PX) {
                    slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
                    continue;
                }
                pb->consumed   = 1;
                cb->object_id  = pb->object_id;
                got_id = 1;
                break;
            }
            slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
        }
        if (!got_id) cb->object_id = s_next_object_id++;
    }

    /* --- Interpolation span (s_curr "from" ↔ s_to "to"). delta_sxy =
     * to.sxy_centroid - from.sxy_centroid is the KNOWN single-game-frame
     * displacement; substitute lerps it by alpha. The velocity sanity
     * cap rejects hash collisions between unrelated batches sharing a
     * centroid bucket (a real object moves ≤ ~30 px / game frame). */
    build_frame_hash_table(&s_to);
    unsigned paired = 0u;
    unsigned velocity_capped = 0u;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* cb = &s_curr->batches[i];
        uint64_t h = cb->hash;
        uint32_t slot = (uint32_t)(h & (MATCHER_HASH_SLOTS - 1u));
        for (uint32_t p = 0; p < MATCHER_PROBE_MAX; ++p) {
            uint16_t entry = s_to.hash_table[slot];
            if (entry == 0u) break;
            matcher_batch_t* tb = &s_to.batches[entry - 1u];
            if (tb->hash == h && !tb->consumed) {
                const int32_t dx = tb->sxy_centroid[0] - cb->sxy_centroid[0];
                const int32_t dy = tb->sxy_centroid[1] - cb->sxy_centroid[1];
                /* Cross-object mispair guards (release tb, probe on):
                 *  - implausible screen-space jump (a real object moves
                 *    ≤ ~30 px / game frame; cap is generous), and
                 *  - very different vertex count (Spyro's ~tens-of-vert
                 *    head batch colliding into a hundreds-of-vert wall
                 *    in the same centroid bucket). vstab verified per-
                 *    moby vertex_count is stable across consecutive
                 *    frames, so a true pair has near-equal counts. */
                uint32_t vc_tol = cb->vertex_count / 4u;
                if (vc_tol < 4u) vc_tol = 4u;
                uint32_t vc_diff = (cb->vertex_count > tb->vertex_count)
                    ? cb->vertex_count - tb->vertex_count
                    : tb->vertex_count - cb->vertex_count;
                if (abs(dx) + abs(dy) > MATCHER_VELOCITY_CAP_PX ||
                    vc_diff > vc_tol) {
                    velocity_capped++;
                    cb->vtrace_velocity_capped = 1;
                    slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
                    continue;
                }
                /* Same logical object. Consume regardless so it can't
                 * re-pair elsewhere and so stats/object-id are right. */
                tb->consumed = 1;
                cb->velocity[0] = tb->centroid[0] - cb->centroid[0];
                cb->velocity[1] = tb->centroid[1] - cb->centroid[1];
                cb->velocity[2] = tb->centroid[2] - cb->centroid[2];
                if (cb->vfp == tb->vfp) {
                    /* Rigid (world / static / camera-moved): the
                     * centroid delta is pure translation → smooth it. */
                    cb->delta_sxy[0] = dx;
                    cb->delta_sxy[1] = dy;
                    cb->has_future_anchor = 1;
                    paired++;
                }
                /* else: ANIMATING (v_local set churned). Leave
                 * delta_sxy {0,0} / has_future_anchor 0 → substitute
                 * returns 0 → the moby renders frozen at its lagged
                 * position = original 30 Hz cinematic cadence (plan
                 * goal #2). No pose-contaminated centroid shove → no
                 * neck-stretch. */
                break;
            }
            slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
        }
        /* Unpaired "from" batch → delta_sxy stays {0,0}: the object
         * despawned (or hash-collided away) between G(N-1) and G(N);
         * it renders frozen at its lagged position for this cycle. */
    }
    g_openpete_render_substitute_pair_count = paired;
    g_openpete_render_substitute_velocity_capped_count = velocity_capped;

    /* Consume any pending OPENPETE_PREV_TRANSFORM override (no-op if
     * none). It overwrites the matched batch's delta_sxy with an
     * explicitly-declared span. */
    apply_prev_transform_override();

    /* Trace pass — one record per "from" batch. iter_kind is set
     * externally by the OT-walk caller (sim-due 0 / intermediate 1).
     * No-op unless --vertex-trace-log is enabled. */
    if (openpete_vtrace_enabled()) {
        const uint32_t batch_count = s_curr->count;
        for (uint32_t i = 0; i < batch_count; ++i) {
            const matcher_batch_t* b = &s_curr->batches[i];
            openpete_vtrace_record_batch(
                i, batch_count,
                b->matrix_span_id, b->object_id,
                b->centroid, b->sxy_centroid, b->delta_sxy,
                (uint16_t)b->vertex_count,
                (uint8_t)b->is_ui_opt_out,
                (uint8_t)b->has_future_anchor,
                (uint8_t)b->vtrace_velocity_capped);
        }
    }
}

openpete_substitute_delta_t openpete_render_matcher_substitute_query(
        const int16_t* p, float alpha) {
    openpete_substitute_delta_t out = { 0.0f, 0.0f };
    if (alpha <= 0.0f) return out;

    uint32_t key = ((uint32_t)(uint16_t)p[0]) |
                   (((uint32_t)(uint16_t)p[1]) << 16);
    openpete_render_cache_entry_t entry;
    /* The vertex belongs to the lagged OT (OT#(N-1)) → its SXY resolves
     * in gen_prev2 (= "from" = G(N-1)), the same generation s_curr was
     * scanned from. */
    if (!openpete_render_cache_lookup_prev2(key, &entry)) {
        g_openpete_render_substitute_vert_miss_count++;
        return out;
    }
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        if (s_curr->batches[i].matrix_span_id != entry.matrix_span_id) continue;
        if (s_curr->batches[i].is_ui_opt_out) {
            /* HUD/UI batches must not lerp — they live in screen-space
             * with near-identity world matrix; a delta would smear the
             * 2D overlay. */
            return out;
        }
        if (!s_curr->batches[i].has_future_anchor) {
            g_openpete_render_substitute_vert_miss_count++;
            return out;
        }
        /* Interpolate along the KNOWN [from→to] span: applied =
         * from + alpha * delta_sxy. alpha=0 → lagged frame exactly,
         * alpha=1 → next frame exactly. Bounded by the segment — no
         * extrapolation overshoot. */
        float dx = (float)s_curr->batches[i].delta_sxy[0] * alpha;
        float dy = (float)s_curr->batches[i].delta_sxy[1] * alpha;
        /* Applied-delta clamp — emergency bound / kill switch. The span
         * itself already bounds motion (≤ one game-frame displacement),
         * so this defaults wide-open; cap=0 disables substitute
         * entirely (intermediate renders = last sim-due, "30 Hz look"). */
        const float cap = g_openpete_substitute_clamp_px;
        if (cap <= 0.0f) return out;  /* 0 = substitute off */
        if (dx >  cap) dx =  cap;
        if (dx < -cap) dx = -cap;
        if (dy >  cap) dy =  cap;
        if (dy < -cap) dy = -cap;
        out.dx = dx;
        out.dy = dy;
        g_openpete_render_substitute_vert_hit_count++;
        return out;
    }
    g_openpete_render_substitute_vert_miss_count++;
    return out;
}

/* Debug-viz predicate. Returns 1 iff the vertex's batch is one the
 * interpolation is actively SMOOTHING this render (rigid: paired
 * from↔to, vfp-matched, not UI). Independent of alpha, so a paused /
 * sim-due frame still reports the classification stably. Used by the
 * --debug-highlight-smoothed render mode to paint smoothed geometry
 * solid red so you can SEE the rigid/animated split the harness can't
 * show. No side effects, no counters. */
int openpete_render_matcher_is_smoothed(const int16_t* p) {
    uint32_t key = ((uint32_t)(uint16_t)p[0]) |
                   (((uint32_t)(uint16_t)p[1]) << 16);
    openpete_render_cache_entry_t entry;
    if (!openpete_render_cache_lookup_prev2(key, &entry)) return 0;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        if (s_curr->batches[i].matrix_span_id != entry.matrix_span_id) continue;
        if (s_curr->batches[i].is_ui_opt_out) return 0;
        return s_curr->batches[i].has_future_anchor ? 1 : 0;
    }
    return 0;
}

/* Finalize: matcher state was fully built + paired in
 * prepare_substitute. Publish stats and swap s_curr↔s_prev so the next
 * render's object-id continuity pass can match against THIS render's
 * "from" batches. (s_to is transient — never swapped.) */
void openpete_render_matcher_finalize_frame(void) {
    /* matched = "from" batches that paired with a "to" (have a valid
     * interpolation span). dropped = last call's "from" batches with
     * no object-id successor this call. */
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

    /* Swap for object-id continuity (see prepare_substitute). */
    matcher_frame_t* tmp = s_prev;
    s_prev = s_curr;
    s_curr = tmp;
    s_curr->count = 0u;
}

