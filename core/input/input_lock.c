/* core/input/input_lock.c - the host input lock. See the header. */

#include "openpete_input_lock.h"

#include <SDL3/SDL.h>

#include <stdatomic.h>

static _Atomic int                s_lock;
static _Atomic unsigned long long s_dropped;

void openpete_input_lock_set(int on)
{
    atomic_store_explicit(&s_lock, on ? 1 : 0, memory_order_relaxed);
}

int openpete_input_lock_active(void)
{
    return atomic_load_explicit(&s_lock, memory_order_relaxed);
}

unsigned long long openpete_input_lock_dropped(void)
{
    return atomic_load_explicit(&s_dropped, memory_order_relaxed);
}

/** @brief Return whether an event reports input from a physical host device.
 *
 * Keyboard and mouse events are physical unless their `which` is
 * OPENPETE_INJECT_DEVICE_ID; joystick and gamepad input events are physical
 * unless the joystick is an SDL virtual one; text, touch, pen and drop
 * events are always physical. Every other type, including device arrival
 * and removal, is not input, so PsyCross's hotplug bookkeeping stays
 * consistent and a pad opened under the lock is never read.
 * @param ev the event.
 * @return 1 for physical-device input, 0 otherwise. */
static int from_physical_device(const SDL_Event* ev)
{
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return ev->key.which != OPENPETE_INJECT_DEVICE_ID;
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
        return 1;
    case SDL_EVENT_MOUSE_MOTION:
        return ev->motion.which != OPENPETE_INJECT_DEVICE_ID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return ev->button.which != OPENPETE_INJECT_DEVICE_ID;
    case SDL_EVENT_MOUSE_WHEEL:
        return ev->wheel.which != OPENPETE_INJECT_DEVICE_ID;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BALL_MOTION:
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        /* `which` sits at the same offset in every joystick input event. */
        return !SDL_IsJoystickVirtual(ev->jaxis.which);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        /* Likewise for every gamepad input event. */
        return !SDL_IsJoystickVirtual(ev->gaxis.which);
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_PEN_PROXIMITY_IN:
    case SDL_EVENT_PEN_PROXIMITY_OUT:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_AXIS:
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_DROP_COMPLETE:
    case SDL_EVENT_DROP_POSITION:
        return 1;
    default:
        return 0;
    }
}

int openpete_input_lock_drops_event(const union SDL_Event* evu)
{
    if (!evu || !openpete_input_lock_active()) return 0;
    if (!from_physical_device((const SDL_Event*)evu)) return 0;
    atomic_fetch_add_explicit(&s_dropped, 1ull, memory_order_relaxed);
    return 1;
}
