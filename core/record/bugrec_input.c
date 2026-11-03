/* core/record/bugrec_input.c - the bug-report recorder's raw host input
 * ring and connected-device list; see openpete_bugrec.h. */
#include "openpete_bugrec.h"

#include "openpete_fiber.h"
#include "openpete_host_input.h"
#include "openpete_input_lock.h"
#include "openpete_input_log.h"
#include "openpete_mutex_once.h"
#include "openpete_time.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

/* Entries searched back from the newest for one to merge a motion or axis
 * event into; every candidate must belong to the event's VBL. */
#define INPUT_MERGE_SCAN 16u

/* The ring and its counters. Appended by the event pump, read by the dump
 * and the report on the sim thread; every access holds s_in_lock. */
static openpete_bugrec_input_t s_in[OPENPETE_BUGREC_INPUT_EVENTS];
static uint64_t s_in_n;          /* entries appended */
static uint64_t s_in_events;     /* events recorded, merged ones included */
static uint64_t s_in_merged;     /* events merged into an earlier entry */
static void*    s_in_lock;       /* SDL_Mutex, openpete_mutex_once */

/** @brief Return whether an event type is merged per VBL into one entry per device and code. */
static int input_mergeable(uint32_t type)
{
    return type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_GAMEPAD_AXIS_MOTION ||
           type == SDL_EVENT_JOYSTICK_AXIS_MOTION;
}

/** @brief Fill a ring entry's device, code and values from an SDL event.
 *
 * Keyboard: code the scancode, a 1 for a press, b the modifier state.
 * Mouse button: code the button, a 1 for a press, b the click count. Mouse
 * motion: a and b the relative motion. Wheel: a and b the scroll amounts.
 * Gamepad and joystick button: code the button, a 1 for a press. Axis:
 * code the axis, a the raw value. Hat: code the hat, a the position bits.
 * Device arrival and removal and window focus carry the id only.
 * @param ev The event.
 * @param e The entry; type and flags are set by the caller.
 * @return 1 when the event type is recorded, 0 for a type the ring skips. */
static int input_fill(const SDL_Event* ev, openpete_bugrec_input_t* e)
{
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        e->which = ev->key.which; e->code = (uint16_t)ev->key.scancode;
        e->a = ev->key.down ? 1.0f : 0.0f; e->b = (float)ev->key.mod;
        if (ev->key.repeat) e->flags |= BUGREC_IN_REPEAT;
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        e->which = ev->button.which; e->code = ev->button.button;
        e->a = ev->button.down ? 1.0f : 0.0f; e->b = (float)ev->button.clicks;
        return 1;
    case SDL_EVENT_MOUSE_MOTION:
        e->which = ev->motion.which; e->a = ev->motion.xrel; e->b = ev->motion.yrel;
        return 1;
    case SDL_EVENT_MOUSE_WHEEL:
        e->which = ev->wheel.which; e->a = ev->wheel.x; e->b = ev->wheel.y;
        return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        e->which = ev->gbutton.which; e->code = ev->gbutton.button;
        e->a = ev->gbutton.down ? 1.0f : 0.0f;
        return 1;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        e->which = ev->gaxis.which; e->code = ev->gaxis.axis; e->a = (float)ev->gaxis.value;
        return 1;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        /* A gamepad reports each input twice, as a joystick and as a gamepad
         * event; the gamepad form is the one bindings read. */
        if (SDL_IsGamepad(ev->jdevice.which)) return 0;
        e->which = ev->jdevice.which;
        if (ev->type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
            e->code = ev->jaxis.axis; e->a = (float)ev->jaxis.value;
        } else if (ev->type == SDL_EVENT_JOYSTICK_HAT_MOTION) {
            e->code = ev->jhat.hat; e->a = (float)ev->jhat.value;
        } else {
            e->code = ev->jbutton.button; e->a = ev->jbutton.down ? 1.0f : 0.0f;
        }
        return 1;
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        e->which = ev->jdevice.which;
        return 1;
    case SDL_EVENT_KEYBOARD_ADDED:
    case SDL_EVENT_KEYBOARD_REMOVED:
        e->which = ev->kdevice.which;
        return 1;
    case SDL_EVENT_MOUSE_ADDED:
    case SDL_EVENT_MOUSE_REMOVED:
        e->which = ev->mdevice.which;
        return 1;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        e->which = ev->window.windowID;
        return 1;
    default:
        return 0;
    }
}

/** @brief Merge a motion or axis entry into the newest entry of its VBL with the same type, device and code.
 *
 * Motion sums the relative amounts; an axis keeps the latest value. The
 * caller holds s_in_lock.
 * @param e The new entry.
 * @return 1 when merged, 0 when the caller appends it. */
static int input_merge_locked(const openpete_bugrec_input_t* e)
{
    const uint64_t held = s_in_n < OPENPETE_BUGREC_INPUT_EVENTS ? s_in_n : OPENPETE_BUGREC_INPUT_EVENTS;
    for (uint64_t k = 1; k <= held && k <= INPUT_MERGE_SCAN; k++) {
        openpete_bugrec_input_t* p = &s_in[(s_in_n - k) % OPENPETE_BUGREC_INPUT_EVENTS];
        if (p->vbl != e->vbl) return 0;
        if (p->type != e->type || p->which != e->which || p->code != e->code ||
            p->flags != e->flags) continue;
        if (e->type == SDL_EVENT_MOUSE_MOTION) { p->a += e->a; p->b += e->b; }
        else p->a = e->a;
        if (p->count < UINT16_MAX) p->count++;
        return 1;
    }
    return 0;
}

void openpete_bugrec_input_event(const union SDL_Event* evu, int locked)
{
    const SDL_Event* ev = (const SDL_Event*)evu;
    openpete_bugrec_input_t e;
    memset(&e, 0, sizeof e);
    if (!input_fill(ev, &e)) return;
    e.t_sdl_ns = ev->common.timestamp;
    e.t_op_ns  = op_now();
    e.vbl      = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    e.type     = (uint16_t)ev->type;
    e.count    = 1;
    if (locked) e.flags |= BUGREC_IN_LOCKED;
    if (e.which == OPENPETE_INJECT_DEVICE_ID) e.flags |= BUGREC_IN_INJECTED;
    SDL_LockMutex(openpete_mutex_once(&s_in_lock));
    s_in_events++;
    if (input_mergeable(ev->type) && input_merge_locked(&e)) {
        s_in_merged++;
    } else {
        s_in[s_in_n % OPENPETE_BUGREC_INPUT_EVENTS] = e;
        s_in_n++;
    }
    SDL_UnlockMutex(openpete_mutex_once(&s_in_lock));
}

size_t openpete_bugrec_input_snapshot(openpete_bugrec_input_t** out)
{
    *out = NULL;
    SDL_LockMutex(openpete_mutex_once(&s_in_lock));
    const uint64_t held = s_in_n < OPENPETE_BUGREC_INPUT_EVENTS ? s_in_n : OPENPETE_BUGREC_INPUT_EVENTS;
    openpete_bugrec_input_t* buf = held ? malloc((size_t)held * sizeof *buf) : NULL;
    if (buf)
        for (uint64_t k = 0; k < held; k++)
            buf[k] = s_in[(s_in_n - held + k) % OPENPETE_BUGREC_INPUT_EVENTS];
    SDL_UnlockMutex(openpete_mutex_once(&s_in_lock));
    *out = buf;
    return buf ? (size_t)held : 0u;
}

void openpete_bugrec_input_counts(openpete_bugrec_input_counts_t* out)
{
    SDL_LockMutex(openpete_mutex_once(&s_in_lock));
    out->appended = s_in_n;
    out->held = s_in_n < OPENPETE_BUGREC_INPUT_EVENTS ? s_in_n : OPENPETE_BUGREC_INPUT_EVENTS;
    out->overwritten = s_in_n - out->held;
    out->events = s_in_events;
    out->merged = s_in_merged;
    SDL_UnlockMutex(openpete_mutex_once(&s_in_lock));
    out->bytes = sizeof s_in;
}

/** @brief Return the SDL name of an event type the ring records. */
static const char* input_type_name(uint32_t type)
{
    switch (type) {
    case SDL_EVENT_KEY_DOWN:                return "key_down";
    case SDL_EVENT_KEY_UP:                  return "key_up";
    case SDL_EVENT_MOUSE_BUTTON_DOWN:       return "mouse_down";
    case SDL_EVENT_MOUSE_BUTTON_UP:         return "mouse_up";
    case SDL_EVENT_MOUSE_MOTION:            return "mouse_motion";
    case SDL_EVENT_MOUSE_WHEEL:             return "mouse_wheel";
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:     return "pad_down";
    case SDL_EVENT_GAMEPAD_BUTTON_UP:       return "pad_up";
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:     return "pad_axis";
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:    return "joy_down";
    case SDL_EVENT_JOYSTICK_BUTTON_UP:      return "joy_up";
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:    return "joy_axis";
    case SDL_EVENT_JOYSTICK_HAT_MOTION:     return "joy_hat";
    case SDL_EVENT_JOYSTICK_ADDED:          return "joy_added";
    case SDL_EVENT_JOYSTICK_REMOVED:        return "joy_removed";
    case SDL_EVENT_GAMEPAD_ADDED:           return "pad_added";
    case SDL_EVENT_GAMEPAD_REMOVED:         return "pad_removed";
    case SDL_EVENT_GAMEPAD_REMAPPED:        return "pad_remapped";
    case SDL_EVENT_KEYBOARD_ADDED:          return "keyboard_added";
    case SDL_EVENT_KEYBOARD_REMOVED:        return "keyboard_removed";
    case SDL_EVENT_MOUSE_ADDED:             return "mouse_added";
    case SDL_EVENT_MOUSE_REMOVED:           return "mouse_removed";
    case SDL_EVENT_WINDOW_FOCUS_GAINED:     return "focus_gained";
    case SDL_EVENT_WINDOW_FOCUS_LOST:       return "focus_lost";
    default:                                return "other";
    }
}

/** @brief Return the name an input entry's code reads as: the scancode, gamepad button or axis name, else empty. */
static const char* input_code_name(const openpete_bugrec_input_t* e)
{
    const char* s = NULL;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        s = SDL_GetScancodeName((SDL_Scancode)e->code);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        s = SDL_GetGamepadStringForButton((SDL_GamepadButton)e->code);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        s = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)e->code);
        break;
    default:
        break;
    }
    return s ? s : "";
}

size_t openpete_bugrec_input_csv(FILE* fp)
{
    openpete_bugrec_input_t* ev = NULL;
    const size_t n = openpete_bugrec_input_snapshot(&ev);
    fprintf(fp, "t_sdl_ns,t_op_ns,vbl,device,event,code,code_name,a,b,count,locked,injected,repeat\n");
    for (size_t i = 0; i < n; i++) {
        const openpete_bugrec_input_t* e = &ev[i];
        fprintf(fp, "%llu,%lld,%u,%u,%s,%u,%s,%g,%g,%u,%d,%d,%d\n",
                (unsigned long long)e->t_sdl_ns, (long long)e->t_op_ns, e->vbl, e->which,
                input_type_name(e->type), e->code, input_code_name(e), (double)e->a, (double)e->b,
                e->count, !!(e->flags & BUGREC_IN_LOCKED), !!(e->flags & BUGREC_IN_INJECTED),
                !!(e->flags & BUGREC_IN_REPEAT));
    }
    free(ev);
    return n;
}

/** @brief Return the pad slot a joystick instance drives, or -1 when it drives none. */
static int input_device_slot(SDL_JoystickID id)
{
    for (int s = 0; s < OPENPETE_PAD_SLOTS; s++)
        if (openpete_host_input_slot_device_id(s) == (unsigned)id) return s;
    return -1;
}

/** @brief Write one joystick's line of the device list: instance id, slot, SDL player index, virtual flag, name, GUID and gamepad mapping. */
static void input_device_joystick(FILE* fp, SDL_JoystickID id)
{
    char guid[64];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
    const char* name = SDL_GetJoystickNameForID(id);
    char* mapping = SDL_IsGamepad(id) ? SDL_GetGamepadMappingForID(id) : NULL;
    fprintf(fp, "joystick id=%u slot=%d player_index=%d virtual=%d gamepad=%d name=\"%s\" guid=%s "
            "mapping=\"%s\"\n", (unsigned)id, input_device_slot(id), SDL_GetJoystickPlayerIndexForID(id),
            SDL_IsJoystickVirtual(id) ? 1 : 0, SDL_IsGamepad(id) ? 1 : 0, name ? name : "", guid,
            mapping ? mapping : "");
    SDL_free(mapping);
}

int openpete_bugrec_devices_write(FILE* fp)
{
    int count = 0, n = 0;
    SDL_JoystickID* joys = SDL_GetJoysticks(&n);
    for (int i = 0; joys && i < n; i++, count++) input_device_joystick(fp, joys[i]);
    SDL_free(joys);
    if (!SDL_IsMainThread()) {
        fprintf(fp, "keyboards and mice: not listed off the main thread\n");
        return count;
    }
    SDL_KeyboardID* kbs = SDL_GetKeyboards(&n);
    for (int i = 0; kbs && i < n; i++, count++) {
        const char* name = SDL_GetKeyboardNameForID(kbs[i]);
        fprintf(fp, "keyboard id=%u name=\"%s\"\n", (unsigned)kbs[i], name ? name : "");
    }
    SDL_free(kbs);
    SDL_MouseID* mice = SDL_GetMice(&n);
    for (int i = 0; mice && i < n; i++, count++) {
        const char* name = SDL_GetMouseNameForID(mice[i]);
        fprintf(fp, "mouse id=%u name=\"%s\"\n", (unsigned)mice[i], name ? name : "");
    }
    SDL_free(mice);
    return count;
}
