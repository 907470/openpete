// core/runtime/freecam.c — host-side state for the debug free camera (F).
// See openpete_freecam.h. Generic, address-free; the Spyro-specific camera
// drive + function-freeze lives in games/spyro-1/features/freecam_native.c.

#include "openpete_freecam.h"
#include "openpete_log.h"

#include <stdlib.h>
#include <string.h>

static bool                  s_active      = false;
static openpete_freecam_axes s_axes        = {0};
static int                   s_speed_index = 2;   /* "normal" (middle step) */
static bool                  s_teleport_req = false;

bool openpete_freecam_active(void) { return s_active; }

void openpete_freecam_toggle(void) {
    s_active = !s_active;
    memset(&s_axes, 0, sizeof s_axes);
    /* A request only makes sense against the pose that was on screen when it
     * was made, so it never survives a toggle in either direction. */
    s_teleport_req = false;
    if (s_active) {
        OPL_INFO_F("freecam",
                   "ON — WASD move, arrows look, Space/LShift up/down, "
                   "[ ] speed (step %d/%d), T teleport Spyro here",
                   s_speed_index, OPENPETE_FREECAM_SPEED_STEPS - 1);
    } else {
        OPL_INFO_F("freecam", "OFF — camera returned to the game");
    }
}

void openpete_freecam_set_axes(const openpete_freecam_axes* ax) {
    if (ax) s_axes = *ax;
}

void openpete_freecam_get_axes(openpete_freecam_axes* out) {
    if (out) *out = s_axes;
}

void openpete_freecam_request_teleport(void) {
    if (s_active) s_teleport_req = true;
}

bool openpete_freecam_take_teleport_request(void) {
    bool req = s_teleport_req;
    s_teleport_req = false;
    return req;
}

int openpete_freecam_speed_index(void) {
    if (s_speed_index < 0) return 0;
    if (s_speed_index >= OPENPETE_FREECAM_SPEED_STEPS)
        return OPENPETE_FREECAM_SPEED_STEPS - 1;
    return s_speed_index;
}

void openpete_freecam_adjust_speed(int dir) {
    int n = s_speed_index + (dir > 0 ? 1 : (dir < 0 ? -1 : 0));
    if (n < 0) n = 0;
    if (n >= OPENPETE_FREECAM_SPEED_STEPS) n = OPENPETE_FREECAM_SPEED_STEPS - 1;
    if (n != s_speed_index) {
        s_speed_index = n;
        OPL_INFO_F("freecam", "speed step %d/%d",
                   s_speed_index, OPENPETE_FREECAM_SPEED_STEPS - 1);
    }
}
