/* openpete_ui_moby.h — HUD/UI moby classifier (address-range form).
 *
 * The game registers the static g_Hud.m_Mobys range at boot
 * (games/spyro-1/game_extras_impl.c); the native renderer classifies
 * moby addresses against it (flat-HUD overlay vs the depth-isolated
 * HUD3D icon pass — see moby.c). Classification is by address range
 * only. */

#ifndef OPENPETE_UI_MOBY_H
#define OPENPETE_UI_MOBY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the guest address range of the static HUD mobys.
 *
 * Stored in two file-scope words with no synchronisation; the game registers
 * it once, before the classifiers are consulted. The range is [lo, hi).
 * Until a call, both classifiers return 0.
 * @param lo First guest address of the range.
 * @param hi One past the last guest address of the range. */
void openpete_ui_moby_set_static_range(uint32_t lo, uint32_t hi);
/** @brief Classify a guest moby address as a flat-HUD moby.
 *
 * Tests the registered static range; the per-frame text range no longer
 * exists, so the result equals openpete_ui_moby_is_addr_static().
 * @param addr Guest moby address.
 * @return 1 when @p addr is nonzero and inside the registered range, else 0. */
int  openpete_ui_moby_is_addr_ui(uint32_t addr);
/** @brief Classify a guest moby address as a static HUD moby drawn as a 3D model.
 *
 * Tests the registered static range (g_Hud.m_Mobys: the gem, dragon, Spyro
 * head, key and digit icons). The same set as openpete_ui_moby_is_addr_ui();
 * the callers keep the two names to separate the flat-HUD and the
 * depth-isolated HUD3D intent.
 * @param addr Guest moby address.
 * @return 1 when @p addr is nonzero and inside the registered range, else 0. */
int  openpete_ui_moby_is_addr_static(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_UI_MOBY_H */
