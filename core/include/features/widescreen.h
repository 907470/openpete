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

/* --- Polybuf relocation (RAM virtualization first consumer) --------------
 *
 * Spyro's double-buffered polygon buffer (0x1C000 per slot in matching
 * decomp) overflows at wide aspect ratios — the cone-cull admits more
 * cells than the buffer can hold. To unblock 21:9 / 32:9, allocate a
 * larger host-backed buffer via the RAM virtualization primitive and
 * post-hook AllocateBuffers (the per-game dispatch arm in
 * games/spyro-1/game_extras_impl.c hooks the Spyro-specific PC) to
 * overwrite the canonical
 * polybuf pointers with addresses in the registered region. See
 * notes/plans/openpete-widescreen-fov.md and
 * notes/plans/openpete-ram-virtualization.md.
 *
 * widescreen_polybuf_setup() is idempotent. Returns 0 on success;
 * non-zero on allocation / region-registration failure.
 *   size_per_slot — bytes per slot (= psx_runtime_polybuf_size). Two
 *                   slots are registered back-to-back.
 *   psx_base       — region base vaddr (must be > 0x80200000). */
int      widescreen_polybuf_setup(uint32_t size_per_slot, uint32_t psx_base);

/* 1 if widescreen_polybuf_setup succeeded; 0 otherwise. Cheap check
 * for the AllocateBuffers post-call hook in game_extras_impl.c. */
int      widescreen_polybuf_active(void);

/* Slot vaddrs inside the registered region. Valid only when
 * widescreen_polybuf_active() returns 1. */
uint32_t widescreen_polybuf_lower_vaddr(void);
uint32_t widescreen_polybuf_higher_vaddr(void);

#ifdef __cplusplus
}
#endif

#endif
