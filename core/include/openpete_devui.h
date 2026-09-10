#ifndef OPENPETE_DEVUI_H
#define OPENPETE_DEVUI_H

/*
 * M-key dev config overlay — flat C API over the ImGui panel
 * (core/runtime/devui.cpp). Everything imgui-typed stays inside the .cpp.
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
 *    must be gated off so typed text never steers the game;
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

/* Create the imgui context + SDL3/Vulkan backends against the native vk
 * window and device. Call once, after rhi_create, with the parsed options
 * (used to seed the knob registry). Returns 1 on success, 0 if unavailable
 * (headless device / backend failure) — in which case all other calls no-op. */
int  openpete_devui_init(struct rhi_device *dev, struct SDL_Window *win,
                         const struct openpete_options *opts);
void openpete_devui_shutdown(void);

/* Whether --devui-open / OPENPETE_DEVUI_OPEN asks for the overlay to start
 * open. The CLI value wins when it is set (0 or 1); the env var decides
 * otherwise. Exposed so a caller that knows the overlay will never exist
 * (headless, --renderer psycross) can say so instead of the flag going
 * silently nowhere. */
int  openpete_devui_open_requested(const struct openpete_options *opts);

/* Whether --devui-inject-mouse / OPENPETE_DEVUI_INJECT_MOUSE carries a
 * parsable "X,Y" pair, and what it is. The overlay posts that position into
 * imgui's IO before every NewFrame, so an unattended run can park the
 * pointer over a widget and read the hover state back out of the log.
 * Returns 1 and fills @p x / @p y when set, 0 otherwise. */
int  openpete_devui_inject_mouse_requested(float *x, float *y);

/* Whether this present needs an imgui frame at all: the overlay is open, the
 * fill-tag inspector is active, or an enabled mod carries an
 * OPENPETE_MOD_UI_ALWAYS section. Every caller that gates a call to
 * openpete_devui_render must ask this and not openpete_devui_is_open, or an
 * always window disappears in that caller's path. */
int  openpete_devui_frame_wanted(void);

int  openpete_devui_available(void);
int  openpete_devui_toggle(void);        /* returns new open state */
int  openpete_devui_is_open(void);
void openpete_devui_close(void);

/* Forward one SDL event to imgui (call for EVERY event, open or closed). */
void openpete_devui_process_event(const union SDL_Event *ev);

/* True while imgui wants the keyboard/mouse (only ever true while open). */
int  openpete_devui_wants_keyboard(void);
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
