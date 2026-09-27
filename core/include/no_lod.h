// core/include/no_lod.h — `--no-lod` feature module.
//
// --no-lod is a native-renderer display policy owned by the native feeders;
// there is no game-side producer.
//
// `openpete_no_lod` is also re-declared in
// third_party/psxrecomp/runner/include/psx_runtime.h so recompiler-
// facing code can read it without dragging in this header.

#ifndef OPENPETE_FEATURES_NO_LOD_H
#define OPENPETE_FEATURES_NO_LOD_H

#include "openpete.h"   /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

/* --no-lod FEATURE flag: full detail at any distance. Set once at boot
 * from --no-lod, native renderer modes only. The native feeders read it
 * as a DISPLAY policy (moby zsf4/tier neutralization + far-record
 * builder in moby.c, terrain HQ classifier, portal always-dome,
 * fog default). */
extern int openpete_no_lod;

/* --fog-scale: terrain fog-distance multiplier, resolved at boot in
 * no_lod_apply_options (explicit --fog-scale wins; else 100.0 = effectively
 * off under extended LOD, 1.0 otherwise). 1.0 = the authentic PSX curve; N scales
 * every channel of the fog model together (DPCS vertex curve, CLUT-row
 * palette ladder, near-tile select, fog-saturation LP handoff — see
 * games/common/renderer/terrain.c). Always > 0 after boot. */
extern float openpete_fog_scale;

/** @brief Resolve the boot fog scale and draw-distance policy from the options.
 *
 * Called once at boot from core/runtime/run.c. The fog scale is resolved
 * before the renderer check: an explicit --fog-scale wins, else extended
 * LOD takes 100.0 (effectively off) and original LOD 1.0; the explicit
 * value is remembered for later live toggles. openpete_no_lod is set to 1
 * only for extended LOD under a native renderer; the PsyCross reference
 * renderer logs a warning and keeps canonical detail.
 * @param opts Parsed options; NULL leaves the fog scale and clears the flag.
 * @return 0 in every case. */
int no_lod_apply_options(const openpete_options_t* opts);

/** @brief Set the draw-distance display policy at runtime.
 *
 * Called by the settings menu's LOD row, the knob registry and the run
 * loop's LOD hotkey. A @p fog_scale above 0 sets the fog multiplier. A
 * value of 0 or below applies the mode default only while fog is on
 * autopilot: an explicit --fog-scale is restored when one was given, else
 * 100.0 for extended and 1.0 for original; after openpete_fog_set or
 * openpete_fog_scale_set the fog scale is left alone. The native feeders
 * read openpete_no_lod and openpete_fog_scale on every walk, so the change
 * applies from the next one. Display policy only, no gameplay effect.
 * @param on Nonzero for extended (full detail), 0 for original.
 * @param fog_scale Fog multiplier, or 0 or below for the mode default. */
void openpete_no_lod_set(int on, float fog_scale);

/** @brief Report whether fog is in effect.
 *
 * The settings menu's FOG row and the knob registry read their state from
 * it.
 * @return 1 while the fog scale is below the 100.0 off sentinel, else 0. */
int openpete_fog_enabled(void);

/** @brief Turn fog on or off from the settings menu's FOG row or the knob registry.
 *
 * On restores the explicit --fog-scale when one was given below the 100.0
 * off sentinel, else the authentic PSX curve (1.0); off sets 100.0
 * (effectively no fog). Takes fog off autopilot, so later LOD toggles leave
 * the fog scale alone.
 * @param on Nonzero for fog on. */
void openpete_fog_set(int on);

/** @brief Set the fog-distance multiplier directly.
 *
 * The knob registry's fog-scale row calls it. Takes fog off autopilot, so
 * LOD switches stop moving it. Terrain re-resolves the scale every walk, so
 * the value applies from the next one.
 * @param scale Multiplier, clamped to [0.25, 100.0]; NaN is ignored. */
void openpete_fog_scale_set(float scale);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FEATURES_NO_LOD_H */
