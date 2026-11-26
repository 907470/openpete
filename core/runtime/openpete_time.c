/* openpete_time.c - high-resolution monotonic clock + hybrid waiter.
 * See openpete_time.h for API contract. */

/* Need Vista+ headers for GetTickCount64 + CreateWaitableTimerExW
 * declarations. MinGW-w64's <_mingw.h> pins _WIN32_WINNT on first
 * include, so this MUST be set before any other #include (including
 * <stdlib.h>, which pulls _mingw.h transitively). The runtime check
 * for CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+) is handled
 * by the lazy-init fallback in op_time_init, so this define is only
 * about which symbols the linker can resolve. */
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#  define _WIN32_WINNT 0x0600
#endif

#include "openpete_time.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <mmsystem.h>  /* timeBeginPeriod */
#else
#  include <time.h>
#  include <errno.h>
#  include <sched.h>
#  if defined(__x86_64__) || defined(__i386__)
#    include <immintrin.h>  /* _mm_pause */
#  endif
#endif

#if defined(__EMSCRIPTEN__)
#  include <emscripten.h>  /* emscripten_sleep (Asyncify) */
#endif

/* ---------------------------------------------------------------- *
 * Lazy init                                                         *
 * ---------------------------------------------------------------- */

/* The init group: written by op_time_init, op_time_reload_env and the two
 * env parsers they call, then sealed by openpete_run before any thread that
 * reads it starts, and read-only afterwards. */
OP_SEAL_DEFINE(time);

static int s_initialized OP_SEALED(time) = 0; /* thread-roles: written before openpete_run seals it, read-only after */
/* Margin before a deadline at which op_sleep_until stops OS-sleeping and
 * spins. With OPENPETE_PACER_SLACK_US set it is that fixed value; otherwise
 * it is sized per thread from the measured wake error of that thread's
 * timer, never above OP_SLACK_CAP_NS. */
static op_time_t s_slack_ns OP_SEALED(time) = 0; /* thread-roles: written before openpete_run seals it, read-only after */
static int       s_slack_fixed OP_SEALED(time) = 0; /* thread-roles: written before openpete_run seals it, read-only after */

/* Adaptive slack. Each OS sleep records how far past its target it woke;
 * the slack is the largest of the last OP_WAKE_WINDOW errors plus
 * OP_SLACK_MARGIN_NS, clamped to [OP_SLACK_FLOOR_NS, OP_SLACK_CAP_NS]. The
 * maximum rather than a percentile: one late wake raises the slack for the
 * next OP_WAKE_WINDOW sleeps, so a burst of scheduler contention is covered
 * from its first late wake on. Until OP_WAKE_MIN samples exist the slack is
 * the cap. The cap is the pre-measurement default of each
 * platform: 2.5 ms on Win32, sized for the Sleep() fallback, and 1.5 ms on
 * POSIX. Per thread because each Win32 thread sleeps on its own timer and
 * the sim and present threads see different scheduler contention. */
#define OP_WAKE_WINDOW      256
#define OP_WAKE_MIN         16
#define OP_SLACK_MARGIN_NS  (100 * OP_NS_PER_US)
#define OP_SLACK_FLOOR_NS   (100 * OP_NS_PER_US)
#if defined(_WIN32)
#  define OP_SLACK_CAP_NS   (2500 * OP_NS_PER_US)
#else
#  define OP_SLACK_CAP_NS   (1500 * OP_NS_PER_US)
#endif

typedef struct {
    op_time_t err[OP_WAKE_WINDOW]; /* OS wake error, ns, >= 0 */
    int       n;                   /* samples recorded, saturating at the window */
    int       next;                /* ring write index */
    op_time_t slack;               /* current slack; 0 = not yet computed */
} op_wake_window;

static _Thread_local op_wake_window t_wake;

/* Pace census (OPENPETE_PACER_CENSUS=1): process-wide counters over every
 * op_sleep_until that had time left, printed by op_time_shutdown. Lateness
 * is the return instant minus the deadline, bucketed at 10 us up to 5 ms. */
#define OP_CENSUS_BUCKETS 501
static int s_census OP_SEALED(time) = 0; /* thread-roles: written before openpete_run seals it, read-only after */
static _Atomic uint64_t s_cen_calls, s_cen_os_sleeps, s_cen_spin_ns;
static _Atomic int64_t  s_cen_spin_max, s_cen_late_max, s_cen_slack_sum;
static _Atomic uint32_t s_cen_late_hist[OP_CENSUS_BUCKETS];

#if defined(_WIN32)
/* QPC frequency, ticks per second. */
static int64_t s_qpc_freq OP_SEALED(time) = 0; /* thread-roles: written before openpete_run seals it, read-only after */
/* High-resolution waitable timer (Win10 1803+). NULL on older Windows;
 * the fallback path uses timeBeginPeriod(1) + Sleep. */
/* The high-resolution timer capability probe. */
static HANDLE  s_hi_res_timer OP_SEALED(time) = NULL; /* thread-roles: written before openpete_run seals it, read-only after */
static int     s_timeperiod_set OP_SEALED(time) = 0; /* thread-roles: written before openpete_run seals it, read-only after */
/* One waitable timer PER THREAD. A waitable timer is a single-shot signal:
 * a second thread arming the same handle re-arms the wait the first thread
 * is blocked in with INFINITE, which parked the sim thread for good once a
 * present thread started sleeping through this path too. Every thread that
 * sleeps gets its own, held in a thread-local. Handles are never closed: a
 * thread may still sleep on its timer after shutdown (the portal-sky worker
 * is never joined), and the process exit releases them. */
#  ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#    define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#  endif
static _Thread_local HANDLE t_timer = NULL;
/** @brief Return the calling thread's high-resolution waitable timer, creating it at the thread's first sleep.
 * @return The handle, or NULL when high-resolution timers are unsupported or creation failed (the caller takes the coarse Sleep path). */
static HANDLE thread_timer(void) {
    if (t_timer) return t_timer;
    if (!s_hi_res_timer) return NULL;          /* no hi-res support: coarse Sleep path */
    HANDLE h = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!h) return NULL;
    t_timer = h;
    return h;
}
#endif

/* Monotonicity guard. clock_gettime is monotonic by definition, but
 * QueryPerformanceCounter can - extremely rarely - go backwards by a few
 * ticks across cores in pathological cases. Keep the prior return and
 * clamp. Per thread: the clamp orders the calls of one thread, and no
 * thread reads another's (on mingw each access is one emutls call). */
static _Thread_local op_time_t t_last_now = 0;

/** @brief Read OPENPETE_PACER_SLACK_US into s_slack_ns and s_slack_fixed.
 *
 * Set and non-empty, the value (microseconds, negative read as 0) fixes
 * the slack for every thread; unset or empty leaves the slack adaptive with
 * OP_SLACK_CAP_NS as its starting value. */
static void parse_slack_env(void) {
    op_seal_check(time);
    const char* s = getenv("OPENPETE_PACER_SLACK_US");
    if (!s || !*s) {
        s_slack_fixed = 0;
        s_slack_ns = OP_SLACK_CAP_NS;
        return;
    }
    long v = strtol(s, NULL, 10);
    if (v < 0) v = 0;
    s_slack_fixed = 1;
    s_slack_ns = (op_time_t)v * OP_NS_PER_US;
}

/** @brief Read OPENPETE_PACER_CENSUS into s_census: on unless unset, empty or starting with '0'. */
static void parse_census_env(void) {
    op_seal_check(time);
    const char* c = getenv("OPENPETE_PACER_CENSUS");
    s_census = (c && *c && *c != '0');
}

void op_time_reload_env(void) {
    op_seal_check(time);
    const char* s = getenv("OPENPETE_PACER_SLACK_US");
    /* Unset now and before: the adaptive slack keeps what it learned. */
    if ((s && *s) || s_slack_fixed) parse_slack_env();
    parse_census_env();
}

void op_time_init(void) {
    if (s_initialized) return;
    op_seal_check(time);
    parse_slack_env();
    parse_census_env();

#if defined(_WIN32)
    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f)) {
        s_qpc_freq = f.QuadPart;
    }
    /* Win10 1803+ supports CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2).
     * No header constant on older toolchains, so use the numeric value
     * and gracefully fall back when CreateWaitableTimerExW rejects it. */
#  ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#    define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#  endif
    s_hi_res_timer = CreateWaitableTimerExW(
        NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    /* A 1 ms system timer period on every timer path, paired with
     * timeEndPeriod in op_time_shutdown. The high-resolution waitable timer
     * does not need it, but every other millisecond timeout in the process
     * does: Sleep() on the fallback path, SDL_WaitEventTimeout in the pump
     * loop and SDL_WaitConditionTimeout in the present thread are otherwise
     * quantised to the default 15.6 ms period. */
    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
        s_timeperiod_set = 1;
    }
#endif

    s_initialized = 1;
}

/** @brief Print the pace census line: sleeps, OS sleeps, spin time and lateness percentiles.
 *
 * Lateness percentiles come from the 10 us histogram, so each is the upper
 * edge of its bucket; the last bucket holds everything at or past 5 ms. */
static void census_print(void) {
    const uint64_t calls = atomic_load(&s_cen_calls);
    uint64_t total = 0, run = 0;
    for (int i = 0; i < OP_CENSUS_BUCKETS; i++) total += atomic_load(&s_cen_late_hist[i]);
    int p50 = -1, p99 = -1, p999 = -1;
    for (int i = 0; i < OP_CENSUS_BUCKETS && total; i++) {
        run += atomic_load(&s_cen_late_hist[i]);
        if (p50 < 0 && run * 100 >= total * 50) p50 = i;
        if (p99 < 0 && run * 100 >= total * 99) p99 = i;
        if (p999 < 0 && run * 1000 >= total * 999) p999 = i;
    }
    fprintf(stderr,
            "[pacer] census: sleeps=%llu os_sleeps=%llu spin_ms=%.1f "
            "spin_us_per_sleep=%.1f spin_max_us=%.1f slack_mean_us=%.1f "
            "late_p50_us<=%d late_p99_us<=%d late_p999_us<=%d late_max_us=%.1f "
            "slack=%s\n",
            (unsigned long long)calls,
            (unsigned long long)atomic_load(&s_cen_os_sleeps),
            (double)atomic_load(&s_cen_spin_ns) / 1e6,
            calls ? (double)atomic_load(&s_cen_spin_ns) / 1e3 / (double)calls : 0.0,
            (double)atomic_load(&s_cen_spin_max) / 1e3,
            calls ? (double)atomic_load(&s_cen_slack_sum) / 1e3 / (double)calls : 0.0,
            (p50 + 1) * 10, (p99 + 1) * 10, (p999 + 1) * 10,
            (double)atomic_load(&s_cen_late_max) / 1e3,
            s_slack_fixed ? "fixed" : "adaptive");
}

/** @brief Raise an atomic maximum to v.
 * @param m The maximum.
 * @param v Candidate value. */
static void census_max(_Atomic int64_t* m, int64_t v) {
    int64_t cur = atomic_load_explicit(m, memory_order_relaxed);
    while (v > cur &&
           !atomic_compare_exchange_weak_explicit(m, &cur, v, memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}

/** @brief Record one op_sleep_until in the pace census.
 * @param slack The slack the sleep used.
 * @param os_slept 1 when the OS-sleep stage ran.
 * @param spin Time spent in the spin stage.
 * @param late Return instant minus the deadline. */
static void census_record(op_time_t slack, int os_slept, op_time_t spin, op_time_t late) {
    atomic_fetch_add_explicit(&s_cen_calls, 1, memory_order_relaxed);
    if (os_slept) atomic_fetch_add_explicit(&s_cen_os_sleeps, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_cen_spin_ns, (uint64_t)spin, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_cen_slack_sum, slack, memory_order_relaxed);
    census_max(&s_cen_spin_max, spin);
    census_max(&s_cen_late_max, late);
    int64_t b = late / (10 * OP_NS_PER_US);
    if (b < 0) b = 0;
    if (b >= OP_CENSUS_BUCKETS) b = OP_CENSUS_BUCKETS - 1;
    atomic_fetch_add_explicit(&s_cen_late_hist[b], 1, memory_order_relaxed);
}

/** @brief Record one OS-sleep wake error in this thread's window and recompute its slack.
 *
 * The slack becomes the largest error in the window plus
 * OP_SLACK_MARGIN_NS, clamped to [OP_SLACK_FLOOR_NS, OP_SLACK_CAP_NS];
 * below OP_WAKE_MIN samples it stays at the cap.
 * @param err How far past its target the OS sleep returned; negative is
 *        recorded as 0. */
static void wake_record(op_time_t err) {
    op_wake_window* w = &t_wake;
    w->err[w->next] = err > 0 ? err : 0;
    w->next = (w->next + 1) % OP_WAKE_WINDOW;
    if (w->n < OP_WAKE_WINDOW) w->n++;
    if (w->n < OP_WAKE_MIN) { w->slack = OP_SLACK_CAP_NS; return; }
    op_time_t hi = 0;
    for (int i = 0; i < w->n; i++)
        if (w->err[i] > hi) hi = w->err[i];
    op_time_t sl = hi + OP_SLACK_MARGIN_NS;
    if (sl < OP_SLACK_FLOOR_NS) sl = OP_SLACK_FLOOR_NS;
    if (sl > OP_SLACK_CAP_NS) sl = OP_SLACK_CAP_NS;
    w->slack = sl;
}

/** @brief Return the slack the calling thread's next sleep uses.
 * @return The fixed slack when OPENPETE_PACER_SLACK_US is set, else this
 *         thread's adaptive slack, OP_SLACK_CAP_NS before it has one. */
static op_time_t thread_slack(void) {
    if (s_slack_fixed) return s_slack_ns;
    return t_wake.slack > 0 ? t_wake.slack : OP_SLACK_CAP_NS;
}

/* Set by the first op_time_shutdown; later calls return at once. */
static atomic_int s_shutdown_done;

void op_time_shutdown(void) {
    if (!s_initialized) return;
    int expect = 0;
    if (!atomic_compare_exchange_strong(&s_shutdown_done, &expect, 1)) return;
#if defined(_WIN32)
    if (s_timeperiod_set) timeEndPeriod(1);
#endif
    if (s_census) census_print();
}

/** @brief Run op_time_init on first use, so the clock functions work without an explicit init call. */
static void ensure_initialized(void) {
    if (!s_initialized) op_time_init();
}

/* Virtual clock (see the header). Advanced only by op_sleep_until on the
 * sim pacer, its one writer (eager production stays off under the virtual
 * clock, so the present thread never sleeps on it); every thread's op_now
 * reads it. Release store by the writer, acquire load by readers. */
static atomic_int         s_virtual_on;
static _Atomic(op_time_t) s_virtual_ns;
static _Atomic(op_clock_step_fn) s_step_hook;

void op_virtual_clock_set_step_hook(op_clock_step_fn fn) {
    atomic_store_explicit(&s_step_hook, fn, memory_order_release);
}

void op_virtual_clock_enable(op_time_t start_ns) {
    atomic_store_explicit(&s_virtual_ns, (start_ns > 0) ? start_ns : OP_NS_PER_SEC,
                          memory_order_release);
    atomic_store_explicit(&s_virtual_on, 1, memory_order_release);
}

int op_virtual_clock_active(void) {
    return atomic_load_explicit(&s_virtual_on, memory_order_acquire);
}

op_time_t op_pacer_slack_ns(void) {
    ensure_initialized();
    return thread_slack();
}

/* ---------------------------------------------------------------- *
 * op_now                                                            *
 * ---------------------------------------------------------------- */

op_time_t op_now(void) {
    if (atomic_load_explicit(&s_virtual_on, memory_order_acquire))
        return atomic_load_explicit(&s_virtual_ns, memory_order_acquire);
    ensure_initialized();
    op_time_t ns;
#if defined(_WIN32)
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    /* freq is typically 10 MHz on modern Windows (100 ns ticks), so
     * (c * 1e9 / freq) fits int64 comfortably for any sane uptime. Do
     * the multiply-then-divide to keep precision. */
    if (s_qpc_freq > 0) {
        /* Avoid overflow: ticks * 1e9 overflows int64 around 9.2e9
         * seconds (~292 years) on 10 MHz, so direct multiply is safe.
         * Larger freqs (rare) reduce the window. Split the math:
         *   ns = (ticks / freq) * 1e9 + ((ticks % freq) * 1e9) / freq */
        int64_t t = c.QuadPart;
        int64_t whole = (t / s_qpc_freq) * OP_NS_PER_SEC;
        int64_t frac  = ((t % s_qpc_freq) * OP_NS_PER_SEC) / s_qpc_freq;
        ns = whole + frac;
    } else {
        ns = (op_time_t)GetTickCount64() * OP_NS_PER_MS;
    }
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return t_last_now;
    }
    ns = (op_time_t)ts.tv_sec * OP_NS_PER_SEC + (op_time_t)ts.tv_nsec;
#endif
    /* Monotonicity clamp - see t_last_now comment. */
    if (ns < t_last_now) ns = t_last_now;
    t_last_now = ns;
    return ns;
}

/* ---------------------------------------------------------------- *
 * Sleep helpers                                                     *
 * ---------------------------------------------------------------- */

/** @brief Issue the CPU's spin-wait hint: YieldProcessor, PAUSE or YIELD, and nothing on other targets.
 *
 * Used in the final spin of op_sleep_until, where it lowers the power and
 * SMT cost of polling the clock. */
static void cpu_relax(void) {
#if defined(_WIN32)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    /* portable, slow */
#endif
}

#if defined(_WIN32)
static void sleep_until_win(op_time_t deadline_ns) {
    op_time_t now = op_now();
    if (now >= deadline_ns) return;
    op_time_t delay_ns = deadline_ns - now;
    HANDLE tm = thread_timer();
    if (tm) {
        /* SetWaitableTimer with negative due time = relative, in 100 ns
         * units. A delay under 100 ns returns at once. */
        LARGE_INTEGER due;
        int64_t hundreds = delay_ns / 100;
        if (hundreds < 1) return;
        due.QuadPart = -hundreds;
        if (SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(tm, INFINITE);
        }
    } else {
        /* Coarse Sleep with timeBeginPeriod(1). Sleep is in integer ms,
         * round DOWN so we always undershoot - the spin tail covers
         * the remainder. */
        DWORD ms = (DWORD)(delay_ns / OP_NS_PER_MS);
        if (ms > 0) Sleep(ms);
    }
}
#else
/** @brief Sleep the calling thread until an absolute op_now() time, restarting after signals.
 *
 * With CLOCK_MONOTONIC (the clock op_now reads) it makes an absolute
 * clock_nanosleep and repeats on EINTR. Without it the delta from op_now is
 * slept with nanosleep, returning at once when the time has passed. Wakes
 * are not early-checked: op_sleep_until spins the remainder.
 * @param deadline_ns Wake time on the op_now() clock. */
static void sleep_until_posix(op_time_t deadline_ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline_ns / OP_NS_PER_SEC);
    ts.tv_nsec = (long)(deadline_ns % OP_NS_PER_SEC);
#  if defined(CLOCK_MONOTONIC) && !defined(__APPLE__)  /* Apple libc has no clock_nanosleep */
    /* TIMER_ABSTIME: target an absolute deadline. Restart on EINTR. */
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (rc == EINTR);
#  else
    /* Fallback: relative nanosleep with current-now delta. */
    op_time_t now = op_now();
    if (now >= deadline_ns) return;
    op_time_t delta = deadline_ns - now;
    struct timespec rel;
    rel.tv_sec  = (time_t)(delta / OP_NS_PER_SEC);
    rel.tv_nsec = (long)(delta % OP_NS_PER_SEC);
    while (nanosleep(&rel, &rel) == -1 && errno == EINTR) {}
#  endif
}
#endif

void op_sleep_until(op_time_t deadline_ns) {
    /* Virtual clock: the deadline IS the next instant. Never block - the
     * caller's schedule advances, wall time doesn't enter into it. */
    if (atomic_load_explicit(&s_virtual_on, memory_order_acquire)) {
        if (deadline_ns > atomic_load_explicit(&s_virtual_ns, memory_order_relaxed)) {
            atomic_store_explicit(&s_virtual_ns, deadline_ns, memory_order_release);
            const op_clock_step_fn hook =
                atomic_load_explicit(&s_step_hook, memory_order_acquire);
            if (hook) hook(deadline_ns);
        }
        return;
    }
    ensure_initialized();

#if defined(__EMSCRIPTEN__)
    /* The browser owns the frame clock. We can neither OS-sleep nor
     * busy-spin: both freeze the tab (single-threaded WASM, no preempt).
     * Instead yield to the event loop via Asyncify (emscripten_sleep
     * unwinds the WASM stack, lets the browser render + dispatch events,
     * then rewinds back here). Always yield at least one turn even when
     * the deadline has passed (unlock / fast-forward re-anchors deadlines
     * to now), so a tight host loop never starves the page. Sub-ms
     * precision isn't available and isn't needed - requestAnimationFrame
     * and the 60 Hz present cadence pace the actual frame output. */
    op_time_t now_em = op_now();
    int ms = (now_em < deadline_ns)
                 ? (int)((deadline_ns - now_em) / OP_NS_PER_MS)
                 : 0;
    emscripten_sleep(ms > 0 ? (unsigned)ms : 0u);
    return;
#else
    const op_time_t slack = thread_slack();
    op_time_t now = op_now();
    if (now >= deadline_ns) return;

    /* Stage 1: OS-sleep until deadline - slack, and feed how late the OS
     * returned into this thread's slack estimate. */
    int os_slept = 0;
    if (deadline_ns - now > slack) {
        const op_time_t sleep_target = deadline_ns - slack;
#if defined(_WIN32)
        sleep_until_win(sleep_target);
#else
        sleep_until_posix(sleep_target);
#endif
        os_slept = 1;
        if (!s_slack_fixed) wake_record(op_now() - sleep_target);
    }

    /* Stage 2: spin until deadline. cpu_relax keeps the spin cheap on
     * SMT cores; sched_yield once early gives co-runners a chance, but
     * we don't yield inside the inner loop - yielding while the slack
     * budget is sub-ms tends to oversleep on Linux. */
#if !defined(_WIN32)
    sched_yield();
#endif
    const op_time_t spin0 = op_now();
    op_time_t t = spin0;
    while (t < deadline_ns) {
        cpu_relax();
        t = op_now();
    }
    if (s_census) census_record(slack, os_slept, t - spin0, t - deadline_ns);
#endif  /* __EMSCRIPTEN__ */
}
