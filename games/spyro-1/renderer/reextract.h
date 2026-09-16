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
#include "renderer/terrain.h"        /* the shared terrain feeder's entries */
#include "renderer/cyclorama.h"      /* cyclorama_reextract, cyclorama_native_extract_faded */
#include "renderer/moby.h"          /* the shared moby feeder's entries */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Per-feeder re-extract entries (defined in each feeder TU). Extract-only:
 * they reproduce the feeder's scene contribution from current gamestate +
 * the (interpolated) g_Camera the orchestrator has written, without calling
 * the game render function or touching gameplay-readable RAM. */
/* Set the sub-tick alpha + whether keyframe-blend interpolation is active
 * (camera bracketed). Call before player_reextract so Spyro's
 * animation blends sub-tick instead of stepping at the canonical tick rate. */
void player_native_set_anim_interp(double alpha, int on);
/* Per record: the seg-row pair the two-pass shape lerp blends (prev = the
 * paired previous record's row, NULL when unpaired → no blend; curr = this
 * record's row) and the record index (the PLAYER tag slot). Defined in
 * renderer/spyro.c. */
void player_native_set_anim_pair(const uint8_t* prev_seg, const uint8_t* curr_seg);
void player_native_set_player_slot(int k);
/* The front-layer flatten cap is shared by the calls of one frame / present:
 * a new key opens a new group (renderer/spyro.c). */
void player_native_set_flat_key(uint32_t key);

/* Sub-tick ROTATION interpolation for Spyro's body/head/tail. The orchestrator
 * interpolates g_Spyro's packed byte euler (+0xC/+0x10/+0x14) at FLOAT precision
 * (shortest-arc per axis) between the prev/curr snapshots and hands the result
 * here, so spyro_compose_euler renders the rotation below the 8-bit euler grid
 * (256/circle = 1.4°/step) instead of stepping on it — the same fix the moby
 * composer got. `active[s]` (s=0 body / 1 head / 2 tail) gates each segment
 * (head/tail off when sticky / mid-transition → byte-exact path); `ang` is a
 * row-major 3×3 of float byte-angles [seg][Z,X,Y]. active=NULL resets (off), so
 * the canonical inline render stays byte-exact. Defined in renderer/spyro.c. */
void player_native_set_rot_interp(const int* active, const double* ang);

/* Flame sub-keyframe slide (0..1): the fractional part of the per-strand flame
 * counter advance for this sub-tick present. The flame feeder lerps each ribbon
 * cross-section between adjacent static spine records by this, so the ribbon
 * animates below the keyframe grid. Set by the orchestrator before
 * flame_reextract; reset to 0 after (the canonical render uses exact
 * keyframes). Defined in renderer/flame.c. */
void flame_native_set_subframe(double frac);

/* Portal previews: the dome rotation matrix is captured prev+curr per canonical
 * tick and lerped by the spine's sub-tick `alpha` (componentwise, like the camera
 * matrix); `interp_on` = camera bracketed (else snap to curr). */
void portal_reextract(struct CPUState* cpu, double alpha, int interp_on);
/* Terrain: mostly camera-driven (the bracket moves it for free), but poly-anim
 * sectors (env vert INTPL — the Sunny-Flight wall) lerp their decoded vert banks
 * prev→cur by `alpha`; `interp_on` = camera bracketed (else snap to curr). */
/* terrain_reextract, terrain_native_topdown_pose, terrain_native_sweep_pose:
 * renderer/terrain.h (the shared feeder). */
/* Compute a whole-level top-down freecam pose from the live sector table:
 * camera at the level bounds' centre, looking straight down (pitch at the
 * freecam clamp), high enough that every sector's bounding sphere projects
 * inside the visible frame. Fills v = {x, y, z, yaw, pitch} in world units.
 * Returns 0 when no level is live (empty sector table). Used by
 * `--freecam-pose topdown` (the per-level perf harness). */
/* Compute waypoint `corner` (0..3) of a low-altitude flight around the live
 * level: a rectangle inset inside the sector bounds, just above the highest
 * geometry, looking horizontally along the direction of travel. Where the
 * top-down pose reveals a whole level in one frame, flying this reveals it
 * progressively, which is what exercises sector-cache eviction and LOD
 * crossings. Fills v = {x, y, z, yaw, pitch} in world units; returns 0 when
 * no level is live. Used by `--freecam-path sweep0;sweep1;...`. */
void shadow_reextract(struct CPUState* cpu);
void shadow_spyro_reextract(struct CPUState* cpu, int slot);
void player_reextract(struct CPUState* cpu);
void particle_reextract(struct CPUState* cpu);
void flame_reextract(struct CPUState* cpu, int slot);
void tracer_reextract(struct CPUState* cpu);
/* Level-transition / balloonist cross-fade backdrop: replays the captured
 * screen-space gouraud mesh (gated on whether func_8004F000 ran this canonical
 * present) so the gradient holds across sub-tick presents instead of strobing. */
void transition_reextract(struct CPUState* cpu);

/* Spyro's projected SXY in the PSX scratchpad (the NCLIP verdict), captured
 * from the canonical player render hook. */
void spyro_native_canon_capture_sxy(void* cpu);

/* Register the orchestrator with the core sub-tick spine. Call once at boot,
 * after the feeders are registered. */
void game_reextract_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_RENDERER_REEXTRACT_H */
