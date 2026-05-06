// core/features/widescreen.h — Hor+ widescreen + FOV slider feature.
//
// See notes/plans/openpete-widescreen-fov.md for full design + history;
// notes/plans/openpete-feature-modules.md for the consolidation plan.
//
// Two entry points, called from core/runtime/run.c at fixed phase
// boundaries:
//
//   widescreen_apply_disp_w_factor(opts)
//     Sets openpete_disp_w_factor + openpete_fov_h_override from
//     --aspect / --fov. Called BEFORE PsyX_Initialise so the GTE/CPUState
//     boot sees the right values.
//
//   widescreen_apply_runtime_knobs(opts)
//     Populates the psx_runtime_* knobs that gate the recompiler's
//     address-keyed substitutions, plus PsyCross's
//     g_psyx_render_aspect_factor. Called AFTER PsyX_Initialise because
//     PsyCross's per-scene state needs the initialiser to have run.
//
// Phase 2.1a of the consolidation plan: just the apply_options math is
// here. Knob storage stays in core/runtime/dispatch.c (Phase 2.1c) and
// per-vaddr dispatch arms stay in games/spyro-1/game_extras_impl.c
// (Phase 2.1b).

#ifndef OPENPETE_FEATURES_WIDESCREEN_H
#define OPENPETE_FEATURES_WIDESCREEN_H

#include "openpete.h"   /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

void widescreen_apply_disp_w_factor(const openpete_options_t* opts);
void widescreen_apply_runtime_knobs(const openpete_options_t* opts);

#ifdef __cplusplus
}
#endif

#endif
