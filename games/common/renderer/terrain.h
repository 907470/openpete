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

/** @brief A freecam pose that frames the whole live level from above.
 *
 * The first pass takes the vertex bounds of every sector over both LOD banks.
 * The camera sits over the bounds' centre at yaw 0 and pitch 992 (the freecam
 * drive clamp, about 3 degrees short of straight down); the second pass sets
 * its height to the lowest at which every vertex projects inside the frame at
 * the canonical H, plus 2%. The height is clamped to 30000 (world/16), under
 * the view-Z at which the game's 16-bit frustum stage rejects every sector, so
 * a level too wide for that height is cropped at the edges. Resolves the named
 * pose `topdown` of `--freecam-pose` and `--freecam-path`.
 * @param cpu guest state holding the live sector table
 * @param v   receives {x, y, z, yaw, pitch}, the position in world units
 * @return 1 on success, 0 when no sector table is loaded or it holds no vertex */
int terrain_native_topdown_pose(struct CPUState* cpu, int32_t v[5]);

/** @brief Waypoint `corner` of a low-altitude rectangular circuit of the live level.
 *
 * Takes the vertex bounds of terrain_native_topdown_pose's first pass. The
 * waypoints are the corners of a rectangle centred on the bounds with 30% of
 * their half-extents, in the order (-x,-y), (+x,-y), (+x,+y), (-x,+y); the
 * camera rides 64 (world/16) above the highest vertex at pitch 0, yawed toward
 * the next waypoint. `corner` is taken modulo 4. Flying the circuit reveals the
 * level progressively, which exercises sector-cache eviction and LOD
 * crossings. Resolves the named poses `sweepN` of `--freecam-pose` and
 * `--freecam-path`.
 * @param cpu    guest state holding the live sector table
 * @param corner the waypoint index, taken modulo 4
 * @param v      receives {x, y, z, yaw, pitch}, the position in world units
 * @return 1 on success, 0 when no sector table is loaded or it holds no vertex */
int terrain_native_sweep_pose(struct CPUState* cpu, int corner, int32_t v[5]);

/* The visibility table of a game whose world draw computes it in the
 * scratchpad and consumes it before returning (Spyro 3): the feeder's
 * visibility hook copies the bytes at that function's return, and this is the
 * copy. Arm it before calling the world draw, then read it after.
 *
 * terrain_visibility_capture stores the byte array at *out (nonzero = the
 * sector is visible) and returns its length, or -1 when no capture was taken
 * this draw, in which case the verdict is in RAM at game_layout.visibility. */
void terrain_visibility_capture_arm(void);

/** @brief Run the canonical native terrain build and feed the world draw deferred for the portal capture.
 *
 * The deferral is terrain_game.env_feed_waits_for_portals; a call with none
 * pending is a no-op. The portal draw calls it once its capture for the tick is
 * complete. */
void terrain_native_flush_deferred(void);
int  terrain_visibility_capture(const uint8_t** out);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_TERRAIN_H */
