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

/* Capture request owners. A mod takes OPENPETE_MOUSE_CAP_MOD0 plus its
 * load index, so the owner ids run as far as the mod set does; the
 * request set grows to hold them and the count is only the size it starts
 * at. */
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

/** @brief Initialise the mouse channel once: read the probe and injection variables and seed the window size.
 *
 * Reads OPENPETE_MOUSE_PROBE, OPENPETE_MOUSE_INJECT_RAW and
 * OPENPETE_MOUSE_INJECT (loading that script), takes the window size in
 * points from the mouse window, and starts the millisecond clock the
 * script's "<n>ms" times count from. A second call does nothing. Until
 * this runs, the event handler, pump iteration and capture apply do
 * nothing.
 * @param fallback_window the window capture and injected events address
 *        when the native renderer has none (headless keeps a hidden SDL
 *        window). */
void openpete_host_mouse_init(struct SDL_Window* fallback_window);
/** @brief Return whether openpete_host_mouse_init has run.
 *
 * @return 1 after the first init call, else 0. */
int  openpete_host_mouse_initialised(void);
/** @brief Return 1 when an OPENPETE_MOUSE_INJECT script loaded at least one line.
 *
 * The dev overlay then follows the channel's event-derived position
 * instead of SDL's own, which a pushed event never moves, so the script's
 * hover reaches imgui widgets. The exit summary prints the host-input end
 * state when it is set.
 * @return 1 when a script is loaded, else 0. */
int  openpete_host_mouse_inject_active(void);

/** @brief Fold one dequeued SDL event into the channel's state and totals, before any other handler sees it.
 *
 * While real mouse input is not accepted (the switch off or the input lock
 * on), motion, button and wheel events not carrying the injection device
 * id are dropped; window events stay, so the window size is always
 * current. Motion sets the position and adds to the motion totals, with
 * the fractional remainder carried; a button event for buttons 1-5 sets
 * the position and mask, and a press adds to that button's press total;
 * the wheel adds in 1/256 notch, flipped direction negated. Enter and
 * leave set the in-window flag; resize refreshes the window size. Does
 * nothing before openpete_host_mouse_init.
 * @param ev the event; NULL is ignored. */
void openpete_host_mouse_handle_event(const union SDL_Event* ev);

/** @brief Set the mouse switch.
 *
 * Off drops real mouse motion, button and wheel events and refuses
 * capture; the injected stream is unaffected. Set from the options at
 * host init and from the knob registry afterwards; read on the pump
 * thread.
 * @param on nonzero turns real mouse input on. */
void openpete_host_mouse_set_enabled(int on);
/** @brief Return the mouse switch's own value.
 *
 * The input lock does not change it; openpete_host_mouse_accepting combines the
 * two.
 * @return 1 when the switch is on, else 0. */
int  openpete_host_mouse_enabled(void);
/** @brief Return whether real mouse input is taken: the switch is on and the input lock is off.
 *
 * Every site that gates on the switch reads this, so the input lock
 * (openpete_input_lock.h) implies the switch's off behaviour without
 * changing the value openpete_host_mouse_enabled reports.
 * @return 1 when accepting, else 0. */
int  openpete_host_mouse_accepting(void);

/** @brief Advance the pump iteration counter and push this iteration's injected events; call once per pump iteration, before polling.
 *
 * Under OPENPETE_MOUSE_PROBE, first logs one line describing the channel
 * when the state or any total changed during the previous iteration. Does
 * nothing before openpete_host_mouse_init. */
void openpete_host_mouse_pump_iteration(void);
/** @brief Return how many pump iterations openpete_host_mouse_pump_iteration has counted.
 *
 * @return the count since boot; 0 before the mouse channel is initialised. */
unsigned openpete_host_mouse_pump_iterations(void);

/** @brief Copy the producer's event-derived mouse state, for the host input snapshot publish.
 *
 * Pump thread only.
 * @param out receives the state; NULL is ignored. */
void openpete_host_mouse_producer_read(openpete_mouse_state_t* out);

/** @brief Apply the capture decision (SDL relative mode) to the mouse window, edge-triggered.
 *
 * Capture is wanted when real input is accepted, some owner requested it,
 * the O menu is closed, the dev overlay does not own the mouse, and the
 * fill-tag inspector is inactive. SDL is called only when the decision
 * changes from the last applied one. Runs on the window-owning thread,
 * next to the cursor idle-hide. Does nothing before
 * openpete_host_mouse_init or while there is no window. */
void openpete_host_mouse_apply_capture(void);

/* ---- totals (any thread) --------------------------------------------- */

/** @brief Return the running count of presses of one mouse button since boot.
 *
 * @param button_index OPENPETE_MOUSE_BTN_*.
 * @return the count, or 0 for an out-of-range index. */
uint64_t openpete_host_mouse_press_total(int button_index);
/** @brief Return the running relative-motion count on one axis since boot.
 *
 * Whole counts only: the fractional remainder of SDL's float motion is carried
 * into later events.
 * @param axis 0 for x, 1 for y.
 * @return the signed count, or 0 for any other axis. */
int64_t  openpete_host_mouse_motion_total(int axis);   /* 0 = x, 1 = y; counts */
/** @brief Return the running wheel travel since boot in 1/256 notch units.
 *
 * @return the signed total, up positive. */
int64_t  openpete_host_mouse_wheel_total(void);        /* 1/256 notch, up positive */

/* ---- ownership and capture --------------------------------------------*/

/** @brief Return which consumer owns the mouse.
 *
 * The dev overlay owns it while open or while imgui wants the mouse, and
 * takes precedence over the O menu, which owns it while open.
 * @return OPENPETE_MOUSE_OWNED_DEVUI, OPENPETE_MOUSE_OWNED_MENU, or
 *         OPENPETE_MOUSE_OWNED_NONE. */
int  openpete_host_mouse_owned(void);
/** @brief Set or clear one owner's capture request bit.
 *
 * The request set grows to hold the owner id and is guarded by a mutex created
 * on first use. Capture is applied later by openpete_host_mouse_apply_capture,
 * and only while every other condition allows it. A negative owner is ignored,
 * and a failed allocation leaves the request unrecorded.
 * @param owner an OPENPETE_MOUSE_CAP_* id, or OPENPETE_MOUSE_CAP_MOD0 plus a
 *        mod's load index.
 * @param on nonzero to request capture, zero to withdraw the request. */
void openpete_host_mouse_request_capture(int owner, int on);
/** @brief Return whether any owner has a capture request set.
 *
 * @return 1 when at least one owner bit is set, else 0. */
int  openpete_host_mouse_capture_requested(void);   /* any owner bit set */
/** @brief Return whether SDL relative mode is applied to the mouse window.
 *
 * The value is what openpete_host_mouse_apply_capture last decided, 0 before
 * its first application.
 * @return 1 when capture is applied, else 0. */
int  openpete_host_mouse_captured(void);            /* applied state */

/* ---- the menu request flag ------------------------------------------- */

/** @brief Request a target open state for the O menu, consumed by openpete_host_mouse_take_menu_request.
 *
 * Raised on any thread (the pump's Escape dismiss, the `menu` inject verb)
 * and consumed by the host loop at its poll point, so the change lands
 * between two reads of the pad-override clear latch. A later request
 * before consumption replaces an earlier one.
 * @param open nonzero requests open, zero requests closed. */
void openpete_host_mouse_request_menu(int open);
/** @brief Take and clear a pending O menu request.
 *
 * @param open receives 1 for an open request and 0 for a close request when one
 *        was pending; NULL is ignored.
 * @return 1 when a request was pending, else 0. */
int  openpete_host_mouse_take_menu_request(int* open);

/** @brief Take and clear the screenshot request the `shot` inject verb raises.
 *
 * The host loop consumes it as a player screenshot (the F12 path, a live
 * capture of the presented window), so a scripted run can show what the
 * pointer did.
 * @return 1 when a request was pending, else 0. */
int  openpete_host_mouse_take_screenshot_request(void);

/* ---- double-click fullscreen gate ------------------------------------ */

/** @brief Return 1 when a left double-click may toggle fullscreen.
 *
 * Allowed when the O menu is closed, no binding capture is active
 * (openpete_keymap_capture_active), no owner has requested capture, and
 * no host or mod action binds mouse:left. The caller separately excludes a
 * click the dev overlay wants. Pump thread.
 * @return 1 when allowed, else 0. */
int      openpete_host_mouse_double_click_allowed(void);
/** @brief Count one left double-click that toggled fullscreen.
 *
 * Called by the consumer that toggles fullscreen. Logs the running count under
 * OPENPETE_MOUSE_PROBE. */
void     openpete_host_mouse_double_click_engaged(void);
/** @brief Return how many double-click fullscreen toggles have been counted.
 *
 * @return the count since boot. */
unsigned openpete_host_mouse_double_click_count(void);

/* ---- probe ----------------------------------------------------------- */

/** @brief Return whether OPENPETE_MOUSE_PROBE is on.
 *
 * With it on, the channel is logged on change and every consumer gesture
 * is logged with its outcome through openpete_host_mouse_probe_log. The
 * variable is read at openpete_host_mouse_init; before that this returns 0.
 * @return 1 when the probe is on, else 0. */
int  openpete_host_mouse_probe(void);
/** @brief Log one consumer gesture line when OPENPETE_MOUSE_PROBE is on.
 *
 * The printf-style message is truncated to 255 characters and logged as
 * `<consumer>: <message>`. Does nothing with the probe off.
 * @param consumer the consumer's name; NULL logs as ?.
 * @param fmt printf format for the message. */
void openpete_host_mouse_probe_log(const char* consumer, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HOST_MOUSE_H */
