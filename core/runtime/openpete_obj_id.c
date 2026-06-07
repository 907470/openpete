/* openpete_obj_id.c — see openpete_obj_id.h. */

#include "openpete_obj_id.h"

static uint32_t s_obj_id = 0;

void openpete_obj_id_set(uint32_t obj_addr) {
    s_obj_id = obj_addr;
}

uint32_t openpete_obj_id_get(void) {
    return s_obj_id;
}
