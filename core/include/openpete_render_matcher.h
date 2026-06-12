/* openpete_render_matcher.h — RT64-style draw-call matcher.
 *
 * Phase 1 of renderer-foundation. At OT-walk time, group primitives by
 * matrix_span_id (= "logical batch"), hash each batch by geometric
 * fingerprint, and match each current-frame batch against the previous
 * frame's batches. Assigns a stable `object_id` to every matched
 * batch; new objects get a freshly minted id; objects that
 * disappeared this frame are counted as "dropped".
 *
 * Phase 1 emits NO render changes — it just produces match data
 * (matched / unmatched_new / unmatched_dropped per frame) that Phase 2
 * will consume for T/R/S lerp.
 *
 * Determinism: matcher state is host-side only; never reads back into
 * PSX RAM. Heuristic match decisions can vary run-to-run without
 * affecting gameplay-readable state (verified by the cache being
 * non-PSX-RAM by construction).
 *
 * Threading: called only from the game fiber / PsyCross OT walker; no
 * cross-thread synchronization needed.
 */

#ifndef OPENPETE_RENDER_MATCHER_H
#define OPENPETE_RENDER_MATCHER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-prim observe hooks called from PsyCross MakeVertexTriangle /
 * MakeVertexQuad. Each VERTTYPE* points to the {x,y} short pair the
 * prim packet stores. Internally the matcher looks up each (x,y) in
 * the render cache's s_prev (= "currently being drawn" frame) to
 * recover the matrix_span_id and world-space MAC values for batching. */
void openpete_render_matcher_observe_tri(const int16_t* p0,
                                          const int16_t* p1,
                                          const int16_t* p2);
void openpete_render_matcher_observe_quad(const int16_t* p0,
                                           const int16_t* p1,
                                           const int16_t* p2,
                                           const int16_t* p3);

/* Finalize the current frame: compute geometric hash for each batch,
 * match against the previous frame's batches, swap the prev/curr
 * matcher buffers. Called once per render frame from PsyX_EndScene
 * (immediately BEFORE the render-cache rotate so the cache's s_prev
 * is still the buffer the matcher observed against). */
void openpete_render_matcher_finalize_frame(void);

/* ----------------------------------------------------------------------
 * Phase 2 — substitute pipeline.
 *
 * Independent of the Phase 1 observe/finalize pipeline. Performs an
 * eager dual-scan of cache.s_prev (= FRAME N-1, currently visible) and
 * cache.s_curr (= FRAME N, next visible), groups each buffer's entries
 * by matrix_span_id, hashes by quantized centroid, and matches them to
 * produce (FRAME N-1, FRAME N) anchor pairs per object.
 *
 * Substitute is then called per prim during the OT walk; it looks up
 * each vertex's matrix_span_id (via the cache.s_prev hit), finds the
 * paired anchor for that batch, lerps screen-space delta at alpha, and
 * mutates the GL vertex buffer entry (NOT the prim packet, NOT PSX
 * RAM — see "Determinism" note in the plan).
 *
 * Phase 2.0 milestone uses uniform per-batch screen-space delta; the
 * full T/R/S+slerp recompose lands in Phase 2.1+ once the skeletal-
 * granularity test passes. */
void openpete_render_matcher_prepare_substitute(void);

/* Query: given a vertex's int16 SXY pair, return the lerp delta (dx, dy
 * in screen-space pixels) that the caller should ADD to the vertex's
 * already-projected position. Zero if the vertex has no matching
 * batch or the batch has no future anchor. PsyCross applies the
 * returned delta to the GL vertex buffer entry's x/y fields. */
typedef struct {
    float dx;
    float dy;
} openpete_substitute_delta_t;

openpete_substitute_delta_t openpete_render_matcher_substitute_query(
    const int16_t* p, float alpha);

/* PGXP-aware variant. pgxp_base != 0 means the caller float-positioned
 * this vertex's GL base via ApplyVertexPGXP, so the delta is computed
 * float-endpoint-to-float-endpoint (sub-pixel consistent) instead of
 * int16-to-int16; falls back to the int delta if either endpoint can't
 * be float-projected. The legacy query above ≡ pgxp_base=0. */
openpete_substitute_delta_t openpete_render_matcher_substitute_query_px(
    const int16_t* p, float alpha, int pgxp_base);

/* Debug viz: 1 iff this vertex's batch is being smoothed (rigid /
 * vfp-matched / non-UI) this render. Alpha-independent. Drives the
 * OPENPETE_DEBUG_HIGHLIGHT_SMOOTHED solid-red overlay. */
int openpete_render_matcher_is_smoothed(const int16_t* p);

/* §12.2 overlay v2 — per-vertex interpolation-state classifier for the
 * F9 highlight. Every snap/fallback cause gets its own color so a
 * visual artifact names its mechanism on sight:
 *   TAG_ABSENT   red      tagged batch, no counterpart in "to"
 *                         (despawned moby / strip culled out) → frozen
 *   TAG_INSANE   yellow   counterpart rejected by tagged_pair_sane
 *                         (world count-shift / moby teleport-shape) → frozen
 *   CORR_FROZEN  orange   whole-batch corr-gate trip (partial strip /
 *                         mesh correspondence). Since the rigid
 *                         fallback landed this means RIGID-FOLLOW
 *                         (whole span rides the mean corresponded
 *                         delta), not a freeze — unless the mean had
 *                         too few hits (RIGID_MIN_HITS) or
 *                         OPENPETE_RIGID_FALLBACK=0, where it still
 *                         freezes at the lagged pose.
 *   HEUR_PAIRED  magenta  RETIRED (§12.1 first slice): the centroid-
 *                         hash fallback was deleted — ordinal-fail now
 *                         freezes. Any magenta on screen is a bug.
 *   LEGACY_WORLD blue     obj_tag==0 world paired by emission ordinal
 *                         (§7.3 composite-reproject path)
 *   VERT_MISS    cyan     batch paired but THIS vertex has no tomap
 *                         counterpart (evicted / culled across the
 *                         endpoints) → vertex rides the batch's mean
 *                         corresponded delta (rigid fill); frozen at
 *                         base only if the mean is unavailable
 * NONE (0) = healthy: interpolating or static. The overlay paints
 * moving-and-healthy green from the applied-delta hook instead. */
enum {
    OPENPETE_DBGST_NONE = 0,
    OPENPETE_DBGST_TAG_ABSENT,
    OPENPETE_DBGST_TAG_INSANE,
    OPENPETE_DBGST_CORR_FROZEN,
    OPENPETE_DBGST_HEUR_PAIRED,
    OPENPETE_DBGST_LEGACY_WORLD,
    OPENPETE_DBGST_VERT_MISS,
    /* §13.14: tag-absent WORLD batch (true one-frame cull-boundary
     * transient) riding a borrowed rigid mean from the nearest
     * same-renderer paired batch instead of freezing at pan speed.
     * WHITE in the F9 overlay. */
    OPENPETE_DBGST_ABS_FOLLOW
};
int openpete_render_matcher_debug_state(const int16_t* p);
/* 0/1. Seeded from CLI/env in run.c, live-toggled by F9. */
extern int g_openpete_dbg_highlight_smoothed;
/* 0/1. Black-out the window on intermediate presents (presentation
 * isolation test). Seeded CLI/env in run.c, live-toggled by F10. */
extern int g_openpete_dbg_black_present;
/* 0/1. Master interpolation enable; live-toggled by 'I'. 0 = lagged
 * frame presented unchanged on intermediates (clean on/off A/B). */
extern int g_openpete_interp_enabled;

/* Phase 2 stats. */
extern unsigned g_openpete_render_substitute_pair_count;
extern unsigned g_openpete_render_substitute_vert_hit_count;
extern unsigned g_openpete_render_substitute_vert_miss_count;
extern unsigned g_openpete_render_substitute_ui_optout_count;

/* Per-frame stats (reset inside finalize; read by PsyX_EndScene's
 * summary line before reset). */
extern unsigned g_openpete_render_matcher_batch_count;
extern unsigned g_openpete_render_matcher_matched_count;
extern unsigned g_openpete_render_matcher_unmatched_new_count;
extern unsigned g_openpete_render_matcher_unmatched_dropped_count;
extern unsigned g_openpete_render_matcher_no_cache_count;
/* Phase 2: number of curr batches that successfully paired with a
 * FRAME-N future-anchor batch (i.e., ready to lerp). */
extern unsigned g_openpete_render_matcher_future_anchored_count;

/* ----------------------------------------------------------------------
 * Phase 4 — OPENPETE_PREV_TRANSFORM escape hatch.
 *
 * Modeled on RT64's gEXPreviousMatrix extension
 * (https://github.com/rt64/rt64/issues/93). For the small number of
 * call sites where the heuristic matcher pairs the wrong objects across
 * frames (mesh teleports, infinite scrollers, level transitions),
 * game-side code can override the matcher's inferred "previous
 * transform" with an explicit one.
 *
 * Usage: just before the game's GTE pass for a specific object emits
 * its addPrim, call:
 *
 *   openpete_render_matcher_set_prev_transform(curr_rt, curr_tr,
 *                                              prev_rt, prev_tr);
 *
 * `curr_rt` / `curr_tr` identify the batch (the matcher looks up the
 * batch whose first vertex has matching RT/TR). `prev_rt` / `prev_tr`
 * declare the matrix the batch had one game frame ago. The matcher
 * uses these to compute screen-space velocity instead of inferring it
 * via hash matching against matcher.prev.
 *
 * One-shot: the override is consumed by the next prepare_substitute
 * call. If multiple batches share the same `curr_rt`/`curr_tr`, only
 * the first is patched (use distinct matrices per object).
 *
 * No call sites currently exist; this is forward-looking infrastructure
 * for the matcher's eventual failure modes. */
void openpete_render_matcher_set_prev_transform(
    const int16_t curr_rt[3][3], const int32_t curr_tr[3],
    const int16_t prev_rt[3][3], const int32_t prev_tr[3]);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RENDER_MATCHER_H */
