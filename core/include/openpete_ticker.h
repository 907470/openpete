// core/include/openpete_ticker.h — free-running 60 Hz vblank ticker.
//
// The ticker runs on its own SDL_AddTimer thread and is the *only* writer
// to `openpete_vbl` once started. It also signals a condvar each tick so
// the host fiber loop can pace itself off the wall clock instead of off
// the game's yield count. See plans/openpete-free-running-vblank-counter.md.
//
// When the ticker is not running (smoke-test path, headless determinism
// branch, fast-boot before first DrawOTag) the vbl counter is advanced
// synchronously inside psyq_VSync(0). `openpete_ticker_drives_vbl` is the
// flag callers test to decide which path to take.

#ifndef OPENPETE_TICKER_H
#define OPENPETE_TICKER_H

#include <SDL_atomic.h>
#include <SDL_mutex.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while the ticker is running. Readers (psyq_VSync, host loop)
 * use this to choose between condvar-pacing and synchronous-increment. */
extern int openpete_ticker_drives_vbl;

/* Host-loop wake primitive. The ticker signals this on every tick;
 * the host loop waits on it between game-fiber slices to gate the
 * next slice on a wall-clock vblank. */
extern SDL_cond*  openpete_ticker_wake_cond;
extern SDL_mutex* openpete_ticker_wake_mutex;

/* Start the ticker at ~60 Hz (17 ms SDL timer — close enough; integer-ms
 * jitter is dominated by display vsync downstream). Allocates the cond +
 * mutex and sets `openpete_ticker_drives_vbl = 1`. Idempotent — second
 * calls are no-ops. */
void openpete_ticker_start(void);

/* Stop the ticker. Safe to call without a prior start. */
void openpete_ticker_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_TICKER_H */
