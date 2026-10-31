/* moby_snap.h - contact planes of pulled mobys against the drawn terrain
 * (the CPU half of the sunk-pickup depth snap, psx_moby_snap.glsl).
 *
 * The game inserts every face of a moby m_DepthOffset ordering-table slots
 * nearer than its average depth (r_moby.s / the moby draw's OT insert), so a
 * pickup set into the ground paints whole over that ground. The moby walk
 * hands each pulled moby's triangles here as it emits them; once the
 * terrain build that feeds the same scene has run, the triangles are tested
 * against the drawn opaque terrain they reach, and each moby's contact
 * planes go to the scene's snap table (openpete_native_snap_add). Either
 * feeder may run first in a scene: the second one resolves. */
#ifndef OPENPETE_COMMON_RENDERER_MOBY_SNAP_H
#define OPENPETE_COMMON_RENDERER_MOBY_SNAP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;

/** @brief Queue one pulled moby of the scene now accumulating for the contact test.
 *
 * Starts a new queue when the scene changed since the last call.
 * @param mid The moby identity (openpete_scene_tri.mid), nonzero.
 * @param mordinal The moby's present-local ordinal, 0 when no face is ranked.
 * @param dofs The authored depth offset (m_DepthOffset), > 0.
 * @param off_m Key depth less raster depth of its faces, world units.
 * @param v Its triangles' corners: PSX screen x, y and key view-Z
 *        (full-world), three corners per triangle.
 * @param n Triangle count. */
void moby_snap_queue(uint32_t mid, int mordinal, int dofs, float off_m,
                     const float (*v)[3][3], int n);
/** @brief Note one walk pass's ranked mobys, ordinal base + k + 1 for mids[k], for the ordinal-collision census.
 * @param mids The pass's ranked moby identities in ordinal order.
 * @param n Their count.
 * @param base The ordinals the scene's earlier passes used. */
void moby_snap_note_ranked(const uint32_t* mids, int n, int base);
/** @brief Mark the resolves that follow as a sub-tick re-extract's at a phase, or as canonical.
 *
 * A canonical resolve stores its planes for the snap carry; a re-extract's
 * resolve carries the planes of the endpoint the phase selects
 * (OPENPETE_MOBY_SNAP_CARRY) and runs the carry verify
 * (OPENPETE_MOBY_SNAP_CARRY_VERIFY).
 * @param rex Nonzero for a sub-tick re-extract.
 * @param alpha The present's phase, 0 at tick T-1 and 1 at tick T; read when rex is nonzero. */
void moby_snap_present(int rex, double alpha);
/** @brief Resolve the queued mobys when the terrain of this scene has been built (after a moby pass). */
void moby_snap_moby_fed(void);
/** @brief Resolve the queued mobys after the terrain build that fed this scene.
 * @param cpu The guest CPU state, unused; the terrain feeder's call site signature. */
void moby_snap_terrain_fed(struct CPUState* cpu);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_MOBY_SNAP_H */
