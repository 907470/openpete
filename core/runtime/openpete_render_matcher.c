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

    /* Batch pairing. `has_future_anchor` = this "from" (gen_prev2 =
     * G(N-1)) batch hash-paired with a "to" (gen_prev = G(N)) batch, so
     * `to_span_id` names the to-batch's matrix_span for the per-vertex
     * v_local correspondence lookup. delta_sxy is the centroid delta,
     * kept ONLY as a trace/diagnostic indicator — the applied motion is
     * now per-vertex (see substitute_query), not this batch aggregate.
     * A single per-batch centroid translation can't represent
     * perspective parallax across a large world span and its anchor
     * wanders under frustum-culling membership change; per-vertex
     * correspondence fixes both and subsumes the old rigid/animated
     * vfp heuristic (animated meshes churn v_local → correspondence
     * naturally misses → those verts freeze at 30 Hz for free). */
    int      has_future_anchor;
    int      vtrace_velocity_capped; /* 1 if cap engaged at least once during this batch's match probe — trace-only */
    int32_t  delta_sxy[2];           /* centroid delta — trace/diagnostic only */
    uint32_t to_span_id;             /* matched to-batch's matrix_span_id */

    /* Phase 3 — HUD/particle opt-out. Captured from the first cache
     * entry of the batch (all entries in a span share the same
     * matrix). If the rotation matrix is near-identity, the batch is
     * almost certainly UI/HUD drawn in screen-space — opt out of lerp
     * so 2D overlays don't smear under interpolation. */
    int16_t  rt[3][3];
    int      is_ui_opt_out;

    /* transform-interp §4: authoritative cross-frame identity — the
     * PSX moby vaddr (0 = world/composite). When nonzero on both
     * endpoints it supersedes the centroid-hash + velocity-cap
     * heuristic for pairing (real identity, no false-merge). */
    uint32_t obj_tag;

    /* transform-interp §7.2 / Option B: emission-ordinal among
     * obj_tag==0 (world/composite) batches this frame. G1 measured
     * span emission order 99.3% stable + vertex_count stable, so
     * {world_ord, ~vertex_count} is a deterministic cross-frame key
     * for untagged world spans — supersedes centroid-hash for them
     * (centroid wanders under frustum-cull membership change).
     * UINT32_MAX = not a world span. */
    uint32_t world_ord;
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

/* ---- Per-vertex correspondence map (the right primitive) ------------
 *
 * Keyed by (to-batch matrix_span_id, v_local[3]) → packed "to" SXY.
 * Built from the gen_prev ("to") scan; queried per vertex in
 * substitute_query: the lagged vertex's own v_local (from its
 * gen_prev2 cache hit) looks up its corresponding screen position one
 * game frame later. delta = to_sxy − from_sxy, per vertex — captures
 * perspective parallax and is immune to culling membership change (a
 * vertex present in only one frame simply has no entry → no delta →
 * renders at base). Animated meshes churn v_local every frame → the
 * lookup misses → those verts freeze at 30 Hz with no special-casing.
 *
 * Open-addressed, seq-stamped so it clears in O(1) per prepare (same
 * trick as the render cache). 32768 slots × 16 B = 512 KB static. */
#define TOMAP_SLOTS 32768u            /* power of two */
#define TOMAP_PROBE 24u
typedef struct {
    uint64_t key;   /* fnv1a64(to_span_id, v_local[3]) */
    uint32_t seq;   /* == s_tomap_seq when live */
    uint32_t sxy;   /* packed: sx in low 16, sy in high 16 */
} tomap_ent_t;
static tomap_ent_t s_tomap[TOMAP_SLOTS];
static uint32_t    s_tomap_seq = 0u;  /* bumped each prepare; stale = empty */

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

/* Per-vertex correspondence map helpers. Key = fnv1a64 over the
 * to-batch's matrix_span_id followed by the 3 model-local v_local
 * shorts. Including the span scopes the lookup to the matched batch
 * pair (so v_local reuse across unrelated batches can't cross-match),
 * while v_local identifies the logical vertex within it. */
static inline uint64_t tomap_key(uint32_t to_span_id, const int16_t vl[3]) {
    /* Fixed 10-byte layout, NO struct (struct padding bytes would be
     * uninitialized → nondeterministic hash → every lookup misses). */
    uint8_t kb[10];
    memcpy(kb,     &to_span_id, 4);
    memcpy(kb + 4, vl,          6);
    return fnv1a64(kb, sizeof(kb));
}
static inline void tomap_insert(uint32_t to_span_id, const int16_t vl[3],
                                uint32_t sxy) {
    uint64_t key = tomap_key(to_span_id, vl);
    uint32_t slot = (uint32_t)(key & (TOMAP_SLOTS - 1u));
    for (uint32_t p = 0; p < TOMAP_PROBE; ++p) {
        tomap_ent_t* e = &s_tomap[slot];
        if (e->seq != s_tomap_seq || e->key == key) {
            /* empty (stale) or same logical vertex → most-recent wins */
            e->key = key; e->sxy = sxy; e->seq = s_tomap_seq;
            return;
        }
        slot = (slot + 1u) & (TOMAP_SLOTS - 1u);
    }
    /* table full along the probe run — drop (vertex just won't smooth) */
}
static inline int tomap_lookup(uint32_t to_span_id, const int16_t vl[3],
                               uint32_t* out_sxy) {
    uint64_t key = tomap_key(to_span_id, vl);
    uint32_t slot = (uint32_t)(key & (TOMAP_SLOTS - 1u));
    for (uint32_t p = 0; p < TOMAP_PROBE; ++p) {
        const tomap_ent_t* e = &s_tomap[slot];
        if (e->seq != s_tomap_seq) return 0;          /* empty → miss */
        if (e->key == key) { *out_sxy = e->sxy; return 1; }
        slot = (slot + 1u) & (TOMAP_SLOTS - 1u);
    }
    return 0;
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

/* Diagnostic: when set, PsyX_EndScene clears the window to black AFTER
 * the game blit but BEFORE the FPS overlay. run.c raises it only around
 * the INTERMEDIATE-render present, so a working presentation path
 * strobes black (with ~30 Hz game flashes) while a broken one shows
 * the normal game and no black at all — isolating "matcher produces
 * no delta" from "intermediate frames never reach the display". */
int g_openpete_dbg_black_present = 0;

/* Master interpolation enable (default on). When 0, substitute_query
 * returns zero delta so intermediate renders present the lagged frame
 * unchanged — a clean live A/B: toggle the 'I' key and see whether
 * --render-fps>30 looks any different with interpolation vs without.
 * If it looks identical either way, interpolation is contributing
 * nothing perceptible (the thing we're chasing). */
int g_openpete_interp_enabled = 1;

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
        /* First vertex of this batch — capture RT for UI-opt-out test
         * and obj_tag for authoritative cross-frame identity. (All
         * verts of a matrix-span share one moby/transform.) */
        memcpy(b->rt, e->rt, sizeof(b->rt));
        b->obj_tag = e->obj_tag;
    }
    accumulate_vertex(b, e);
    /* For the "to" generation (gen_prev = G(N)) also record every
     * vertex's screen position keyed by (its span, v_local) so the
     * per-vertex correspondence query can find where this logical
     * vertex ended up one game frame later. */
    if (f == &s_to)
        tomap_insert(e->matrix_span_id, e->v_local, e->sxy_key);
}

static int matrix_is_near_identity(const int16_t rt[3][3]);

/* transform-interp §4: authoritative pairing by real moby identity.
 * Linear scan — frame batch counts are small (tens–hundreds), host-
 * side, once per game frame. Returns batch index or -1. obj_tag==0
 * (world/composite) is NOT identified this way; those keep the
 * centroid/ordinal path. */
static int find_unconsumed_by_obj_tag(matcher_frame_t* f, uint32_t tag) {
    if (tag == 0u) return -1;
    for (uint32_t i = 0; i < f->count; ++i)
        if (f->batches[i].obj_tag == tag && !f->batches[i].consumed)
            return (int)i;
    return -1;
}

/* transform-interp §7.2 / Option B: pair an untagged world span by
 * its deterministic emission ordinal, gated by vertex_count
 * similarity (G1: per-span vertex_count stable; a count mismatch
 * means an ordinal slipped across a cull boundary → bail to the
 * centroid-hash fallback rather than mispair). */
static int find_unconsumed_by_world_ord(matcher_frame_t* f, uint32_t ord,
                                        uint32_t ref_vc) {
    if (ord == 0xFFFFFFFFu) return -1;
    for (uint32_t i = 0; i < f->count; ++i) {
        matcher_batch_t* b = &f->batches[i];
        if (b->world_ord != ord || b->consumed) continue;
        uint32_t tol = ref_vc / 4u; if (tol < 4u) tol = 4u;
        uint32_t d = (b->vertex_count > ref_vc)
            ? b->vertex_count - ref_vc : ref_vc - b->vertex_count;
        return (d > tol) ? -1 : (int)i;
    }
    return -1;
}

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
        b->world_ord = 0xFFFFFFFFu;
    }
    /* Assign world ordinal in batch (≈emission) order to untagged
     * spans — the deterministic Option-B cross-frame key. */
    uint32_t wo = 0u;
    for (uint32_t i = 0; i < f->count; ++i) {
        matcher_batch_t* b = &f->batches[i];
        if (b->vertex_count == 0) continue;
        if (b->obj_tag == 0u && !b->is_ui_opt_out)
            b->world_ord = wo++;
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
    /* O(1) clear of the per-vertex correspondence map: every entry
     * with seq != this now reads as empty. */
    s_tomap_seq++;

    /* Scan both interpolation endpoints. "from" = gen_prev2 (G(N-1),
     * the lagged OT we display); "to" = gen_prev (G(N)). The "to" scan
     * also populates the per-vertex correspondence map (sub_scan_cb). */
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
        /* Authoritative identity first: a tagged moby inherits its
         * prior object_id by real vaddr — no centroid bucket, no
         * velocity cap (a fast-moving moby kept its id wrongly only
         * because the cap rejected it). World (tag 0) → heuristic. */
        if (cb->obj_tag != 0u) {
            int pi = find_unconsumed_by_obj_tag(s_prev, cb->obj_tag);
            if (pi >= 0) {
                s_prev->batches[pi].consumed = 1;
                cb->object_id = s_prev->batches[pi].object_id;
                continue;
            }
        }
        /* World (untagged): deterministic ordinal+signature identity
         * (G1-backed) before the centroid heuristic. */
        if (cb->obj_tag == 0u && cb->world_ord != 0xFFFFFFFFu) {
            int pi = find_unconsumed_by_world_ord(s_prev, cb->world_ord,
                                                  cb->vertex_count);
            if (pi >= 0) {
                s_prev->batches[pi].consumed = 1;
                cb->object_id = s_prev->batches[pi].object_id;
                continue;
            }
        }
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
        /* Authoritative span pairing for tagged mobys: pair "from"↔
         * "to" by real vaddr, bypassing centroid hash + velocity/
         * vertex-count caps (which exist only to reject heuristic
         * mispairs — real identity needs neither). Downstream per-
         * vertex tomap lookup keyed by to_span_id is unchanged. */
        if (cb->obj_tag != 0u) {
            int ti = find_unconsumed_by_obj_tag(&s_to, cb->obj_tag);
            if (ti >= 0) {
                matcher_batch_t* tb = &s_to.batches[ti];
                tb->consumed = 1;
                cb->velocity[0] = tb->centroid[0] - cb->centroid[0];
                cb->velocity[1] = tb->centroid[1] - cb->centroid[1];
                cb->velocity[2] = tb->centroid[2] - cb->centroid[2];
                cb->delta_sxy[0] = tb->sxy_centroid[0] - cb->sxy_centroid[0];
                cb->delta_sxy[1] = tb->sxy_centroid[1] - cb->sxy_centroid[1];
                cb->to_span_id   = tb->matrix_span_id;
                cb->has_future_anchor = 1;
                paired++;
                continue;
            }
        }
        /* World (untagged): ordinal+signature span pairing (Option B)
         * before centroid-hash. Same downstream per-vertex tomap. */
        if (cb->obj_tag == 0u && cb->world_ord != 0xFFFFFFFFu) {
            int ti = find_unconsumed_by_world_ord(&s_to, cb->world_ord,
                                                  cb->vertex_count);
            if (ti >= 0) {
                matcher_batch_t* tb = &s_to.batches[ti];
                tb->consumed = 1;
                cb->velocity[0] = tb->centroid[0] - cb->centroid[0];
                cb->velocity[1] = tb->centroid[1] - cb->centroid[1];
                cb->velocity[2] = tb->centroid[2] - cb->centroid[2];
                cb->delta_sxy[0] = tb->sxy_centroid[0] - cb->sxy_centroid[0];
                cb->delta_sxy[1] = tb->sxy_centroid[1] - cb->sxy_centroid[1];
                cb->to_span_id   = tb->matrix_span_id;
                cb->has_future_anchor = 1;
                paired++;
                continue;
            }
        }
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
                /* Paired. Consume so it can't re-pair elsewhere.
                 * has_future_anchor only scopes the per-vertex
                 * correspondence lookup to this to-batch's span — the
                 * actual rigid/animated decision and the applied
                 * motion are made PER VERTEX in substitute_query (a
                 * vertex whose v_local exists one frame later gets its
                 * true screen delta; an animated mesh churns v_local so
                 * its verts simply miss and freeze at 30 Hz). No vfp
                 * gate, no centroid shove — that's what caused both the
                 * neck-stretch and the world jitter. delta_sxy is kept
                 * only as a coarse trace/diagnostic indicator. */
                tb->consumed = 1;
                cb->velocity[0] = tb->centroid[0] - cb->centroid[0];
                cb->velocity[1] = tb->centroid[1] - cb->centroid[1];
                cb->velocity[2] = tb->centroid[2] - cb->centroid[2];
                cb->delta_sxy[0] = dx;
                cb->delta_sxy[1] = dy;
                cb->to_span_id   = tb->matrix_span_id;
                cb->has_future_anchor = 1;
                paired++;
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
    if (!g_openpete_interp_enabled) return out;  /* 'I' kill switch */
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
        const matcher_batch_t* b = &s_curr->batches[i];
        if (b->matrix_span_id != entry.matrix_span_id) continue;
        if (b->is_ui_opt_out) {
            /* HUD/UI batches must not lerp — screen-space, near-identity
             * world matrix; a delta would smear the 2D overlay. */
            return out;
        }
        if (!b->has_future_anchor) {           /* batch didn't pair */
            g_openpete_render_substitute_vert_miss_count++;
            return out;
        }
        /* Per-vertex correspondence: where did THIS logical vertex
         * (identified by its model-local v_local, recovered from the
         * gen_prev2 cache hit) end up one game frame later? Look it up
         * in the matched to-batch's span. A hit gives this vertex's
         * own true screen motion — captures perspective parallax
         * across the batch. A miss means the vertex has no counterpart
         * in G(N): either newly culled-in, or the mesh is animating
         * (v_local churned). Either way → no delta → it renders at its
         * base (lagged) position = clean 30 Hz, no smear. */
        uint32_t to_sxy;
        if (!tomap_lookup(b->to_span_id, entry.v_local, &to_sxy)) {
            g_openpete_render_substitute_vert_miss_count++;
            return out;
        }
        int16_t from_sx = (int16_t)p[0];
        int16_t from_sy = (int16_t)p[1];
        int16_t to_sx   = (int16_t)(uint16_t)(to_sxy & 0xFFFFu);
        int16_t to_sy   = (int16_t)(uint16_t)(to_sxy >> 16);
        float dx = (float)((int)to_sx - (int)from_sx) * alpha;
        float dy = (float)((int)to_sy - (int)from_sy) * alpha;
        /* Applied-delta clamp — catastrophic safety / kill switch only.
         * True per-vertex motion is already bounded by one game frame;
         * cap=0 disables substitute entirely ("30 Hz look"). */
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

/* Debug-viz predicate. Returns 1 iff THIS vertex actually receives a
 * non-zero per-vertex correspondence delta this render (paired batch,
 * not UI, v_local found one frame later, and it moved). Mirrors
 * substitute_query's decision exactly so the red overlay shows
 * precisely the geometry frame-gen is moving — rigid world under
 * camera motion lights up; animated/culled-in/static verts don't.
 * Alpha-independent (uses raw screen delta, not alpha-scaled). No
 * side effects, no counters. */
int openpete_render_matcher_is_smoothed(const int16_t* p) {
    uint32_t key = ((uint32_t)(uint16_t)p[0]) |
                   (((uint32_t)(uint16_t)p[1]) << 16);
    openpete_render_cache_entry_t entry;
    if (!openpete_render_cache_lookup_prev2(key, &entry)) return 0;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        const matcher_batch_t* b = &s_curr->batches[i];
        if (b->matrix_span_id != entry.matrix_span_id) continue;
        if (b->is_ui_opt_out || !b->has_future_anchor) return 0;
        uint32_t to_sxy;
        if (!tomap_lookup(b->to_span_id, entry.v_local, &to_sxy)) return 0;
        int16_t from_sx = (int16_t)p[0];
        int16_t from_sy = (int16_t)p[1];
        int16_t to_sx   = (int16_t)(uint16_t)(to_sxy & 0xFFFFu);
        int16_t to_sy   = (int16_t)(uint16_t)(to_sxy >> 16);
        return (to_sx != from_sx || to_sy != from_sy) ? 1 : 0;
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

