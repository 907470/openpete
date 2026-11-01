#ifndef OPENPETE_DEVUI_H
#define OPENPETE_DEVUI_H

/*
 * M-key dev config overlay - flat C API over the ImGui panel
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
 *    bookkeeping; imgui frames run only while the overlay is open, the
 *    fill-tag inspector is active, or a mod's always section is enabled),
 *    and the pump thread calls openpete_devui_pump_text_input after each
 *    drain;
 *  - while wants_keyboard() the host hotkey block and the keyboard->pad path
 *    must be gated off so typed text never steers the game, except that the
 *    overlay's own toggle action stays readable unless wants_text_input();
 *  - while wants_mouse() the double-click-fullscreen shortcut is skipped.
 * The sim keeps running and the pad is NOT suppressed - this is a dev tool;
 * watching the live scene while flipping knobs is the point.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_options;
struct rhi_device;
struct SDL_Window;
union SDL_Event;

/** @brief Create the imgui context and its SDL3 and Vulkan backends against a window and its device.
 *
 * Call after rhi_create. The launcher calls it on the device it creates and
 * draws through openpete_devui_render_frame; the overlay itself stays
 * unattached until openpete_devui_init. Stores window state in
 * openpete-devui.ini beside the executable, one file for every game and
 * the launcher. A second call on the same device returns 1 at once.
 * @param dev The device.
 * @param win Its window.
 * @return 1 when the context exists on @p dev, 0 when a handle is missing,
 *         the device exposes no imgui info (headless), a backend fails to
 *         initialise, or the context exists on another device. */
int  openpete_devui_create(struct rhi_device *dev, struct SDL_Window *win);

/** @brief Adopt the newest per-game overlay ini as openpete-devui.ini, once per process.
 *
 * When openpete-devui.ini does not exist beside the executable and one or
 * more openpete-devui-<game id>.ini files (written by builds that kept one
 * ini per game) do, the one with the latest modification time is copied to
 * openpete-devui.ini through a temporary renamed into place; every
 * per-game file is only read and stays in place. Runs headless too: it
 * touches files only. openpete_devui_create calls it before loading the
 * ini; the session start calls it so a headless run migrates as well.
 * @return 1 when a file was copied, 0 when nothing was due, -1 when the
 *         copy failed. */
int  openpete_devui_seed_ini(void);

/** @brief Attach the dev overlay to the session: create the imgui context unless the launcher did, then seed the knob registry.
 *
 * Call after rhi_create. Creates the context through openpete_devui_create
 * when none exists, seeds the knob registry from the options, latches
 * OPENPETE_DEVUI_INJECT_MOUSE, and opens the overlay at boot when
 * openpete_devui_open_requested says so. A second call after success
 * returns 1 at once. On failure every other call is a no-op.
 * @param dev The native render device.
 * @param win The native window.
 * @param opts The parsed options.
 * @return 1 on success, 0 when openpete_devui_create fails. */
int  openpete_devui_init(struct rhi_device *dev, struct SDL_Window *win,
                         const struct openpete_options *opts);

/** @brief Record one imgui frame whose content a callback draws, into the current offscreen pass.
 *
 * The launcher's per-present hook: called between rhi_begin_offscreen and
 * rhi_end_offscreen. Feeds imgui the queued SDL events
 * (openpete_devui_process_event), starts the frame, calls @p draw, and
 * records the draw data. Needs only the context (openpete_devui_create),
 * not the attached overlay.
 * @param dev The device the context was created on.
 * @param draw Issues the frame's imgui calls.
 * @param ud Passed to @p draw.
 * @return 1 when a frame was recorded, 0 when no context exists on @p dev,
 *         @p draw is NULL or no command buffer is recording. */
int  openpete_devui_render_frame(struct rhi_device *dev, void (*draw)(void *ud), void *ud);

/** @brief Record one imgui frame as openpete_devui_render_frame does, with passes recorded under imgui's draw data.
 *
 * @p under runs after @p draw has built the frame and before the draw data
 * is recorded, in the same offscreen pass: a pass whose content the frame's
 * imgui calls decide (the launcher's shapes) draws beneath the frame's
 * imgui output.
 * @param dev The device the context was created on.
 * @param draw Issues the frame's imgui calls.
 * @param ud Passed to @p draw.
 * @param under Records the passes under imgui's output; NULL for none.
 * @param under_ud Passed to @p under.
 * @return As openpete_devui_render_frame. */
int  openpete_devui_render_frame_under(struct rhi_device *dev, void (*draw)(void *ud), void *ud,
                                       void (*under)(void *under_ud), void *under_ud);

/** @brief Destroy the imgui backends and context and return the overlay to its uninitialised state.
 *
 * Clears the open and boot-reveal state and the stored device. No-op before
 * init or after a previous shutdown. Must run before the render device is
 * destroyed. */
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

/** @brief Report whether openpete_devui_init succeeded and shutdown has not run.
 *
 * @return 1 while the imgui context exists, else 0. */
int  openpete_devui_available(void);

/** @brief Flip the overlay between open and closed.
 *
 * @return The new open state, 1 for open; 0 when closed or before init, in
 *         which case nothing changed. */
int  openpete_devui_toggle(void);

/** @brief Report whether the config panel is open.
 *
 * Reads the open flag, which is a plain bool with no synchronisation.
 * @return 1 when initialised and open, else 0. */
int  openpete_devui_is_open(void);

/** @brief Close the config panel.
 *
 * Clears the open flag whether or not the overlay was initialised; a
 * later frame still runs while the inspector or an always mod section is up. */
void openpete_devui_close(void);

/** @brief Open the config panel scrolled to its top, every section as the user left it.
 *
 * Host loop: sets the open flag and posts a one-shot request the next panel
 * frame consumes, which expands a collapsed window and scrolls it to the
 * top. Logs "open at top". No-op before init. */
void openpete_devui_open_top(void);

/** @brief Run the overlay's host-loop input step: the `devui` toggle and the pad navigation snapshot.
 *
 * Call once per host-loop iteration after openpete_keymap_poll. A press of
 * `devui` opens the overlay (closing the settings menu); while it is open a
 * press arms the close and the release closes it, so a held button never
 * reaches the game after the overlay lets the pad go. While open, publishes
 * the levels of the KM_CTX_DEVUI actions and whether player 1's pad is
 * connected for the imgui frame, and counts, per open, the iterations with
 * pad navigation engaged and the presses of the move and activate actions
 * made while engaged. Each open and close is logged; the close line carries
 * the counts and imgui's own gamepad move and activation counts. */
void openpete_devui_host_poll(void);

/** @brief Report whether the overlay navigates with the pad: open, and an imgui window holds navigation focus.
 *
 * Published by the imgui frame after it records (one present stale). A
 * click outside every overlay window clears imgui's navigation focus and
 * returns 0, which hands the pad back to the game.
 * @return 1 while engaged; 0 before init, while closed, or otherwise. */
int  openpete_devui_padnav_engaged(void);

/** @brief Queue one SDL event for imgui; call for every event, open or closed.
 *
 * The pump and the imgui frame may run on different threads, so the event
 * goes into a single-producer, single-consumer ring of 512 slots that
 * openpete_devui_render drains on the frame's thread. A full ring drops the
 * newest event, not the oldest, so a press is never lost ahead of its
 * release; the drop count is logged on the first drop and every 256th.
 * Only the types imgui's SDL3 backend reads are queued; the rest are
 * ignored. No-op before openpete_devui_create.
 * @param ev The event; NULL is ignored. */
void openpete_devui_process_event(const union SDL_Event *ev);

/** @brief Report whether imgui captured the keyboard on the last drawn present.
 *
 * Reads the want openpete_devui_render published after its last imgui
 * frame; bare navigation focus counts. Always 0 while the overlay is closed.
 * @return 1 while imgui wants the keyboard; 0 before init, while closed, or
 *         otherwise. */
int  openpete_devui_wants_keyboard(void);
/** @brief Report whether an imgui text field has focus and consumes characters.
 *
 * The subset of openpete_devui_wants_keyboard in which a field takes text;
 * bare navigation focus reads 0 here. Always 0 while the overlay is closed.
 * @return 1 while a text field has focus; 0 before init, while closed, or
 *         otherwise. */
int  openpete_devui_wants_text_input(void);
/** @brief Start or stop SDL text input on the overlay's window to follow openpete_devui_wants_text_input.
 *
 * SDL_StartTextInput and SDL_StopTextInput belong to the thread that pumps
 * the window's events, while the imgui frame that decides a text field has
 * focus runs on the present thread, so the imgui backend's own IME hook is
 * left uninstalled and the pump thread calls this after each drain. Calls SDL
 * only when the published want differs from the state it last applied. */
void openpete_devui_pump_text_input(void);
/** @brief Report whether imgui captured the mouse on the last drawn present.
 *
 * Holds with the overlay open, and also with it closed while the fill-tag
 * inspector is active.
 * @return 1 while imgui wants the mouse; 0 before init, with neither the
 *         overlay nor the inspector up, or otherwise. */
int  openpete_devui_wants_mouse(void);

/** @brief Build and record one imgui frame into the current offscreen pass.
 *
 * Called from the live-present compose point, between rhi_begin_offscreen
 * (or rhi_begin_offscreen_load) and rhi_end_offscreen, at most once per
 * present. Drains the queued SDL events on every call after init, so imgui's
 * key and mouse state stays current while nothing is drawn. Draws the panel
 * when the overlay is open, the OPENPETE_MOD_UI_ALWAYS sections, and the
 * fill-tag inspector's crosshair and panel, then publishes the capture wants.
 * When none of the three is up, dev is not the init device, or the device
 * has no current command buffer, it records nothing and publishes every
 * want as 0. The first draw with the overlay open calls
 * knob_registry_late_stamp once.
 * @param dev The device the overlay was initialised on. */
void openpete_devui_render(struct rhi_device *dev);

/** @brief The state notes a mod row's tooltip can show.
 *
 * The table the tooltip draws from, exposed so the surface vocabulary check
 * scans the same strings the player reads.
 * @param count Receives the note count; may be NULL.
 * @return The static table. */
const char *const *openpete_devui_mod_notes(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_DEVUI_H */
