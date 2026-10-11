/* core/input/keymap_sdl.c - the SDL half of the keymap.
 *
 * keymap.c owns the registry, the layering and the edge latches and is
 * deliberately SDL-free so it links into console unit tests. Everything
 * that needs SDL - turning a binding name into a code, turning it back
 * for the report, and reading whether a source is held - lives here and
 * is installed through the two seams at boot.
 *
 * Unqualified gamepad reads use slot 0 of the published input snapshot:
 * host actions (the freecam toggle, menu nav) are player-1 affordances,
 * and PsyCross already owns the slot->controller assignment for the PSX
 * pads themselves. "padN:" sources read slot N-1 through the slot pair. */

#include "openpete_keymap.h"

#include <SDL3/SDL.h>

#include "openpete_host_input.h"
#include "openpete_host_mouse.h"

#include <string.h>

/** @brief Keyboard read for the keymap: whether a scancode is held in the host's published input snapshot.
 *
 * Every keyboard-sourced binding reads through here, whether or not a
 * gamepad is connected. This read and the pad reads below use the snapshot
 * rather than SDL's arrays: the event pump refreshes those arrays and need
 * not run on the thread evaluating bindings, while the snapshot is
 * published once per pump, so one binding evaluation sees one consistent
 * sample. The key state merges synthesized keystrokes (SDL reports them
 * with which == 0, as from a Steam Input config that maps a pad button to
 * a key) with physical ones, so such a keystroke can trip a binding.
 * Excluding them would build the key state from the SDL_EVENT_KEY_* stream
 * and drop events whose which is not a physical keyboard; this function is
 * the one site that change touches.
 * @param scancode the SDL scancode.
 * @return nonzero when held. */
static int km_sdl_key_down(int scancode) {
    return openpete_host_input_key_down(scancode);
}

/** @brief Read a player-1 pad button (slot 0) from the published input snapshot.
 *
 * @param button an SDL_GamepadButton; out of range reads 0. */
static int km_sdl_pad_button(int button) {
    return openpete_host_input_pad_button(button);
}

/** @brief Read a player-1 pad axis (slot 0) from the published input snapshot.
 *
 * @param axis an SDL_GamepadAxis; out of range reads 0.
 * @return the axis value, -32768..32767; 0 when no pad is in the slot. */
static int km_sdl_pad_axis(int axis) {
    return openpete_host_input_pad_axis(axis);
}

/** @brief Read a pad button of a host pad slot from the published input snapshot.
 *
 * @param slot the pad slot, 0-based; out of range reads 0.
 * @param button an SDL_GamepadButton; out of range reads 0. */
static int km_sdl_pad_button_slot(int slot, int button) {
    return openpete_host_input_pad_button_slot(slot, button);
}

/** @brief Read a pad axis of a host pad slot from the published input snapshot.
 *
 * @param slot the pad slot, 0-based; out of range reads 0.
 * @param axis an SDL_GamepadAxis; out of range reads 0.
 * @return the axis value, -32768..32767; 0 when no pad is in the slot. */
static int km_sdl_pad_axis_slot(int slot, int axis) {
    return openpete_host_input_pad_axis_slot(slot, axis);
}

/** @brief Mouse-button read for the keymap: whether a button is held in the host's published input snapshot.
 *
 * The motion and wheel totals below read the mouse channel's atomics, which
 * any thread may read.
 * @param code the button index; out of range reads 0.
 * @return 1 when held, 0 otherwise. */
static int km_sdl_mouse_button(int code) {
    return openpete_host_input_mouse_button(code);
}

/** @brief Return the running mouse motion total of an axis from the mouse channel.
 *
 * @param axis KM_MOUSE_X or KM_MOUSE_Y.
 * @return the total in counts. */
static long long km_sdl_mouse_motion_total(int axis) {
    return (long long)openpete_host_mouse_motion_total(axis);
}

/** @brief Return the running wheel total from the mouse channel, in 1/OPENPETE_MOUSE_WHEEL_UNIT notch, up positive. */
static long long km_sdl_mouse_wheel_total(void) {
    return (long long)openpete_host_mouse_wheel_total();
}

/** @brief Return the mouse channel's current owner as an OPENPETE_MOUSE_OWNED_* value. */
static int km_sdl_mouse_owned(void) {
    return openpete_host_mouse_owned();
}

/* SDL's own button vocabulary is still the mapping-database spelling -
 * "a", "b", "x", "y" - which names the four face buttons by an Xbox
 * layout rather than by position, so "pad:a" means different physical
 * buttons to different readers. SDL3 renamed the enum to SOUTH/EAST/
 * WEST/NORTH for exactly that reason; these aliases give the config file
 * the same clarity, plus the PlayStation names, which are the natural
 * spelling for a PSX port. SDL's own names still parse.
 *
 * Output prefers the positional name (see km_sdl_code_to_name), so
 * --keys-report reads the same way the config is written. */
static const struct { const char* alias; SDL_GamepadButton btn; }
g_button_aliases[] = {
    { "south",    SDL_GAMEPAD_BUTTON_SOUTH }, { "cross",    SDL_GAMEPAD_BUTTON_SOUTH },
    { "east",     SDL_GAMEPAD_BUTTON_EAST  }, { "circle",   SDL_GAMEPAD_BUTTON_EAST  },
    { "west",     SDL_GAMEPAD_BUTTON_WEST  }, { "square",   SDL_GAMEPAD_BUTTON_WEST  },
    { "north",    SDL_GAMEPAD_BUTTON_NORTH }, { "triangle", SDL_GAMEPAD_BUTTON_NORTH },
    /* Shoulder/stick clicks by their PSX names, so a whole [keys.pad]
     * table can be written in one vocabulary. */
    { "l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  },
    { "r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
    { "l3", SDL_GAMEPAD_BUTTON_LEFT_STICK     },
    { "r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK    },
    { "select", SDL_GAMEPAD_BUTTON_BACK       },
};

/* The triggers are axes, not buttons, so L2/R2 alias into the axis table. */
static const struct { const char* alias; SDL_GamepadAxis axis; }
g_axis_aliases[] = {
    { "l2", SDL_GAMEPAD_AXIS_LEFT_TRIGGER  },
    { "r2", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER },
};

/** @brief Resolve a binding name to an SDL scancode, gamepad button or gamepad axis code.
 *
 * Gamepad names try the positional and PlayStation aliases (g_button_aliases,
 * g_axis_aliases) case-insensitively before SDL's own names; keyboard names
 * use SDL_GetScancodeFromName.
 * @param kind KM_SRC_KEY, KM_SRC_PADBTN or KM_SRC_PADAXIS; any other kind
 *        gives -1.
 * @param name the token name; NULL or empty gives -1.
 * @return the code, or -1 when the name is unknown. */
static int km_sdl_name_to_code(int kind, const char* name) {
    if (!name || !*name) return -1;
    switch (kind) {
    case KM_SRC_KEY: {
        SDL_Scancode sc = SDL_GetScancodeFromName(name);
        return sc == SDL_SCANCODE_UNKNOWN ? -1 : (int)sc;
    }
    case KM_SRC_PADBTN: {
        for (size_t i = 0; i < SDL_arraysize(g_button_aliases); i++)
            if (SDL_strcasecmp(g_button_aliases[i].alias, name) == 0)
                return (int)g_button_aliases[i].btn;
        SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
        return b == SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)b;
    }
    case KM_SRC_PADAXIS: {
        for (size_t i = 0; i < SDL_arraysize(g_axis_aliases); i++)
            if (SDL_strcasecmp(g_axis_aliases[i].alias, name) == 0)
                return (int)g_axis_aliases[i].axis;
        SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
        return a == SDL_GAMEPAD_AXIS_INVALID ? -1 : (int)a;
    }
    default: return -1;
    }
}

/** @brief Return the report name of a source code.
 *
 * The four face buttons report their positional names ("south", "east",
 * "west", "north"); other buttons, axes and scancodes report SDL's name.
 * @param kind KM_SRC_KEY, KM_SRC_PADBTN or KM_SRC_PADAXIS.
 * @param code the SDL code.
 * @return the name, or NULL for another kind or a code SDL has no string
 *         for. */
static const char* km_sdl_code_to_name(int kind, int code) {
    switch (kind) {
    case KM_SRC_KEY:     return SDL_GetScancodeName((SDL_Scancode)code);
    case KM_SRC_PADBTN:
        /* Report the positional name for the four face buttons - the
         * alias table's first entry for each - so the report and the
         * config file speak the same vocabulary. */
        switch ((SDL_GamepadButton)code) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
        case SDL_GAMEPAD_BUTTON_EAST:  return "east";
        case SDL_GAMEPAD_BUTTON_WEST:  return "west";
        case SDL_GAMEPAD_BUTTON_NORTH: return "north";
        default: break;
        }
        return SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
    case KM_SRC_PADAXIS: return SDL_GetGamepadStringForAxis((SDL_GamepadAxis)code);
    default: return NULL;
    }
}

/** @brief Return the label the current keyboard layout gives a positional scancode.
 *
 * "Q" for the scancode SDL names "A" on AZERTY. Used only by the
 * --keys-report table; bindings stay positional.
 * @param scancode the SDL scancode.
 * @return SDL's key name for the layout's keycode, or NULL for a scancode
 *         outside 1..SDL_SCANCODE_COUNT-1 or with no keycode. */
static const char* km_sdl_key_label(int scancode) {
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return NULL;
    SDL_Keycode kc = SDL_GetKeyFromScancode((SDL_Scancode)scancode,
                                            SDL_KMOD_NONE, false);
    if (kc == SDLK_UNKNOWN) return NULL;
    return SDL_GetKeyName(kc);
}

/* The controller families whose non-face buttons carry their own names. */
typedef enum { PADFAM_XBOX, PADFAM_PLAYSTATION, PADFAM_NINTENDO } pad_family_t;

/** @brief Return the button-naming family of an SDL gamepad type.
 *
 * An unknown, standard or virtual pad takes the Xbox names, as SDL's own
 * face labels do for SDL_GAMEPAD_TYPE_STANDARD. */
static pad_family_t pad_family(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return PADFAM_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return PADFAM_NINTENDO;
    default:
        return PADFAM_XBOX;
    }
}

/** @brief Return the face-button label SDL gives a button on a gamepad type, or NULL for a non-face button. */
static const char* pad_face_label(SDL_GamepadType t, SDL_GamepadButton b) {
    switch (SDL_GetGamepadButtonLabelForType(t, b)) {
    case SDL_GAMEPAD_BUTTON_LABEL_A:        return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B:        return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X:        return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y:        return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS:    return "Cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:   return "Circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:   return "Square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
    default:                                return NULL;
    }
}

/* Per family: back, guide, start, left stick, right stick, left shoulder,
 * right shoulder, left trigger, right trigger. */
enum { PADN_BACK, PADN_GUIDE, PADN_START, PADN_LSTICK, PADN_RSTICK,
       PADN_LSHOULDER, PADN_RSHOULDER, PADN_LTRIGGER, PADN_RTRIGGER, PADN_COUNT };
static const char* const k_pad_names[3][PADN_COUNT] = {
    [PADFAM_XBOX]        = { "View", "Guide", "Menu", "LS", "RS",
                             "LB", "RB", "LT", "RT" },
    [PADFAM_PLAYSTATION] = { "Share", "PS", "Options", "L3", "R3",
                             "L1", "R1", "L2", "R2" },
    [PADFAM_NINTENDO]    = { "-", "Home", "+", "Left Stick", "Right Stick",
                             "L", "R", "ZL", "ZR" },
};

/** @brief Return the family name of a non-face, non-d-pad button, or NULL. */
static const char* pad_named_button(pad_family_t f, SDL_GamepadButton b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_BACK:           return k_pad_names[f][PADN_BACK];
    case SDL_GAMEPAD_BUTTON_GUIDE:          return k_pad_names[f][PADN_GUIDE];
    case SDL_GAMEPAD_BUTTON_START:          return k_pad_names[f][PADN_START];
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return k_pad_names[f][PADN_LSTICK];
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return k_pad_names[f][PADN_RSTICK];
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return k_pad_names[f][PADN_LSHOULDER];
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return k_pad_names[f][PADN_RSHOULDER];
    case SDL_GAMEPAD_BUTTON_DPAD_UP:        return "D-pad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return "D-pad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return "D-pad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return "D-pad Right";
    default:                                return NULL;
    }
}

/** @brief Return the label of an axis source in a family's names: a stick direction or a trigger. */
static const char* pad_axis_label(pad_family_t f, SDL_GamepadAxis a, int invert) {
    const int neg = invert < 0;
    switch (a) {
    case SDL_GAMEPAD_AXIS_LEFTX:         return neg ? "Left Stick Left"  : "Left Stick Right";
    case SDL_GAMEPAD_AXIS_LEFTY:         return neg ? "Left Stick Up"    : "Left Stick Down";
    case SDL_GAMEPAD_AXIS_RIGHTX:        return neg ? "Right Stick Left" : "Right Stick Right";
    case SDL_GAMEPAD_AXIS_RIGHTY:        return neg ? "Right Stick Up"   : "Right Stick Down";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return k_pad_names[f][PADN_LTRIGGER];
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return k_pad_names[f][PADN_RTRIGGER];
    default:                             return NULL;
    }
}

/** @brief Label a pad source by the family of player 1's connected controller.
 *
 * The family comes from the published snapshot's slot-0 gamepad type, so the
 * label follows the pad in hand: "A" on an Xbox pad and "Cross" on a
 * PlayStation pad for the same south button. With no pad connected the Xbox
 * names apply. A pad trigger bound as a button reads as the trigger.
 * @param kind KM_SRC_PADBTN or KM_SRC_PADAXIS.
 * @param code the SDL button or axis.
 * @param invert the source's sign; -1 selects the negative stick direction.
 * @return a static label, or NULL for another kind or an unnamed code. */
static const char* km_sdl_pad_label(int kind, int code, int invert) {
    const SDL_GamepadType t = (SDL_GamepadType)openpete_host_input_pad_type(0);
    const pad_family_t f = pad_family(t);
    if (kind == KM_SRC_PADAXIS)
        return pad_axis_label(f, (SDL_GamepadAxis)code, invert);
    if (kind != KM_SRC_PADBTN) return NULL;
    const char* face = pad_face_label(t, (SDL_GamepadButton)code);
    return face ? face : pad_named_button(f, (SDL_GamepadButton)code);
}

void openpete_keymap_install_sdl(void) {
    static const openpete_keymap_input_fns fns = {
        km_sdl_key_down, km_sdl_pad_button, km_sdl_pad_axis,
        km_sdl_pad_button_slot, km_sdl_pad_axis_slot,
        km_sdl_mouse_button, km_sdl_mouse_motion_total,
        km_sdl_mouse_wheel_total, km_sdl_mouse_owned
    };
    openpete_keymap_set_lookup(km_sdl_name_to_code, km_sdl_code_to_name,
                               km_sdl_key_label);
    openpete_keymap_set_input(&fns);
    openpete_keymap_set_pad_label(km_sdl_pad_label);
}
