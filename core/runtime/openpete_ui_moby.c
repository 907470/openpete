/* openpete_ui_moby.c — see openpete_ui_moby.h. */

#include "openpete_ui_moby.h"

static uint32_t s_static_lo = 0;
static uint32_t s_static_hi = 0;

void openpete_ui_moby_set_static_range(uint32_t lo, uint32_t hi) {
    s_static_lo = lo;
    s_static_hi = hi;
}

/* HUD/UI moby classifier, address-range form. The per-current-moby form
 * (set_current/is_current_ui) and the per-frame TEXT range died with the
 * PsyCross --aspect stack + the render cache (2026-07-06) — the text
 * range was only ever populated in PsyCross aspect mode, so on the
 * native path this has always been the static g_Hud.m_Mobys range. */
int openpete_ui_moby_is_addr_ui(uint32_t a) {
    return a != 0 && a >= s_static_lo && a < s_static_hi;
}

/* STATIC range (g_Hud.m_Mobys: the in-game gem/dragon/Spyro-head/key/digit
 * icons) — real 3D models that need a Z-buffer (the HUD3D pass). Same set
 * as is_addr_ui today; both names kept because moby_native distinguishes
 * the intents (flat-HUD vs HUD3D) at its call sites. */
int openpete_ui_moby_is_addr_static(uint32_t a) {
    return a != 0 && a >= s_static_lo && a < s_static_hi;
}
