#ifndef OPENPETE_DEVUI_H
#define OPENPETE_DEVUI_H

/*
 * M-key dev config overlay — flat C API over the ImGui panel
 * (core/ui/devui.cpp). Everything imgui-typed stays inside the .cpp.
 *
 * Scope: native-vk live presents only. Never initialized headless or under
 * --renderer psycross; every call is a safe no-op before init/after shutdown.
 * The panel lists the knob registry (openpete_knobs.h): LIVE rows edit
 * through the registry's setters, RESTART_ONLY rows display the resolved
 * boot value + source.
 *
 * Input contract (run.c owns the wiring):
 *  - every SDL event is forwarded via openpete_devui_process_event (cheap io
 *    bookkeeping; imgui frames only run while the overlay is open);
 *  - while wants_keyboard() the host hotkey block and the keyboard->pad path
 *    must be gated off so typed text never steers the game, except that the
 *    overlay's own toggle action stays readable unless wants_text_input();
 *  - while wants_mouse() the double-click-fullscreen shortcut is skipped.
 * The sim keeps running and the pad is NOT suppressed - this is a dev tool;
 * watching the live scene while flipping knobs is the point.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_options;
struct rhi_device;
struct SDL_Window;
union SDL_Event;

/** @brief Create the imgui context and its SDL3 and Vulkan backends against the native vk window and device.
 *
 * Call after rhi_create. Seeds the knob registry from the options, latches
 * OPENPETE_DEVUI_INJECT_MOUSE, stores window state in openpete-devui.ini
 * beside the executable, and opens the overlay at boot when
 * openpete_devui_open_requested says so. A second call after success
 * returns 1 at once. On failure every other call is a no-op.
 * @param dev The native render device.
 * @param win The native window.
 * @param opts The parsed options.
 * @return 1 on success, 0 when a handle is missing, the device exposes no
 *         imgui info (headless), or a backend fails to initialise. */
int  openpete_devui_init(struct rhi_device *dev, struct SDL_Window *win,
                         const struct openpete_options *opts);
void openpete_devui_shutdown(void);

/** @brief Report whether --devui-open or OPENPETE_DEVUI_OPEN asks for the overlay to start open.
 *
 * The CLI value wins when set (0 or 1); the environment variable decides
 * otherwise. Exposed so a caller that knows the overlay will never exist
 * (headless, --renderer psycross) can say so instead of the request having
 * no effect.
 * @param opts The parsed options, or NULL to consult the environment only.
 * @return 1 when the overlay should start open, else 0. */
int  openpete_devui_open_requested(const struct openpete_options *opts);

/** @brief Parse OPENPETE_DEVUI_INJECT_MOUSE as an "X,Y" cursor position.
 *
 * The variable has no CLI spelling. The overlay latches the position at init
 * and posts it into imgui's IO before every NewFrame, so an unattended run
 * can park the pointer over a widget and read the hover state from the log.
 * @param x Receives X when the pair parses; may be NULL.
 * @param y Receives Y when the pair parses; may be NULL.
 * @return 1 when the variable holds a parsable pair, else 0. */
int  openpete_devui_inject_mouse_requested(float *x, float *y);

/** @brief Report whether this present needs an imgui frame at all.
 *
 * True when the overlay is open, the fill-tag inspector is active, or an
 * enabled mod carries an OPENPETE_MOD_UI_ALWAYS section. Every caller that
 * gates a call to openpete_devui_render asks this and not
 * openpete_devui_is_open, or an always window disappears in that caller's
 * path.
 * @return 1 when a frame is wanted; 0 before init or otherwise. */
int  openpete_devui_frame_wanted(void);

int  openpete_devui_available(void);
int  openpete_devui_toggle(void);        /* returns new open state */
int  openpete_devui_is_open(void);
void openpete_devui_close(void);

/** @brief Queue one SDL event for imgui; call for every event, open or closed.
 *
 * The pump and the imgui frame may run on different threads, so the event
 * goes into a single-producer, single-consumer ring of 512 slots that
 * openpete_devui_render drains on the frame's thread. A full ring drops the
 * newest event, not the oldest, so a press is never lost ahead of its
 * release; the drop count is logged on the first drop and every 256th.
 * No-op before init.
 * @param ev The event; NULL is ignored. */
void openpete_devui_process_event(const union SDL_Event *ev);

/* True while imgui wants the keyboard/mouse (only ever true while open).
 * wants_text_input() is the subset of wants_keyboard() in which a field has
 * focus and is consuming characters; bare nav focus reads 1 on the former
 * and 0 on the latter. */
int  openpete_devui_wants_keyboard(void);
int  openpete_devui_wants_text_input(void);
int  openpete_devui_wants_mouse(void);

/* Build + record the overlay into the current offscreen pass. Call from the
 * live-present compose point, between rhi_begin_offscreen and
 * rhi_end_offscreen, at most once per present. No-op unless
 * openpete_devui_frame_wanted(). */
void openpete_devui_render(struct rhi_device *dev);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_DEVUI_H */
