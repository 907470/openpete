/* spyro_native_reextract.h — Spyro 1's sub-tick re-feed orchestrator.
 *
 * Step 2 of the native-renderer high-FPS plan
 * (notes/plans/openpete-native-renderer.md §"Movement"). On each
 * intermediate (sub-tick) present the core spine
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

#ifndef SPYRO_NATIVE_REEXTRACT_H
#define SPYRO_NATIVE_REEXTRACT_H

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Per-feeder re-extract entries (defined in each feeder TU). Extract-only:
 * they reproduce the feeder's scene contribution from current gamestate +
 * the (interpolated) g_Camera the orchestrator has written, without calling
 * the game render function or touching gameplay-readable RAM. */
/* Set the sub-tick alpha + whether keyframe-blend interpolation is active
 * (camera bracketed). Call before spyro_player_native_reextract so Spyro's
 * animation blends sub-tick instead of stepping at the canonical tick rate. */
void spyro_native_set_anim_interp(double alpha, int on);

/* Sub-tick moby ROTATION interpolation toggle. When on, the moby euler composer
 * (shaded_compose_euler) renders each moby's rotation at FLOAT precision —
 * shortest-arc interpolating the m_Rotation byte euler (256/circle) between the
 * moby's prev/curr interp-objs snapshots at its per-object alpha, with the
 * sin/cos sampled by linear interpolation of the in-RAM table — instead of the
 * byte-quantized angle. Removes the 1.4°/step stair-stepping on slowly-rotating
 * mobys (gems, idle dragons, HUD icons). Set by the orchestrator around the
 * feeder re-extract; reset (off) so the canonical inline render stays byte-exact.
 * Defined in features/moby_native.c. */
void moby_native_set_rot_interp(int on);

/* Flame sub-keyframe slide (0..1): the fractional part of the per-strand flame
 * counter advance for this sub-tick present. The flame feeder lerps each ribbon
 * cross-section between adjacent static spine records by this, so the ribbon
 * animates below the keyframe grid. Set by the orchestrator before
 * flame_native_reextract; reset to 0 after (the canonical render uses exact
 * keyframes). Defined in features/flame_native.c. */
void flame_native_set_subframe(double frac);

void cyclorama_native_reextract(struct CPUState* cpu);
void terrain_native_reextract(struct CPUState* cpu);
void moby_native_reextract(struct CPUState* cpu);
void shadow_native_reextract(struct CPUState* cpu);
void spyro_player_native_reextract(struct CPUState* cpu);
void particle_native_reextract(struct CPUState* cpu);
void flame_native_reextract(struct CPUState* cpu);

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

/* Register the orchestrator with the core sub-tick spine. Call once at boot,
 * after the feeders are registered. */
void spyro_native_reextract_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_NATIVE_REEXTRACT_H */
