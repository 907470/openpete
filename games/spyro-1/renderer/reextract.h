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
/* player_native_set_*: defined in games/common/renderer/player.c, documented in games/common/renderer/player.h. */
void player_native_set_anim_interp(double alpha, int on);
void player_native_set_anim_pair(const uint8_t* prev_seg, const uint8_t* curr_seg);
void player_native_set_player_slot(int k);
void player_native_set_flat_key(uint32_t key);

void player_native_set_rot_interp(const int* active, const double* ang);

/** @brief Sub-tick rotation for the dragon-rescue star (D_80076248).
 *
 * The rotation is the record's +0x28 euler bytes lerped at float precision,
 * {Z, X, Y} byte angles, which the star feeder composes through the resolved camera
 * matrix in place of the record's bytes. NULL resets, so a canonical present
 * composes from the bytes. Defined in renderer/particle.c.
 * @param ang_zxy three byte angles {Z, X, Y}, or NULL. */
void dragonstar_native_set_rot_interp(const double* ang_zxy);

/** @brief Portal previews of a sub-tick present.
 *
 * Replays each leaf capture of the last canonical RenderPortals through the
 * canonical feeders (feed_portal_quad, feed_portal_dome). The dome rotation
 * matrix is captured prev+curr per canonical tick and lerped by the spine's
 * sub-tick `alpha` (componentwise, like the camera matrix), snapping to curr
 * when a component moved more than 1200 in the tick; a dome the canonical feed
 * drew with the derived matrix re-derives it instead. Each capture's clip rect
 * is rebuilt under the present's camera by its rect rule. The clip rect
 * (D_8007AA00) goes through the sub-tick view stage or is restored, and the
 * GTE rotation registers are restored. Draws nothing when the native scene is
 * off, nothing was captured, or RP_PORTAL was not drawn on the canonical
 * present (RenderPortals stops during a level load and leaves a stale capture).
 * @param cpu       guest state, the sub-tick view staged
 * @param alpha     sub-tick fraction between the previous and current tick
 * @param interp_on 1 = camera bracketed, else snap to curr */
void portal_reextract(struct CPUState* cpu, double alpha, int interp_on);
/* terrain_reextract, terrain_native_topdown_pose, terrain_native_sweep_pose:
 * renderer/terrain.h (the shared feeder). */
void shadow_reextract(struct CPUState* cpu);
void shadow_spyro_reextract(struct CPUState* cpu, int slot);
void player_reextract(struct CPUState* cpu);
void particle_reextract(struct CPUState* cpu);
void tracer_reextract(struct CPUState* cpu);
/* Level-transition / balloonist cross-fade backdrop: replays the captured
 * screen-space gouraud mesh (gated on whether func_8004F000 ran this canonical
 * present) so the gradient holds across sub-tick presents instead of strobing. */
void transition_reextract(struct CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_RENDERER_REEXTRACT_H */
