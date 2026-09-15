// games/spyro-1/dist/splash.h — first-run splash & disc resolver.
#ifndef SPYRO1_SPLASH_H
#define SPYRO1_SPLASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Open/close the pre-game splash window (no-op fail-soft: callers must
 * tolerate open() returning -1, e.g. no display — the console path
 * still works). */
int  spyro1_splash_open(void);
void spyro1_splash_set_enabled(int enabled);
int  spyro1_splash_ui_enabled(void);
void spyro1_splash_close(void);
int  spyro1_splash_active(void);

/* Size the splash window opens at (call before the first open; 0 =
 * the game window's default). Lets an explicit --window carry through
 * so the vk-mode adoption below needs no resize. */
void spyro1_splash_set_window_size(int w, int h);

/* Adoption handoff (windowed vk mode): destroy the splash's renderer +
 * gamepad but keep the OS window alive and return it (an SDL_Window*;
 * void* keeps this header SDL-free). The splash is inactive afterwards
 * and the caller owns the window. NULL if the splash never opened. */
void* spyro1_splash_detach_window(void);

/* DROP mode: block until a file is dropped (returns the path, owned by
 * the splash) or the user quits (NULL). */
const char* spyro1_splash_wait_for_disc(void);

/* SELECTOR mode: pick one of `count` labels. Returns the index, -1 on
 * quit, or -2 if a file was dropped instead (fetch it with
 * spyro1_splash_dropped_path() and ingest). */
int spyro1_splash_select(const char* const* labels, int count);
const char* spyro1_splash_dropped_path(void);

/* PROGRESS: render stage + bar and pump events. Safe to call when the
 * splash isn't open (no-op). Wired as spyro1_progress_cb by main.
 * Thread-aware: called from inside a spyro1_splash_run_task worker it
 * only stores the update (the runner loop renders on the UI thread). */
void spyro1_splash_progress(const char* stage, int done, int total);

/* Run a blocking pipeline stage (ingest copy, first-boot module build)
 * on a worker thread while the UI thread keeps pumping + rendering the
 * splash — a Windows title-bar drag traps the pumping thread in the OS
 * modal move loop, and must not stall the compile. Blocks until fn
 * returns; returns fn's return value. Falls back to calling fn directly
 * when the splash UI is disabled (headless/console) or thread creation
 * fails. Not reentrant: don't call from inside a task. */
typedef int (*spyro1_task_fn)(void* userdata);
int spyro1_splash_run_task(spyro1_task_fn fn, void* userdata);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_SPLASH_H */
