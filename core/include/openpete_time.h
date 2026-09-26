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

/* ---- PSX NTSC vertical rate (240p, non-interlaced) ----------------------
 *
 * The rate the console's video output actually locks to, derived from the
 * NTSC colour subcarrier:
 *
 *     fsc = 315/88 MHz             = 3 579 545.4545… Hz
 *     fH  = fsc * 2/455            = 15 734.26573… Hz  (line rate)
 *     VBI = fH / 263 lines         = 2 250 000/37 609 Hz = 59.8261054… Hz
 *
 * 263 lines because Spyro runs 240p: the GPU emits a full frame of 263
 * scanlines and never drops the half-line that produces the familiar 59.94.
 * That figure is the *interlaced* (480i) field rate — 262.5 lines — and does
 * not apply here. Using it ran the engine 0.19 % fast (~6.9 s/hour).
 *
 * A tick is two VBLs: the game calls VSync(2), so gameplay advances at
 * 1 125 000/37 609 Hz = 29.9130527… Hz.
 *
 * Both decimals repeat with period 37 608, so the rational is the only
 * lossless spelling — hence op_period_t below carries num/den rather than
 * collapsing to a double.
 *
 * Neither period is a whole number of nanoseconds — a VBL is
 * 16 715 111 + 1/9 ns. op_vbl_step()/op_tick_step() carry that ninth in an
 * integer accumulator, so a deadline chain stays exact indefinitely instead
 * of shedding 0.11 ns per step.
 *
 * (A PAL path would be fH = 15 625 Hz over 314 lines = 15 625/314 Hz.)
 */
#define OP_VBL_HZ_NUM      ((op_time_t)2250000)   /* VBL rate numerator   */
#define OP_VBL_HZ_DEN      ((op_time_t)37609)     /* VBL rate denominator */
#define OP_TICK_VBLS       ((op_time_t)2)         /* VSync(2): 2 VBL/tick */

/* Whole-ns part of one period; the remainder is carried by the steppers. */
#define OP_VBL_PERIOD_NS   ((op_time_t)16715111)  /* + 1/9 ns */
#define OP_TICK_PERIOD_NS  ((op_time_t)33430222)  /* + 2/9 ns */

/* Canonical rates as doubles, for logging and for consumers that genuinely
 * need a float (mod time bases, encoder rate reports). Never step a deadline
 * with these — use op_period_t below, which does not round. */
#define OP_VBL_HZ   (2250000.0 / 37609.0)   /* 59.8261054… */
#define OP_TICK_HZ  (1125000.0 / 37609.0)   /* 29.9130527… */

/* ---- exact periodic deadlines -------------------------------------------
 *
 * A period is `whole` nanoseconds plus the exact fraction `rem/den`, and a
 * deadline carries the running fraction alongside it. Advancing never
 * rounds, so a deadline chain tracks the true rate for as long as the
 * process lives rather than shedding the truncated remainder every step
 * (a plain integer-ns VBL period loses 0.11 ns per VBL).
 *
 * This matters beyond the VBL itself: --record-cfr divides a tick into
 * `mult` presents, and 33 430 222 + 2/9 over 3 is 11 143 407 + 11/27 — a
 * denominator the VBL's ninth cannot express. Hence the general rational
 * rather than a fixed carry.
 *
 * Re-anchoring (slip clamp, fast-forward exit, a live --render-fps change)
 * abandons the old grid deliberately, so op_deadline_anchor() clears the
 * accumulator with the timestamp.
 */
typedef struct {
    op_time_t whole;   /* whole ns per period                */
    op_time_t rem;     /* remainder numerator, 0 <= rem < den */
    op_time_t den;     /* remainder denominator, >= 1         */
} op_period_t;

typedef struct {
    op_time_t at;      /* the deadline itself                       */
    op_time_t acc;     /* accrued remainder numerator, 0 <= acc < den */
} op_deadline_t;

static inline op_time_t op_gcd(op_time_t a, op_time_t b) {
    while (b) { const op_time_t t = a % b; a = b; b = t; }
    return a ? a : 1;
}

/* Reduce rem/den to lowest terms. Keeps denominators small enough to read in
 * a debugger and keeps op_period_mul/div from compounding a denominator
 * across repeated use. */
static inline op_period_t op_period_reduce(op_period_t p) {
    if (p.rem == 0) { p.den = 1; return p; }
    const op_time_t g = op_gcd(p.rem, p.den);
    p.rem /= g; p.den /= g;
    return p;
}

/* Build a period of exactly num/den seconds. */
static inline op_period_t op_period_from_secs(op_time_t num, op_time_t den) {
    op_period_t p;
    const op_time_t total = num * OP_NS_PER_SEC;
    p.whole = total / den;
    p.rem   = total % den;
    p.den   = den;
    return op_period_reduce(p);
}

/* Build a period of exactly `ns` nanoseconds (no fractional part). */
static inline op_period_t op_period_from_ns(op_time_t ns) {
    op_period_t p; p.whole = ns; p.rem = 0; p.den = 1; return p;
}

/* Divide a period into `n` equal parts, exactly. */
static inline op_period_t op_period_div(op_period_t p, op_time_t n) {
    op_period_t q;
    const op_time_t num = p.whole * p.den + p.rem;   /* period = num/den ns */
    const op_time_t den = p.den * n;
    q.whole = num / den;
    q.rem   = num % den;
    q.den   = den;
    return op_period_reduce(q);
}

/* Multiply a period by `n`, exactly (n >= 1). */
static inline op_period_t op_period_mul(op_period_t p, op_time_t n) {
    op_period_t q;
    const op_time_t num = (p.whole * p.den + p.rem) * n;
    q.whole = num / p.den;
    q.rem   = num % p.den;
    q.den   = p.den;
    return op_period_reduce(q);
}

/* Nominal length in ns, rounded — for logs, ratios and threshold math only. */
static inline op_time_t op_period_ns(op_period_t p) {
    return p.whole + (p.rem * 2 >= p.den ? 1 : 0);
}

static inline double op_period_hz(op_period_t p) {
    return (double)OP_NS_PER_SEC * (double)p.den
         / ((double)p.whole * (double)p.den + (double)p.rem);
}

/* Reset a deadline to `at`, dropping any accrued fraction. Use whenever the
 * grid is deliberately abandoned (slip, unlock, live rate change). */
static inline void op_deadline_anchor(op_deadline_t* d, op_time_t at) {
    d->at = at; d->acc = 0;
}

/* Advance a deadline by `steps` exact periods (steps >= 1). */
static inline void op_deadline_advance(op_deadline_t* d, op_period_t p,
                                       op_time_t steps) {
    d->at  += p.whole * steps;
    d->acc += p.rem * steps;
    if (d->acc >= p.den) { d->at += d->acc / p.den; d->acc %= p.den; }
}

/* The exact NTSC periods, as op_period_t. */
#define OP_VBL_PERIOD()   op_period_from_secs(OP_VBL_HZ_DEN, OP_VBL_HZ_NUM)
#define OP_TICK_PERIOD()  op_period_from_secs(OP_VBL_HZ_DEN, \
                                              OP_VBL_HZ_NUM / OP_TICK_VBLS)

/* The VBL period under --tick-scale. F == 1 keeps the exact rational; any
 * other F is a debug slow-mo whose whole point is to leave the canonical
 * rate, so it collapses to a rounded whole-ns period. */
static inline op_period_t op_scaled_vbl_period(double tick_scale) {
    const op_period_t p = OP_VBL_PERIOD();
    if (tick_scale == 1.0) return p;
    const double ns = ((double)p.whole * (double)p.den + (double)p.rem)
                      / (double)p.den / tick_scale;
    return op_period_from_ns((op_time_t)(ns + 0.5));
}

/* Current monotonic time, nanoseconds. Strictly increasing across calls
 * on the same thread (we never lie even if the OS source has rounding —
 * the previous-return clamp guarantees it). */
op_time_t op_now(void);

/** @brief Block until op_now() reaches @p deadline_ns.
 *
 * Returns at once when the deadline has passed. Otherwise, when more than
 * op_pacer_slack_ns() remains, OS-sleeps until deadline - slack (a Win32
 * waitable timer with a Sleep() fallback, else clock_nanosleep on
 * CLOCK_MONOTONIC or a relative nanosleep), then spins on
 * op_now() with cpu_relax() until the deadline; POSIX builds call
 * sched_yield() once before the spin. Under the virtual clock it never
 * blocks: it advances the virtual time to the deadline when the deadline
 * is later. Under Emscripten it yields to the browser event loop for the
 * remaining whole milliseconds, at least one turn even past the deadline.
 * @param deadline_ns Monotonic deadline in ns. */
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
