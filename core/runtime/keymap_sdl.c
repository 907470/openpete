/* core/runtime/keymap_sdl.c — the SDL half of the keymap.
 *
 * keymap.c owns the registry, the layering and the edge latches and is
 * deliberately SDL-free so it links into console unit tests. Everything
 * that needs SDL — turning a binding name into a code, turning it back
 * for the report, and reading whether a source is held — lives here and
 * is installed through the two seams at boot.
 *
 * Gamepad reads use the first CONNECTED pad rather than a slot index:
 * host actions (the freecam toggle, menu nav) are player-1 affordances,
 * and PsyCross already owns the slot->controller assignment for the PSX
 * pads themselves. */

#include "openpete_keymap.h"

#include <SDL3/SDL.h>

#include <string.h>

static SDL_Gamepad* km_first_gamepad(void) {
    /* Re-queried per call rather than cached: pads hot-plug, and the
     * list walk is a cheap array scan inside SDL. */
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    SDL_Gamepad* found = NULL;
    if (ids) {
        for (int i = 0; i < n && !found; i++)
            found = SDL_GetGamepadFromID(ids[i]);
        SDL_free(ids);
    }
    return found;
}

/* Every keyboard-sourced binding reads through here. A pass-through: the
 * real keyboard always drives bindings, including while a gamepad is
 * connected.
 *
 * We previously zeroed the keyboard (allowlisting only the fast-forward
 * key) whenever a gamepad was present, so a Steam-Input desktop config
 * couldn't fire a hotkey via a phantom keystroke (SDL surfaces those with
 * which==0, so the *polled* SDL_GetKeyboardState array can't tell them
 * from a real press). But that penalised every controller user — testers
 * reported keyboard hotkeys dead whenever any pad was plugged in — to
 * defend an esoteric, Steam-only misconfiguration. Re-accepted trade-off:
 * a Steam Input config that maps a button to a keyboard key can trip a
 * binding; the cure is Steam-side (remap the button / disable Steam
 * Input), as run.c's earlier revert (036381d) noted.
 *
 * This is the seam for the correct fix if it ever bites a real user:
 * build keyboard state from the SDL_EVENT_KEY_* stream and drop events
 * whose `which` is not a physical keyboard, instead of polling the merged
 * array. One function now, rather than every hotkey call site. */
static int km_sdl_key_down(int scancode) {
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return 0;
    const bool* state = SDL_GetKeyboardState(NULL);
    return (state && state[scancode]) ? 1 : 0;
}

static int km_sdl_pad_button(int button) {
    SDL_Gamepad* gp = km_first_gamepad();
    if (!gp) return 0;
    return SDL_GetGamepadButton(gp, (SDL_GamepadButton)button) ? 1 : 0;
}

static int km_sdl_pad_axis(int axis) {
    SDL_Gamepad* gp = km_first_gamepad();
    if (!gp) return 0;
    return SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)axis);
}

/* SDL's own button vocabulary is still the mapping-database spelling —
 * "a", "b", "x", "y" — which names the four face buttons by an Xbox
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

static const char* km_sdl_code_to_name(int kind, int code) {
    switch (kind) {
    case KM_SRC_KEY:     return SDL_GetScancodeName((SDL_Scancode)code);
    case KM_SRC_PADBTN:
        /* Report the positional name for the four face buttons — the
         * alias table's first entry for each — so the report and the
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

/* The label the current layout prints for a positional scancode — "Q" for
 * the scancode SDL names "A" on AZERTY. Report-only: bindings stay
 * positional, this just tells the reader which physical key that is. */
static const char* km_sdl_key_label(int scancode) {
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return NULL;
    SDL_Keycode kc = SDL_GetKeyFromScancode((SDL_Scancode)scancode,
                                            SDL_KMOD_NONE, false);
    if (kc == SDLK_UNKNOWN) return NULL;
    return SDL_GetKeyName(kc);
}

void openpete_keymap_install_sdl(void) {
    static const openpete_keymap_input_fns fns = {
        km_sdl_key_down, km_sdl_pad_button, km_sdl_pad_axis
    };
    openpete_keymap_set_lookup(km_sdl_name_to_code, km_sdl_code_to_name,
                               km_sdl_key_label);
    openpete_keymap_set_input(&fns);
}
