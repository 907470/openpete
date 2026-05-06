/* openpete_ui_moby.c — see openpete_ui_moby.h. */

#include "openpete_ui_moby.h"

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
}

uint32_t openpete_ui_moby_get_current(void) {
    return s_current;
}

int openpete_ui_moby_is_current_ui(void) {
    uint32_t a = s_current;
    if (a == 0) return 0;
    if (a >= s_static_lo && a < s_static_hi) return 1;
    if (a >= s_text_lo   && a < s_text_hi  ) return 1;
    return 0;
}
