/* core/runtime/discontinuity.c — guest-state discontinuity broadcast.
 * See openpete_discontinuity.h for the contract. */

#include "openpete_discontinuity.h"
#include "openpete_log.h"

#include <stddef.h>

#define MAX_SUBSCRIBERS 16

static openpete_discontinuity_fn s_subs[MAX_SUBSCRIBERS];
static int s_sub_count = 0;

int openpete_on_guest_discontinuity(openpete_discontinuity_fn fn) {
    if (!fn) return -1;
    for (int i = 0; i < s_sub_count; ++i)
        if (s_subs[i] == fn) return 0;   /* idempotent re-register */
    if (s_sub_count >= MAX_SUBSCRIBERS) {
        OPL_ERROR_F("discontinuity",
                    "subscriber table full (%d); bump MAX_SUBSCRIBERS",
                    MAX_SUBSCRIBERS);
        return -1;
    }
    s_subs[s_sub_count++] = fn;
    return 0;
}

void openpete_guest_discontinuity_notify(void) {
    for (int i = 0; i < s_sub_count; ++i) s_subs[i]();
    OPL_INFO_F("discontinuity", "notified %d subscribers", s_sub_count);
}
