// games/spyro-1/splash.h — first-run splash & disc resolver (D2.5b).
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

/* DROP mode: block until a file is dropped (returns the path, owned by
 * the splash) or the user quits (NULL). */
const char* spyro1_splash_wait_for_disc(void);

/* SELECTOR mode: pick one of `count` labels. Returns the index, -1 on
 * quit, or -2 if a file was dropped instead (fetch it with
 * spyro1_splash_dropped_path() and ingest). */
int spyro1_splash_select(const char* const* labels, int count);
const char* spyro1_splash_dropped_path(void);

/* PROGRESS: render stage + bar and pump events. Safe to call when the
 * splash isn't open (no-op). Wired as spyro1_progress_cb by main. */
void spyro1_splash_progress(const char* stage, int done, int total);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_SPLASH_H */
