/* games/spyro-1/renderer/reextract.h — Spyro 1's sub-tick re-feed
 * orchestrator.
 *
 * On each intermediate (sub-tick) present the core spine
 * (openpete_native_register_reextract) calls our orchestrator, which:
 *   1. queries the interp ring for prev/curr camera + alpha,
 *   2. brackets g_Camera (save → write the interpolated camera → restore),
 *      entirely on the host fiber so the next sim tick reads canonical RAM,
 *   3. rebuilds the native scene by calling each feeder's extract-only
 *      re-extract entry (NO game render call, NO verdict writes).
 *
 * Feeders that read g_Camera directly (terrain, …) interpolate for free via
 * the bracket; feeders that bake the camera get converted to re-derive from
 * g_Camera incrementally. Determinism is by the restore discipline + the
 * oracle gate.
 */

#ifndef SPYRO_RENDERER_REEXTRACT_H
#define SPYRO_RENDERER_REEXTRACT_H

#include <stdint.h>   /* uint32_t (canonical-render epoch API) */

/* The interface the shared renderer TUs call into; Spyro 1 defines it here
 * (reextract.c) and in cyclorama.c. */
#include "renderer/orchestrator.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Per-feeder re-extract entries (defined in each feeder TU). Extract-only:
 * they reproduce the feeder's scene contribution from current gamestate +
 * the (interpolated) g_Camera the orchestrator has written, without calling
 * the game render function or touching gameplay-readable RAM. */
/* Set the sub-tick alpha + whether keyframe-blend interpolation is active
 * (camera bracketed). Call before spyro_player_reextract so Spyro's
 * animation blends sub-tick instead of stepping at the canonical tick rate. */
void spyro_native_set_anim_interp(double alpha, int on);
/* Per record: the seg-row pair the two-pass shape lerp blends (prev = the
 * paired previous record's row, NULL when unpaired → no blend; curr = this
 * record's row) and the record index (the PLAYER tag slot). Defined in
 * renderer/spyro.c. */
void spyro_native_set_anim_pair(const uint8_t* prev_seg, const uint8_t* curr_seg);
void spyro_native_set_player_slot(int k);
/* The front-layer flatten cap is shared by the calls of one frame / present:
 * a new key opens a new group (renderer/spyro.c). */
void spyro_native_set_flat_key(uint32_t key);

/* Sub-tick ROTATION interpolation for Spyro's body/head/tail. The orchestrator
 * interpolates g_Spyro's packed byte euler (+0xC/+0x10/+0x14) at FLOAT precision
 * (shortest-arc per axis) between the prev/curr snapshots and hands the result
 * here, so spyro_compose_euler renders the rotation below the 8-bit euler grid
 * (256/circle = 1.4°/step) instead of stepping on it — the same fix the moby
 * composer got. `active[s]` (s=0 body / 1 head / 2 tail) gates each segment
 * (head/tail off when sticky / mid-transition → byte-exact path); `ang` is a
 * row-major 3×3 of float byte-angles [seg][Z,X,Y]. active=NULL resets (off), so
 * the canonical inline render stays byte-exact. Defined in renderer/spyro.c. */
void spyro_native_set_rot_interp(const int* active, const double* ang);

/* Sub-tick moby ROTATION interpolation toggle. When on, the moby euler composer
 * (shaded_compose_euler) renders each moby's rotation at FLOAT precision —
 * shortest-arc interpolating the m_Rotation byte euler (256/circle) between the
 * moby's prev/curr interp-objs snapshots at its per-object alpha, with the
 * sin/cos sampled by linear interpolation of the in-RAM table — instead of the
 * byte-quantized angle. Removes the 1.4°/step stair-stepping on slowly-rotating
 * mobys (gems, idle dragons, HUD icons). Set by the orchestrator around the
 * feeder re-extract; reset (off) so the canonical inline render stays byte-exact.
 * Defined in renderer/moby.c. */
void moby_native_set_rot_interp(int on);

/* Occlusion-cull pan allowance: the camera's rotation this tick expressed as
 * screen-pixel motion (rad/tick × ~px-per-radian, overestimated). The moby
 * feeder inflates cached screen rects by it per tick of cache age, so a pan
 * cannot cull a moby whose stale rect has swept toward a wall edge. Set by
 * the orchestrator each re-extract from the prev/curr camera matrices.
 * Defined in renderer/moby.c. */
void moby_native_set_occl_pan(float px_per_tick);

/* The mobys the canonical .s render passes drew this tick, taken from their cull
 * records (REGULAR + SPECULAR, deduped). Most are also reachable by walking
 * g_LevelMobys, but not all: the balloonist relocates the balloon into the render
 * scratch for the arrival flight, and anything drawn from outside the array has
 * no prev/curr snapshot and no fractional position — it steps at the sim rate
 * while everything around it interpolates. Both the interp capture and the
 * sub-tick lerp walk consult this list so that class is covered generally.
 * Returns the entry count and points *out at the (borrowed) array.
 * Defined in renderer/moby.c. */
int moby_native_canon_drawn_mobys(const uint32_t** out);

/* Flame sub-keyframe slide (0..1): the fractional part of the per-strand flame
 * counter advance for this sub-tick present. The flame feeder lerps each ribbon
 * cross-section between adjacent static spine records by this, so the ribbon
 * animates below the keyframe grid. Set by the orchestrator before
 * flame_reextract; reset to 0 after (the canonical render uses exact
 * keyframes). Defined in renderer/flame.c. */
void flame_native_set_subframe(double frac);

/* Sky dome + background: live g_Camera matrices when the canonical call used
 * them, else the stack-built (transition) matrix pair lerped prev→curr by
 * `alpha`; `interp_on` = camera bracketed (else snap to curr). */
void cyclorama_reextract(struct CPUState* cpu, double alpha, int interp_on);
/* One extracted cyclorama dome triangle: screen space (float), per-vertex PSX
 * 0xBBGGRR colour. */
typedef struct { float sx[3], sy[3]; unsigned int rgb[3]; } cyclorama_native_tri;
/* Re-derive the DPCS-faded dome that the cross-fade renderer func_8004F000
 * draws (level exit + balloonist), from g_Cyclorama and g_Camera's matrices.
 * That renderer culls faces against the 4:3 outcode box, so its prims stop at
 * the 4:3 edges; this extraction uses the ACTIVE (Hor+ widened) bounds, so the
 * revealed sides of a wide frame get real dome instead of background clear.
 * Fills up to `max` triangles and returns the count. Defined in
 * cyclorama.c. */
int cyclorama_native_extract_faded(struct CPUState* cpu, cyclorama_native_tri* out, int max);
/* cyclorama_native_env_clear_commit (renderer/orchestrator.h) is defined in
 * cyclorama.c: it re-reads the clear colour from g_CurDB->m_DrawEnv in PSX RAM
 * rather than the PutDrawEnv thunk latch, which trails the OT build by one
 * VSync and flashed the stale homeworld background for one present at the
 * portal-entry transition boundary. */
/* Portal previews: the dome rotation matrix is captured prev+curr per canonical
 * tick and lerped by the spine's sub-tick `alpha` (componentwise, like the camera
 * matrix); `interp_on` = camera bracketed (else snap to curr). */
void portal_reextract(struct CPUState* cpu, double alpha, int interp_on);
/* Terrain: mostly camera-driven (the bracket moves it for free), but poly-anim
 * sectors (env vert INTPL — the Sunny-Flight wall) lerp their decoded vert banks
 * prev→cur by `alpha`; `interp_on` = camera bracketed (else snap to curr). */
void terrain_reextract(struct CPUState* cpu, double alpha, int interp_on);
/* Compute a whole-level top-down freecam pose from the live sector table:
 * camera at the level bounds' centre, looking straight down (pitch at the
 * freecam clamp), high enough that every sector's bounding sphere projects
 * inside the visible frame. Fills v = {x, y, z, yaw, pitch} in world units.
 * Returns 0 when no level is live (empty sector table). Used by
 * `--freecam-pose topdown` (the per-level perf harness). */
int terrain_native_topdown_pose(struct CPUState* cpu, int32_t v[5]);
/* Compute waypoint `corner` (0..3) of a low-altitude flight around the live
 * level: a rectangle inset inside the sector bounds, just above the highest
 * geometry, looking horizontally along the direction of travel. Where the
 * top-down pose reveals a whole level in one frame, flying this reveals it
 * progressively, which is what exercises sector-cache eviction and LOD
 * crossings. Fills v = {x, y, z, yaw, pitch} in world units; returns 0 when
 * no level is live. Used by `--freecam-path sweep0;sweep1;...`. */
int terrain_native_sweep_pose(struct CPUState* cpu, int corner, int32_t v[5]);
void moby_reextract(struct CPUState* cpu);
void shadow_reextract(struct CPUState* cpu);
void shadow_spyro_reextract(struct CPUState* cpu, int slot);
void spyro_player_reextract(struct CPUState* cpu);
void particle_reextract(struct CPUState* cpu);
void flame_reextract(struct CPUState* cpu, int slot);
void tracer_reextract(struct CPUState* cpu);
/* Level-transition / balloonist cross-fade backdrop: replays the captured
 * screen-space gouraud mesh (gated on whether func_8004F000 ran this canonical
 * present) so the gradient holds across sub-tick presents instead of strobing. */
void transition_reextract(struct CPUState* cpu);

/* Canonical-scratch capture, called from the canonical render hooks while the
 * transient .s render-pass scratch is still live (the sub-tick re-extract
 * restores it so the body feeders can re-derive geometry).
 *   _range: a g_SonyImage sub-range (off from g_SonyImage base, length).
 *   _sxy:   Spyro's projected SXY in the PSX scratchpad (the NCLIP verdict). */
void spyro_native_canon_capture_range(void* cpu, unsigned off, unsigned len);
void spyro_native_canon_capture_sxy(void* cpu);
/* _recs: the FULL regular cull-record region, captured at the moby hook before
 * the Spyro pass reuses the overlapping 0x1900 seg-scratch bytes (so records at
 * queue index >=14 aren't lost — see the .c for the temporal-reuse overlap). */
void spyro_native_canon_capture_recs(void* cpu);

/* Canonical-render epoch: bumped once per canonical present (from
 * render_paths_commit, the same anchor that latches the canon-drawn mask). A
 * capture is replayable on the sub-ticks after a canonical present iff it was
 * taken DURING that present — i.e. its stamp equals the last committed epoch.
 * (openpete_frame is a 60 Hz VBL counter, 2 per 30 Hz game tick, so frame==
 * frame stamp checks never match at re-extract time.)
 *   _commit: latch the in-flight epoch as "fresh" + start the next (once per
 *            canonical present); declared in renderer/orchestrator.h.
 *   _stamp:  the value a producer hook must store at capture time.
 *   _fresh:  whether a stored stamp belongs to the last canonical present.
 * Per-producer freshness within RP_MOBY (three canonical producers share that
 * path bit — REGULAR, SPECULAR, SHADED/text — and a load/pause screen runs
 * ONLY the SHADED/text one): */
uint32_t spyro_native_canon_epoch_stamp(void);
int      spyro_native_canon_epoch_fresh(uint32_t stamp);
int      spyro_native_canon_mobyreg_fresh(void);  /* colour 0x1100 + cull records (REGULAR hook) */
int      spyro_native_canon_shaded_fresh(void);   /* shaded list 0x2400 (SHADED/text hook) */

/* The camera a producer drew under. A feeder re-derives its geometry from
 * g_Camera, which the sub-tick bracket interpolates between tick-end
 * snapshots; a producer whose draw ran under matrices the tick did not end
 * with (the game-over screen rebuilds them from the skybox rotation after
 * drawing the letters and Spyro) must be re-derived from its own camera.
 *   _capture_camera: at the producer hook, record g_Camera's transform block.
 *   _prod_camera_enter/_leave: around that producer's feeder in the
 *   re-extract; enter installs the captured camera when the commit found it
 *   differs from the tick-end camera (returns 1), leave restores. */
enum { SPYRO_PROD_MOBYREG = 0, SPYRO_PROD_SPYRO = 1, SPYRO_PROD_SHADED = 2 };
void spyro_native_canon_capture_camera(void* cpu, int producer);
int  spyro_native_prod_camera_enter(int producer);
void spyro_native_prod_camera_leave(void);

/* Register the orchestrator with the core sub-tick spine. Call once at boot,
 * after the feeders are registered. */
void game_reextract_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_RENDERER_REEXTRACT_H */
