/* core/input/host_mouse.c — the host mouse channel. See the header. */

#include "openpete_host_mouse.h"

#include "openpete_devui.h"
#include "openpete_fillpick.h"
#include "openpete_host_input.h"
#include "openpete_input_lock.h"
#include "openpete_keymap.h"
#include "openpete_log.h"
#include "openpete_menu.h"
#include "openpete_native_render.h"
#include "mod_vec.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- producer state (pump thread only) --------------------------------- */

static int   s_inited;
static openpete_mouse_state_t s_state;      /* event-derived */
static SDL_Window* s_fallback_window;
/* Fractional remainders of SDL's float relative motion, carried so the
 * integer count totals lose nothing over time. */
static float s_rem_x, s_rem_y;

/* ---- totals ------------------------------------------------------------ */

static _Atomic uint64_t s_press[OPENPETE_MOUSE_BUTTONS];
static _Atomic int64_t  s_motion[2];
static _Atomic int64_t  s_wheel;

/* ---- ownership / capture / requests ------------------------------------ */

/* One bit per OPENPETE_MOUSE_CAP_* owner, 64 owners per word. The word
 * list grows with the owner id, so a mod's owner id (MOD0 + its load
 * index) always has a bit however many mods are loaded. The bits are
 * only ever touched under s_capture_mx; readers ask s_capture_n, the
 * number of owners currently requesting capture, which needs no lock. */
static mod_fvec      s_capture_req;
static SDL_Mutex*    s_capture_mx;
static _Atomic int   s_capture_n;
static _Atomic int      s_captured;
static _Atomic int      s_menu_req;      /* 0 none, 1 = want closed, 2 = want open */
static _Atomic unsigned s_dbl_count;
static _Atomic int      s_shot_req;      /* the `shot` verb: a player screenshot */
static _Atomic unsigned s_pump_iter;

/* ---- probe ------------------------------------------------------------- */

static int s_probe;

/* ---- the switch -------------------------------------------------------- */

static _Atomic int s_enabled = 1;

/* Marks a pushed event as coming from the injection script rather than from a
 * physical device, so the switch can drop one and keep the other. SDL numbers
 * real mice from its device-id allocator (small positive values) and reserves
 * the top of the range for its own synthetic sources (touch, pen), so a value
 * below those reserved ids and far above any allocated one belongs to nothing
 * else. */
#define INJECT_MOUSE_ID ((SDL_MouseID)OPENPETE_INJECT_DEVICE_ID)

/* OPENPETE_MOUSE_INJECT_RAW=1 pushes the injected events unmarked, so the
 * switch sees them as coming from a physical device. It is the negative
 * control for the switch: the same script that moves the totals with the
 * switch on must move nothing with it off. */
static int s_inj_raw;

static SDL_MouseID inj_mouse_id(void)
{
    return s_inj_raw ? (SDL_MouseID)0 : INJECT_MOUSE_ID;
}

/* ---- injection --------------------------------------------------------- */

typedef enum {
    INJ_MOVE, INJ_DOWN, INJ_UP, INJ_CLICK2, INJ_REL, INJ_WHEEL, INJ_MENU, INJ_CAPTURE,
    INJ_SHOT, INJ_KEYDOWN, INJ_KEYUP
} inj_verb_t;

typedef struct {
    unsigned   iter;     /* pump iteration, or milliseconds since init when is_ms */
    int        is_ms;
    inj_verb_t verb;
    float      a, b;
    int        button;   /* SDL_BUTTON_* for DOWN/UP/CLICK2; SDL_Scancode for KEYDOWN/KEYUP */
} inj_line_t;

static inj_line_t* s_inj;
static int         s_inj_n, s_inj_next;

static int inj_button(const char* s)
{
    if (!strcmp(s, "left"))   return SDL_BUTTON_LEFT;
    if (!strcmp(s, "middle")) return SDL_BUTTON_MIDDLE;
    if (!strcmp(s, "right"))  return SDL_BUTTON_RIGHT;
    if (!strcmp(s, "x1"))     return SDL_BUTTON_X1;
    if (!strcmp(s, "x2"))     return SDL_BUTTON_X2;
    return 0;
}

static void inj_load(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) {
        OPL_ERROR_F("mouse", "OPENPETE_MOUSE_INJECT: cannot open %s", path);
        return;
    }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char when[24] = {0}, verb[16] = {0}, arg1[32] = {0}, arg2[32] = {0};
        char* hash = strchr(line, '#');
        if (hash) *hash = '\0';
        int n = sscanf(line, "%23s %15s %31s %31s", when, verb, arg1, arg2);
        if (n < 2) continue;   /* blank or comment */
        inj_line_t L;
        memset(&L, 0, sizeof L);
        /* "<n>" is a pump iteration; "<n>ms" is wall time since init. The
         * pump rate differs by an order of magnitude between platforms (the
         * Windows pump returns from its 1 ms wait early), so a windowed
         * script that must land its lines in distinct host iterations is
         * written in ms; headless scripts, where the pump iteration is the
         * host iteration, stay exact. */
        {
            char* end = NULL;
            L.iter = (unsigned)strtoul(when, &end, 10);
            if (end && !strcmp(end, "ms")) L.is_ms = 1;
            else if (!end || *end) {
                OPL_ERROR_F("mouse", "OPENPETE_MOUSE_INJECT: %s:%d: bad time '%s'",
                            path, lineno, when);
                continue;
            }
        }
        int ok = 1;
        if (!strcmp(verb, "move") && n == 4) {
            L.verb = INJ_MOVE; L.a = strtof(arg1, NULL); L.b = strtof(arg2, NULL);
        } else if (!strcmp(verb, "rel") && n == 4) {
            L.verb = INJ_REL; L.a = strtof(arg1, NULL); L.b = strtof(arg2, NULL);
        } else if ((!strcmp(verb, "down") || !strcmp(verb, "up") ||
                    !strcmp(verb, "click2")) && n >= 3) {
            L.verb = !strcmp(verb, "down") ? INJ_DOWN
                   : !strcmp(verb, "up")   ? INJ_UP : INJ_CLICK2;
            L.button = inj_button(arg1);
            ok = L.button != 0;
        } else if (!strcmp(verb, "wheel") && n >= 3) {
            L.verb = INJ_WHEEL; L.a = strtof(arg1, NULL);
        } else if (!strcmp(verb, "menu") && n >= 3) {
            L.verb = INJ_MENU; L.a = (float)atoi(arg1);
        } else if (!strcmp(verb, "capture") && n >= 3) {
            L.verb = INJ_CAPTURE; L.a = (float)atoi(arg1);
        } else if (!strcmp(verb, "shot")) {
            L.verb = INJ_SHOT;
        } else if ((!strcmp(verb, "keydown") || !strcmp(verb, "keyup")) && n >= 3) {
            /* The key is an SDL scancode name ("F", "O", "Escape"). */
            L.verb = !strcmp(verb, "keydown") ? INJ_KEYDOWN : INJ_KEYUP;
            L.button = (int)SDL_GetScancodeFromName(arg1);
            ok = L.button != (int)SDL_SCANCODE_UNKNOWN;
        } else {
            ok = 0;
        }
        if (!ok) {
            OPL_ERROR_F("mouse", "OPENPETE_MOUSE_INJECT: %s:%d: bad line: %s",
                        path, lineno, line);
            continue;
        }
        inj_line_t* grown = realloc(s_inj, sizeof *s_inj * (size_t)(s_inj_n + 1));
        if (!grown) break;
        s_inj = grown;
        s_inj[s_inj_n++] = L;
    }
    fclose(f);
    OPL_INFO_F("mouse", "OPENPETE_MOUSE_INJECT: %d lines from %s", s_inj_n, path);
}

static SDL_Window* mouse_window(void);

/* A key line does two things, matching the two ways a real key reaches the
 * process: it pushes the key event (the Escape and quit-key handling and the
 * dev overlay read events) and it sets the key's held state in the polled
 * snapshot (every binding reads that). A pushed event never updates SDL's own
 * keyboard array, so the held state is carried by the host input module. */
static void inj_push_key(int scancode, int down)
{
    openpete_host_input_inject_key(scancode, down, s_inj_raw);
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.timestamp = SDL_GetTicksNS();
    ev.key.windowID = mouse_window() ? SDL_GetWindowID(mouse_window()) : 0;
    ev.key.which = (SDL_KeyboardID)inj_mouse_id();
    ev.key.scancode = (SDL_Scancode)scancode;
    ev.key.key = SDL_GetKeyFromScancode((SDL_Scancode)scancode, SDL_KMOD_NONE, false);
    ev.key.down = down ? true : false;
    SDL_PushEvent(&ev);
}

static SDL_Window* mouse_window(void)
{
    SDL_Window* w = openpete_native_render_window();
    return w ? w : s_fallback_window;
}

static void inj_push_motion(float x, float y, float xrel, float yrel)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.timestamp = SDL_GetTicksNS();
    ev.motion.windowID = mouse_window() ? SDL_GetWindowID(mouse_window()) : 0;
    ev.motion.which = inj_mouse_id();
    ev.motion.state = (SDL_MouseButtonFlags)s_state.buttons;
    ev.motion.x = x;  ev.motion.y = y;
    ev.motion.xrel = xrel; ev.motion.yrel = yrel;
    SDL_PushEvent(&ev);
}

static void inj_push_button(int button, int down, int clicks)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.timestamp = SDL_GetTicksNS();
    ev.button.windowID = mouse_window() ? SDL_GetWindowID(mouse_window()) : 0;
    ev.button.which = inj_mouse_id();
    ev.button.button = (Uint8)button;
    ev.button.down = down ? true : false;
    ev.button.clicks = (Uint8)clicks;
    ev.button.x = s_state.px; ev.button.y = s_state.py;
    SDL_PushEvent(&ev);
}

static void inj_push_wheel(float notches)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_MOUSE_WHEEL;
    ev.wheel.timestamp = SDL_GetTicksNS();
    ev.wheel.windowID = mouse_window() ? SDL_GetWindowID(mouse_window()) : 0;
    ev.wheel.which = inj_mouse_id();
    ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    ev.wheel.y = notches;
    ev.wheel.mouse_x = s_state.px; ev.wheel.mouse_y = s_state.py;
    SDL_PushEvent(&ev);
}

static Uint64 s_init_ticks_ms;

/* Fire every scripted line whose time has arrived. Lines are taken in file
 * order, so a line scripted for an earlier time than its predecessor fires
 * late rather than never. */

static void inj_step(unsigned iter)
{
    const unsigned ms = (unsigned)(SDL_GetTicks() - s_init_ticks_ms); /* clock-guard: wall */ /* the harness clock */
    while (s_inj_next < s_inj_n) {
        const inj_line_t* L = &s_inj[s_inj_next];
        if ((L->is_ms ? ms : iter) < L->iter) break;
        s_inj_next++;
        switch (L->verb) {
        case INJ_MOVE:
            inj_push_motion(L->a, L->b, L->a - s_state.px, L->b - s_state.py);
            break;
        case INJ_REL:
            inj_push_motion(s_state.px + L->a, s_state.py + L->b, L->a, L->b);
            break;
        case INJ_DOWN:   inj_push_button(L->button, 1, 1); break;
        case INJ_UP:     inj_push_button(L->button, 0, 1); break;
        case INJ_CLICK2:
            inj_push_button(L->button, 1, 2);
            inj_push_button(L->button, 0, 2);
            break;
        case INJ_WHEEL:  inj_push_wheel(L->a); break;
        case INJ_MENU:   openpete_host_mouse_request_menu(L->a != 0.0f); break;
        case INJ_CAPTURE:
            openpete_host_mouse_request_capture(OPENPETE_MOUSE_CAP_HARNESS,
                                                L->a != 0.0f);
            break;
        case INJ_SHOT:
            atomic_store_explicit(&s_shot_req, 1, memory_order_release);
            break;
        case INJ_KEYDOWN: inj_push_key(L->button, 1); break;
        case INJ_KEYUP:   inj_push_key(L->button, 0); break;
        }
        if (s_probe)
            OPL_INFO_F("mouse", "inject iter=%u ms=%u line=%d verb=%d", iter, ms,
                       s_inj_next - 1, (int)L->verb);
    }
}

/* ---- init -------------------------------------------------------------- */

void openpete_host_mouse_init(struct SDL_Window* fallback_window)
{
    if (s_inited) return;
    s_fallback_window = fallback_window;
    const char* p = getenv("OPENPETE_MOUSE_PROBE");
    s_probe = (p && *p && *p != '0') ? 1 : 0;
    const char* raw = getenv("OPENPETE_MOUSE_INJECT_RAW");
    s_inj_raw = (raw && *raw && *raw != '0') ? 1 : 0;
    const char* inj = getenv("OPENPETE_MOUSE_INJECT");
    if (inj && *inj) inj_load(inj);
    SDL_Window* w = mouse_window();
    if (w) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(w, &ww, &wh);
        s_state.win_w = (float)ww; s_state.win_h = (float)wh;
    }
    s_init_ticks_ms = SDL_GetTicks(); /* clock-guard: wall */ /* the harness clock */
    s_inited = 1;
    if (s_probe)
        OPL_INFO_F("mouse", "probe on; window %gx%g points", s_state.win_w,
                   s_state.win_h);
}

void openpete_host_mouse_set_enabled(int on)
{
    atomic_store_explicit(&s_enabled, on ? 1 : 0, memory_order_relaxed);
}

int openpete_host_mouse_enabled(void)
{
    return atomic_load_explicit(&s_enabled, memory_order_relaxed);
}

int openpete_host_mouse_accepting(void)
{
    return openpete_host_mouse_enabled() && !openpete_input_lock_active();
}

int openpete_host_mouse_initialised(void) { return s_inited; }
int openpete_host_mouse_inject_active(void) { return s_inj_n > 0; }

/* ---- events ------------------------------------------------------------ */

static void refresh_window_size(Uint32 window_id)
{
    SDL_Window* w = window_id ? SDL_GetWindowFromID(window_id) : mouse_window();
    if (!w) return;
    int ww = 0, wh = 0;
    if (SDL_GetWindowSize(w, &ww, &wh) && ww > 0 && wh > 0) {
        s_state.win_w = (float)ww; s_state.win_h = (float)wh;
    }
}

static void add_motion(float xrel, float yrel)
{
    float fx = xrel + s_rem_x, fy = yrel + s_rem_y;
    float ix = truncf(fx), iy = truncf(fy);
    s_rem_x = fx - ix; s_rem_y = fy - iy;
    if (ix != 0.0f)
        atomic_fetch_add_explicit(&s_motion[0], (int64_t)ix, memory_order_relaxed);
    if (iy != 0.0f)
        atomic_fetch_add_explicit(&s_motion[1], (int64_t)iy, memory_order_relaxed);
}

void openpete_host_mouse_handle_event(const union SDL_Event* evu)
{
    if (!s_inited || !evu) return;
    const SDL_Event* ev = (const SDL_Event*)evu;
    /* The switch drops pointer events from real devices. Window enter, leave,
     * focus and resize events carry no pointer data and stay, so the window
     * size the consumers read is current whichever way the switch is set. */
    if (!openpete_host_mouse_accepting()) {
        switch (ev->type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (ev->motion.which != INJECT_MOUSE_ID) return;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev->button.which != INJECT_MOUSE_ID) return;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (ev->wheel.which != INJECT_MOUSE_ID) return;
            break;
        default: break;
        }
    }
    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
        refresh_window_size(ev->motion.windowID);
        s_state.px = ev->motion.x; s_state.py = ev->motion.y;
        add_motion(ev->motion.xrel, ev->motion.yrel);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = ev->button.button;
        if (b < 1 || b > OPENPETE_MOUSE_BUTTONS) break;
        unsigned bit = 1u << (b - 1);
        s_state.px = ev->button.x; s_state.py = ev->button.y;
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            s_state.buttons |= bit;
            atomic_fetch_add_explicit(&s_press[b - 1], 1u, memory_order_relaxed);
        } else {
            s_state.buttons &= ~bit;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        float y = ev->wheel.y;
        if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
        long long units = llroundf(y * (float)OPENPETE_MOUSE_WHEEL_UNIT);
        if (units)
            atomic_fetch_add_explicit(&s_wheel, (int64_t)units, memory_order_relaxed);
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_ENTER: s_state.in_window = 1; break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: s_state.in_window = 0; break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* SDL applies relative mode only to the focused window and restores
         * it on focus gain; the probe records the transition so a capture
         * check across a focus change names its instant. */
        if (s_probe)
            OPL_INFO_F("mouse", "focus %s (capture requested=%d applied=%d)",
                       ev->type == SDL_EVENT_WINDOW_FOCUS_GAINED ? "gained" : "lost",
                       openpete_host_mouse_capture_requested(),
                       openpete_host_mouse_captured());
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        refresh_window_size(ev->window.windowID);
        break;
    default: break;
    }
}

void openpete_host_mouse_pump_iteration(void)
{
    if (!s_inited) return;
    unsigned it = atomic_fetch_add_explicit(&s_pump_iter, 1u, memory_order_relaxed) + 1u;

    /* Channel-on-change log, one line per iteration at most, describing the
     * previous iteration's events. */
    if (s_probe) {
        static openpete_mouse_state_t last;
        static uint64_t last_press[OPENPETE_MOUSE_BUTTONS];
        static int64_t  last_motion[2], last_wheel;
        uint64_t pr[OPENPETE_MOUSE_BUTTONS];
        int64_t mo[2], wh;
        int changed = memcmp(&last, &s_state, sizeof last) != 0;
        for (int i = 0; i < OPENPETE_MOUSE_BUTTONS; i++) {
            pr[i] = atomic_load_explicit(&s_press[i], memory_order_relaxed);
            if (pr[i] != last_press[i]) changed = 1;
        }
        for (int i = 0; i < 2; i++) {
            mo[i] = atomic_load_explicit(&s_motion[i], memory_order_relaxed);
            if (mo[i] != last_motion[i]) changed = 1;
        }
        wh = atomic_load_explicit(&s_wheel, memory_order_relaxed);
        if (wh != last_wheel) changed = 1;
        if (changed) {
            OPL_INFO_F("mouse", "iter=%u pos=%g,%g win=%gx%g mask=0x%x in=%d "
                       "press=%llu/%llu/%llu/%llu/%llu dx=%lld dy=%lld wheel=%lld "
                       "owned=%d captured=%d",
                       it - 1u, s_state.px, s_state.py, s_state.win_w, s_state.win_h,
                       s_state.buttons, (int)s_state.in_window,
                       (unsigned long long)pr[0], (unsigned long long)pr[1],
                       (unsigned long long)pr[2], (unsigned long long)pr[3],
                       (unsigned long long)pr[4], (long long)mo[0], (long long)mo[1],
                       (long long)wh, openpete_host_mouse_owned(),
                       openpete_host_mouse_captured());
            last = s_state;
            memcpy(last_press, pr, sizeof pr);
            memcpy(last_motion, mo, sizeof mo);
            last_wheel = wh;
        }
    }

    inj_step(it);
}

unsigned openpete_host_mouse_pump_iterations(void)
{
    return atomic_load_explicit(&s_pump_iter, memory_order_relaxed);
}

void openpete_host_mouse_producer_read(openpete_mouse_state_t* out)
{
    if (out) *out = s_state;
}

/* ---- totals ------------------------------------------------------------ */

uint64_t openpete_host_mouse_press_total(int b)
{
    if (b < 0 || b >= OPENPETE_MOUSE_BUTTONS) return 0;
    return atomic_load_explicit(&s_press[b], memory_order_relaxed);
}

int64_t openpete_host_mouse_motion_total(int axis)
{
    if (axis < 0 || axis > 1) return 0;
    return atomic_load_explicit(&s_motion[axis], memory_order_relaxed);
}

int64_t openpete_host_mouse_wheel_total(void)
{
    return atomic_load_explicit(&s_wheel, memory_order_relaxed);
}

/* ---- ownership --------------------------------------------------------- */

static int devui_owner(void)
{
    return openpete_devui_is_open() || openpete_devui_wants_mouse();
}

int openpete_host_mouse_owned(void)
{
    if (devui_owner()) return OPENPETE_MOUSE_OWNED_DEVUI;
    if (openpete_menu_is_open()) return OPENPETE_MOUSE_OWNED_MENU;
    return OPENPETE_MOUSE_OWNED_NONE;
}

/* ---- capture ----------------------------------------------------------- */

void openpete_host_mouse_request_capture(int owner, int on)
{
    size_t word = (size_t)owner >> 6;
    uint64_t bit = (uint64_t)1 << ((unsigned)owner & 63u);
    uint64_t* w;
    int was;
    if (owner < 0) return;
    if (!s_capture_mx) {
        SDL_Mutex* mx = SDL_CreateMutex();
        if (!mx) return;
        /* Two first requests can race here; the loser drops its mutex
         * and uses the winner's. */
        if (SDL_CompareAndSwapAtomicPointer((void**)&s_capture_mx, NULL, mx)
            == false)
            SDL_DestroyMutex(mx);
    }
    SDL_LockMutex(s_capture_mx);
    if (!s_capture_req.elem_size)
        /* Census unit is words: the request set used to be exactly one
         * 64-bit word, which is why owner ids above 63 had no bit. */
        mod_fvec_init(&s_capture_req, sizeof(uint64_t), 1, "mouse_cap_bits",
                      1);
    if (mod_fvec_count(&s_capture_req) <= word &&
        mod_fvec_resize(&s_capture_req, word + 1) != 0) {
        SDL_UnlockMutex(s_capture_mx);
        return;
    }
    w = (uint64_t*)mod_fvec_at(&s_capture_req, word);
    was = (*w & bit) != 0;
    if (on && !was) {
        *w |= bit;
        atomic_fetch_add_explicit(&s_capture_n, 1, memory_order_relaxed);
    } else if (!on && was) {
        *w &= ~bit;
        atomic_fetch_sub_explicit(&s_capture_n, 1, memory_order_relaxed);
    }
    SDL_UnlockMutex(s_capture_mx);
}

int openpete_host_mouse_capture_requested(void)
{
    return atomic_load_explicit(&s_capture_n, memory_order_relaxed) != 0;
}

int openpete_host_mouse_captured(void)
{
    return atomic_load_explicit(&s_captured, memory_order_relaxed);
}

void openpete_host_mouse_apply_capture(void)
{
    if (!s_inited) return;
    /* The fill-tag inspector is excluded outright: under relative mode imgui
     * still receives the clamped pointer, and a WantCaptureMouse flip as it
     * drifts over the panel would release and re-capture in a loop. */
    int want = openpete_host_mouse_accepting()
            && openpete_host_mouse_capture_requested()
            && !openpete_menu_is_open()
            && !devui_owner()
            && !openpete_fillpick_active();
    static int applied = -1;
    if (want == applied) return;
    SDL_Window* w = mouse_window();
    if (!w) return;
    if (!SDL_SetWindowRelativeMouseMode(w, want ? true : false))
        OPL_WARN_F("mouse", "relative mode %d: %s", want, SDL_GetError());
    applied = want;
    atomic_store_explicit(&s_captured, want, memory_order_relaxed);
    if (s_probe)
        OPL_INFO_F("mouse",
                   "capture %s (req_owners=%d menu=%d devui=%d fillpick=%d enabled=%d)",
                   want ? "on" : "off",
                   atomic_load_explicit(&s_capture_n, memory_order_relaxed),
                   openpete_menu_is_open(), devui_owner(),
                   openpete_fillpick_active(), openpete_host_mouse_accepting());
}

/* ---- menu request ------------------------------------------------------ */

void openpete_host_mouse_request_menu(int open)
{
    atomic_store_explicit(&s_menu_req, open ? 2 : 1, memory_order_release);
}

int openpete_host_mouse_take_menu_request(int* open)
{
    int r = atomic_exchange_explicit(&s_menu_req, 0, memory_order_acq_rel);
    if (!r) return 0;
    if (open) *open = (r == 2);
    return 1;
}

int openpete_host_mouse_take_screenshot_request(void)
{
    return atomic_exchange_explicit(&s_shot_req, 0, memory_order_acq_rel);
}

/* ---- double-click gate ------------------------------------------------- */

int openpete_host_mouse_double_click_allowed(void)
{
    return !openpete_menu_is_open()
        && !openpete_host_mouse_capture_requested()
        && !openpete_keymap_mouse_left_bound();
}

void openpete_host_mouse_double_click_engaged(void)
{
    unsigned n = atomic_fetch_add_explicit(&s_dbl_count, 1u, memory_order_relaxed) + 1u;
    if (s_probe) OPL_INFO_F("mouse", "double-click fullscreen engaged (count=%u)", n);
}

unsigned openpete_host_mouse_double_click_count(void)
{
    return atomic_load_explicit(&s_dbl_count, memory_order_relaxed);
}

/* ---- probe ------------------------------------------------------------- */

int openpete_host_mouse_probe(void) { return s_probe; }

void openpete_host_mouse_probe_log(const char* consumer, const char* fmt, ...)
{
    if (!s_probe) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    OPL_INFO_F("mouse", "%s: %s", consumer ? consumer : "?", msg);
}
