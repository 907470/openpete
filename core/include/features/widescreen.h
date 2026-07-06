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

/* --- Polybuf relocation (RAM virtualization first consumer) --------------
 *
 * Spyro's double-buffered polygon buffer (0x1C000 per slot in matching
 * decomp) overflows when the --no-lod producer stack admits far-LOD
 * geometry. Allocate a larger host-backed buffer via the RAM
 * virtualization primitive and post-hook AllocateBuffers (the per-game
 * dispatch arm in games/spyro-1/game_extras_impl.c hooks the
 * Spyro-specific PC) to overwrite the canonical polybuf pointers with
 * addresses in the registered region. See
 * notes/plans/openpete-polybuf-host-redirect.md and
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

/* Record the canonical (un-relocated) per-slot poly-buffer bases the
 * game computed in AllocateBuffers, so psx_runtime_polybuf_to_canon()
 * can map host-relocated derived pointers back to their hardware value.
 * Called from the AllocateBuffers post-call hook before it overwrites
 * g_DB[0/1].m_PolyBuf with the host-relocated bases. */
void widescreen_polybuf_set_canonical(uint32_t canon_lo, uint32_t canon_hi);

#ifdef __cplusplus
}
#endif

#endif
