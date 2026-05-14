// core/include/openpete_ticker.h — NTSC PSX-VBL pacer for the host loop.
//
// Phase 1a of openpete-frame-pacing.md. The ticker runs on its own
// SDL thread and signals a condvar at exactly 60000/1001 Hz (59.94,
// NTSC PSX VBL), sleeping to a phase-locked deadline via
// op_sleep_until — sub-ms jitter on a quiet host.
//
// The host loop in run.c blocks on that condvar between game-fiber
// slices, which paces wall-clock at 59.94 Hz without polling.
//
// IMPORTANT: the ticker does not drive `openpete_vbl`. vbl is advanced
// synchronously by psyq_VSync(0) — exactly one increment per yield —
// because BK2 input replay relies on yields-per-logical-frame being
// deterministic regardless of pacing. The ticker only affects how fast
// host-loop iterations happen in wall time.

#ifndef OPENPETE_TICKER_H
#define OPENPETE_TICKER_H

#include <SDL_atomic.h>
#include <SDL_mutex.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while the ticker is running. The host loop tests this to
 * decide whether to block on the condvar (yes — paced) or skip the wait
 * (no — headless / smoke-test). */
extern int openpete_ticker_active;

/* Non-zero when the host loop should not pace itself (Tab held,
 * --headless, scheduled fast-forward window). Read by run.c only;
 * psyq_VSync ignores this — vbl is yield-driven and deterministic
 * regardless of fast-forward state. */
extern int openpete_unlock_fps;

/* Host-loop wake primitive. The ticker signals this on every tick;
 * the host loop waits on it between game-fiber slices to gate the
 * next slice on a wall-clock vblank. `_seq` increments on every fire so
 * the host wait can detect "tick happened" without racing on a side
 * counter (vbl is yield-driven now and useless for this). */
extern SDL_cond*    openpete_ticker_wake_cond;
extern SDL_mutex*   openpete_ticker_wake_mutex;
extern SDL_atomic_t openpete_ticker_seq;

/* Start the ticker at NTSC 59.94 Hz on a dedicated SDL thread that
 * sleeps to phase-locked deadlines via op_sleep_until. Allocates the
 * cond + mutex and sets `openpete_ticker_active = 1`. Idempotent —
 * second calls are no-ops. Calls op_time_init() up front to pay the
 * Win32 timer-resolution boost outside the first VBL. */
void openpete_ticker_start(void);

/* Stop the ticker. Safe to call without a prior start. */
void openpete_ticker_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_TICKER_H */
