// core/features/widescreen.h — FOV slider + polybuf relocation.
//
// 2026-07-06: the PsyCross-mode --aspect substitution stack (projection
// widening, cull-widening knobs, render bracket, cyclorama multi-pass)
// was RETIRED — see the widescreen.c header note and Phase 6 of
// notes/plans/openpete-native-renderer.md. Widescreen is a native-
// renderer display policy now (window-derived; core/runtime/
// native_render.c). --aspect only sizes the window.
//
// See notes/plans/openpete-widescreen-fov.md for the feature's full
// design + history.

#ifndef OPENPETE_FEATURES_WIDESCREEN_H
#define OPENPETE_FEATURES_WIDESCREEN_H

#include "openpete.h"   /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

/* --fov: derive openpete_fov_h_override (GTE H override) from
 * opts->fov_deg. Called from run.c before PsyX_Initialise, PsyCross
 * renderer mode only (the native renderer projects canonically; its
 * FOV policy is Phase-7 work). */
void widescreen_apply_fov(const openpete_options_t* opts);

/* Live FOV override in degrees (0 = clear back to the game default). The GTE
 * consumer re-reads the override per RTPS. PsyCross reference mode only. */
void widescreen_set_fov(float fov_deg);

#ifdef __cplusplus
}
#endif

#endif
