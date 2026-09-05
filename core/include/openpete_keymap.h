#ifndef OPENPETE_KEYMAP_H
#define OPENPETE_KEYMAP_H

/*
 * The keymap — every key and pad button openpete reads, in one table.
 *
 * Three input surfaces used to be hardcoded in three places: the PSX pad
 * in PsyCross's PsyX_Sys_InitialiseInput, ~30 host hotkeys as raw
 * SDL_SCANCODE_* reads in run.c, and mod [[binding]] rows in mod.toml.
 * They shared no vocabulary and none of them could be rebound. This
 * table is the single registry; `[keys]` in openpete.toml overrides it.
 *
 * Bindings resolve through the same layered scope ladder as the rest of
 * the config (openpete_config.h): defaults < global < [game.<id>] <
 * [disc.<serial>] < [disc.<sha>] < [saved], and exe-dir < user-dir
 * within a tier. Merge is PER ACTION — a scope that names one action
 * leaves every other one alone — and WITHIN an action a scope replaces
 * the whole source list rather than appending, which is what makes
 * `fillpick = "none"` able to unbind something a lower scope bound.
 *
 * This header is SDL-free, and so is keymap.c: the name<->code lookups
 * live behind openpete_keymap_set_lookup so the resolver can be linked
 * into console unit tests (and so a headless run that never brings up
 * SDL's gamepad subsystem still resolves cleanly). keymap_sdl.c
 * installs the real SDL-backed implementation.
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- source kinds --------------------------------------------------- */

typedef enum {
    KM_SRC_NONE = 0,
    KM_SRC_KEY,       /* keyboard scancode */
    KM_SRC_PADBTN,    /* gamepad button */
    KM_SRC_PADAXIS    /* gamepad axis (invert flips the sign) */
} km_srckind_t;

typedef struct {
    unsigned char kind;    /* km_srckind_t */
    signed char   invert;  /* KM_SRC_PADAXIS only: "pad:-leftx" */
    short         code;    /* SDL scancode / gamepad button / gamepad axis */
    /* Which player's device the source reads, 1-based; 0 means UNQUALIFIED.
     * The distinction matters inside a [keys.padN] block, where an
     * unqualified "pad:" reads that block's own player — so player 2's cross
     * can be written "pad:south" rather than forcing "pad2:" on every row —
     * while an explicit "padM:" still cross-wires deliberately. For host
     * actions, unqualified means player 1, as it always did.
     * Meaningless for KM_SRC_KEY: there is one keyboard. */
    unsigned char device;
} km_source_t;

/* Most actions bind one key plus one pad source; four is slack enough
 * that no real config hits the cap, and it keeps the store flat. */
#define KM_MAX_SOURCES 4

/* ---- domains, kinds, contexts --------------------------------------- */

typedef enum { KM_DOM_HOST = 0, KM_DOM_PAD, KM_DOM_PAD2,
               KM_DOM_PAD3, KM_DOM_PAD4, KM_DOM_COUNT } km_domain_t;

typedef enum { KM_DIGITAL = 0, KM_AXIS } km_kind_t;

/* Which input context owns the binding. The conflict scan only reports a
 * clash between actions whose contexts can be live at the same moment,
 * so the collisions the port has always had — WASD as both the pad's
 * left stick (GAMEPLAY) and the freecam's fly axes (FREECAM), arrows as
 * both the d-pad and menu nav — stay quiet, while a genuine double-bind
 * inside one context gets named. */
typedef enum {
    KM_CTX_ALWAYS = 0,   /* overlaps everything */
    KM_CTX_GAMEPLAY,     /* no overlay open, freecam disengaged */
    KM_CTX_MENU,         /* the O settings menu is open */
    KM_CTX_FREECAM,      /* freecam or camlock engaged */
    KM_CTX_FILLPICK,     /* the fill-tag inspector is engaged */
    KM_CTX_COUNT
} km_ctx_t;

/* Registry flags. */
#define KM_DEV      0x1u   /* a development instrument: DEFAULTS TO UNBOUND
                            * so it is never live in a shipped build unless
                            * a config binds it back. */
#define KM_REQUIRED 0x2u   /* losing every source makes something
                            * unreachable; warn (never error) at resolve. */

/* ---- the action registry -------------------------------------------- */

/* X(sym, id, kind, ctx, flags, default_binding, purpose)
 *
 * `default_binding` is spelled in the TOML vocabulary, not as
 * SDL_SCANCODE_*, so --keys-report prints defaults and overrides in one
 * spelling and so every default is itself parsed at init — a typo fails
 * loudly at boot instead of silently never matching. */

#define OPENPETE_KEYMAP_HOST_ACTIONS(X)                                        \
  /* --- overlays ----------------------------------------------------- */     \
  X(MENU,      "menu",      KM_DIGITAL, KM_CTX_ALWAYS,   KM_REQUIRED,          \
        "O",              "open/close the settings menu")                      \
  X(DEVUI,     "devui",     KM_DIGITAL, KM_CTX_ALWAYS,   KM_DEV,               \
        "M",              "open/close the dev config overlay")                 \
  /* The d-pad AND the left stick drive menu nav, and cross doubles as
   * "increase" — the analog fallbacks matter because the settings menu is
   * reachable on a pad with no keyboard attached. */                          \
  X(MENU_UP,   "menu_up",   KM_DIGITAL, KM_CTX_MENU,     KM_REQUIRED,          \
        "Up,pad:dpup,pad:-lefty",    "settings menu: previous row")            \
  X(MENU_DOWN, "menu_down", KM_DIGITAL, KM_CTX_MENU,     KM_REQUIRED,          \
        "Down,pad:dpdown,pad:lefty", "settings menu: next row")                \
  X(MENU_LEFT, "menu_left", KM_DIGITAL, KM_CTX_MENU,     KM_REQUIRED,          \
        "Left,pad:dpleft,pad:-leftx", "settings menu: decrease value")         \
  X(MENU_RIGHT,"menu_right",KM_DIGITAL, KM_CTX_MENU,     KM_REQUIRED,          \
        "Right,pad:dpright,pad:leftx,pad:south",                               \
        "settings menu: increase value")                                       \
  X(MENU_CLOSE,"menu_close",KM_DIGITAL, KM_CTX_MENU,     0,                    \
        "pad:start,pad:east", "settings menu: close (on release)")             \
  /* Activates ACTION rows (EXIT GAME), which ignore left/right entirely —
   * cycling a value must never be able to fire a button. Cross doubles as
   * "increase value" on the ordinary rows; the two never contend, because a
   * row is either a value or an action, never both. */                        \
  X(MENU_SELECT,"menu_select",KM_DIGITAL, KM_CTX_MENU,   0,                    \
        "Return,pad:south",   "settings menu: activate the selected action")   \
  /* --- savestates / rewind / pacing --------------------------------- */     \
  X(STATE_SAVE, "savestate_save",  KM_DIGITAL, KM_CTX_GAMEPLAY, 0,             \
        "1",              "quick-save to the active slot")                     \
  X(STATE_LOAD, "savestate_load",  KM_DIGITAL, KM_CTX_GAMEPLAY, 0,             \
        "2",              "quick-load from the active slot")                   \
  X(STATE_CYCLE,"savestate_cycle", KM_DIGITAL, KM_CTX_GAMEPLAY, 0,             \
        "3",              "cycle the active savestate slot")                   \
  X(REWIND,     "rewind",          KM_DIGITAL, KM_CTX_GAMEPLAY, 0,             \
        "Backspace",      "hold to rewind (needs --rewind)")                   \
  X(PAUSE,      "pause",           KM_DIGITAL, KM_CTX_ALWAYS,   0,             \
        "F7",             "pause / resume the sim")                            \
  X(FRAME_STEP, "frame_step",      KM_DIGITAL, KM_CTX_ALWAYS,   0,             \
        "F8",             "advance one tick while paused")                     \
  X(TURBO,      "turbo",           KM_DIGITAL, KM_CTX_ALWAYS,   0,             \
        "Tab",            "hold to run unlocked (fast-forward)")               \
  X(FULLSCREEN, "fullscreen",      KM_DIGITAL, KM_CTX_ALWAYS,   0,             \
        "none",           "toggle fullscreen (Alt+Enter always works too)")    \
  /* Deliberately NOT KM_REQUIRED: unbinding this is a supported choice —
   * the window close button and the OS still quit — and warning about it
   * would nag exactly the person who asked for it. */                         \
  X(QUIT,       "quit",            KM_DIGITAL, KM_CTX_ALWAYS,   0,             \
        "Escape",         "quit the game")                                     \
  /* --- camera -------------------------------------------------------- */    \
  X(FREECAM,    "freecam",   KM_DIGITAL, KM_CTX_ALWAYS,  0,                    \
        "F,pad:leftstick", "engage/disengage the debug free camera")           \
  X(CAMLOCK,    "camlock",   KM_DIGITAL, KM_CTX_ALWAYS,  KM_DEV,               \
        "G",              "lock the presented view while the sim runs")        \
  X(TELEPORT,   "teleport",  KM_DIGITAL, KM_CTX_ALWAYS,  0,                    \
        "T",              "drop Spyro at the free camera")                     \
  X(FLY_FORWARD,"fly_forward", KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "W",              "free camera: move forward")                         \
  X(FLY_BACK,   "fly_back",    KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "S",              "free camera: move back")                            \
  X(FLY_LEFT,   "fly_left",    KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "A",              "free camera: strafe left")                          \
  X(FLY_RIGHT,  "fly_right",   KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "D",              "free camera: strafe right")                         \
  X(FLY_UP,     "fly_up",      KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "Space,pad:rightshoulder", "free camera: rise")                        \
  X(FLY_DOWN,   "fly_down",    KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "Left Shift,pad:leftshoulder", "free camera: descend")                 \
  X(LOOK_UP,    "look_up",     KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "Up",             "free camera: pitch up")                             \
  X(LOOK_DOWN,  "look_down",   KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "Down",           "free camera: pitch down")                           \
  X(LOOK_LEFT,  "look_left",   KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "Left",           "free camera: yaw left")                             \
  X(LOOK_RIGHT, "look_right",  KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "Right",          "free camera: yaw right")                            \
  X(SPEED_UP,   "speed_up",    KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "],pad:righttrigger", "free camera: faster")                           \
  X(SPEED_DOWN, "speed_down",  KM_DIGITAL, KM_CTX_FREECAM, 0,                  \
        "[,pad:lefttrigger",  "free camera: slower")                           \
  X(FLY_AXIS_X, "fly_axis_x",  KM_AXIS,    KM_CTX_FREECAM, 0,                  \
        "pad:leftx",      "free camera: strafe axis")                          \
  X(FLY_AXIS_Y, "fly_axis_y",  KM_AXIS,    KM_CTX_FREECAM, 0,                  \
        "pad:-lefty",     "free camera: forward axis")                         \
  X(LOOK_AXIS_X,"look_axis_x", KM_AXIS,    KM_CTX_FREECAM, 0,                  \
        "pad:rightx",     "free camera: yaw axis")                             \
  X(LOOK_AXIS_Y,"look_axis_y", KM_AXIS,    KM_CTX_FREECAM, 0,                  \
        "pad:-righty",    "free camera: pitch axis")                           \
  /* --- development instruments (KM_DEV: unbound unless configured) --- */    \
  X(INTERP_TOGGLE, "interp_toggle", KM_DIGITAL, KM_CTX_ALWAYS, KM_DEV,         \
        "I",              "toggle frame interpolation")                        \
  X(LOD_TOGGLE,    "lod_toggle",    KM_DIGITAL, KM_CTX_ALWAYS, KM_DEV,         \
        "L",              "toggle sector LOD original/extended")               \
  X(FILLPICK,      "fillpick",      KM_DIGITAL, KM_CTX_ALWAYS, KM_DEV,         \
        "P",              "toggle the LP fill-tag inspector")                  \
  X(FILLPICK_PREVIEW,"fillpick_preview", KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV,  \
        "H",              "fill-tag inspector: toggle preview")                \
  X(FILLPICK_FILL,   "fillpick_fill",    KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV,  \
        "Y",              "fill-tag inspector: mark FILL")                     \
  X(FILLPICK_NOFILL, "fillpick_nofill",  KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV,  \
        "N",              "fill-tag inspector: mark NOFILL")                   \
  X(FILLPICK_NONE,   "fillpick_none",    KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV,  \
        "B",              "fill-tag inspector: clear the verdict")             \
  X(FILLPICK_UNDO,   "fillpick_undo",    KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV,  \
        "U",              "fill-tag inspector: undo")

/* The PSX pad. Defaults reproduce PsyCross's PsyX_Sys_InitialiseInput
 * exactly, so remapping is opt-in and a stock config plays identically.
 * KM_REQUIRED on the face buttons and d-pad: unbinding every source of
 * one of these makes the game unplayable, which is worth a warning even
 * though it is a legitimate thing for a pad-only player to do. */
#define OPENPETE_KEYMAP_PAD_ACTIONS(X)                                         \
  X(CROSS,    "cross",    KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "C,pad:south",        "PSX pad: cross")                                \
  X(CIRCLE,   "circle",   KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "V,pad:east",         "PSX pad: circle")                               \
  X(SQUARE,   "square",   KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "X,pad:west",         "PSX pad: square")                               \
  X(TRIANGLE, "triangle", KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "Z,pad:north",        "PSX pad: triangle")                             \
  X(L1,       "l1",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Left Shift,pad:leftshoulder",   "PSX pad: L1")                        \
  X(L2,       "l2",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Left Ctrl,pad:lefttrigger",     "PSX pad: L2")                        \
  X(L3,       "l3",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "[,pad:leftstick",               "PSX pad: L3")                        \
  X(R1,       "r1",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Right Shift,pad:rightshoulder", "PSX pad: R1")                        \
  X(R2,       "r2",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Right Ctrl,pad:righttrigger",   "PSX pad: R2")                        \
  X(R3,       "r3",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "],pad:rightstick",              "PSX pad: R3")                        \
  X(START,    "start",    KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "Return,pad:start",   "PSX pad: start")                                \
  X(SELECT,   "select",   KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Space,pad:back",     "PSX pad: select")                               \
  X(DPAD_UP,  "dpad_up",   KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,           \
        "Up,pad:dpup",        "PSX pad: d-pad up")                             \
  X(DPAD_DOWN,"dpad_down", KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,           \
        "Down,pad:dpdown",    "PSX pad: d-pad down")                           \
  X(DPAD_LEFT,"dpad_left", KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,           \
        "Left,pad:dpleft",    "PSX pad: d-pad left")                           \
  X(DPAD_RIGHT,"dpad_right",KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,          \
        "Right,pad:dpright",  "PSX pad: d-pad right")                          \
  X(STICK_UP,   "stick_up",    KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                 \
        "W",              "PSX pad: left stick up (keyboard)")                 \
  X(STICK_DOWN, "stick_down",  KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                 \
        "S",              "PSX pad: left stick down (keyboard)")               \
  X(STICK_LEFT, "stick_left",  KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                 \
        "A",              "PSX pad: left stick left (keyboard)")               \
  X(STICK_RIGHT,"stick_right", KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                 \
        "D",              "PSX pad: left stick right (keyboard)")              \
  X(LEFT_X,   "left_x",   KM_AXIS, KM_CTX_GAMEPLAY, 0,                         \
        "pad:leftx",      "PSX pad: left stick X")                             \
  X(LEFT_Y,   "left_y",   KM_AXIS, KM_CTX_GAMEPLAY, 0,                         \
        "pad:lefty",      "PSX pad: left stick Y")                             \
  X(RIGHT_X,  "right_x",  KM_AXIS, KM_CTX_GAMEPLAY, 0,                         \
        "pad:rightx",     "PSX pad: right stick X")                            \
  X(RIGHT_Y,  "right_y",  KM_AXIS, KM_CTX_GAMEPLAY, 0,                         \
        "pad:righty",     "PSX pad: right stick Y")

/* Action ids. HOST first, then one pad block per player — the enum and the
 * registry are generated from the same lists so they cannot drift. */
#define KM_ENUM_HOST(sym, id, k, c, f, d, p)  OPK_##sym,
#define KM_ENUM_PAD(sym, id, k, c, f, d, p)   OPK_PAD_##sym,
#define KM_ENUM_PAD2(sym, id, k, c, f, d, p)  OPK_PAD2_##sym,
#define KM_ENUM_PAD3(sym, id, k, c, f, d, p)  OPK_PAD3_##sym,
#define KM_ENUM_PAD4(sym, id, k, c, f, d, p)  OPK_PAD4_##sym,

typedef enum {
    OPENPETE_KEYMAP_HOST_ACTIONS(KM_ENUM_HOST)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ENUM_PAD)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ENUM_PAD2)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ENUM_PAD3)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ENUM_PAD4)
    OPK_ACTION_COUNT
} openpete_action_t;

/* First action of one player's pad block. The blocks are generated from one
 * list, so a decoder can walk any player's with the same offsets. */
#define OPK_PAD_BLOCK_FIRST(player) \
    ((player) == 0 ? OPK_PAD_CROSS : (player) == 1 ? OPK_PAD2_CROSS \
   : (player) == 2 ? OPK_PAD3_CROSS : OPK_PAD4_CROSS)

/* ---- name lookup seam ----------------------------------------------- */

/* Turn a token name into a code, or return -1 for "no such name".
 * `kind` is KM_SRC_KEY / KM_SRC_PADBTN / KM_SRC_PADAXIS. */
typedef int (*openpete_keymap_name_to_code_fn)(int kind, const char* name);
/* Reverse, for the report. Returns NULL when the code has no name. */
typedef const char* (*openpete_keymap_code_to_name_fn)(int kind, int code);
/* The label the CURRENT keyboard layout prints for a scancode ("Q" for
 * the scancode SDL calls "A" on AZERTY), or NULL. Report-only garnish. */
typedef const char* (*openpete_keymap_key_label_fn)(int scancode);

void openpete_keymap_set_lookup(openpete_keymap_name_to_code_fn to_code,
                                openpete_keymap_code_to_name_fn to_name,
                                openpete_keymap_key_label_fn label);

/* ---- live input seam ------------------------------------------------- */

/* Where the resolved bindings are actually READ. Behind a seam for the
 * same reason as the lookups: it keeps the store, the edge latch and the
 * deadzone logic in this SDL-free TU (so the unit test can drive them
 * with fake input) and leaves keymap_sdl.c as pure glue. All three may
 * be NULL — every query then reads as "not held", which is what a
 * headless run wants. */
typedef struct {
    int (*key_down)(int scancode);     /* 0/1 */
    int (*pad_button)(int button);     /* 0/1 */
    int (*pad_axis)(int axis);         /* [-32768, 32767] */
    /* Slot-qualified reads, for sources carrying a device > 0. Optional: a
     * NULL pair makes every such source read as "not held" rather than
     * silently falling back to player 1's device, which would let a mistyped
     * "pad2:" quietly control player 1. The unqualified pair above still
     * serves device 0, so existing bindings do not change path. */
    int (*pad_button_slot)(int slot, int button);
    int (*pad_axis_slot)(int slot, int axis);
} openpete_keymap_input_fns;

void openpete_keymap_set_input(const openpete_keymap_input_fns* fns);

/* ---- resolving against a caller's own snapshot ----------------------- */

/* Reads supplied by a caller that already holds the input state it wants the
 * bindings resolved against, rather than letting them reach for live SDL.
 * That is what keeps an extra player's pad decode replayable: it is resolved
 * on the producer side from the sample being published, so the bytes a tick
 * consumes are the bytes the input log records for it. Any member may be
 * NULL, reading as "not held". */
typedef struct {
    int (*key_down)(void* ctx, int scancode);            /* 0/1 */
    int (*pad_button)(void* ctx, int slot, int button);  /* 0/1 */
    int (*pad_axis)(void* ctx, int slot, int axis);      /* [-32768, 32767] */
    void* ctx;
} openpete_keymap_reader_t;

/* Level state / analog value of `a`, resolved against `rd` (NULL = the
 * installed live reads). `default_slot` serves sources with no explicit
 * "padN:" qualifier, so a [keys.pad2] row written "pad:south" reads player
 * 2's device. Unlike openpete_key_down these do not consult the edge latch:
 * they resolve the bindings directly, so a caller running outside the
 * host-loop poll order gets this instant's answer rather than the last
 * poll's. */
int   openpete_keymap_action_down_via(openpete_action_t a, int default_slot,
                                      const openpete_keymap_reader_t* rd);
float openpete_keymap_action_axis_via(openpete_action_t a, int default_slot,
                                      const openpete_keymap_reader_t* rd);

/* As above, returning the raw [-32768, 32767] value of the winning source
 * instead of a deadzoned float, for callers reproducing a fixed-point axis
 * conversion. A digital source that is held reads as full throw. */
int   openpete_keymap_action_axis_raw_via(openpete_action_t a, int default_slot,
                                          const openpete_keymap_reader_t* rd);

/* Number of sources bound to `a`; 0 means the action is unbound. */
int   openpete_keymap_action_bound(openpete_action_t a);

/* Install the SDL-backed lookups and reads (keymap_sdl.c). Call before
 * openpete_keymap_init, which parses every registry default and needs
 * the name lookup to be live. */
void openpete_keymap_install_sdl(void);

/* ---- boot ------------------------------------------------------------ */

/* Seed every action from its registry default. Safe to call twice; the
 * second call resets the store. Must run after openpete_keymap_set_lookup
 * or every default fails to parse. */
void openpete_keymap_init(void);

/* Record one layer's binding for `domain`.`action`. `tokens`/`n` is the
 * value as written: a single string, or an array's elements. n == 0 means
 * an empty array, which unbinds exactly like the token "none".
 *
 * `rank` is the caller's packed (tier, file) precedence — higher wins,
 * ties go to the later call. `origin` is the display label ("game.spyro-1").
 * Returns 0, or -1 after reporting an unknown action / unparseable token. */
int openpete_keymap_set(const char* domain, const char* action,
                        const char* const* tokens, int n,
                        int rank, const char* origin, const char* where);

/* [keys.mod.<id>] — a mod's declared [[binding]] name, rebound from the
 * config. Stored apart from the action registry because mods are
 * discovered after the config is read, so the name cannot be validated
 * against a table here. */
int openpete_keymap_set_mod(const char* mod_id, const char* name,
                            const char* const* tokens, int n,
                            int rank, const char* where);

/* Pick winners and run the conflict + KM_REQUIRED scans. Returns the
 * number of warnings emitted (0 = clean); never fails a boot. */
int openpete_keymap_resolve(void);

/* ---- queries --------------------------------------------------------- */

/* Advance the edge latches. Call exactly once per host-loop iteration,
 * after SDL events are pumped and before any openpete_key_pressed. */
void openpete_keymap_poll(void);

int   openpete_key_down(openpete_action_t a);     /* level: held now */
int   openpete_key_pressed(openpete_action_t a);  /* edge: pressed this poll */
float openpete_key_axis(openpete_action_t a);     /* [-1, 1], deadzoned */

/* While an overlay owns the keyboard, KEYBOARD sources go inert but pad
 * sources keep working — mouse-only devui interaction is meant to leave
 * the game playable. Suspending by source kind rather than wholesale is
 * what preserves that. */
void openpete_keymap_suspend_keyboard(int on);

/* ---- diagnostics / consumers ---------------------------------------- */

/* The resolved sources for an action, for consumers that need the raw
 * binding rather than a live read — the PsyCross pad stamp needs the
 * scancode and gamepad code themselves. Returns the count. */
int openpete_keymap_sources(openpete_action_t a, km_source_t* out, int cap);

/* Winning layer label for an action ("default", "game.spyro-1"). */
const char* openpete_keymap_origin(openpete_action_t a);

/* Full table: every action, its binding, the scope that won, and any
 * conflicts. Unbound rows print "-" with a reason, so this doubles as
 * the ship-time audit of what a stock install responds to. */
void openpete_keymap_report(FILE* out);

/* True when --keys-report was passed (the caller prints and exits). */
int openpete_keymap_report_requested(void);
void openpete_keymap_request_report(void);

/* Resolve a mod's declared binding name to an action-less source list.
 * [keys.mod.<id>] overrides the mod's own mod.toml `key`; returns 1 and
 * fills `out` when the config bound it, 0 when the mod's own key stands. */
int openpete_keymap_mod_binding(const char* mod_id, const char* name,
                                km_source_t* out, int cap, int* out_n);

/* True when any keyboard source for this scancode is held — the raw
 * escape hatch mod bindings and the fallback path use. */
int openpete_keymap_scancode_down(int scancode);

/* Live read of one resolved source, for consumers holding a km_source_t
 * rather than an action (mod bindings). Honours the keyboard suspend. */
int openpete_keymap_source_down(const km_source_t* s);

/* Does `scancode` drive `a`? A pure binding query, not a live read — for
 * the SDL event filter, which sees one key event at a time and cannot use
 * the polled edge latch. Unaffected by the keyboard suspend. */
int openpete_keymap_key_binds(openpete_action_t a, int scancode);

/* "This key press has already been acted on; do not let the polled
 * bindings act on it as well."
 *
 * openpete has two input paths that can both see one physical press: the
 * SDL event filter (which gets first look, one event at a time) and the
 * polled keymap. They read different things — an event versus
 * SDL_GetKeyboardState — so swallowing the event does NOT stop the poll
 * seeing the key held, and both fire. With `menu` bound to the same key
 * the filter dismisses on, that meant the menu closed and instantly
 * reopened.
 *
 * The filter calls this for any press it consumes. The key then reads as
 * not-held to every polled binding until it is physically released, so
 * one press produces one action. Releasing and pressing again works
 * normally — which is what makes a dismiss key double as a toggle. */
void openpete_keymap_consume_key(int scancode);

/* Display spelling of a mod binding's EFFECTIVE first source — the
 * [keys.mod.<id>] override when there is one, else `fallback` (the mod's
 * own declared key) unchanged. Lets the cross-mod conflict scan compare
 * what is actually bound, so a user who rebinds to resolve a clash stops
 * being warned about it. Returns a pointer into a static buffer. */
const char* openpete_keymap_mod_effective_key(const char* mod_id,
                                              const char* name,
                                              const char* fallback);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_KEYMAP_H */
