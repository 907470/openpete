/* openpete_ui_moby.c — see openpete_ui_moby.h. */

#include "openpete_ui_moby.h"

#include <stdio.h>   /* §13.16 diag logging */
#include <stdlib.h>

static uint32_t s_static_lo = 0;
static uint32_t s_static_hi = 0;
static uint32_t s_text_lo   = 0;
static uint32_t s_text_hi   = 0;
static uint32_t s_current   = 0;

void openpete_ui_moby_set_static_range(uint32_t lo, uint32_t hi) {
    s_static_lo = lo;
    s_static_hi = hi;
}

void openpete_ui_moby_set_text_range(uint32_t lo, uint32_t hi) {
    s_text_lo = lo;
    s_text_hi = hi;
}

void openpete_ui_moby_set_current(uint32_t moby_addr) {
    s_current = moby_addr;
    /* §13.16 diag: log every UNIQUE moby addr that reaches the UI
     * classifier, with its verdict and the live text range — names
     * the address-space mismatch behind the HUD reposition flicker. */
    if (moby_addr) {
        static int s_log = -1;
        if (s_log < 0) { const char* e = getenv("OPENPETE_UI_CLASS_LOG");
                         s_log = (e && *e) ? 1 : 0; }
        if (s_log) {
            /* Every sighting (no dedupe), canonical-polybuf region
             * only (low 29 bits >= 0x140000 — above static globals,
             * below the 2MB top) — verdict FLIPS across sightings of
             * one addr are the flicker. */
            static unsigned s_n = 0;
            if (((moby_addr & 0x1FFFFFFFu) >= 0x00140000u) &&
                s_n < 400000u) {
                s_n++;
                fprintf(stderr,
                        "[ui-class] a=%08X ui=%d txt=[%08X,%08X)\n",
                        moby_addr,
                        openpete_ui_moby_is_addr_ui(moby_addr),
                        s_text_lo, s_text_hi);
            }
        }
    }
}

uint32_t openpete_ui_moby_get_current(void) {
    return s_current;
}

int openpete_ui_moby_is_addr_ui(uint32_t a) {
    if (a == 0) return 0;
    if (a >= s_static_lo && a < s_static_hi) return 1;
    if (a >= s_text_lo   && a < s_text_hi  ) return 1;
    return 0;
}

int openpete_ui_moby_is_current_ui(void) {
    return openpete_ui_moby_is_addr_ui(s_current);
}

/* STATIC range only (g_Hud.m_Mobys: the in-game gem/dragon/Spyro-head/key/digit
 * icons) — these are real 3D models that need a Z-buffer (the HUD3D pass). The
 * text range (func_80017FE4 letter mobys: RESCUED/CRASHED) is excluded — it's
 * flat 2D and keeps the depth-off HUD behaviour (incl. "RESCUED under Spyro"). */
int openpete_ui_moby_is_addr_static(uint32_t a) {
    return a != 0 && a >= s_static_lo && a < s_static_hi;
}
