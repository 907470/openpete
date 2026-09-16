/* cyclorama.h — the shared sky-dome feeder's exports to the orchestrator and
 * the transition feeder. The feeder (games/common/renderer/cyclorama.c) reads
 * every address from game_layout.cyc, .cam, .fog and .db. */
#ifndef OPENPETE_COMMON_RENDERER_CYCLORAMA_H
#define OPENPETE_COMMON_RENDERER_CYCLORAMA_H

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Sky dome + background: live camera matrices when the canonical call used
 * them, else the stack-built (transition) matrix pair lerped prev->curr by
 * `alpha`; `interp_on` = camera bracketed (else snap to curr). */
void cyclorama_reextract(struct CPUState* cpu, double alpha, int interp_on);

/* One extracted cyclorama dome triangle: screen space (float), per-vertex PSX
 * 0xBBGGRR colour. */
typedef struct { float sx[3], sy[3]; unsigned int rgb[3]; } cyclorama_native_tri;

/* Re-derive the DPCS-faded dome the cross-fade renderer draws (level exit and
 * balloonist), from the cyclorama header and the camera's own matrices. That
 * renderer culls faces against the 4:3 outcode box, so its prims stop at the
 * 4:3 edges; this extraction uses the active (Hor+ widened) bounds, so the
 * revealed sides of a wide frame get real dome instead of background clear.
 * Fills up to `max` triangles and returns the count. */
int cyclorama_native_extract_faded(struct CPUState* cpu, cyclorama_native_tri* out, int max);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_CYCLORAMA_H */
