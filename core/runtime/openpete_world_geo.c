/* openpete_world_geo.c — see openpete_world_geo.h. */

#include "openpete_world_geo.h"

static uint32_t s_current = 0;
static uint16_t s_vidx    = 0;   /* per-strip emission ordinal */

void openpete_world_geo_set_current(uint32_t strip_addr) {
    s_current = strip_addr;
    s_vidx    = 0;               /* new strip → restart ordinal */
}

void openpete_world_geo_clear(void) {
    s_current = 0;
    s_vidx    = 0;
}

uint32_t openpete_world_geo_get_current(void) {
    return s_current;
}

uint16_t openpete_world_geo_next_vidx(void) {
    return s_vidx++;
}
