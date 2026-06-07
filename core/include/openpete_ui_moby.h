/* openpete_ui_moby.h — side-channel UI-moby classification.
 *
 * Per-moby tracking of "is the current GTE projection for a UI moby?"
 * driven by a recompiler instruction substitution at the per-moby
 * iteration site of `func_80022A2C` (Spyro's shaded-moby renderer).
 * The substitution emits `openpete_ui_moby_set_current(cpu->s8)` after
 * the loop's `lw s8, 0($ra)`, stashing the freshly-loaded moby
 * pointer host-side. `gte_rtps_internal` can then check
 * `openpete_ui_moby_is_current_ui()` to decide whether to apply UI
 * compensation (counter-scale + centring shift) or the standard
 * Hor+ widening.
 *
 * UI-moby membership is by PSX-vaddr range:
 *   - g_Hud.m_Mobys[12] static range — registered once.
 *   - Text-moby buffer range — registered per frame (the polybuf top
 *     is the upper bound; `g_HudMobys` running pointer is the lower).
 *
 * Designed to be game-agnostic in core/ — Spyro-specific addresses
 * are passed in by the games/spyro-1/ caller. */

#ifndef OPENPETE_UI_MOBY_H
#define OPENPETE_UI_MOBY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the static UI-moby range. `lo` is inclusive, `hi` is
 * exclusive. Call once at startup with the address bounds of any
 * always-UI moby array (e.g. Spyro's g_Hud.m_Mobys[12]). */
void openpete_ui_moby_set_static_range(uint32_t lo, uint32_t hi);

/* Set the per-frame text-moby buffer range. Called at draw-frame
 * start with the upper bound (= polybuf base + 0x1C000 in Spyro),
 * and updated each iteration of the moby loop with the running
 * `g_HudMobys` pointer as the lower bound — but for performance,
 * pass both endpoints once at frame end of HUD-emit and let the
 * range stay fixed for projection. Setting `lo > hi` disables. */
void openpete_ui_moby_set_text_range(uint32_t lo, uint32_t hi);

/* Stash the moby pointer that GTE will project next. Called from
 * the recompiler-emitted prologue of each per-moby iteration in
 * func_80022A2C. */
void openpete_ui_moby_set_current(uint32_t moby_addr);

/* True iff `addr` falls in either UI range (static HUD or per-frame
 * text-moby buffer). Used both by is_current_ui() and by the PGXP/
 * interp reproject path, which must mirror the live Hor+ widening
 * (centring shift for UI mobys, multiply for everything else) when
 * reprojecting a cached vertex by its obj_tag. */
int  openpete_ui_moby_is_addr_ui(uint32_t addr);

/* True iff the most recently stashed moby pointer falls in either
 * UI range. Cheap two-comparison check on hot path. */
int  openpete_ui_moby_is_current_ui(void);

/* Read back the stashed moby pointer (for diagnostics). */
uint32_t openpete_ui_moby_get_current(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_UI_MOBY_H */
