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
 * games/spyro-1/renderer/terrain.c). Always > 0 after boot. */
extern float openpete_fog_scale;

/* Boot-time entry: resolve the fog scale and set openpete_no_lod for
 * native renderer modes (warn + ignore in PsyCross reference mode).
 * Returns 0 on success. Called from core/runtime/run.c. */
int no_lod_apply_options(const openpete_options_t* opts);

/* Live toggle (in-game settings menu): set the draw-distance display policy at
 * runtime. `on` != 0 = extended (full detail). `fog_scale` > 0 overrides the fog
 * multiplier; <= 0 uses the mode default (100.0 extended / 1.0 original, matching
 * no_lod_apply_options) — but only while fog is on autopilot: an explicit
 * --fog-scale or a prior openpete_fog_set keeps winning. The native feeders read
 * openpete_no_lod / openpete_fog_scale per frame, so it takes effect on the next
 * frame. Native-renderer display policy only (no gameplay effect). */
void openpete_no_lod_set(int on, float fog_scale);

/* 1 while fog is effective (scale below the ~100x "effectively off"
 * sentinel). The menu's FOG row reads its display state from this. */
int openpete_fog_enabled(void);

/* Live fog toggle (in-game settings menu FOG row): on = the user's explicit
 * --fog-scale if one was given (and is itself below the off-sentinel), else
 * the authentic PSX curve (1.0); off = effectively no fog (100.0). Marks fog
 * as user-set, so later LOD toggles leave the fog scale alone. */
void openpete_fog_set(int on);

/* Set the fog-distance multiplier directly (clamped to [0.25, off-sentinel]).
 * Marks fog as manually touched, so LOD switches stop moving it. */
void openpete_fog_scale_set(float scale);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FEATURES_NO_LOD_H */
