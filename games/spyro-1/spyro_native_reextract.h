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

#include <stdint.h>   /* uint32_t (canonical-render epoch API) */

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

/* Sub-tick ROTATION interpolation for Spyro's body/head/tail. The orchestrator
 * interpolates g_Spyro's packed byte euler (+0xC/+0x10/+0x14) at FLOAT precision
 * (shortest-arc per axis) between the prev/curr snapshots and hands the result
 * here, so spyro_compose_euler renders the rotation below the 8-bit euler grid
 * (256/circle = 1.4°/step) instead of stepping on it — the same fix the moby
 * composer got. `active[s]` (s=0 body / 1 head / 2 tail) gates each segment
 * (head/tail off when sticky / mid-transition → byte-exact path); `ang` is a
 * row-major 3×3 of float byte-angles [seg][Z,X,Y]. active=NULL resets (off), so
 * the canonical inline render stays byte-exact. Defined in features/spyro_native.c. */
void spyro_native_set_rot_interp(const int* active, const double* ang);

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

/* Occlusion-cull pan allowance: the camera's rotation this tick expressed as
 * screen-pixel motion (rad/tick × ~px-per-radian, overestimated). The moby
 * feeder inflates cached screen rects by it per tick of cache age, so a pan
 * cannot cull a moby whose stale rect has swept toward a wall edge. Set by
 * the orchestrator each re-extract from the prev/curr camera matrices.
 * Defined in features/moby_native.c. */
void moby_native_set_occl_pan(float px_per_tick);

/* The mobys the canonical .s render passes drew this tick, taken from their cull
 * records (REGULAR + SPECULAR, deduped). Most are also reachable by walking
 * g_LevelMobys, but not all: the balloonist relocates the balloon into the render
 * scratch for the arrival flight, and anything drawn from outside the array has
 * no prev/curr snapshot and no fractional position — it steps at the sim rate
 * while everything around it interpolates. Both the interp capture and the
 * sub-tick lerp walk consult this list so that class is covered generally.
 * Returns the entry count and points *out at the (borrowed) array.
 * Defined in features/moby_native.c. */
int moby_native_canon_drawn_mobys(const uint32_t** out);

/* Flame sub-keyframe slide (0..1): the fractional part of the per-strand flame
 * counter advance for this sub-tick present. The flame feeder lerps each ribbon
 * cross-section between adjacent static spine records by this, so the ribbon
 * animates below the keyframe grid. Set by the orchestrator before
 * flame_native_reextract; reset to 0 after (the canonical render uses exact
 * keyframes). Defined in features/flame_native.c. */
void flame_native_set_subframe(double frac);

/* Sub-tick FRACTIONAL interpolated world-position resolver (the lerp32 1-unit
 * floor fix). On each sub-tick present the orchestrator brackets every
 * interpolated object (camera / Spyro / each moby) by writing the lerped
 * position to int32 RAM — which ROUNDS to the 1-world-unit grid. Feeders that
 * project against those positions read the rounded RAM and inherit a uniform
 * 1-unit positional floor (sub-pixel, but it caps near-geometry continuity).
 * This returns the SAME interpolation at DOUBLE precision (un-rounded) for the
 * given `key` (G_CAMERA, G_SPYRO, or a moby vaddr): feeders use it for their
 * camera/object-relative deltas instead of re-reading the floored RAM, so the
 * projection tracks the smooth sub-tick path. Returns 1 + fills out[3] when a
 * bracket is active for `key` this present; 0 otherwise (feeder falls back to
 * reading RAM, which equals the canonical curr at the 30 Hz lockstep). Defined
 * in spyro_native_reextract.c. */
int openpete_native_interp_pos(unsigned int key, double out[3]);

void cyclorama_native_reextract(struct CPUState* cpu);
/* Present-time draw-env clear sync (called from render_paths_commit, once per
 * canonical present): re-reads the clear colour from g_CurDB->m_DrawEnv in PSX
 * RAM — the env DrawOTag will use for the scene being presented — instead of
 * the PutDrawEnv thunk latch, which trails the OT build by one VSync and
 * flashed the stale homeworld background for one present at the portal-entry
 * transition boundary. Defined in cyclorama_native.c. */
void cyclorama_native_env_clear_commit(void);
/* Portal previews: the dome rotation matrix is captured prev+curr per canonical
 * tick and lerped by the spine's sub-tick `alpha` (componentwise, like the camera
 * matrix); `interp_on` = camera bracketed (else snap to curr). */
void portal_native_reextract(struct CPUState* cpu, double alpha, int interp_on);
/* Terrain: mostly camera-driven (the bracket moves it for free), but poly-anim
 * sectors (env vert INTPL — the Sunny-Flight wall) lerp their decoded vert banks
 * prev→cur by `alpha`; `interp_on` = camera bracketed (else snap to curr). */
void terrain_native_reextract(struct CPUState* cpu, double alpha, int interp_on);
void moby_native_reextract(struct CPUState* cpu);
void shadow_native_reextract(struct CPUState* cpu);
void spyro_player_native_reextract(struct CPUState* cpu);
void particle_native_reextract(struct CPUState* cpu);
void flame_native_reextract(struct CPUState* cpu);
void tracer_native_reextract(struct CPUState* cpu);
/* Level-transition / balloonist cross-fade backdrop: replays the captured
 * screen-space gouraud mesh (gated on whether func_8004F000 ran this canonical
 * present) so the gradient holds across sub-tick presents instead of strobing. */
void transition_native_reextract(struct CPUState* cpu);

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
 * frame stamp checks never match at re-extract time — the W0.6 gate was dead.)
 *   _commit: latch the in-flight epoch as "fresh" + start the next (once per
 *            canonical present).
 *   _stamp:  the value a producer hook must store at capture time.
 *   _fresh:  whether a stored stamp belongs to the last canonical present.
 * Per-producer freshness within RP_MOBY (three canonical producers share that
 * path bit — REGULAR, SPECULAR, SHADED/text — and a load/pause screen runs
 * ONLY the SHADED/text one): */
void     spyro_native_canon_epoch_commit(void);
uint32_t spyro_native_canon_epoch_stamp(void);
int      spyro_native_canon_epoch_fresh(uint32_t stamp);
int      spyro_native_canon_mobyreg_fresh(void);  /* colour 0x1100 + cull records (REGULAR hook) */
int      spyro_native_canon_shaded_fresh(void);   /* shaded list 0x2400 (SHADED/text hook) */

/* Register the orchestrator with the core sub-tick spine. Call once at boot,
 * after the feeders are registered. */
void spyro_native_reextract_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_NATIVE_REEXTRACT_H */
