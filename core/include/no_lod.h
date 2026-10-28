// core/include/no_lod.h - `--no-lod` feature module.
//
// --no-lod is a native-renderer display policy owned by the native feeders;
// there is no game-side producer.

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
 * no_lod_apply_options (explicit --fog-scale wins; else 1.0 under either
 * LOD mode). 1.0 = the authentic PSX curve; N scales
 * every channel of the fog model together (DPCS vertex curve, CLUT-row
 * palette ladder, near-tile select, fog-saturation LP handoff - see
 * games/common/renderer/terrain.c). Always > 0 after boot. */
extern float openpete_fog_scale;

/** @brief Resolve the boot fog scale and draw-distance policy from the options.
 *
 * Called once at boot from core/runtime/run.c. The fog scale is resolved
 * before the renderer check: an explicit --fog-scale wins, else either LOD
 * mode takes 1.0, the authentic curve; the explicit
 * value is remembered for later live toggles. The fog form takes --fog's
 * pin, or follows the LOD mode when it is unset. openpete_no_lod is set to 1
 * only for extended LOD under a native renderer; the PsyCross reference
 * renderer logs a warning and keeps canonical detail.
 * @param opts Parsed options; NULL leaves the fog scale and clears the flag.
 * @return 0 in every case. */
int no_lod_apply_options(const openpete_options_t* opts);

/** @brief Set the draw-distance display policy at runtime.
 *
 * Called by the settings menu's LOD row, the knob registry and the run
 * loop's LOD hotkey. A @p fog_scale above 0 sets the fog multiplier. A
 * value of 0 or below applies the mode default only while the scale is on
 * autopilot: an explicit --fog-scale is restored when one was given, else
 * 1.0 in either mode; after openpete_fog_scale_set the fog scale is left
 * alone. An unpinned fog form (openpete_fog_mode) follows the new mode. The native feeders
 * read openpete_no_lod and openpete_fog_scale on every walk, so the change
 * applies from the next one. Display policy only, no gameplay effect.
 * @param on Nonzero for extended (full detail), 0 for original.
 * @param fog_scale Fog multiplier, or 0 or below for the mode default. */
void openpete_no_lod_set(int on, float fog_scale);

/** @brief The distance-fog form in effect: the pinned mode, else the one the LOD mode gives.
 *
 * Unpinned (OPENPETE_FOG_AUTO, the default) it follows openpete_no_lod on
 * every call: OPENPETE_FOG_ORIGINAL under original LOD, PER_PIXEL under
 * extended. The terrain walk latches it once per walk; the moby emit and the
 * portal previews read it as they draw. Display policy only.
 * @return OPENPETE_FOG_ORIGINAL, OPENPETE_FOG_PER_PIXEL or OPENPETE_FOG_OFF. */
int openpete_fog_mode(void);

/** @brief The pinned distance-fog form, or OPENPETE_FOG_AUTO while it follows the LOD mode.
 * @return The pinned openpete_fog_mode_t value, or OPENPETE_FOG_AUTO. */
int openpete_fog_mode_pinned(void);

/** @brief Pin the distance-fog form, or return it to following the LOD mode.
 *
 * Called by the knob registry's fog row and the settings menu. A pinned mode
 * survives later LOD switches; OPENPETE_FOG_AUTO unpins it. A value outside
 * the enum is ignored.
 * @param mode An openpete_fog_mode_t value, OPENPETE_FOG_AUTO included. */
void openpete_fog_mode_set(int mode);

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
