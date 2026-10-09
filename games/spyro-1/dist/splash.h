// games/spyro-1/dist/splash.h - first-run splash & disc resolver.
#ifndef SPYRO1_SPLASH_H
#define SPYRO1_SPLASH_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opens the pre-game splash window, its renderer and the first connected gamepad.
 *
 * Initializes SDL video and gamepad, creates the window at the size
 * spyro1_splash_set_window_size requested (else 640x448), and draws in a
 * fixed 640x448 logical space letterboxed into it. Callers tolerate -1 (no
 * display); the console path works without the splash.
 * @return 0 when open (including already open), -1 on an SDL failure. */
int  spyro1_splash_open(void);
/** @brief Sets whether the splash UI may show, from main as the negation of headless.
 *
 * @param enabled Non-zero to allow the splash. */
void spyro1_splash_set_enabled(int enabled);
/** @brief Reports whether the splash UI may show.
 *
 * @return The value spyro1_splash_set_enabled stored. */
int  spyro1_splash_ui_enabled(void);
/** @brief Closes the splash's gamepad, renderer and window. */
void spyro1_splash_close(void);
/** @brief Reports whether the splash window is open.
 *
 * @return 1 when open, else 0. */
int  spyro1_splash_active(void);

/** @brief Sets the size the splash window opens at.
 *
 * Called before the first open; 0 selects 640x448, the game window's
 * default size. An explicit --window carries through so the vk-mode
 * adoption needs no resize.
 * @param w The window width, or 0.
 * @param h The window height, or 0. */
void spyro1_splash_set_window_size(int w, int h);

/** @brief Hands the splash's OS window to the caller, destroying the splash's renderer and gamepad.
 *
 * Windowed vk mode adopts the window instead of opening a second one. The
 * SDL_Renderer is destroyed here because its swapchain has to release the
 * window surface before SDL_Vulkan_CreateSurface can claim it. The splash is
 * inactive afterwards and the caller owns the window.
 * @return The SDL_Window* (void* keeps this header SDL-free), or NULL when the splash is not open. */
void* spyro1_splash_detach_window(void);

/** @brief Runs the drop screen until a disc image is dropped or chosen, or the user quits.
 *
 * A file dropped on the window, or chosen in the file dialog Space, Enter
 * or the gamepad's south button opens, is returned. Escape, the gamepad's
 * east button, or closing the window quits.
 * @return The path, owned by the splash, or NULL on quit or when the splash is not open. */
const char* spyro1_splash_wait_for_disc(void);

/** @brief Runs the version selector until a label is picked, the user quits, or a file is dropped.
 *
 * Shows at most six labels. Up and down (keys, d-pad or left stick) move the
 * selection; Enter or the gamepad's south button picks; Escape, the east
 * button or closing the window quits. A dropped file is stored for
 * spyro1_splash_dropped_path, for the caller to ingest. With the harness
 * variable OPENPETE_SPLASH_SELECT set to an index below count, that label is
 * picked after the first frame is drawn.
 * @param labels The entry labels.
 * @param count The number of labels.
 * @return The picked index, -1 on quit, when the splash is not open or when
 *         count is not positive, or -2 when a file was dropped. */
int spyro1_splash_select(const char* const* labels, int count);

/** @brief Returns the disc image path from the last file drop or file-dialog choice.
 *
 * The buffer belongs to the splash and is overwritten by the next drop or
 * choice; it is empty before the first. spyro1_splash_select returns -2 after
 * a drop, and spyro1_splash_wait_for_disc returns the same buffer.
 * @return The path, never NULL. */
const char* spyro1_splash_dropped_path(void);

/** @brief Reports a progress update: renders the stage line and bar, or stores it for the task runner.
 *
 * Wired as spyro1_progress_cb by main. A no-op when the splash UI is
 * disabled. Called from inside a spyro1_splash_run_task worker it only
 * stores the update under the task lock, and the runner loop renders it on
 * the UI thread. On the UI thread it opens the splash when not yet open
 * (returning when that fails), drains pending events and renders.
 * @param stage The stage text.
 * @param done The completed units.
 * @param total The total units, or 0 for no bar. */
void spyro1_splash_progress(const char* stage, int done, int total);

typedef int (*spyro1_task_fn)(void* userdata);
/** @brief Runs a blocking pipeline stage on a worker thread while the UI thread renders the splash.
 *
 * Serves the ingest copy and the first-boot module build: a Windows
 * title-bar drag traps the pumping thread in the OS modal move loop, so the
 * stage must not run there. Blocks until fn returns. While fn runs, the UI
 * thread polls and swallows events and redraws the last progress reported
 * through spyro1_splash_progress every 16 ms, and from an SDL event watch on
 * window expose and move events. The window opens on the first progress
 * report when it is not open and stays unopened when no display is
 * available. fn runs inline when the splash UI is disabled, the lock cannot
 * be created or the thread cannot be started. Not reentrant: do not call it
 * from inside a task.
 * @param fn The stage to run.
 * @param userdata Passed to fn.
 * @return fn's return value. */
int spyro1_splash_run_task(spyro1_task_fn fn, void* userdata);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_SPLASH_H */
