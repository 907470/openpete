/* openpete_world_geo.c — see openpete_world_geo.h. */

#include "openpete_world_geo.h"

static uint32_t s_current = 0;

void openpete_world_geo_set_current(uint32_t strip_addr) {
    s_current = strip_addr;
}

void openpete_world_geo_clear(void) {
    s_current = 0;
}

uint32_t openpete_world_geo_get_current(void) {
    return s_current;
}
