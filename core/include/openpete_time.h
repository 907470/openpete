/* openpete_time.h - high-resolution monotonic clock + hybrid waiter.
 *
 * Leaf module: no SDL, no openpete dependencies. The pacer (ticker.c, run.c dual-pacer) builds
 * on this. The point of the abstraction is that *every* time source in
 * openpete that cares about wall-clock pacing routes through here, so the
 * platform-specific knobs (Win32 waitable timer, Linux clock_nanosleep,
 * spin-tail granularity, env-overridable slack) live in exactly one TU.
 *
 * Sources are always monotonic - never wall clock - so daylight-saving
 * jumps, ntpd corrections, and `date -s` cannot disturb pacing.
 *
 *   op_now()                      monotonic time in ns
 *   op_sleep_until(deadline_ns)   block until clock >= deadline; hybrid
 *                                 sleep-then-spin, sub-ms accuracy
 *   op_pacer_slack_ns()           current safety margin (env-overridable)
 *
 * Determinism note: this module changes the *timing* of co_switches, not
 * their count or order.
 */

#ifndef OPENPETE_TIME_H
#define OPENPETE_TIME_H

#include <stdint.h>

#include "openpete_thread_roles.h"

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
 *     fsc = 315/88 MHz             = 3 579 545.4545... Hz
 *     fH  = fsc * 2/455            = 15 734.26573... Hz  (line rate)
 *     VBI = fH / 263 lines         = 2 250 000/37 609 Hz = 59.8261054... Hz
 *
 * 263 lines because Spyro runs 240p: the GPU emits a full frame of 263
 * scanlines and never drops the half-line that produces the familiar 59.94.
 * That figure is the *interlaced* (480i) field rate - 262.5 lines - and does
 * not apply here. Using it ran the engine 0.19 % fast (~6.9 s/hour).
 *
 * A tick is two VBLs: the game calls VSync(2), so gameplay advances at
 * 1 125 000/37 609 Hz = 29.9130527... Hz.
 *
 * Both decimals repeat with period 37 608, so the rational is the only
 * lossless spelling - hence op_period_t below carries num/den rather than
 * collapsing to a double.
 *
 * Neither period is a whole number of nanoseconds - a VBL is
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
 * with these - use op_period_t below, which does not round. */
#define OP_VBL_HZ   (2250000.0 / 37609.0)   /* 59.8261054... */
#define OP_TICK_HZ  (1125000.0 / 37609.0)   /* 29.9130527... */

/* ---- exact periodic deadlines -------------------------------------------
 *
 * A period is `whole` nanoseconds plus the exact fraction `rem/den`, and a
 * deadline carries the running fraction alongside it. Advancing never
 * rounds, so a deadline chain tracks the true rate for as long as the
 * process lives rather than shedding the truncated remainder every step
 * (a plain integer-ns VBL period loses 0.11 ns per VBL).
 *
 * This matters beyond the VBL itself: --record-cfr divides a tick into
 * `mult` presents, and 33 430 222 + 2/9 over 3 is 11 143 407 + 11/27 - a
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

/** @brief Return the greatest common divisor of a and b, or 1 when it is 0.
 *
 * Euclid's algorithm on the values as given; negative operands are not
 * normalized.
 * @param a first operand.
 * @param b second operand.
 * @return the divisor, at least 1 for non-negative operands. */
static inline op_time_t op_gcd(op_time_t a, op_time_t b) {
    while (b) { const op_time_t t = a % b; a = b; b = t; }
    return a ? a : 1;
}

/** @brief Reduce a period's fraction rem/den to lowest terms.
 *
 * A zero remainder sets den to 1. Keeps denominators readable in a debugger
 * and stops op_period_mul/div from compounding a denominator across
 * repeated use.
 * @param p the period.
 * @return the same period with rem/den reduced. */
static inline op_period_t op_period_reduce(op_period_t p) {
    if (p.rem == 0) { p.den = 1; return p; }
    const op_time_t g = op_gcd(p.rem, p.den);
    p.rem /= g; p.den /= g;
    return p;
}

/** @brief Build a period of exactly num/den seconds.
 *
 * @param num numerator in seconds.
 * @param den denominator, >= 1.
 * @return the period, reduced. */
static inline op_period_t op_period_from_secs(op_time_t num, op_time_t den) {
    op_period_t p;
    const op_time_t total = num * OP_NS_PER_SEC;
    p.whole = total / den;
    p.rem   = total % den;
    p.den   = den;
    return op_period_reduce(p);
}

/** @brief Build a period of exactly ns nanoseconds with no fractional part.
 *
 * @param ns the length in nanoseconds.
 * @return the period. */
static inline op_period_t op_period_from_ns(op_time_t ns) {
    op_period_t p; p.whole = ns; p.rem = 0; p.den = 1; return p;
}

/** @brief Divide a period into n equal parts exactly.
 *
 * @param p the period.
 * @param n the part count, >= 1.
 * @return p / n, reduced. */
static inline op_period_t op_period_div(op_period_t p, op_time_t n) {
    op_period_t q;
    const op_time_t num = p.whole * p.den + p.rem;   /* period = num/den ns */
    const op_time_t den = p.den * n;
    q.whole = num / den;
    q.rem   = num % den;
    q.den   = den;
    return op_period_reduce(q);
}

/** @brief Multiply a period by n exactly.
 *
 * @param p the period.
 * @param n the multiplier, >= 1.
 * @return p * n, reduced. */
static inline op_period_t op_period_mul(op_period_t p, op_time_t n) {
    op_period_t q;
    const op_time_t num = (p.whole * p.den + p.rem) * n;
    q.whole = num / p.den;
    q.rem   = num % p.den;
    q.den   = p.den;
    return op_period_reduce(q);
}

/** @brief Return a period's length in nanoseconds, rounded half up.
 *
 * For logs, ratios and threshold math only; deadlines advance through
 * op_deadline_advance, which keeps the exact fraction.
 * @param p the period.
 * @return the rounded length in nanoseconds. */
static inline op_time_t op_period_ns(op_period_t p) {
    return p.whole + (p.rem * 2 >= p.den ? 1 : 0);
}

/** @brief Return a period's rate in Hz as a double.
 *
 * Computes 1e9 / (whole + rem/den) in floating point, for logs and display;
 * not for stepping deadlines.
 * @param p the period, nonzero.
 * @return the rate in Hz. */
static inline double op_period_hz(op_period_t p) {
    return (double)OP_NS_PER_SEC * (double)p.den
         / ((double)p.whole * (double)p.den + (double)p.rem);
}

/** @brief Reset a deadline to at, dropping any accrued fraction.
 *
 * Used where the grid is abandoned (slip clamp, unlock, live rate change).
 * @param d the deadline.
 * @param at the new deadline in nanoseconds. */
static inline void op_deadline_anchor(op_deadline_t* d, op_time_t at) {
    d->at = at; d->acc = 0;
}

/** @brief Advance a deadline by steps exact periods.
 *
 * Adds steps whole parts and steps remainder numerators, then carries every
 * full den of accrued remainder into the deadline, so no fraction is lost.
 * @param d the deadline.
 * @param p the period.
 * @param steps the period count, >= 1. */
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

/** @brief Return the VBL period under --tick-scale F, the exact VBL period divided by F.
 *
 * F == 1 returns the exact rational. Any other F is a debug speed change
 * that leaves the canonical rate, so it returns the period rounded to whole
 * nanoseconds.
 * @param tick_scale the engine speed factor F, > 0.
 * @return the scaled VBL period. */
static inline op_period_t op_scaled_vbl_period(double tick_scale) {
    const op_period_t p = OP_VBL_PERIOD();
    if (tick_scale == 1.0) return p;
    const double ns = ((double)p.whole * (double)p.den + (double)p.rem)
                      / (double)p.den / tick_scale;
    return op_period_from_ns((op_time_t)(ns + 0.5));
}

/** @brief Return the current monotonic time in nanoseconds.
 *
 * Under the virtual clock it returns the virtual time. Otherwise it reads
 * QueryPerformanceCounter (GetTickCount64 when QPC has no frequency) on
 * Win32, CLOCK_MONOTONIC elsewhere, and clamps to the previous return. The
 * previous return is one process-wide plain static read and written without
 * synchronization, so the clamp orders calls on one thread only; concurrent
 * callers race on it and are ordered only as far as the underlying clock
 * is. A failed clock_gettime returns the previous value. The first
 * real-clock call initializes the module.
 * @return Nanoseconds since an arbitrary monotonic epoch. */
op_time_t op_now(void);

/** @brief Block until op_now() reaches @p deadline_ns.
 *
 * Returns at once when the deadline has passed. Otherwise, when more than
 * op_pacer_slack_ns() remains, OS-sleeps until deadline - slack (a Win32
 * waitable timer with a Sleep() fallback, else clock_nanosleep on
 * CLOCK_MONOTONIC or a relative nanosleep) and records how late that sleep
 * returned into the thread's adaptive slack, then spins on
 * op_now() with cpu_relax() until the deadline; POSIX builds call
 * sched_yield() once before the spin. With OPENPETE_PACER_CENSUS=1 each
 * call with time left is counted in the census op_time_shutdown prints. Under the virtual clock it never
 * blocks: it advances the virtual time to the deadline when the deadline
 * is later. Under Emscripten it yields to the browser event loop for the
 * remaining whole milliseconds, at least one turn even past the deadline.
 * @param deadline_ns Monotonic deadline in ns. */
void op_sleep_until(op_time_t deadline_ns);

/** @brief Return the margin before a deadline at which the calling thread's op_sleep_until stops OS-sleeping and spins.
 *
 * With OPENPETE_PACER_SLACK_US set (microseconds, negative read as 0,
 * parsed once by op_time_init) every thread uses that fixed margin.
 * Otherwise the margin is per thread and measured: the largest of the
 * thread's last 256 OS-sleep wake errors plus 100 us, clamped to
 * [100 us, cap], where the cap is 2500 us on Windows and 1500 us elsewhere
 * and is also the margin until 16 wake errors have been recorded. A larger
 * margin spends more CPU spinning and overshoots a deadline less often when
 * the OS oversleeps.
 * @return The margin in nanoseconds. */
op_time_t op_pacer_slack_ns(void);

/** @brief Replace the monotonic source with a counter that only op_sleep_until advances.
 *
 * op_now() then reports scheduled time rather than elapsed wall time, and
 * op_sleep_until() moves the counter to its deadline instead of blocking.
 * The host loop's sim and present deadlines land on an exact grid however
 * long a render took, so an offline capture produces the same present
 * sequence on a slow software rasterizer as on a fast GPU. Every consumer
 * of op_now() (interpolation stamps, the present PLL, the world-liveness
 * gate) follows the same clock, so sub-tick alphas land on exact fractions
 * of a tick. There is no disable: the clock stays virtual for the process.
 * @param start_ns Initial clock value; a value <= 0 is replaced by one
 *        second, since several consumers read a zero timestamp as unset. */
void op_virtual_clock_enable(op_time_t start_ns);

/** @brief Report whether the virtual clock drives op_now().
 *
 * @return 1 after op_virtual_clock_enable, else 0. */
int  op_virtual_clock_active(void);

/* The clock's init group (the sleep margin, the census flag and the Win32
 * timer state), sealed by openpete_run after op_time_init; op_time_init and
 * op_time_reload_env are its writers. */
OP_SEAL_DECLARE(time);

/** @brief Initialize the clock module: the sleep margin, and on Win32 the QPC frequency and timer.
 *
 * Parses OPENPETE_PACER_SLACK_US and OPENPETE_PACER_CENSUS. On Win32 it
 * reads the QPC frequency, creates a high-resolution waitable timer
 * (Windows 10 1803 and later; the Sleep fallback serves when that fails),
 * and requests a 1 ms system timer period with timeBeginPeriod(1) on either
 * path, since every millisecond timeout in the process (Sleep, SDL event
 * and condition-variable waits) is otherwise quantised to 15.6 ms. Idempotent:
 * a call after the first returns at once. op_now, op_sleep_until and op_pacer_slack_ns call it on
 * first use; calling it early moves that cost out of the host loop. */
void op_time_init(void);

/** @brief Re-read OPENPETE_PACER_SLACK_US and OPENPETE_PACER_CENSUS.
 *
 * op_time_init reads both at the first clock call, which precedes the
 * configuration; this re-read after the configuration is materialized lets
 * an openpete.toml [env] value, game scopes included, apply. A set slack
 * fixes it; an unset slack leaves an adaptive slack as it is and returns a
 * fixed one to adaptive at its starting value. */
void op_time_reload_env(void);

/** @brief Undo the timer-resolution boost and print the pace census, once.
 *
 * On Win32 it pairs timeBeginPeriod with timeEndPeriod. With
 * OPENPETE_PACER_CENSUS=1 it prints the pace census line to stderr. Runs
 * once behind an atomic flag; later calls, and a call before op_time_init,
 * do nothing. The clock stays initialized and the waitable timers stay
 * open, so a thread that sleeps after it (a detached worker) still sleeps
 * on its own timer; process exit releases them. */
void op_time_shutdown(void);

/** @brief Wake every thread blocked in the OS-sleep stage of op_sleep_until on Win32.
 *
 * Sets each per-thread waitable timer with a zero due time, which signals
 * it; CancelWaitableTimer would not release a waiter. The woken thread
 * then spins the rest of its wait to the deadline. Lets a shutdown join a
 * sleeping thread without waiting out the sleep. No-op before op_time_init
 * and on POSIX, where the sleep is bounded by its deadline. */
void op_time_wake_pending_sleeps(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_TIME_H */
