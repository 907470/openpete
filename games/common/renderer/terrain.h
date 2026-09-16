/* terrain.h — the shared terrain feeder's exports to a game's orchestrator.
 * The feeder itself (renderer/terrain.c) registers as the feature module
 * `terrain_module`; these entries are called by the sub-tick re-extract
 * orchestrator and the debug-camera pose helpers. */
#ifndef OPENPETE_COMMON_RENDERER_TERRAIN_H
#define OPENPETE_COMMON_RENDERER_TERRAIN_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;

/* Sub-tick re-extract: rebuild the terrain scene from the current gamestate
 * and the camera the orchestrator installed; `alpha` lerps the vertex banks
 * of poly-anim sectors, `interp_on` = the camera is bracketed (else snap). */
void terrain_reextract(struct CPUState* cpu, double alpha, int interp_on);

/* A top-down camera pose (x, y, z in world units, yaw, pitch) that frames the
 * whole level from above; returns 0 when no sector table is loaded. */
int terrain_native_topdown_pose(struct CPUState* cpu, int32_t v[5]);

/* Waypoint `corner` (0..3) of a low-altitude circuit of the level. */
int terrain_native_sweep_pose(struct CPUState* cpu, int corner, int32_t v[5]);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_TERRAIN_H */
