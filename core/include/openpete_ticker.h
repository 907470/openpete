// core/include/openpete_ticker.h - host-loop pacing clock lifecycle.
//
// The host loop paces itself: it steps phase-locked absolute integer-ns
// deadlines and sleeps to them with op_sleep_until (see the pacer at the
// bottom of the host loop in run.c). There is no pacing thread. This
// header holds the clock's init/teardown pair and the fast-forward flag.
//
// `openpete_vbl` is NOT driven from here. It is advanced synchronously
// by psyq_VSync(0) - exactly one increment per game-fiber yield -
// because BK2 input replay relies on yields-per-logical-frame being
// deterministic regardless of pacing. Pacing only affects how fast
// host-loop iterations happen in wall time, never their count or order.

#ifndef OPENPETE_TICKER_H
#define OPENPETE_TICKER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero when the host loop should not pace itself (Tab held,
 * --headless, scheduled fast-forward window). Read by run.c only;
 * psyq_VSync ignores this - vbl is yield-driven and deterministic
 * regardless of fast-forward state. */
extern int openpete_unlock_fps;

/** @brief Bring up the pacing clock (op_time_init) before the host loop's first deadline.
 *
 * The Win32 timer-resolution boost and the QPC frequency probe are then
 * paid before the first VBL instead of inside it. Idempotent. */
void openpete_ticker_start(void);

/** @brief Finish the pacing clock (op_time_shutdown): undo the timer-resolution boost, once.
 *
 * Safe to call without a prior start. */
void openpete_ticker_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_TICKER_H */
