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

/** @brief Re-reads the heavy-fog verdict when the loaded level's sector table changes.
 *
 * The terrain walk and the moby passes call it before they read the verdict.
 * @param cpu The guest CPU state, read only. */
void terrain_heavy_fog_refresh(struct CPUState* cpu);

/** @brief Whether the loaded level is a heavy-fog level: its HP depth cue pulls vertex colours into one far colour.
 *
 * Set by terrain_heavy_fog_refresh from every sector's near and far colour
 * pairs at the level's first walk or moby pass: heavy when in at least 45%
 * of the pairs the far colour lies within 24 of the per-channel far median
 * on every channel and the near colour does not (S1 misty_bog and
 * beast_makers, S2 mystic_marsh and shady_oasis, whose far palette is one
 * colour, S3 harbor_speedway, S1 jacques, and S3 spider_town, bugbot_factory
 * and dino_mines area 2).
 * @return 1 for a heavy-fog level, 0 otherwise or before the first refresh. */
int terrain_heavy_fog(void);

/** @brief The heavy-fog level's fog colour: the per-channel median of its far colours, 0x00BBGGRR.
 *
 * Valid while terrain_heavy_fog() is 1; 0 before the first refresh.
 * @return The colour. */
uint32_t terrain_heavy_fog_colour(void);

/** @brief Blends a colour toward a far colour on the terrain's vertex depth-cue curve.
 *
 * The HP walk's per-corner fog (terrain_fog_blend): IR0 = fogRef - SZ with SZ
 * = m3 * the walk's fog scale (4 / --fog-scale) and fogRef the last walk's
 * reference (LOD distance >> 2), the GTE DPCS blend near at IR0 >= 4096, far
 * at IR0 <= 0. Returns nearC unchanged before the first terrain walk.
 * @param nearC The colour at zero fog, 0x00BBGGRR.
 * @param farC The fog target, 0x00BBGGRR.
 * @param m3 The view-Z in world/16.
 * @return The blended colour, 0x00BBGGRR. */
uint32_t terrain_fog_cue(uint32_t nearC, uint32_t farC, double m3);

/** @brief Packs the heavy-fog texel fade stepped at one ladder row, the original fog form's per-face step.
 *
 * The game steps its CLUT ladder per face; the step is t = row / 15,
 * pre-weighted into the affine aN = 1 - t, bN = 0.5 t on every channel
 * (word b bit 30, psx_hq_fog.glsl), rounded to the field's 1/256 units in
 * integers, so the stage draws texel * aN + bN at every pixel. terrain.vert
 * flat_atmo_words builds the same words.
 * @param row The ladder row, clamped to 0..15.
 * @param w Receives words a and b. */
void terrain_fog_step_words(int row, uint32_t w[2]);

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

/* ---- contact candidates for the sunk-pickup snap (moby_snap.c) ---------- */

/* A pulled moby's search box: its PSX screen rectangle and view-Z range
 * (full-world units), widened by the caller. */
typedef struct { float x0, y0, x1, y1, z0, z1; } terrain_snap_box;
/* One drawn opaque terrain triangle a search box reaches: its corners as
 * drawn (PSX screen sx, sy and full-world view-Z), whether its face is
 * double-sided, how many triangles its face drew (2 flat, 8 the 2x2
 * quadrants, 16 a split triangle, 32 the 4x4 cells), and the boxes whose
 * rectangle and depth range it meets (bit k of hit[k >> 6]). */
typedef struct {
    float    sx[3], sy[3], vz[3];
    uint8_t  dside, ftris;
    uint64_t hit[2];
} terrain_snap_tri;
/* What one capture cost: candidate faces, the triangles they drew, and the
 * sectors the retained cache owns that were walked for them. */
typedef struct { int faces, tris, sectors; } terrain_snap_census;

/** @brief Whether the last terrain build fed the scene now accumulating, so its triangles and walk inputs are the contact source of this present.
 * @return 1 when they are, else 0. */
int  terrain_snap_ready(void);
/** @brief Read the projection the last terrain build drew with.
 * @param H Receives the projection distance in PSX screen pixels.
 * @param ofx Receives the optical axis' screen X.
 * @param ofy Receives the optical axis' screen Y. */
void terrain_snap_view(float* H, float* ofx, float* ofy);
/** @brief Read the camera the last terrain build walked with, as the affine map from world to the snap's camera space.
 *
 * A world point w (full-world units: 16 times the walk's sector origin plus
 * vertex) lands at camera point c = L (w - P), 16 (m1, m2, m3) of the walk's
 * vertex transform: the full-world view-Z space terrain_snap_tri carries and
 * terrain_snap_view unprojects screen points into.
 * @param L Receives the linear part (the camera matrix over 4096, with the
 *        walk's axis order and signs).
 * @param P Receives the camera position, full-world units.
 * @return 1 when a build is latched, else 0 (L and P untouched). */
int  terrain_snap_camera(double L[3][3], double P[3]);
/** @brief Collect the drawn opaque terrain triangles that reach a set of search boxes.
 *
 * The walk's own run supplies the sectors it walked. A visible sector the
 * retained cache owns (drawn by the cache's vertex stage, which mirrors the
 * walk bit-exactly) is walked again by walk_sector into a private run, with
 * its faces masked to those whose corner rectangle and depth range meet a
 * box; nothing reaches the scene. Blended and hole-fill triangles are left
 * out: neither cuts a moby by depth.
 * @param b The boxes.
 * @param nb Box count, at most 128.
 * @param out Receives the triangles, valid until the next call.
 * @param cen Receives the cost counters; may be NULL.
 * @return The triangle count, or -1 when terrain_snap_ready does not hold. */
int  terrain_snap_capture(const terrain_snap_box* b, int nb, const terrain_snap_tri** out,
                          terrain_snap_census* cen);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_TERRAIN_H */
