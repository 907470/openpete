/* openpete_time.h — high-resolution monotonic clock + hybrid waiter.
 *
 * Phase 0 of notes/plans/openpete-frame-pacing.md. Leaf module: no SDL,
 * no openpete dependencies. The pacer (ticker.c, run.c dual-pacer) builds
 * on this. The point of the abstraction is that *every* time source in
 * openpete that cares about wall-clock pacing routes through here, so the
 * platform-specific knobs (Win32 waitable timer, Linux clock_nanosleep,
 * spin-tail granularity, env-overridable slack) live in exactly one TU.
 *
 * Sources are always monotonic — never wall clock — so daylight-saving
 * jumps, ntpd corrections, and `date -s` cannot disturb pacing.
 *
 *   op_now()                      monotonic time in ns
 *   op_sleep_until(deadline_ns)   block until clock >= deadline; hybrid
 *                                 sleep-then-spin, sub-ms accuracy
 *   op_pacer_slack_ns()           current safety margin (env-overridable)
 *
 * Determinism note: this module changes the *timing* of co_switches, not
 * their count or order. See the plan's "Determinism contract" section.
 */

#ifndef OPENPETE_TIME_H
#define OPENPETE_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t op_time_t; /* nanoseconds since arbitrary monotonic epoch */

#define OP_NS_PER_SEC  ((op_time_t)1000000000)
#define OP_NS_PER_MS   ((op_time_t)1000000)
#define OP_NS_PER_US   ((op_time_t)1000)

/* Current monotonic time, nanoseconds. Strictly increasing across calls
 * on the same thread (we never lie even if the OS source has rounding —
 * the previous-return clamp guarantees it). */
op_time_t op_now(void);

/* Block until op_now() >= deadline_ns. Returns immediately if already
 * past the deadline. Hybrid strategy:
 *   1. If (deadline - now) > slack, OS-sleep until deadline - slack.
 *   2. Spin (yielding) until deadline reached.
 * "slack" is op_pacer_slack_ns().
 *
 * Overshoot on a quiet Linux dev box should be well under 100 µs; on
 * a noisy Windows box with the waitable-timer-high-res path it should
 * be under 500 µs. The frame-pacing harness in
 * notes/plans/openpete-frame-pacing.md measures the actual figure
 * per platform and gates regressions.
 */
void op_sleep_until(op_time_t deadline_ns);

/* Current safety margin between OS-sleep wake and the real deadline.
 * Defaults: 1500 µs Linux, 2500 µs Windows. Overridable at startup via
 * env var OPENPETE_PACER_SLACK_US (parsed once, lazy-initialized on
 * first call). Larger = more CPU spent spinning but tighter deadline;
 * smaller = less spin but more risk of overshoot when the OS oversleeps. */
op_time_t op_pacer_slack_ns(void);

/* Virtual clock: replace the monotonic source with a counter that only the
 * pacer advances, so op_now() reports *scheduled* time rather than elapsed
 * wall time and op_sleep_until() jumps straight to its deadline instead of
 * blocking. The host loop's sim/present deadlines then land on an exact grid
 * no matter how long a frame actually took to render, which is what an
 * offline capture needs: a software rasterizer that manages 8 fps still
 * produces the same present sequence a 120 fps machine would.
 *
 * Everything downstream of op_now() — the interpolation stamps, the present
 * PLL, the world-liveness gate — follows the same clock, so sub-tick alphas
 * land on exact fractions of a tick. Once enabled it stays enabled for the
 * process; there is no coherent way back to wall time mid-run.
 *
 * `start_ns` seeds the clock. Pass something comfortably above zero: several
 * consumers treat a zero timestamp as "unset". */
void op_virtual_clock_enable(op_time_t start_ns);

/* Non-zero while the virtual clock is driving op_now(). */
int  op_virtual_clock_active(void);

/* One-shot init: requests timer-resolution boosts (Win32
 * timeBeginPeriod(1)) and probes for high-resolution waitable timers.
 * Idempotent. Optional — op_now / op_sleep_until self-init on first
 * call, but callers that want startup latency out of the hot loop can
 * call this explicitly. */
void op_time_init(void);

/* One-shot teardown: releases any OS resources acquired by op_time_init
 * (Win32 timeEndPeriod, waitable-timer handle). Idempotent. */
void op_time_shutdown(void);

/* Cancel any in-flight op_sleep_until on Win32 by force-signalling the
 * shared waitable timer. Callers that want to wake a sleeping thread
 * during shutdown (e.g. the ticker thread join in openpete_ticker_stop)
 * use this to bound shutdown latency at a few µs instead of waiting
 * for the natural period boundary. No-op on POSIX — clock_nanosleep is
 * already bounded by the deadline, and the natural period (~17 ms) is
 * acceptable shutdown latency there. */
void op_time_wake_pending_sleeps(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_TIME_H */
