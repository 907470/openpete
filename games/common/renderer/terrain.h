/* terrain.h - the shared terrain feeder's exports to a game's orchestrator.
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

/** @brief Rebuilds the terrain scene for a sub-tick present from the current gamestate and the camera the orchestrator installed.
 *
 * Extract only: the world draw (func_800258F0) is not called and no verdict
 * is written; the walk reuses the canonical tick's occlusion group. Returns
 * without effect when the native scene or the terrain render path is off,
 * or when the canonical present did not draw terrain (during a level
 * transition the game stops drawing the world and the level data in RAM is
 * stale). Static terrain moves with the interpolated camera alone; poly-anim
 * sectors, whose vertices the game rewrites once per tick, lerp their
 * previous and current decoded banks at `alpha` so they move every present.
 * OPENPETE_TERRAIN_VANIM=0 and OPENPETE_TERRAIN_TANIM=0 snap the vertex and
 * UV-scroll animation to the current tick; OPENPETE_TERRAIN_CANIM=0 holds
 * the environment-animation colours at the previous tick.
 * @param cpu guest state, the sub-tick camera installed
 * @param alpha sub-tick fraction between the previous and current tick
 * @param interp_on 1 = the camera is bracketed; 0 snaps the vertex animation
 *        to the current tick */
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

/** @brief Arms the capture of the sector visibility table for the next world draw.
 *
 * On a game whose world draw computes the table in the scratchpad and
 * consumes it before returning (Spyro 3), the feeder's visibility hook
 * copies the bytes once the visibility function returns. Arming clears the
 * previous capture; on a game with no visibility hook it arms nothing and
 * the verdict stays in RAM at game_layout.visibility. Arm before calling the
 * world draw, then read terrain_visibility_capture after. */
void terrain_visibility_capture_arm(void);

/** @brief Run the canonical native terrain build and feed the world draw deferred for the portal capture.
 *
 * The deferral is terrain_game.env_feed_waits_for_portals; a call with none
 * pending is a no-op. The portal draw calls it once its capture for the tick is
 * complete. */
void terrain_native_flush_deferred(void);
/** @brief Run a deferred terrain build and feed whose portal draw never released it.
 *
 * The world draw's hook calls it before the next draw and the canonical
 * commit calls it before the scene presents, so a held feed still reaches
 * the scene of its VBL. A pending feed at either point is never expected and
 * is counted as late in the deferral census; a call with none pending is a
 * no-op. */
void terrain_native_flush_deferred_late(void);
/** @brief Returns the sector visibility table captured during the last armed world draw.
 * @param out receives the byte array (nonzero = the sector is visible); may
 *        be NULL
 * @return the table's length, or -1 when no capture was taken this draw, in
 *         which case the verdict is in RAM at game_layout.visibility */
int  terrain_visibility_capture(const uint8_t** out);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_TERRAIN_H */
