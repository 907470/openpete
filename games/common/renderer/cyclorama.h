/* cyclorama.h - the shared sky-dome feeder's exports to the orchestrator and
 * the transition feeder. The feeder (games/common/renderer/cyclorama.c) reads
 * every address from game_layout.cyc, .cam, .fog and .db. */
#ifndef OPENPETE_COMMON_RENDERER_CYCLORAMA_H
#define OPENPETE_COMMON_RENDERER_CYCLORAMA_H

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/** @brief Rebuild the sky dome and the scene background for a sub-tick present without running the game's sky draw.
 *
 * Does nothing when the native scene is off, when the canonical tick did not
 * draw the cyclorama path, or before a canonical draw has been captured. When
 * the canonical call used the camera's own view and projection matrices they
 * are read live, since the camera bracket interpolates them in place. Else
 * (the level transition builds both matrices on its stack) the captured
 * prev/curr matrix pair is lerped componentwise by `alpha`, rounded to
 * nearest; it snaps to curr when `interp_on` is 0, when prev is missing or
 * came from the camera's own matrices, or when any component moved more than
 * 1200 Q12 units in the tick. The triangles and the background colour are
 * pushed to the native scene.
 * @param cpu The guest CPU state, read only.
 * @param alpha The present's sub-tick fraction between prev and curr.
 * @param interp_on Nonzero when the camera is bracketed this present. */
void cyclorama_reextract(struct CPUState* cpu, double alpha, int interp_on);

/* One extracted cyclorama dome triangle: screen space (float), per-vertex PSX
 * 0xBBGGRR colour. */
typedef struct { float sx[3], sy[3]; unsigned int rgb[3]; } cyclorama_native_tri;

/** @brief Re-derive the DPCS-faded dome the cross-fade renderer draws at a level exit and at the balloonist.
 *
 * The cross-fade renderer (Spyro 1 func_8004F000) draws the same cyclorama
 * dome as the sky draw, with every sector colour first depth-cued toward the
 * fog far colour by the fog factor, and with the camera's own view and
 * projection matrices as the cull and draw matrices. It culls faces against
 * the 4:3 outcode box [0,512]x[0,256], so its prims stop at the 4:3 edges;
 * this extraction walks every sector through the Hor+ bounds, the near-plane
 * clip and the trivial reject the main sky uses, so the revealed sides of a
 * wide view get dome instead of the background clear.
 * @param cpu The guest CPU state, read only.
 * @param out Receives the triangles in screen space.
 * @param max Capacity of `out`.
 * @return The number of triangles written, at most `max`. */
int cyclorama_native_extract_faded(struct CPUState* cpu, cyclorama_native_tri* out, int max);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_CYCLORAMA_H */
