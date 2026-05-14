/* openpete_render_cache.h — shared world-space cache for the renderer
 * foundation. See notes/plans/openpete-renderer-foundation.md.
 *
 * Producer: psxrecomp's gte_rtps_internal writes one entry per projected
 *   vertex, immediately after push_sxy(). Key is the packed int16-saturated
 *   SXY that the game will subsequently mfc2 into prim packets.
 *
 * Consumer: PsyCross's OT walker (ParsePrimitive → MakeVertexTriangle /
 *   MakeVertexQuad) looks up per-vertex by SXY hash to recover world-space
 *   position, model-local vertex, and the RT/TR/OFX/OFY/H projection state.
 *
 * Determinism: renderer-side state only. Never reads back into PSX RAM.
 * Cannot perturb gameplay-readable state by construction.
 *
 * Phase 0 goal: pgxp_hit ≈ 1,945/1,945 prims per frame (and ≈ 5,400/5,400
 * verts per frame) on level_10 active gameplay, no behavior change.
 */

#ifndef OPENPETE_RENDER_CACHE_H
#define OPENPETE_RENDER_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Schema per renderer-foundation.md "Cache schema". sxy_key is packed as
 * ((u16)(int16)sy << 16) | (u16)(int16)sx — matches both the GTE SXY[]
 * register layout AND the (x,y) shorts the game writes into prim packets. */
typedef struct openpete_render_cache_entry {
    uint32_t sxy_key;
    uint32_t seq;            /* matches frame->seq when entry is current */
    uint32_t matrix_span_id; /* bumped on RT/TR change */
    int32_t  mac[3];         /* pre-projection world-space (q12) */
    int16_t  v_local[3];     /* model-local vertex pre-transform */
    int16_t  _pad0;
    int16_t  rt[3][3];       /* rotation matrix snapshot */
    int32_t  tr[3];          /* translation vector snapshot */
    int32_t  ofx;            /* canonical projection state (pre-Hor+) */
    int32_t  ofy;
    uint16_t h;
    uint16_t _pad1;
} openpete_render_cache_entry_t;

/* 8192 slots × ~64 B ≈ 512 KB per frame × 2 frames = 1 MB. Comfortably
 * fits in L2 on any host CPU. Load factor at 1945 verts ≈ 24 %. */
#define OPENPETE_RENDER_CACHE_SLOTS     8192u
#define OPENPETE_RENDER_CACHE_PROBE_MAX 16u

/* Producer (called from gte_rtps_internal). Fills sxy_key/seq/matrix_span_id
 * from current state; caller fills the rest (mac/v_local/rt/tr/ofx/ofy/h).
 * Bumps matrix_span_id internally when RT/TR observed-change since prior
 * emit. */
void openpete_render_cache_emit_vertex(const openpete_render_cache_entry_t* src);

/* Consumer (called from PsyCross OT walker per vertex). Returns 1 on
 * hit (out filled), 0 on miss. */
int openpete_render_cache_lookup_curr(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out);
int openpete_render_cache_lookup_prev(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out);

/* Rotate: previous <- current; bump current's seq (logical clear). Called
 * from PsyX_BeginScene per render frame. */
void openpete_render_cache_begin_frame(void);

/* Iterate every populated slot in s_curr (the "next-visible" buffer just
 * emitted by the game fiber's GTE pass). Used by Phase 2's
 * future-anchor scan to build FRAME-N batches from the in-flight cache
 * before the rotate moves it to s_prev. The callback is called once
 * per valid entry; iteration order is implementation-defined. */
typedef void (*openpete_render_cache_iter_fn)(
    const openpete_render_cache_entry_t* entry, void* userdata);

void openpete_render_cache_for_each_curr_entry(
    openpete_render_cache_iter_fn cb, void* userdata);

/* Iterate every populated slot in s_prev (the "currently visible"
 * buffer; consumer of OT walker lookups). Used by Phase 2's dual-scan
 * to build FRAME-N-1 anchors alongside FRAME-N future anchors. */
void openpete_render_cache_for_each_prev_entry(
    openpete_render_cache_iter_fn cb, void* userdata);

/* Diagnostic counters (per render frame; reset by PsyX_EndScene summary). */
extern unsigned g_openpete_render_cache_emit_count;
extern unsigned g_openpete_render_cache_lookup_count;
extern unsigned g_openpete_render_cache_hit_count;
extern unsigned g_openpete_render_cache_probe_overflow_count;
extern unsigned g_openpete_render_cache_matrix_span_id;

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RENDER_CACHE_H */
