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
#include "openpete_world_geo.h"
#include "openpete_vertex_trace.h"

/* transform-interp §7.5-R3: a batch is WORLD geometry if it's
 * untagged (legacy obj_tag==0 fallback) or carries the recompiler's
 * stable terrain strip tag (bit30 set, disjoint from moby vaddrs). */
static inline int is_world_obj_tag(uint32_t t) {
    return (t == 0u) || (t & OPENPETE_WORLD_GEO_TAG_BIT) != 0u;
}

/* §7.5-R3: batches are keyed by matrix_span_id, which equals one
 * object only when each object has its own RT/TR (true for mobys).
 * World strips SHARE the camera view matrix → one span collapses
 * many strips into a single batch, destroying per-strip identity.
 * For world-tagged strips, remap to a synthetic per-strip span id =
 * (1<<31) | mix(matrix_span_id, strip_tag). This is a pure function
 * of (span, strip) so it's cross-frame stable, and the top bit puts
 * it in a range disjoint from real (small, sequential)
 * matrix_span_ids — so every existing span-keyed mechanism (batch
 * create, tomap, find_span, pairing, substitute selection) delivers
 * strip granularity with zero ripple. Mobys and untagged world
 * (obj_tag==0, legacy world_ord path) are returned unchanged. */
static inline uint32_t eff_span(const openpete_render_cache_entry_t* e) {
    if (e->obj_tag & OPENPETE_WORLD_GEO_TAG_BIT)
        return (1u << 31) | (e->matrix_span_id * 2654435761u
                             ^ e->obj_tag);
    return e->matrix_span_id;
}

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* §7.5-R3: world is now per-STRIP batched (eff_span splits the shared
 * terrain view-matrix span into one batch per strip tag) instead of
 * ~1 batch per matrix_span. A level's visible terrain can be many
 * hundreds of strips, so the old 256 cap would silently overflow
 * (find_or_create → NULL → strip gets no interp). 4096 covers full
 * terrain + mobys with headroom; ~matcher_batch_t·4096·3 frames is a
 * few MB, trivial host-side. */
#define MATCHER_MAX_BATCHES 4096u

typedef struct matcher_batch {
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
    int32_t  tr[3];          /* §7.3: span translation, for composite
                              * world reprojection (world spans pair
                              * by world_ord/v_local; this is the to-
                              * span matrix the from-verts re-project
                              * through at α). */
    int      is_ui_opt_out;

    /* transform-interp §4: authoritative cross-frame identity — the
     * PSX moby vaddr (0 = world/composite). When nonzero on both
     * endpoints it supersedes the centroid-hash + velocity-cap
     * heuristic for pairing (real identity, no false-merge). */
    uint32_t obj_tag;

    /* §11/#29 duplicate-tag disambiguation. One object can emit
     * MULTIPLE batches sharing its obj_tag (Spyro's body segments each
     * load their own RT/TR; near/far world-strip tiers reuse one strip
     * tag). `real_span` is the RAW emission-order matrix-span counter
     * of the batch's first vertex (eff_span() may replace the KEY with
     * a synthetic hash for world strips, destroying order); `tag_rank`
     * is this batch's 0-based position among same-tag batches ordered
     * by real_span. Emission order within one object's render is a
     * fixed code path, so rank is cross-frame stable — pairing by
     * (obj_tag, tag_rank) makes segments pair segment-to-segment
     * instead of first-unconsumed (which cross-paired under differing
     * scan orders: Spyro solid red, still-camera yellow/green strip
     * flicker, title-cyclorama cyan). */
    uint32_t real_span;
    uint32_t tag_rank;

    /* PHASE-D: 1 if this world strip's vertices carry SOURCE-ADDRESS
     * keys (world_vkey bit31 — KSEG0 addresses) rather than PHASE-B
     * emission ordinals. Source keys are immune to emission-count
     * variation, so the F5b count-eq gate is skipped for these. */
    int world_src_keyed;

    /* First-entry gte_pc — RTPS caller attribution for diagnostics
     * (which renderer produced this batch). Diag-only. */
    uint32_t gte_pc;

    /* Max world_vkey seen in this batch. For COUNTER-keyed strips
     * (vkey = 0..N-1 emission ordinals) max+1 is the EMITTED vertex
     * count — unlike vertex_count (cached entries), it is immune to
     * sxy-collision eviction, because ordinals are assigned at RTPS
     * time and eviction only removes entries afterwards. The world
     * pair gate compares THIS, so eviction noise can't freeze a
     * strip whose keys are all still valid. (Meaningless for
     * source-keyed strips, which skip the gate entirely.) */
    uint32_t vkey_max;

    /* §12.2 overlay v2: why this batch is (not) interpolating, set
     * during the span-pairing pass. OPENPETE_DBGST_* in the header.
     * Purely diagnostic — read only by the F9 highlight overlay. */
    uint8_t  dbg_state;

    /* transform-interp §7.2 / Option B: emission-ordinal among
     * obj_tag==0 (world/composite) batches this frame. G1 measured
     * span emission order 99.3% stable + vertex_count stable, so
     * {world_ord, ~vertex_count} is a deterministic cross-frame key
     * for untagged world spans — supersedes centroid-hash for them
     * (centroid wanders under frustum-cull membership change).
     * UINT32_MAX = not a world span. */
    uint32_t world_ord;

    /* §"Animated meshes": per-batch v_local correspondence tally
     * (from-verts whose v_local resolves in the paired to-span).
     * Low ratio ⇒ animating/churning/partial ⇒ freeze the WHOLE
     * span (not per-vertex) to avoid the tear-apart artifact where
     * some verts move and some don't. */
    uint32_t corr_hit, corr_tot;
    /* Σ(|dx|+|dy|) over resolved from-verts — the strip's would-be
     * applied motion. Captured by corr_scan_cb; consumed by the
     * OPENPETE_CORR_LOG threshold-sweep capture (raw + threshold-
     * independent: the active gate only decides what's DRAWN, so a
     * single instrumented run sweeps every candidate offline). */
    uint64_t corr_dpx_sum;

    /* Rigid-fallback motion: mean SIGNED screen delta over this
     * batch's CORRESPONDED from-verts. The tomap is keyed by unique
     * per-vertex ids (source addrs / emission ordinals), so a lookup
     * is exact-or-miss — never wrong — which makes this mean
     * trustworthy even at low coverage. Two consumers:
     *   (a) sub-threshold corr-gate batches RIGID-FOLLOW it instead
     *       of freezing a full game frame behind their neighbors;
     *   (b) per-vert tomap misses inside interpolating batches ride
     *       it instead of holding at the lagged base.
     * Both freeze flavors were the camera-rotation "smear between
     * the before/after frame": at pan speed the lagged pose sits
     * tens-to-hundreds of px from the interpolating neighbors. */
    int64_t  corr_dx_sum, corr_dy_sum;
    /* Σ(dx²+dy²) over corresponded verts — with the signed sums this
     * gives the per-vert motion VARIANCE around the rigid mean. High
     * spread = the batch's motion is incoherent (e.g. a flattened
     * shadow pass amplifying animation into floor streaks) — per-vert
     * lerp can't represent it; the spread gate freezes the batch. */
    uint64_t corr_d2_sum;
    float    rigid_dx, rigid_dy;
    int      rigid_valid;  /* corr_hit >= RIGID_MIN_HITS */
    int      rigid_only;   /* sub-threshold: ALL verts ride rigid */

    /* Camera-independent v_local centroid (Option-B world key). */
    int64_t  vloc_sum[3];
    int32_t  vloc_centroid[3];
} matcher_batch_t;

typedef struct matcher_frame {
    matcher_batch_t batches[MATCHER_MAX_BATCHES];
    uint32_t        count;
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
/* Sized for ~30k world+moby verts/frame: the old 32768 slots ran at
 * ~91% load where the 24-probe insert silently DROPS the vertex
 * ("just won't smooth"). 131072 ~ 23% load, drops ~zero. 16B/slot
 * x 131072 = 2 MB static. */
#define TOMAP_SLOTS 131072u           /* power of two */
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

/* World-span diagnostic (OPENPETE_WORLD_DIAG=1, read-only stderr).
 * Answers "why zero blue on world": counts where obj_tag==0 spans
 * fall out — pairing → anchor → ui-opt-out → §7.3 reproject guards. */
static unsigned long s_wd_batches, s_wd_uiopt, s_wd_anchored,
    s_wd_q_total, s_wd_q_no_tb, s_wd_q_reproj_fail, s_wd_q_applied,
    s_wd_frames, s_wd_q_calls, s_wd_q_prev2miss, s_wd_q_world_entry,
    s_wd_q_ge1, s_wd_q_ge4, s_wd_q_maxmag, s_wd_q_samemtx,
    /* §7.5-R3 PHASE-C: split world-batch tally into bit30-tagged vs
     * legacy (obj_tag==0) so the post-hook-expansion shrinkage of the
     * legacy `world_reproject` path is directly measurable. */
    s_wd_batches_tagged, s_wd_batches_legacy;
/* #28 residual-yellow attribution: histogram world-batch sane-gate
 * REJECTIONS (TAG_INSANE) and missing-counterpart (TAG_ABSENT) by
 * first-entry gte_pc + key family. Diag-only (OPENPETE_WORLD_DIAG). */
static struct { uint32_t pc; unsigned long insane_src, insane_ctr,
                absent; } s_wd_pc[24];
static unsigned s_wd_pc_n;
/* Sample ring of rejected-pair shapes (a=from, b=to). */
static struct { uint32_t pc, amax, bmax, avc, bvc; } s_wd_smp[8];
static unsigned s_wd_smp_n;
/* Sample ring of ABSENT batch identities (tag/rank/shape). */
static struct { uint32_t pc, tag, rank, vmax, vc; } s_wd_abs[8];
static unsigned s_wd_abs_n;
/* Orange (corr-freeze) attribution tallies. */
static unsigned long s_wd_orange_mig, s_wd_orange_same, s_wd_orange_zero;
/* Rigid-fallback engagement: gate trips that rigid-followed vs froze,
 * and per-vert tomap misses filled with the batch mean. */
static unsigned long s_wd_rigid_span, s_wd_rigid_froze, s_wd_rigid_fill;
/* §13.6 spread-gate trips (incoherent moby batches frozen). */
static unsigned long s_wd_spread_froze;
static void wd_pc_tally(uint32_t pc, int src_keyed, int absent) {
    unsigned i;
    for (i = 0; i < s_wd_pc_n; ++i) if (s_wd_pc[i].pc == pc) break;
    if (i == s_wd_pc_n) {
        if (s_wd_pc_n >= 24u) return;
        s_wd_pc[s_wd_pc_n].pc = pc;
        s_wd_pc[s_wd_pc_n].insane_src = s_wd_pc[s_wd_pc_n].insane_ctr = 0;
        s_wd_pc[s_wd_pc_n].absent = 0;
        s_wd_pc_n++;
    }
    if (absent)          s_wd_pc[i].absent++;
    else if (src_keyed)  s_wd_pc[i].insane_src++;
    else                 s_wd_pc[i].insane_ctr++;
}
static int world_diag(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_WORLD_DIAG");
                 v = (e && *e && atoi(e) != 0) ? 1 : 0; }
    return v;
}

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
    /* Camera-INDEPENDENT signature: v_local is model/sector-local
     * (invariant under camera motion — G3 proved mac/centroid is
     * camera-baked, so the mac-centroid hash bucket moves with the
     * camera and world spans never re-pair). v_local centroid is
     * stable for static terrain → the Option-B world key. */
    b->vloc_sum[0] += (int64_t)e->v_local[0];
    b->vloc_sum[1] += (int64_t)e->v_local[1];
    b->vloc_sum[2] += (int64_t)e->v_local[2];
    /* Refine-keyed re-projections sit in their own key namespace
     * (bit30) — excluded from vkey_max so they can't poison the
     * counter-family emitted-count gate. */
    if (!(e->world_vkey & OPENPETE_WORLD_VKEY_REFINE_BIT) &&
        e->world_vkey > b->vkey_max) b->vkey_max = e->world_vkey;
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

/* §13.8 same-pixel disambiguation. The render cache now keeps up to
 * 4 COEXISTING entries per pixel (census: ~36% of gameplay emits
 * collide; ~840/frame cross-object). A displayed vertex's lookup is
 * inherently ambiguous at pixel granularity — but a PRIM belongs to
 * exactly one object, so the observe hooks (called per prim, before
 * the per-vert substitute queries) vote a consensus obj_tag across
 * the prim's candidate lists, and the per-vert selection prefers the
 * candidate matching it. A vert whose pixel was previously won by
 * another object now resolves ITS OWN object's entry and motion.
 * Consensus needs >=2 verts agreeing; otherwise selection falls back
 * to the oldest candidate (the pre-§13.8 single-entry semantics). */
#define PRIM_CAND_MAX 4
static uint32_t s_prim_tag;
static int      s_prim_tag_valid;

static int select_prev2_entry(const int16_t* p,
                              openpete_render_cache_entry_t* out) {
    openpete_render_cache_entry_t c[PRIM_CAND_MAX];
    int n = openpete_render_cache_lookup_prev2_multi(
                sxy_key_from_pair(p), c, PRIM_CAND_MAX);
    int i;
    if (!n) return 0;
    if (s_prim_tag_valid) {
        for (i = 0; i < n; ++i)
            if (c[i].obj_tag == s_prim_tag) { *out = c[i]; return 1; }
    }
    *out = c[0];
    return 1;
}

static void prim_consensus(const int16_t* p0, const int16_t* p1,
                           const int16_t* p2, const int16_t* p3) {
    uint32_t tag[12]; unsigned cnt[12]; unsigned ntag = 0;
    const int16_t* ps[4];
    int v;
    ps[0] = p0; ps[1] = p1; ps[2] = p2; ps[3] = p3;
    s_prim_tag_valid = 0;
    for (v = 0; v < 4; ++v) {
        openpete_render_cache_entry_t c[PRIM_CAND_MAX];
        uint32_t seen[PRIM_CAND_MAX]; unsigned nseen = 0;
        int n, i;
        if (!ps[v]) continue;
        n = openpete_render_cache_lookup_prev2_multi(
                sxy_key_from_pair(ps[v]), c, PRIM_CAND_MAX);
        for (i = 0; i < n; ++i) {
            uint32_t t = c[i].obj_tag;
            unsigned k, j;
            int dup = 0;
            for (k = 0; k < nseen; ++k)
                if (seen[k] == t) { dup = 1; break; }
            if (dup) continue;           /* one vote per vert per tag */
            if (nseen < PRIM_CAND_MAX) seen[nseen++] = t;
            for (j = 0; j < ntag; ++j)
                if (tag[j] == t) { cnt[j]++; break; }
            if (j == ntag && ntag < 12) { tag[ntag] = t; cnt[ntag] = 1; ntag++; }
        }
    }
    {
        unsigned best = 0, bi = 0, j;
        for (j = 0; j < ntag; ++j)
            if (cnt[j] > best) { best = cnt[j]; bi = j; }
        if (best >= 2u) { s_prim_tag = tag[bi]; s_prim_tag_valid = 1; }
    }
}

/* Observe hooks are RETAINED in the API for source compatibility with
 * Phase 1's plumbing in PsyX_GPU.cpp, but no longer drive matcher state
 * — Phase 2 builds matcher.curr eagerly via cache scan in
 * openpete_render_matcher_prepare_substitute(). The observe hooks
 * only bump prim_count on the batch (used for per-batch stats), since
 * eager scan is vertex-level and doesn't know prim boundaries.
 * §13.8: they additionally run the prim-consensus vote above, being
 * the only per-PRIM call the walker makes before the per-vert
 * substitute queries. */
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
    openpete_render_cache_entry_t e;
    prim_consensus(p0, p1, p2, NULL);
    uint32_t key = ((uint32_t)(uint16_t)p0[0]) |
                   (((uint32_t)(uint16_t)p0[1]) << 16);
    if (!openpete_render_cache_lookup_prev2(key, &e)) {
        g_openpete_render_matcher_no_cache_count++;
        return;
    }
    matcher_batch_t* b = find_batch_by_span(eff_span(&e));
    if (b) b->prim_count++;
}

void openpete_render_matcher_observe_quad(const int16_t* p0,
                                           const int16_t* p1,
                                           const int16_t* p2,
                                           const int16_t* p3) {
    openpete_render_cache_entry_t e;
    prim_consensus(p0, p1, p2, p3);
    uint32_t key = ((uint32_t)(uint16_t)p0[0]) |
                   (((uint32_t)(uint16_t)p0[1]) << 16);
    if (!openpete_render_cache_lookup_prev2(key, &e)) {
        g_openpete_render_matcher_no_cache_count++;
        return;
    }
    matcher_batch_t* b = find_batch_by_span(eff_span(&e));
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
 * to-batch's (eff) span id followed by a per-vertex discriminator
 * that identifies the logical vertex within the matched batch pair:
 *
 *   - TAGGED (moby AND world): world_vkey.
 *       world: the vertex's SOURCE ADDRESS in strip data (PHASE-D,
 *       stream variants; drop-immune) or the per-strip emission
 *       ordinal (PHASE-B fallback, subdivided/LOD variants). Terrain
 *       v_local is camera-RELATIVE and churns every frame.
 *       moby (#25): the per-object emission ordinal. Moby v_local is
 *       model-local but ANIMATED meshes morph it every keyframe
 *       sample — the old v_local key was exactly why animated mobys
 *       (Spyro included) missed and froze at 30 Hz. Emission order
 *       is the pose-invariant identity (G1: perm-cost ratio 0.005),
 *       so (span, ordinal) pairs the same logical vertex across two
 *       poses and the lerp morphs between them.
 *   - UNTAGGED (obj_tag==0): the 3 v_local shorts (legacy world
 *     path; the tomap isn't queried for these — §7.3 reproject).
 *
 * Including the span scopes the lookup to the matched batch pair. */
static inline uint64_t tomap_key_e(uint32_t to_span_id,
                                    const openpete_render_cache_entry_t* e) {
    /* Fixed byte layout, NO struct (padding bytes would be
     * uninitialized → nondeterministic hash → every lookup misses). */
    if (e->obj_tag != 0u) {
        uint8_t kb[8];
        memcpy(kb,     &to_span_id,    4);
        memcpy(kb + 4, &e->world_vkey, 4);
        return fnv1a64(kb, sizeof(kb));
    }
    uint8_t kb[10];
    memcpy(kb,     &to_span_id, 4);
    memcpy(kb + 4, e->v_local,  6);
    return fnv1a64(kb, sizeof(kb));
}
static inline void tomap_insert(uint32_t to_span_id,
                                const openpete_render_cache_entry_t* e,
                                uint32_t sxy) {
    uint64_t key = tomap_key_e(to_span_id, e);
    uint32_t slot = (uint32_t)(key & (TOMAP_SLOTS - 1u));
    for (uint32_t p = 0; p < TOMAP_PROBE; ++p) {
        tomap_ent_t* te = &s_tomap[slot];
        if (te->seq != s_tomap_seq || te->key == key) {
            /* empty (stale) or same logical vertex → most-recent wins */
            te->key = key; te->sxy = sxy; te->seq = s_tomap_seq;
            return;
        }
        slot = (slot + 1u) & (TOMAP_SLOTS - 1u);
    }
    /* table full along the probe run — drop (vertex just won't smooth) */
}
static inline int tomap_lookup(uint32_t to_span_id,
                               const openpete_render_cache_entry_t* e,
                               uint32_t* out_sxy) {
    uint64_t key = tomap_key_e(to_span_id, e);
    uint32_t slot = (uint32_t)(key & (TOMAP_SLOTS - 1u));
    for (uint32_t p = 0; p < TOMAP_PROBE; ++p) {
        const tomap_ent_t* te = &s_tomap[slot];
        if (te->seq != s_tomap_seq) return 0;         /* empty → miss */
        if (te->key == key) { *out_sxy = te->sxy; return 1; }
        slot = (slot + 1u) & (TOMAP_SLOTS - 1u);
    }
    return 0;
}



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
    matcher_batch_t* b = find_or_create_batch_in(f, eff_span(e));
    if (!b) return;
    if (b->vertex_count == 0) {
        /* First vertex of this batch — capture RT for UI-opt-out test
         * and obj_tag for authoritative cross-frame identity. (All
         * verts of a matrix-span share one moby/transform.) */
        memcpy(b->rt, e->rt, sizeof(b->rt));
        b->tr[0] = e->tr[0]; b->tr[1] = e->tr[1]; b->tr[2] = e->tr[2];
        b->obj_tag = e->obj_tag;
        /* #29: raw (pre-eff_span) emission-order span for tag_rank. */
        b->real_span = e->matrix_span_id;
        /* PHASE-D: key family — a strip is rendered wholly by one
         * loop variant, so the first vertex speaks for the batch. */
        b->world_src_keyed =
            (e->obj_tag & OPENPETE_WORLD_GEO_TAG_BIT) != 0u &&
            (e->world_vkey & OPENPETE_WORLD_VKEY_SRC_BIT) != 0u;
        b->gte_pc = e->gte_pc;
    }
    accumulate_vertex(b, e);
    /* For the "to" generation (gen_prev = G(N)) also record every
     * vertex's screen position keyed by (its span, v_local) so the
     * per-vertex correspondence query can find where this logical
     * vertex ended up one game frame later. */
    if (f == &s_to)
        tomap_insert(eff_span(e), e, e->sxy_key);
}

static int matrix_is_near_identity(const int16_t rt[3][3]);

/* transform-interp §4: authoritative pairing by real moby identity.
 * Linear scan — frame batch counts are small (tens–hundreds), host-
 * side, once per game frame. Returns batch index or -1. obj_tag==0
 * (world/composite) is NOT identified this way; those keep the
 * centroid/ordinal path.
 *
 * #29: the rank parameter disambiguates duplicate tags — an object
 * emitting several batches under one tag (Spyro's segments, near/far
 * strip tiers) pairs k-th batch with k-th batch instead of first-
 * unconsumed (whose scan-order dependence cross-paired segments).
 * Rank missing in `f` (segment culled that frame) ⇒ -1 ⇒ the caller
 * freezes the batch for one cycle — correct snap semantics. */
static int find_unconsumed_by_obj_tag(matcher_frame_t* f, uint32_t tag,
                                      uint32_t rank) {
    if (tag == 0u) return -1;
    for (uint32_t i = 0; i < f->count; ++i) {
        const matcher_batch_t* b = &f->batches[i];
        if (b->obj_tag == tag && b->tag_rank == rank && !b->consumed)
            return (int)i;
    }
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

/* Minimum % of a paired span's from-verts that must resolve in the
 * to-span for the span to interpolate; below this the WHOLE span
 * freezes at its lagged pose (clean 30 Hz — the §"Animated meshes"
 * behavior — instead of per-vertex partial-correspondence tearing).
 * Env-tunable for visual A/B (0 disables the gate). */
static int corr_min_pct(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_INTERP_CORR_MIN_PCT");
                 v = (e && *e) ? atoi(e) : 60;
                 if (v < 0) v = 0; if (v > 100) v = 100; }
    return v;
}

/* §7.5-R3 PHASE-B: stricter whole-strip-freeze threshold for bit30
 * WORLD strips. Terrain triangles tear visibly if a strip is only
 * PARTIALLY corresponded K-1↔K (some verts frustum-culled in one
 * frame): moved + frozen verts in the same tri → degenerate/popping
 * polys (user: "very flickery, geo popping out of existence" at
 * 60fps). A strip must be near-FULLY present on both endpoints to
 * interpolate; otherwise freeze the whole strip (clean 30 Hz for it,
 * no tear).
 *
 * Default 75, from the offline threshold sweep over 1.63M
 * batch-cycle records (corr-sweep.py over an
 * OPENPETE_CORR_LOG capture of both demo levels): vs the original
 * 90 it HALVES the frozen population and the freeze-flicker rate
 * (6.8 vs 16.4 toggles/1k observations â oscillation is the most
 * visible failure mode) and cuts forfeited motion 3.3Ã, for +70%
 * tear-px (priced at 4Ã in the sweep score; 75 stays optimal for
 * any tear-weight â¤ ~6 and the 70â80 basin is flat within 0.5%).
 * Env-tunable; 0 disables (per-vertex partial, the flickery
 * PHASE-B-initial behavior). */
static int world_corr_min_pct(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_WORLD_CORR_MIN_PCT");
                 v = (e && *e) ? atoi(e) : 75;
                 if (v < 0) v = 0; if (v > 100) v = 100; }
    return v;
}

/* §13.6 motion-coherence (spread) gate for MOBY batches. The
 * midpoint harness caught batches whose pairing is correct yet whose
 * per-vert motion is violently INCOHERENT — the canonical case is a
 * cutscene model's flattened drop-shadow pass, where small animation
 * becomes 25–90 px floor streaks on some verts while others sit
 * still (measured: 64% of the whole shadow >4px tail from three such
 * batches). Per-vert lerp can't represent that and the rigid mean is
 * equally wrong for most verts, so when the standard deviation of
 * the corresponded deltas exceeds this bound the batch FREEZES at
 * its lagged pose (honest 30 Hz — the animated-mesh fallback).
 * Spyro/mobys are unaffected: body segments are separate batches
 * with low within-segment spread (animation moves a segment's verts
 * together). World strips are exempt (rigid terrain; partial-cull
 * handled by the corr ratio + source keys). Env-tunable; 0 disables. */
static int moby_spread_max(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_SPREAD_MAX");
                 v = (e && *e) ? atoi(e) : 16;
                 if (v < 0) v = 0; }
    return v;
}

/* Rigid-fallback (camera-rotation smear fix). When a paired batch
 * has enough corresponded verts to trust their mean signed delta,
 * that mean substitutes for (a) the whole batch when the corr gate
 * trips (rigid-follow instead of a one-frame freeze) and (b) any
 * individual vert whose tomap lookup misses (instead of a zero
 * delta that stretches its polys at pan speed). RIGID_MIN_HITS
 * guards the mean against being built from a handful of verts.
 * Env OPENPETE_RIGID_FALLBACK=0 restores freeze semantics for A/B. */
#define RIGID_MIN_HITS 4u
static int rigid_fallback_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_RIGID_FALLBACK");
                 v = (e && *e) ? (atoi(e) != 0) : 1; }
    return v;
}

/* §5 / §11 F0 defense-in-depth for TAGGED pairs. Real-vaddr identity
 * pairing deliberately bypasses the centroid-bucket velocity cap (a
 * fast-moving moby must keep its id), but that left NO guard when the
 * identity itself is wrong — slot/address reuse (G2: ~18 silent
 * position-teleports per 543k pairs) or any future tag-plumbing bug
 * (the moby-queue FIFO desync shuffled ALL tags and stretched
 * same-class mobys into each other). A same-vaddr pair whose screen
 * displacement exceeds this bound in one game frame is treated as a
 * discontinuity: do not pair → the batch freezes one frame (§5 snap
 * semantics) instead of lerping across the screen. Generous default —
 * real same-object motion measures ≤ ~30 px/frame; reuse/desync jumps
 * are typically hundreds. Env-tunable; 0 disables. */
static int tagged_teleport_px(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TAGGED_TELEPORT_PX");
                 v = (e && *e) ? atoi(e) : 192;
                 if (v < 0) v = 0; }
    return v;
}

/* Companion shape guard: same instance ⇒ same model ⇒ near-equal
 * vertex counts across consecutive frames (vstab-verified). A tagged
 * pair failing this is a wrong-identity pair (reuse swapped the slot
 * to a different class, or the tag itself is corrupt) — snap.
 *
 * For bit30 WORLD strips the moby checks don't apply — strip identity
 * (the strip-list pointer) cannot alias the way a reused moby slot
 * can, and fast camera pans legitimately move strips thousands of
 * px/frame (PHASE-B measured 2724 px), so the teleport bound would
 * freeze the world exactly when smoothing matters most. World gating
 * (§11 F5b, the "warble" fix) instead protects against EMISSION
 * ordinal shifts: a strip that emitted a different vertex sequence
 * between the two endpoints would pair counter-keyed verts with
 * their NEIGHBORS through the ordinal tomap, visibly warping on
 * every intermediate present. PHASE-D resolution per key family:
 * source-keyed strips need no gate at all (each vertex carries its
 * own drop-immune key); counter-keyed strips gate on EMITTED-count
 * equality via vkey_max (assigned at RTPS time — immune to the
 * sxy-collision cache eviction that perturbs the cached
 * vertex_count whenever an animating moby overlaps the strip).
 * Equal-emitted-count add+drop swaps still slip through for counter
 * strips — closing that needs per-quad source keys for the
 * subdivided variants (#28 scope). Env OPENPETE_WORLD_COUNT_EQ=0
 * disables for visual A/B. */
static int world_count_eq_gate(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_WORLD_COUNT_EQ");
                 v = (e && *e) ? (atoi(e) != 0) : 1; }
    return v;
}
static int tagged_pair_sane(const matcher_batch_t* a,
                            const matcher_batch_t* b) {
    if (a->obj_tag & OPENPETE_WORLD_GEO_TAG_BIT) {
        /* PHASE-D: source-keyed strips are immune to the ordinal-
         * shift failure the count-eq gate exists for — a dropped/
         * evicted vertex just misses its own key while every other
         * vertex keeps its exact correspondence. */
        if (a->world_src_keyed && b->world_src_keyed) return 1;
        /* COUNTER-keyed strips (LOD/subdivided variants): gate on the
         * EMITTED count (vkey_max — ordinals are assigned at RTPS
         * time, so eviction can't change it), not the CACHED
         * vertex_count, which sxy-collision eviction perturbs every
         * frame an animating moby overlaps the strip (the residual
         * yellow still-camera flicker after the stream variants went
         * source-keyed). A real emission change (LOD tier flip, clip
         * differences) still freezes — and a mixed src/counter pair
         * (tier flip across the endpoints) freezes automatically
         * because an address never equals an ordinal. */
        if (world_count_eq_gate() && a->vkey_max != b->vkey_max)
            return 0;
        return 1;
    }
    /* MOBY arm (#25): with ordinal vkeys the shape guard tightens
     * from the old 25% cached-count tolerance to EMITTED-count
     * equality (vkey_max — assigned at RTPS time, immune to the
     * sxy-collision cache eviction that perturbs vertex_count).
     * Animation preserves a model's vertex count, so animated pairs
     * sail through; an emitted-count change means a different vertex
     * STREAM (moby LOD tier flip, slot reuse to another class) where
     * ordinal pairing would morph between two different models —
     * snap instead. */
    if (a->vkey_max != b->vkey_max) return 0;
    const int cap = tagged_teleport_px();
    if (cap > 0) {
        const int32_t dx = b->sxy_centroid[0] - a->sxy_centroid[0];
        const int32_t dy = b->sxy_centroid[1] - a->sxy_centroid[1];
        if (abs(dx) + abs(dy) > cap) return 0;
    }
    return 1;
}

/* find-only (batches already exist from sub_scan_cb's prev2 pass). */
static matcher_batch_t* find_span(matcher_frame_t* f, uint32_t span) {
    for (uint32_t i = 0; i < f->count; ++i)
        if (f->batches[i].matrix_span_id == span) return &f->batches[i];
    return NULL;
}

/* Post-pairing pass over the "from" (gen_prev2) verts: tally, per
 * paired span, how many resolve in its matched to-span. */
static void corr_scan_cb(const openpete_render_cache_entry_t* e, void* ud) {
    (void)ud;
    matcher_batch_t* b = find_span(s_curr, eff_span(e));
    if (!b || !b->has_future_anchor) return;
    b->corr_tot++;
    uint32_t to_sxy;
    if (tomap_lookup(b->to_span_id, e, &to_sxy)) {
        int16_t fx = (int16_t)(uint16_t)(e->sxy_key & 0xFFFFu);
        int16_t fy = (int16_t)(uint16_t)(e->sxy_key >> 16);
        int16_t tx = (int16_t)(uint16_t)(to_sxy & 0xFFFFu);
        int16_t ty = (int16_t)(uint16_t)(to_sxy >> 16);
        int32_t d  = abs((int)tx - (int)fx) + abs((int)ty - (int)fy);
        b->corr_hit++;
        b->corr_dpx_sum += (uint64_t)d;
        /* Signed sums for the rigid-fallback mean. Note the
         * OPENPETE_CORR_LOG capture path runs this scan a second
         * time; hit and sums double together, so the mean is
         * invariant (same reason the gate ratio tolerates it). */
        b->corr_dx_sum += (int64_t)((int)tx - (int)fx);
        b->corr_dy_sum += (int64_t)((int)ty - (int)fy);
        {
            int64_t ddx = (int64_t)((int)tx - (int)fx);
            int64_t ddy = (int64_t)((int)ty - (int)fy);
            b->corr_d2_sum += (uint64_t)(ddx * ddx + ddy * ddy);
        }
    }
}

/* ---- Midpoint-verification harness (OPENPETE_INTERP_VERIFY=path) ----
 *
 * §13.5: the empirical interpolation metric. Sub-frame ground truth
 * does not exist (the sim is the only truth and it is 30 Hz), so the
 * strongest available empiricism is leave-one-out midpoint
 * reconstruction: for a vertex alive in G(N-1), G(N), G(N+1),
 *
 *   err = |(pos(N-1)+pos(N+1))/2 − pos(N)| = |Δ2 − Δ1| / 2
 *
 * — the trajectory's second difference, where Δ1/Δ2 are the
 * PRODUCTION per-vertex tomap deltas of two consecutive display
 * cycles. Zero iff screen motion is locally linear AND both pairings
 * hit the same logical vertex. Signatures: mispair = tens-to-
 * hundreds-px outlier; curvature = small + smooth (and production
 * spans HALF this chain, so curvature error here ~4× upper-bounds
 * production — second differences scale quadratically with step);
 * freeze cost = |Δ1| (the hold-last-frame baseline, reported as
 * "gain" = err/hold); rigid-fill cost = |Δ2 − batch mean|.
 *
 * Chaining works because a vertex's identity WITHIN one generation —
 * (eff_span, world_vkey), the tomap key — is unchanged when that
 * generation moves from the "to" role (cycle K-1) to the "from" role
 * (cycle K): entries are immutable after their GTE pass. Cycle K
 * stores Δ2 keyed by the FROM-side identity; cycle K+1 reads it as
 * Δ1. Two seq-stamped maps ping-pong. Runs once per cache ROTATION
 * (prepare is per-render; within one display cycle the generation
 * pair is unchanged and a re-run would chain a generation against
 * itself → spurious err=0). Output: 48-byte LE records, one per
 * (batch, game frame), consumed by interp-verify.py.
 * Zero overhead when the env var is unset. */
#define IVMAP_SLOTS 131072u           /* match tomap headroom */
#define IVMAP_PROBE 24u
typedef struct { uint64_t key; uint32_t seq; int16_t dx, dy; } iv_ent_t;
static iv_ent_t  s_iv_map_a[IVMAP_SLOTS], s_iv_map_b[IVMAP_SLOTS];
static iv_ent_t* s_iv_rd = s_iv_map_a;  /* last cycle's Δ (read as Δ1) */
static iv_ent_t* s_iv_wr = s_iv_map_b;  /* this cycle's Δ2 (next Δ1)  */
static uint32_t  s_iv_rd_seq, s_iv_wr_seq, s_iv_seq_ctr;
typedef struct { float esum, e2sum, emax, hsum, rsum;
                 uint16_t nc, nb, nrig, h[4]; } iv_stat_t;
static iv_stat_t s_iv_stat[MATCHER_MAX_BATCHES];
static FILE*     s_iv_fp;
static int       s_iv_fp_init;
static uint32_t  s_iv_cycle, s_iv_last_rot;

static void iv_insert(uint64_t key, int16_t dx, int16_t dy) {
    uint32_t slot = (uint32_t)(key & (IVMAP_SLOTS - 1u));
    uint32_t p;
    for (p = 0; p < IVMAP_PROBE; ++p) {
        iv_ent_t* te = &s_iv_wr[slot];
        if (te->seq != s_iv_wr_seq || te->key == key) {
            te->key = key; te->seq = s_iv_wr_seq;
            te->dx = dx; te->dy = dy;
            return;
        }
        slot = (slot + 1u) & (IVMAP_SLOTS - 1u);
    }
}
static int iv_lookup(uint64_t key, int16_t* dx, int16_t* dy) {
    uint32_t slot = (uint32_t)(key & (IVMAP_SLOTS - 1u));
    uint32_t p;
    for (p = 0; p < IVMAP_PROBE; ++p) {
        const iv_ent_t* te = &s_iv_rd[slot];
        if (te->seq != s_iv_rd_seq) return 0;
        if (te->key == key) { *dx = te->dx; *dy = te->dy; return 1; }
        slot = (slot + 1u) & (IVMAP_SLOTS - 1u);
    }
    return 0;
}

static void iv_scan_cb(const openpete_render_cache_entry_t* e, void* ud) {
    matcher_batch_t* b = find_span(s_curr, eff_span(e));
    uint32_t to_sxy;
    (void)ud;
    /* Tagged, paired, per-vertex-corresponded verts only: untagged
     * legacy rides the §7.3 composite path (no per-vert truth), UI is
     * opted out of motion entirely, unpaired batches have no Δ2 (the
     * chain just breaks and restarts when the batch re-pairs). Note
     * rigid_only batches ARE included — verification wants the exact
     * trajectory even where production chose whole-span rigid. */
    if (!b || b->is_ui_opt_out || b->obj_tag == 0u) return;
    if (!b->has_future_anchor) return;
    if (!tomap_lookup(b->to_span_id, e, &to_sxy)) return;
    {
        iv_stat_t* st = &s_iv_stat[(uint32_t)(b - s_curr->batches)];
        int16_t fx = (int16_t)(uint16_t)(e->sxy_key & 0xFFFFu);
        int16_t fy = (int16_t)(uint16_t)(e->sxy_key >> 16);
        int16_t tx = (int16_t)(uint16_t)(to_sxy & 0xFFFFu);
        int16_t ty = (int16_t)(uint16_t)(to_sxy >> 16);
        int d2x = (int)tx - (int)fx, d2y = (int)ty - (int)fy;
        /* Lookup key: this vert's identity in the CURRENT from-
         * generation (where last cycle's insert filed it). Insert
         * key: its identity in the to-generation — which is what
         * eff_span will evaluate to when that generation becomes
         * prev2 next cycle. Span ids are generation-local; vkey is
         * the cross-frame part. */
        int16_t d1x, d1y;
        int have1 = iv_lookup(tomap_key_e(eff_span(e), e), &d1x, &d1y);
        iv_insert(tomap_key_e(b->to_span_id, e),
                  (int16_t)d2x, (int16_t)d2y);
        if (!have1) {
            if (st->nb < 0xFFFFu) st->nb++;
            return;
        }
        if (st->nc >= 0xFFFFu) return;
        {
            float ex = 0.5f * ((float)d2x - (float)d1x);
            float ey = 0.5f * ((float)d2y - (float)d1y);
            float err  = sqrtf(ex * ex + ey * ey);
            float hold = sqrtf((float)d1x * (float)d1x +
                               (float)d1y * (float)d1y);
            st->nc++;
            st->esum  += err;
            st->e2sum += err * err;
            if (err > st->emax) st->emax = err;
            st->hsum  += hold;
            if      (err <= 1.0f) st->h[0]++;
            else if (err <= 2.0f) st->h[1]++;
            else if (err <= 4.0f) st->h[2]++;
            else                  st->h[3]++;
            if (b->rigid_valid && st->nrig < 0xFFFFu) {
                float rx = (float)d2x - b->rigid_dx;
                float ry = (float)d2y - b->rigid_dy;
                st->rsum += sqrtf(rx * rx + ry * ry);
                st->nrig++;
            }
        }
    }
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
        for (int axis = 0; axis < 3; ++axis)
            b->vloc_centroid[axis] =
                (int32_t)(b->vloc_sum[axis] / (int64_t)b->vertex_count);
        b->has_future_anchor = 0;
        b->delta_sxy[0] = b->delta_sxy[1] = 0;
        b->velocity[0] = b->velocity[1] = b->velocity[2] = 0;
        b->dbg_state = 0;
        b->is_ui_opt_out = matrix_is_near_identity(b->rt);
        b->vtrace_velocity_capped = 0;
        b->consumed = 0;
        b->world_ord = 0xFFFFFFFFu;
        b->corr_hit = b->corr_tot = 0u;
        b->corr_dpx_sum = 0u;
        b->corr_d2_sum = 0u;
        b->corr_dx_sum = b->corr_dy_sum = 0;
        b->rigid_dx = b->rigid_dy = 0.0f;
        b->rigid_valid = 0;
        b->rigid_only  = 0;
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
    /* #29: per-tag emission rank for duplicate-tag disambiguation.
     * rank = #same-tag batches whose first vertex was emitted earlier
     * (real_span is monotonic in emission order; equal real_span +
     * equal tag would have merged into one batch, so the order is
     * total — the index tie-break is belt-and-braces). O(n²) over the
     * frame's batches: counts are hundreds, once per game frame,
     * host-side. */
    for (uint32_t i = 0; i < f->count; ++i) {
        matcher_batch_t* b = &f->batches[i];
        b->tag_rank = 0u;
        if (b->vertex_count == 0 || b->obj_tag == 0u) continue;
        uint32_t r = 0u;
        for (uint32_t j = 0; j < f->count; ++j) {
            const matcher_batch_t* o = &f->batches[j];
            if (j == i || o->vertex_count == 0) continue;
            if (o->obj_tag != b->obj_tag) continue;
            if (o->real_span < b->real_span ||
                (o->real_span == b->real_span && j < i)) r++;
        }
        b->tag_rank = r;
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
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* cb = &s_curr->batches[i];
        /* Authoritative identity first: a tagged moby inherits its
         * prior object_id by real vaddr — no centroid bucket, no
         * velocity cap (a fast-moving moby kept its id wrongly only
         * because the cap rejected it). tagged_pair_sane is the §5
         * discontinuity guard: a same-vaddr pair with a teleport-
         * scale jump or a different shape is a slot reuse / tag bug —
         * issue a fresh id (snap) instead. */
        if (cb->obj_tag != 0u) {
            int pi = find_unconsumed_by_obj_tag(s_prev, cb->obj_tag,
                                                cb->tag_rank);
            if (pi >= 0 && tagged_pair_sane(cb, &s_prev->batches[pi])) {
                s_prev->batches[pi].consumed = 1;
                cb->object_id = s_prev->batches[pi].object_id;
                continue;
            }
            cb->object_id = s_next_object_id++;
            continue;
        }
        /* Untagged (legacy world / particles / shadows): deterministic
         * ordinal+signature identity (G1-backed). §12.1 FINAL SLICE:
         * the centroid-hash fallback that used to follow is DELETED —
         * it was object_id-only (trace grouping, no motion), and a
         * 64-unit centroid bucket inheriting ids across physically
         * different batches was exactly the phantom-jump noise the
         * velocity cap existed to patch. Identity pairs exactly or
         * the batch gets a fresh id. No hashes, no caps, anywhere. */
        if (cb->world_ord != 0xFFFFFFFFu) {
            int pi = find_unconsumed_by_world_ord(s_prev, cb->world_ord,
                                                  cb->vertex_count);
            if (pi >= 0) {
                s_prev->batches[pi].consumed = 1;
                cb->object_id = s_prev->batches[pi].object_id;
                continue;
            }
        }
        cb->object_id = s_next_object_id++;
    }

    /* --- Interpolation span (s_curr "from" ↔ s_to "to"). delta_sxy =
     * to.sxy_centroid - from.sxy_centroid is the KNOWN single-game-frame
     * displacement; substitute lerps it by alpha. Pairing is identity-
     * only: (obj_tag, tag_rank) for tagged batches, world_ord for
     * untagged — no centroid-hash fallback (§12.1, deleted below). */
    unsigned paired = 0u;
    unsigned velocity_capped = 0u;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* cb = &s_curr->batches[i];
        /* Authoritative span pairing for tagged mobys: pair "from"↔
         * "to" by real vaddr, bypassing the centroid hash (which exists
         * only to reject heuristic mispairs — real identity needs no
         * bucket). tagged_pair_sane (§5 / §11 F0) still applies: on a
         * teleport-scale jump or shape mismatch the pair is dropped and
         * the batch freezes one frame instead of stretching. Downstream
         * per-vertex tomap lookup keyed by to_span_id is unchanged. */
        if (cb->obj_tag != 0u) {
            int ti = find_unconsumed_by_obj_tag(&s_to, cb->obj_tag,
                                                cb->tag_rank);
            if (ti >= 0 && tagged_pair_sane(cb, &s_to.batches[ti])) {
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
            }
            /* Tagged batches NEVER fall through to the centroid-hash
             * heuristic. If the authoritative tag has no sane
             * counterpart in "to" (despawned moby / culled-out strip /
             * count-shifted strip), the right outcome is UNPAIRED →
             * the batch freezes at its lagged pose this cycle. The
             * heuristic would instead pair it with whatever neighbor
             * shares its centroid bucket — under camera rotation that
             * paired count-rejected edge strips with adjacent strips,
             * and the cross-strip ordinal tomap flung their verts
             * across the screen (§11 F5c "spikes"). */
            if (!cb->has_future_anchor) {
                cb->dbg_state = (ti < 0) ? OPENPETE_DBGST_TAG_ABSENT
                                         : OPENPETE_DBGST_TAG_INSANE;
                /* TEMP: on an ordinal-tag absent, dump the full
                 * to-side moby tag list at the failing instant. */
                if (ti < 0 && getenv("OPENPETE_ORD_ABSENT_DUMP") &&
                    (cb->obj_tag & 0xF0000000u) == 0x30000000u) {
                    static unsigned s_oad_last = 0;
                    static unsigned s_oad_n = 0;
                    if (s_oad_n < 6u || ++s_oad_last >= 18000u) {
                        uint32_t k;
                        s_oad_last = 0; s_oad_n++;
                        fprintf(stderr,
                            "[ord-absent] from tag=%08X r=%u vc=%u "
                            "vmax=%u | to-moby:", cb->obj_tag,
                            cb->tag_rank, cb->vertex_count, cb->vkey_max);
                        for (k = 0; k < s_to.count; ++k) {
                            uint32_t t = s_to.batches[k].obj_tag;
                            if (t != 0u &&
                                !(t & OPENPETE_WORLD_GEO_TAG_BIT) &&
                                (t & 0x30000000u))
                                fprintf(stderr, " %08X/r%u/c%d", t,
                                    s_to.batches[k].tag_rank,
                                    s_to.batches[k].consumed);
                        }
                        fprintf(stderr, "\n");
                    }
                }
                if (world_diag()) {
                    /* Tally ALL tagged batches (world AND moby) — moby
                     * rows attribute e.g. a solid-red shadow to its
                     * emitting PC. */
                    wd_pc_tally(cb->gte_pc, cb->world_src_keyed, ti < 0);
                    /* OPENPETE_ABS_MOBY_ONLY=1 restricts the absent
                     * sampler to moby-class tags (world edge-cull
                     * floods the 8-slot window otherwise). */
                    if (ti < 0 && s_wd_abs_n < 8u &&
                        !(getenv("OPENPETE_ABS_MOBY_ONLY") &&
                          (cb->obj_tag & OPENPETE_WORLD_GEO_TAG_BIT))) {
                        s_wd_abs[s_wd_abs_n].pc   = cb->gte_pc;
                        s_wd_abs[s_wd_abs_n].tag  = cb->obj_tag;
                        s_wd_abs[s_wd_abs_n].rank = cb->tag_rank;
                        s_wd_abs[s_wd_abs_n].vmax = cb->vkey_max;
                        /* vc repurposed: to-side census for this tag —
                         * low byte = count of same-tag batches in s_to,
                         * bit8 = first one consumed, bits16+ = its rank. */
                        {
                            uint32_t n = 0, fr = 0, fc = 0;
                            for (uint32_t k = 0; k < s_to.count; ++k) {
                                if (s_to.batches[k].obj_tag != cb->obj_tag)
                                    continue;
                                if (!n) { fr = s_to.batches[k].tag_rank;
                                          fc = s_to.batches[k].consumed; }
                                n++;
                            }
                            s_wd_abs[s_wd_abs_n].vc =
                                (n & 0xFFu) | (fc ? 0x100u : 0u) |
                                (fr << 16);
                        }
                        s_wd_abs_n++;
                    }
                    if (ti >= 0 && !cb->world_src_keyed &&
                        s_wd_smp_n < 8u) {
                        const matcher_batch_t* tb2 = &s_to.batches[ti];
                        s_wd_smp[s_wd_smp_n].pc   = cb->gte_pc;
                        s_wd_smp[s_wd_smp_n].amax = cb->vkey_max;
                        s_wd_smp[s_wd_smp_n].bmax = tb2->vkey_max;
                        s_wd_smp[s_wd_smp_n].avc  = cb->vertex_count;
                        s_wd_smp[s_wd_smp_n].bvc  = tb2->vertex_count;
                        s_wd_smp_n++;
                    }
                }
            }
            continue;
        }
        /* World (untagged): ordinal+signature span pairing (Option B)
         * before centroid-hash. Same downstream per-vertex tomap. */
        if (cb->obj_tag == 0u && cb->world_ord != 0xFFFFFFFFu) {
            int ti = find_unconsumed_by_world_ord(&s_to, cb->world_ord,
                                                  cb->vertex_count);
            if (ti >= 0) {
                matcher_batch_t* tb = &s_to.batches[ti];
                cb->dbg_state = OPENPETE_DBGST_LEGACY_WORLD;
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
        /* §12.1 (first slice): the centroid-hash span-pairing fallback
         * is DELETED. It only ever fired for untagged batches whose
         * world-ordinal pairing failed (tagged batches already pair-or-
         * freeze), and when it fired it was usually WRONG — the 64-unit
         * centroid bucket cross-paired sky strips whose ordinal slipped
         * with their neighbors, warping them on every intermediate
         * present (the user-visible "flickers of magenta"). An unpaired
         * batch now freezes at its lagged pose for one cycle — a brief
         * 30 Hz patch, visually quiet, and the honest §12 fallback.
         * (The overlay's magenta HEUR_PAIRED state is consequently
         * never emitted; the enum stays for the legend's history.)
         *
         * Unpaired "from" batch → delta_sxy stays {0,0}: the object
         * despawned / ordinal slipped between G(N-1) and G(N); it
         * renders frozen at its lagged position for this cycle. */
    }
    g_openpete_render_substitute_pair_count = paired;
    g_openpete_render_substitute_velocity_capped_count = velocity_capped;

    /* Consume any pending OPENPETE_PREV_TRANSFORM override (no-op if
     * none). It overwrites the matched batch's delta_sxy with an
     * explicitly-declared span. */
    apply_prev_transform_override();

    /* OPENPETE_CORR_LOG=<path>: per-cycle per-world-batch raw corr
     * records for the offline threshold sweep
     * (corr-sweep.py). 16B LE records:
     *   u32 obj_tag, u16 corr_tot, u16 corr_hit, u32 dpx_sum(sat),
     *   u32 cycle. Threshold-independent by construction. */
    {
        static FILE* s_cl_fp = NULL;
        static int   s_cl_init = 0;
        static uint32_t s_cl_cycle = 0;
        if (!s_cl_init) {
            const char* p_ = getenv("OPENPETE_CORR_LOG");
            s_cl_init = 1;
            if (p_ && *p_) s_cl_fp = fopen(p_, "wb");
        }
        if (s_cl_fp) {
            /* The corr scan normally runs just below, gated on the
             * thresholds; run it here unconditionally for capture. */
            openpete_render_cache_for_each_prev2_entry(corr_scan_cb, NULL);
            for (uint32_t i = 0; i < s_curr->count; ++i) {
                const matcher_batch_t* b = &s_curr->batches[i];
                if (!b->has_future_anchor) continue;
                if (!(b->obj_tag & OPENPETE_WORLD_GEO_TAG_BIT)) continue;
                if (b->corr_tot == 0u) continue;
                uint8_t rec[16];
                uint16_t tot = (uint16_t)(b->corr_tot > 0xFFFFu
                                          ? 0xFFFFu : b->corr_tot);
                uint16_t hit = (uint16_t)(b->corr_hit > 0xFFFFu
                                          ? 0xFFFFu : b->corr_hit);
                uint32_t dpx = (uint32_t)(b->corr_dpx_sum > 0xFFFFFFFFu
                                          ? 0xFFFFFFFFu : b->corr_dpx_sum);
                memcpy(rec + 0,  &b->obj_tag, 4);
                memcpy(rec + 4,  &tot, 2);
                memcpy(rec + 6,  &hit, 2);
                memcpy(rec + 8,  &dpx, 4);
                memcpy(rec + 12, &s_cl_cycle, 4);
                fwrite(rec, 1, 16, s_cl_fp);
            }
            s_cl_cycle++;
        }
    }

    /* §"Animated meshes": batch-level correspondence gate. Tally how
     * many of each paired span's from-verts resolve one game-frame
     * later; if the ratio is low the mesh is animating/churning or
     * the pairing is partial — freeze the WHOLE span (drop the
     * anchor) so substitute_query returns 0 for every one of its
     * verts. This is the documented Tier-1 behavior and the fix for
     * the per-vertex tear-apart ("polys disappearing from mobys
     * during movement") that obj_tag's cap-bypass exposed. */
    {
        const int min_pct = corr_min_pct();
        if (min_pct > 0 || world_corr_min_pct() > 0) {
            openpete_render_cache_for_each_prev2_entry(corr_scan_cb, NULL);
            for (uint32_t i = 0; i < s_curr->count; ++i) {
                matcher_batch_t* b = &s_curr->batches[i];
                if (!b->has_future_anchor) continue;
                /* Legacy untagged world (obj_tag==0) is EXEMPT: it
                 * rides the §7.3 composite-reproject path (whole-span
                 * consistent, no tear) and its v_local-keyed corr is
                 * ~0 by construction — freezing it was the round-2
                 * "zero blue on world". */
                if (b->obj_tag == 0u) continue;
                /* Rigid-fallback mean for EVERY paired tagged batch
                 * (consumed below for sub-threshold rigid-follow and
                 * by substitute_query for per-vert miss fill). */
                if (rigid_fallback_on() && b->corr_hit >= RIGID_MIN_HITS) {
                    b->rigid_dx = (float)b->corr_dx_sum / (float)b->corr_hit;
                    b->rigid_dy = (float)b->corr_dy_sum / (float)b->corr_hit;
                    b->rigid_valid = 1;
                }
                int is_w = (b->obj_tag & OPENPETE_WORLD_GEO_TAG_BIT) != 0u;
                /* §7.5-R3 PHASE-B: bit30 WORLD strips use the
                 * per-vertex (eff_span,world_vidx) tomap. corr_hit/
                 * corr_tot is now the real vidx coverage K-1↔K. A
                 * partially-corresponded strip must not lerp per-vert
                 * (moved + held verts in one tri tear/pop). Mobys
                 * keep the v_local-churn animation threshold. The
                 * corr_tot<8 noise guard is bypassed for world —
                 * small terrain strips tear too. */
                /* §13.6 spread gate (mobys only, see moby_spread_max
                 * rationale). Variance of the corresponded deltas
                 * around their mean; double-tally under CORR_LOG
                 * cancels (all sums and hit double together). */
                if (!is_w && b->corr_hit >= 8u && moby_spread_max() > 0) {
                    double n  = (double)b->corr_hit;
                    double mx = (double)b->corr_dx_sum / n;
                    double my = (double)b->corr_dy_sum / n;
                    double var = (double)b->corr_d2_sum / n
                                 - mx * mx - my * my;
                    double lim = (double)moby_spread_max();
                    if (var > lim * lim) {
                        b->has_future_anchor = 0;   /* incoherent: freeze */
                        b->dbg_state = OPENPETE_DBGST_CORR_FROZEN;
                        if (paired) paired--;
                        s_wd_spread_froze++;
                        continue;
                    }
                }
                int thr = is_w ? world_corr_min_pct() : min_pct;
                if (thr <= 0) continue;
                if (!is_w && b->corr_tot < 8u) continue;
                if (b->corr_tot == 0u) continue;
                if (b->corr_hit * 100u < (uint32_t)thr * b->corr_tot) {
                    /* Sub-threshold: rigid-follow when the mean is
                     * trustworthy (whole-span consistent — no tear —
                     * and motion-true to first order), else the old
                     * whole-span freeze. Freezing was the camera-
                     * rotation smear: at pan speed the frozen span
                     * sat a full game frame (tens-to-hundreds of px)
                     * behind its interpolating neighbors. */
                    if (b->rigid_valid) {
                        b->rigid_only = 1;
                        s_wd_rigid_span++;
                    } else {
                        b->has_future_anchor = 0;      /* whole-span freeze */
                        if (paired) paired--;
                        s_wd_rigid_froze++;
                    }
                    b->dbg_state = OPENPETE_DBGST_CORR_FROZEN;
                    /* Orange attribution: same-variant gate trip
                     * (eviction / partial cull) vs cross-variant (LOD
                     * tier migration — from/to RTPS PCs differ). */
                    if (world_diag() && is_w) {
                        const matcher_batch_t* tb =
                            find_span(&s_to, b->to_span_id);
                        if (tb && tb->gte_pc != b->gte_pc) s_wd_orange_mig++;
                        else                               s_wd_orange_same++;
                        if (b->corr_hit == 0u) s_wd_orange_zero++;
                    }
                }
            }
            g_openpete_render_substitute_pair_count = paired;
        }
    }

    /* Midpoint-verification capture — see the harness block above
     * corr_scan_cb for the methodology. Gated on the env var; runs
     * the chain pass only when the cache ring actually rotated. */
    {
        if (!s_iv_fp_init) {
            const char* p_ = getenv("OPENPETE_INTERP_VERIFY");
            s_iv_fp_init = 1;
            if (p_ && *p_) s_iv_fp = fopen(p_, "wb");
        }
        if (s_iv_fp) {
            uint32_t rot = openpete_render_cache_rotation_count();
            if (rot != s_iv_last_rot) {
                s_iv_last_rot = rot;
                { iv_ent_t* t = s_iv_rd; s_iv_rd = s_iv_wr; s_iv_wr = t; }
                s_iv_rd_seq = s_iv_wr_seq;
                s_iv_wr_seq = ++s_iv_seq_ctr;
                /* Camera-cut cycle (§5/#24): the trajectory is
                 * GENUINELY discontinuous — perfect pairing would
                 * still register a huge second difference. Skip the
                 * scan, leave the write map empty so chains restart
                 * cleanly after the cut (next cycle = all breaks). */
                if (openpete_render_interp_snap_get()) {
                    s_iv_cycle++;
                    goto iv_done;
                }
                openpete_render_cache_for_each_prev2_entry(iv_scan_cb, NULL);
                for (uint32_t i = 0; i < s_curr->count; ++i) {
                    iv_stat_t* st = &s_iv_stat[i];
                    if (!st->nc && !st->nb) continue;
                    {
                        const matcher_batch_t* b = &s_curr->batches[i];
                        uint8_t rec[48];
                        uint16_t zpad = 0;
                        memcpy(rec + 0,  &b->obj_tag, 4);
                        memcpy(rec + 4,  &b->gte_pc,  4);
                        memcpy(rec + 8,  &st->nc,     2);
                        memcpy(rec + 10, &st->nb,     2);
                        memcpy(rec + 12, &st->h[0],   2);
                        memcpy(rec + 14, &st->h[1],   2);
                        memcpy(rec + 16, &st->h[2],   2);
                        memcpy(rec + 18, &st->h[3],   2);
                        memcpy(rec + 20, &st->esum,   4);
                        memcpy(rec + 24, &st->e2sum,  4);
                        memcpy(rec + 28, &st->emax,   4);
                        memcpy(rec + 32, &st->hsum,   4);
                        memcpy(rec + 36, &st->rsum,   4);
                        memcpy(rec + 40, &st->nrig,   2);
                        memcpy(rec + 42, &zpad,       2);
                        memcpy(rec + 44, &s_iv_cycle, 4);
                        fwrite(rec, 1, 48, s_iv_fp);
                    }
                    memset(st, 0, sizeof(*st));
                }
                s_iv_cycle++;
iv_done:        ;
            }
        }
    }

    /* TEMP: OPENPETE_ORD_CENSUS=1 — dump ordinal-class (0x1xxxxxxx)
     * tags on both sides every 300 prepares. */
    if (getenv("OPENPETE_ORD_CENSUS")) {
        static unsigned s_oc_n = 0;
        if (++s_oc_n % 300u == 0u) {
            uint32_t i;
            fprintf(stderr, "[ord-census] from:");
            for (i = 0; i < s_curr->count; ++i) {
                uint32_t t = s_curr->batches[i].obj_tag;
                if ((t & 0xD0000000u) == 0x10000000u)
                    fprintf(stderr, " %08X/r%u", t,
                            s_curr->batches[i].tag_rank);
            }
            fprintf(stderr, "\n[ord-census] to  :");
            for (i = 0; i < s_to.count; ++i) {
                uint32_t t = s_to.batches[i].obj_tag;
                if ((t & 0xD0000000u) == 0x10000000u)
                    fprintf(stderr, " %08X/r%u", t,
                            s_to.batches[i].tag_rank);
            }
            fprintf(stderr, "\n");
        }
    }

    /* World-span diagnostic: per-batch fallout tally + periodic dump. */
    if (world_diag()) {
        for (uint32_t i = 0; i < s_curr->count; ++i) {
            const matcher_batch_t* b = &s_curr->batches[i];
            if (b->vertex_count == 0u || !is_world_obj_tag(b->obj_tag))
                continue;
            s_wd_batches++;
            if (b->obj_tag & OPENPETE_WORLD_GEO_TAG_BIT) s_wd_batches_tagged++;
            else s_wd_batches_legacy++;
            if (b->is_ui_opt_out) s_wd_uiopt++;
            else if (b->has_future_anchor) s_wd_anchored++;
        }
        if (++s_wd_frames % 300u == 0u) {
            fprintf(stderr,
              "[world-diag] f=%lu | world-batches=%lu (tagged=%lu legacy=%lu) "
              "uiopt=%lu anchored=%lu | sq_calls=%lu prev2miss=%lu "
              "world_entry=%lu | world: total=%lu no_to_span=%lu "
              "reproj_fail=%lu APPLIED=%lu | mag>=1px=%lu >=4px=%lu "
              "max=%lupx samemtx=%lu\n",
              s_wd_frames, s_wd_batches, s_wd_batches_tagged,
              s_wd_batches_legacy, s_wd_uiopt, s_wd_anchored,
              s_wd_q_calls, s_wd_q_prev2miss, s_wd_q_world_entry,
              s_wd_q_total, s_wd_q_no_tb, s_wd_q_reproj_fail,
              s_wd_q_applied, s_wd_q_ge1, s_wd_q_ge4, s_wd_q_maxmag, s_wd_q_samemtx);
            if (s_wd_pc_n) {
                fprintf(stderr, "[world-yellow]");
                for (unsigned i = 0; i < s_wd_pc_n; ++i)
                    fprintf(stderr, " %08X:is=%lu ic=%lu ab=%lu",
                            s_wd_pc[i].pc, s_wd_pc[i].insane_src,
                            s_wd_pc[i].insane_ctr, s_wd_pc[i].absent);
                fprintf(stderr, "\n");
                memset(s_wd_pc, 0, sizeof(s_wd_pc)); s_wd_pc_n = 0;
            }
            {
                /* bit29 (shadow-subtag) batch census per endpoint. */
                unsigned cs = 0, ts = 0;
                for (uint32_t k = 0; k < s_curr->count; ++k)
                    if ((s_curr->batches[k].obj_tag & 0x20000000u) &&
                        !(s_curr->batches[k].obj_tag &
                          OPENPETE_WORLD_GEO_TAG_BIT)) cs++;
                for (uint32_t k = 0; k < s_to.count; ++k)
                    if ((s_to.batches[k].obj_tag & 0x20000000u) &&
                        !(s_to.batches[k].obj_tag &
                          OPENPETE_WORLD_GEO_TAG_BIT)) ts++;
                fprintf(stderr,
                  "[orange] mig=%lu same=%lu zerocorr=%lu | "
                  "rigid: span=%lu froze=%lu fill=%lu | spread=%lu\n",
                  s_wd_orange_mig, s_wd_orange_same, s_wd_orange_zero,
                  s_wd_rigid_span, s_wd_rigid_froze, s_wd_rigid_fill,
                  s_wd_spread_froze);
                s_wd_orange_mig = s_wd_orange_same = s_wd_orange_zero = 0;
                s_wd_rigid_span = s_wd_rigid_froze = s_wd_rigid_fill = 0;
                s_wd_spread_froze = 0;
                fprintf(stderr, "[b29] from=%u to=%u |", cs, ts);
                { unsigned pr = 0;
                  for (uint32_t k = 0; k < s_curr->count && pr < 3; ++k)
                      if ((s_curr->batches[k].obj_tag & 0x20000000u) &&
                          !(s_curr->batches[k].obj_tag &
                            OPENPETE_WORLD_GEO_TAG_BIT)) {
                          fprintf(stderr, " f:%08X",
                                  s_curr->batches[k].obj_tag); pr++; }
                  pr = 0;
                  for (uint32_t k = 0; k < s_to.count && pr < 3; ++k)
                      if ((s_to.batches[k].obj_tag & 0x20000000u) &&
                          !(s_to.batches[k].obj_tag &
                            OPENPETE_WORLD_GEO_TAG_BIT)) {
                          fprintf(stderr, " t:%08X",
                                  s_to.batches[k].obj_tag); pr++; } }
                fprintf(stderr, "\n");
            }
            if (s_wd_abs_n) {
                fprintf(stderr, "[absent-smp]");
                for (unsigned i = 0; i < s_wd_abs_n; ++i)
                    fprintf(stderr, " %08X:tag=%08X r=%u max=%u vc=%u",
                            s_wd_abs[i].pc, s_wd_abs[i].tag,
                            s_wd_abs[i].rank, s_wd_abs[i].vmax,
                            s_wd_abs[i].vc);
                fprintf(stderr, "\n");
                s_wd_abs_n = 0;
            }
            if (s_wd_smp_n) {
                fprintf(stderr, "[yellow-smp]");
                for (unsigned i = 0; i < s_wd_smp_n; ++i)
                    fprintf(stderr, " %08X:max%u/%u vc%u/%u",
                            s_wd_smp[i].pc, s_wd_smp[i].amax,
                            s_wd_smp[i].bmax, s_wd_smp[i].avc,
                            s_wd_smp[i].bvc);
                fprintf(stderr, "\n");
                s_wd_smp_n = 0;
            }
        }
    }

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

/* §7.3 world composite reprojection. Replicates the PSX RTPS path in
 * float (presentation only — never feeds gameplay; determinism
 * unaffected): MAC = RT_q12·v + TR (SHORTMATRIX is Q12, 1.0=4096);
 * screen = (OFX + MAC·(H/SZ)) / 65536  (OFX/OFY are Q16, SZ=MAC3).
 * Returns 0 if behind the projection plane (SZ≤0). Used ONLY via the
 * DIFFERENCE project(to)-project(from), so any constant scale/offset
 * imprecision in this reimplementation cancels and endpoints stay
 * exact (the §9 α-anchoring requirement, by construction). */
static int world_reproject(const int16_t rt[3][3], const int32_t tr[3],
                           const int16_t v[3], int32_t ofx, int32_t ofy,
                           uint16_t h, double* sx, double* sy) {
    double vx = v[0], vy = v[1], vz = v[2];
    double m1 = (rt[0][0]*vx + rt[0][1]*vy + rt[0][2]*vz) / 4096.0 + tr[0];
    double m2 = (rt[1][0]*vx + rt[1][1]*vy + rt[1][2]*vz) / 4096.0 + tr[1];
    double m3 = (rt[2][0]*vx + rt[2][1]*vy + rt[2][2]*vz) / 4096.0 + tr[2];
    if (m3 <= 1.0) return 0;                 /* behind / on the plane */
    double hsz = (double)h / m3;
    *sx = ((double)ofx + m1 * hsz) / 65536.0;
    *sy = ((double)ofy + m2 * hsz) / 65536.0;
    return 1;
}

openpete_substitute_delta_t openpete_render_matcher_substitute_query(
        const int16_t* p, float alpha) {
    return openpete_render_matcher_substitute_query_px(p, alpha, 0);
}

/* pgxp_base != 0 ⇒ the caller (ApplyVertexPGXP) float-positioned this
 * vertex's GL base at the cache entry's sub-pixel projection. The delta
 * must then be float-endpoint-to-float-endpoint: an int16-SXY delta on
 * a float base lands every intermediate present at
 * float(K-1) + α·(int(K)−int(K-1)) = float(K) + (frac(K-1)−frac(K)),
 * a per-vertex ±1px error that changes every sim tick — re-adding at
 * 30 Hz exactly the quantization wobble PGXP exists to remove
 * (transform-interp plan §11 F5). */
openpete_substitute_delta_t openpete_render_matcher_substitute_query_px(
        const int16_t* p, float alpha, int pgxp_base) {
    openpete_substitute_delta_t out = { 0.0f, 0.0f };
    if (!g_openpete_interp_enabled) return out;  /* 'I' kill switch */
    if (alpha <= 0.0f) return out;
    s_wd_q_calls++;

    openpete_render_cache_entry_t entry;
    /* The vertex belongs to the lagged OT (OT#(N-1)) → its SXY resolves
     * in gen_prev2 (= "from" = G(N-1)), the same generation s_curr was
     * scanned from. §13.8: consensus-tag selection over the pixel's
     * coexisting candidates. */
    if (!select_prev2_entry(p, &entry)) {
        s_wd_q_prev2miss++;
        g_openpete_render_substitute_vert_miss_count++;
        return out;
    }
    if (is_world_obj_tag(entry.obj_tag)) s_wd_q_world_entry++;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        const matcher_batch_t* b = &s_curr->batches[i];
        if (b->matrix_span_id != eff_span(&entry)) continue;
        if (b->is_ui_opt_out) {
            /* HUD/UI batches must not lerp — screen-space, near-identity
             * world matrix; a delta would smear the 2D overlay. */
            return out;
        }
        if (!b->has_future_anchor) {           /* batch didn't pair */
            g_openpete_render_substitute_vert_miss_count++;
            return out;
        }
        if (b->rigid_only) {
            /* Sub-threshold corr gate: the whole span rides the mean
             * corresponded delta rigidly — consistent (no tear) and
             * motion-true to first order, instead of holding a full
             * game frame behind its neighbors (the camera-rotation
             * smear). Per-vert exactness is forfeited deliberately:
             * mixing exact and fallback verts at low coverage is the
             * tear the gate exists to prevent. */
            float dx = b->rigid_dx * alpha;
            float dy = b->rigid_dy * alpha;
            const float rcap = g_openpete_substitute_clamp_px;
            if (rcap <= 0.0f) return out;
            if (dx >  rcap) dx =  rcap; if (dx < -rcap) dx = -rcap;
            if (dy >  rcap) dy =  rcap; if (dy < -rcap) dy = -rcap;
            out.dx = dx; out.dy = dy;
            g_openpete_render_substitute_vert_hit_count++;
            return out;
        }
        /* §7.3 — LEGACY untagged world (obj_tag==0): the terrain
         * RTPS variants NOT yet covered by the §7.5-R3 strip-tag
         * recompiler hook. Composite per-span reprojection: reproject
         * THIS vertex through the span's own from-matrix (exact, from
         * the gen_prev2 hit `entry`) and the matched to-span's
         * matrix, α·(to−from). Whole-span-consistent (no tear),
         * α=0 ⇒ 0 (residual cancels). KNOWN LIMITATION (measured in
         * §7.5-R3 PHASE-A: max 2px, samemtx≈68%): uses K-1 camera-
         * relative v_local for both endpoints ⇒ captures camera
         * ROTATION but is blind to camera TRANSLATION. This is why
         * PHASE-B exists — bit30 strips DON'T come here; they fall
         * through to the per-vertex tomap path below, which
         * tomap_key_e now keys by the cross-frame-exact world_vidx
         * (emission ordinal), giving full endpoint-exact motion
         * incl. camera translation. Expanding the recomp hook to the
         * remaining RTPS variants shrinks this legacy path to zero. */
        if (b->obj_tag == 0u) {
            s_wd_q_total++;
            const matcher_batch_t* tb = find_span(&s_to, b->to_span_id);
            if (!tb || tb->vertex_count == 0u) {
                s_wd_q_no_tb++;
                g_openpete_render_substitute_vert_miss_count++;
                return out;
            }
            double fx, fy, tx, ty;
            if (!world_reproject(entry.rt, entry.tr, entry.v_local,
                                 entry.ofx, entry.ofy, entry.h, &fx, &fy) ||
                !world_reproject(tb->rt, tb->tr, entry.v_local,
                                 entry.ofx, entry.ofy, entry.h, &tx, &ty)) {
                s_wd_q_reproj_fail++;
                g_openpete_render_substitute_vert_miss_count++;
                return out;
            }
            s_wd_q_applied++;
            if (memcmp(entry.rt, tb->rt, sizeof(entry.rt)) == 0 &&
                entry.tr[0]==tb->tr[0] && entry.tr[1]==tb->tr[1] &&
                entry.tr[2]==tb->tr[2]) s_wd_q_samemtx++;
            float dx = (float)((tx - fx) * (double)alpha);
            float dy = (float)((ty - fy) * (double)alpha);
            { unsigned long mg = (unsigned long)(fabs(dx) + fabs(dy));
              if (mg >= 1u) s_wd_q_ge1++;
              if (mg >= 4u) s_wd_q_ge4++;
              if (mg > s_wd_q_maxmag) s_wd_q_maxmag = mg; }
            const float capw = g_openpete_substitute_clamp_px;
            if (capw <= 0.0f) return out;
            if (dx >  capw) dx =  capw; if (dx < -capw) dx = -capw;
            if (dy >  capw) dy =  capw; if (dy < -capw) dy = -capw;
            out.dx = dx; out.dy = dy;
            g_openpete_render_substitute_vert_hit_count++;
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
        /* §7.5-R3 PHASE-B: bit30 world reaches here. tomap_key_e keyed
         * it by (eff_span, world_vidx) — the cross-frame-exact
         * emission ordinal — so this is the SAME strip vertex's
         * actual K screen position. Diag accounting mirrors the §7.3
         * block so PHASE-B is measurable in OPENPETE_WORLD_DIAG. */
        const int wtag = (b->obj_tag & OPENPETE_WORLD_GEO_TAG_BIT) != 0u;
        uint32_t to_sxy;
        if (wtag) s_wd_q_total++;
        if (!tomap_lookup(b->to_span_id, &entry, &to_sxy)) {
            if (wtag) s_wd_q_no_tb++;
            if (b->rigid_valid) {
                /* No exact counterpart (evicted / culled across the
                 * endpoints) — ride the batch's mean corresponded
                 * delta instead of holding at the lagged base. A
                 * zero-delta vert inside an interpolating batch
                 * stretches its polys by the full frame-to-frame
                 * displacement at pan speed (the moby smear). */
                float dx = b->rigid_dx * alpha;
                float dy = b->rigid_dy * alpha;
                const float mcap = g_openpete_substitute_clamp_px;
                if (mcap <= 0.0f) return out;
                if (dx >  mcap) dx =  mcap; if (dx < -mcap) dx = -mcap;
                if (dy >  mcap) dy =  mcap; if (dy < -mcap) dy = -mcap;
                out.dx = dx; out.dy = dy;
                s_wd_rigid_fill++;
                g_openpete_render_substitute_vert_hit_count++;
                return out;
            }
            g_openpete_render_substitute_vert_miss_count++;
            return out;
        }
        int16_t from_sx = (int16_t)p[0];
        int16_t from_sy = (int16_t)p[1];
        int16_t to_sx   = (int16_t)(uint16_t)(to_sxy & 0xFFFFu);
        int16_t to_sy   = (int16_t)(uint16_t)(to_sxy >> 16);
        if (wtag) {
            s_wd_q_applied++;
            unsigned long mg = (unsigned long)(abs((int)to_sx-(int)from_sx)
                                             + abs((int)to_sy-(int)from_sy));
            if (mg >= 1u) s_wd_q_ge1++;
            if (mg >= 4u) s_wd_q_ge4++;
            if (mg > s_wd_q_maxmag) s_wd_q_maxmag = mg;
        }
        float dx, dy;
        openpete_pgxp_lookup_t pf_from, pf_to;
        if (pgxp_base &&
            openpete_render_cache_pgxp_project(&entry, &pf_from) &&
            openpete_render_cache_lookup_pgxp_to(to_sxy, &pf_to)) {
            /* PGXP-consistent: both endpoints at float sub-pixel
             * precision, and the delta is relative to the same float
             * "from" the GL base was set to — α=1 lands exactly on
             * float(K), α=0 exactly on the base. The base offset
             * (pf_from − int p[]) is already in the GL vertex; only
             * the motion is added here. */
            dx = (pf_to.px - pf_from.px) * alpha;
            dy = (pf_to.py - pf_from.py) * alpha;
        } else {
            /* Int path (PGXP off / miss): base is the int16 SXY, so an
             * int-endpoint delta is self-consistent. */
            dx = (float)((int)to_sx - (int)from_sx) * alpha;
            dy = (float)((int)to_sy - (int)from_sy) * alpha;
        }
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
/* §12.2 overlay v2 — see header for the state table. Mirrors
 * substitute_query's decision tree, side-effect-free. */
int openpete_render_matcher_debug_state(const int16_t* p) {
    openpete_render_cache_entry_t entry;
    if (!select_prev2_entry(p, &entry))
        return OPENPETE_DBGST_NONE;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        const matcher_batch_t* b = &s_curr->batches[i];
        if (b->matrix_span_id != eff_span(&entry)) continue;
        if (b->is_ui_opt_out) return OPENPETE_DBGST_NONE;
        if (b->dbg_state != OPENPETE_DBGST_NONE &&
            b->dbg_state != OPENPETE_DBGST_LEGACY_WORLD &&
            b->dbg_state != OPENPETE_DBGST_HEUR_PAIRED)
            return b->dbg_state;            /* frozen-batch causes */
        if (!b->has_future_anchor) return OPENPETE_DBGST_NONE;
        /* Paired batch: vertex-level outcome. obj_tag==0 legacy rides
         * the §7.3 whole-span reproject (no per-vertex tomap). */
        if (b->obj_tag == 0u) return b->dbg_state;
        uint32_t to_sxy;
        if (!tomap_lookup(b->to_span_id, &entry, &to_sxy))
            return OPENPETE_DBGST_VERT_MISS;
        return b->dbg_state;                /* HEUR_PAIRED or NONE */
    }
    return OPENPETE_DBGST_NONE;
}

int openpete_render_matcher_is_smoothed(const int16_t* p) {
    openpete_render_cache_entry_t entry;
    if (!select_prev2_entry(p, &entry)) return 0;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        const matcher_batch_t* b = &s_curr->batches[i];
        if (b->matrix_span_id != eff_span(&entry)) continue;
        if (b->is_ui_opt_out || !b->has_future_anchor) return 0;
        uint32_t to_sxy;
        if (!tomap_lookup(b->to_span_id, &entry, &to_sxy)) return 0;
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

