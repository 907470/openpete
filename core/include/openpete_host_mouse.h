#ifndef OPENPETE_HOST_MOUSE_H
#define OPENPETE_HOST_MOUSE_H

/* The host mouse channel.
 *
 * One producer, the SDL event pump, feeds every mouse consumer in the
 * process: the `mouse:` keymap sources, the O settings menu, the freecam and
 * camlock look term, and mods. The consumers run at different cadences on
 * different threads (the sim worker's host iteration, the game tick, the
 * present thread, a mod hook), so the channel publishes two shapes:
 *
 *   state    position, window size, button mask and in-window flag, derived
 *            from the event stream and copied into the host input snapshot
 *            at each publish (openpete_host_input_publish_sdl), so a
 *            consumer reads one consistent sample without touching SDL.
 *   totals   monotonic running counts since boot: presses per button,
 *            relative motion per axis, wheel travel. A consumer keeps the
 *            total it last read and takes `total - last`; nothing is lost
 *            across a stall, and every consumer receives the whole motion.
 *
 * Position and buttons are event-derived rather than read from
 * SDL_GetMouseState because a pushed event never updates SDL's own mouse
 * state, and the injection seam below pushes events.
 *
 * Ownership. While the O menu is open it owns the mouse; while the dev
 * overlay is open or an imgui present wants the mouse, the overlay owns it,
 * taking precedence over the menu (the fill-tag inspector panel is drawn
 * under the open menu). A total carries no consumer identity, so freezing
 * is the consumer's act: it reads the owner alongside the totals and, while
 * owned, snaps its markers to the current totals and reports neutral.
 *
 * Capture (SDL relative mode) is a per-owner request bit; the pump thread
 * computes `want` from the bits and the ownership state and applies it
 * edge-triggered on the window-owning thread.
 *
 * The switch (openpete_host_mouse_set_enabled, --mouse / OPENPETE_MOUSE) is
 * the channel's off position: with it off the handler drops every real mouse
 * event before it reaches the state or the totals, and capture resolves to
 * off, so the pointer stays a free desktop cursor over the window. Injected
 * events (below) are exempt, since a scripted capture drives them
 * deliberately. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

union SDL_Event;
struct SDL_Window;

/* Button mask bits and the press-counter indices (bit i <-> index i). */
#define OPENPETE_MOUSE_LEFT   0x01u
#define OPENPETE_MOUSE_MIDDLE 0x02u
#define OPENPETE_MOUSE_RIGHT  0x04u
#define OPENPETE_MOUSE_X1     0x08u
#define OPENPETE_MOUSE_X2     0x10u
#define OPENPETE_MOUSE_BUTTONS 5

enum {
    OPENPETE_MOUSE_BTN_LEFT = 0, OPENPETE_MOUSE_BTN_MIDDLE, OPENPETE_MOUSE_BTN_RIGHT,
    OPENPETE_MOUSE_BTN_X1, OPENPETE_MOUSE_BTN_X2
};

/* openpete_host_mouse_owned() values. */
enum {
    OPENPETE_MOUSE_OWNED_NONE  = 0,
    OPENPETE_MOUSE_OWNED_MENU  = 1,
    OPENPETE_MOUSE_OWNED_DEVUI = 2
};

/* Capture request owners. Mods take OPENPETE_MOUSE_CAP_MOD0 + index. */
enum {
    OPENPETE_MOUSE_CAP_CAMERA  = 0,   /* freecam or camlock */
    OPENPETE_MOUSE_CAP_HARNESS = 1,   /* the `capture` inject verb */
    OPENPETE_MOUSE_CAP_MOD0    = 2,
    OPENPETE_MOUSE_CAP_COUNT   = OPENPETE_MOUSE_CAP_MOD0 + 64
};

/* Wheel totals are in 1/256 of a notch so fractional touchpad wheels
 * accumulate; a consumer steps on the floor-advance of the notch count. */
#define OPENPETE_MOUSE_WHEEL_UNIT 256

typedef struct {
    float    px, py;        /* window points, origin top-left */
    float    win_w, win_h;  /* window size in points */
    unsigned buttons;       /* OPENPETE_MOUSE_* mask */
    uint8_t  in_window;
} openpete_mouse_state_t;

/* ---- producer (pump thread) ------------------------------------------ */

/* Parses OPENPETE_MOUSE_INJECT / OPENPETE_MOUSE_INJECT_RAW /
 * OPENPETE_MOUSE_PROBE and marks the channel initialised. `fallback_window` is the window capture applies to when the
 * native renderer has none (headless keeps a hidden SDL window). */
void openpete_host_mouse_init(struct SDL_Window* fallback_window);
int  openpete_host_mouse_initialised(void);
/* 1 when an inject script is loaded: imgui then follows the channel's
 * event-derived position instead of SDL's own (which a pushed event never
 * moves), so the script's hover reaches imgui widgets too. */
int  openpete_host_mouse_inject_active(void);

/* Every dequeued SDL event passes through here before any other handler. */
void openpete_host_mouse_handle_event(const union SDL_Event* ev);

/* The mouse switch. Off drops real mouse motion, button and wheel events and
 * refuses capture; the injected stream is unaffected. Set from the options at
 * host init and from the knob registry afterwards; read on the pump thread. */
void openpete_host_mouse_set_enabled(int on);
int  openpete_host_mouse_enabled(void);

/* Once per pump iteration, before the events are polled: advances the
 * iteration counter and pushes this iteration's injected events. */
void openpete_host_mouse_pump_iteration(void);
unsigned openpete_host_mouse_pump_iterations(void);

/* Copy of the producer's event-derived state, for the snapshot publish. */
void openpete_host_mouse_producer_read(openpete_mouse_state_t* out);

/* Applies the capture decision (SDL relative mode) edge-triggered. Runs on
 * the window-owning thread, next to the cursor idle-hide. */
void openpete_host_mouse_apply_capture(void);

/* ---- totals (any thread) --------------------------------------------- */

uint64_t openpete_host_mouse_press_total(int button_index);
int64_t  openpete_host_mouse_motion_total(int axis);   /* 0 = x, 1 = y; counts */
int64_t  openpete_host_mouse_wheel_total(void);        /* 1/256 notch, up positive */

/* ---- ownership and capture --------------------------------------------*/

int  openpete_host_mouse_owned(void);
void openpete_host_mouse_request_capture(int owner, int on);
int  openpete_host_mouse_capture_requested(void);   /* any owner bit set */
int  openpete_host_mouse_captured(void);            /* applied state */

/* ---- the menu request flag ------------------------------------------- */

/* A target open state for the O menu, raised on any thread (the pump's
 * Escape dismiss, the `menu` inject verb) and consumed by the host loop at
 * its poll point, so the toggle lands between two reads of the pad-override
 * clear latch. Repeated requests before consumption are idempotent. */
void openpete_host_mouse_request_menu(int open);
int  openpete_host_mouse_take_menu_request(int* open);

/* The `shot` inject verb asks for a player screenshot (the F12 path, a
 * live capture of the presented window), so a scripted run can show what
 * the pointer did. Consumed by the host loop. */
int  openpete_host_mouse_take_screenshot_request(void);

/* ---- double-click fullscreen gate ------------------------------------ */

/* 1 when a left double-click may toggle fullscreen: the menu is closed, no
 * owner has requested capture, and no host or mod action binds mouse:left.
 * Pump thread. */
int      openpete_host_mouse_double_click_allowed(void);
void     openpete_host_mouse_double_click_engaged(void);
unsigned openpete_host_mouse_double_click_count(void);

/* ---- probe ----------------------------------------------------------- */

/* OPENPETE_MOUSE_PROBE=1: the channel is logged on change and every
 * consumer gesture is logged with its outcome through this. */
int  openpete_host_mouse_probe(void);
void openpete_host_mouse_probe_log(const char* consumer, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HOST_MOUSE_H */
