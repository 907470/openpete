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

/* Per-frame stats (reset inside finalize; read by PsyX_EndScene's
 * summary line before reset). */
extern unsigned g_openpete_render_matcher_batch_count;
extern unsigned g_openpete_render_matcher_matched_count;
extern unsigned g_openpete_render_matcher_unmatched_new_count;
extern unsigned g_openpete_render_matcher_unmatched_dropped_count;
extern unsigned g_openpete_render_matcher_no_cache_count;

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RENDER_MATCHER_H */
