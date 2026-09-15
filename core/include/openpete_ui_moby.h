/* openpete_ui_moby.h — HUD/UI moby classifier (address-range form).
 *
 * The game registers the static g_Hud.m_Mobys range at boot
 * (games/spyro-1/game_extras_impl.c); the native renderer classifies
 * moby addresses against it (flat-HUD overlay vs the depth-isolated
 * HUD3D icon pass — see moby.c). The per-current-moby channel
 * (set_current/is_current_ui, fed by recompiler-injected loop tags) and
 * the per-frame text range died with the PsyCross --aspect stack + the
 * render cache, 2026-07-06. */

#ifndef OPENPETE_UI_MOBY_H
#define OPENPETE_UI_MOBY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void openpete_ui_moby_set_static_range(uint32_t lo, uint32_t hi);
int  openpete_ui_moby_is_addr_ui(uint32_t addr);
int  openpete_ui_moby_is_addr_static(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_UI_MOBY_H */
