// core/features/no_lod.h — `--no-lod` feature module.
//
// See notes/plans/openpete-disable-lods.md for the design + history.
// 2026-07-06: the game-side producer stack was deleted (Phase 6 of
// openpete-native-renderer.md); --no-lod is a native-renderer display
// policy owned by the native feeders.
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
 * builder in moby_native.c, terrain HQ classifier, portal always-dome,
 * fog default). See notes/plans/openpete-disable-lods.md. */
extern int openpete_no_lod;

/* --fog-scale: terrain fog-distance multiplier, resolved at boot in
 * no_lod_apply_options (explicit --fog-scale wins; else 4.0 under
 * --no-lod, 1.0 otherwise). 1.0 = the authentic PSX curve; N scales
 * every channel of the fog model together (DPCS vertex curve, CLUT-row
 * palette ladder, near-tile select, fog-saturation LP handoff — see
 * games/spyro-1/features/terrain_native.c). Always > 0 after boot. */
extern float openpete_fog_scale;

/* Boot-time entry: resolve the fog scale and set openpete_no_lod for
 * native renderer modes (warn + ignore in PsyCross reference mode).
 * Returns 0 on success. Called from core/runtime/run.c. */
int no_lod_apply_options(const openpete_options_t* opts);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FEATURES_NO_LOD_H */
