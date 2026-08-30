/* openpete_time.c — high-resolution monotonic clock + hybrid waiter.
 * See openpete_time.h for API contract. Phase 0 of
 * notes/plans/openpete-frame-pacing.md. */

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

static int s_initialized = 0;
static op_time_t s_slack_ns = 0;

#if defined(_WIN32)
/* QPC frequency, ticks per second. */
static int64_t s_qpc_freq = 0;
/* High-resolution waitable timer (Win10 1803+). NULL on older Windows;
 * the fallback path uses timeBeginPeriod(1) + Sleep. */
static HANDLE  s_hi_res_timer = NULL;   /* capability probe + the first thread's timer */
static int     s_timeperiod_set = 0;
/* One waitable timer PER THREAD. A waitable timer is a single-shot signal:
 * a second thread arming the same handle re-arms the wait the first thread
 * is blocked in with INFINITE, which parked the sim thread for good once a
 * present thread started sleeping through this path too. Every thread that
 * sleeps gets its own; the table lets a shutdown wake all of them. */
#  ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#    define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#  endif
#define OP_TIMER_MAX_THREADS 8
static HANDLE  s_thread_timers[OP_TIMER_MAX_THREADS];
static volatile LONG s_thread_timer_n = 0;
static _Thread_local HANDLE t_timer = NULL;
static HANDLE thread_timer(void) {
    if (t_timer) return t_timer;
    if (!s_hi_res_timer) return NULL;          /* no hi-res support: coarse Sleep path */
    HANDLE h = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!h) return NULL;
    LONG i = InterlockedIncrement(&s_thread_timer_n) - 1;
    if (i < OP_TIMER_MAX_THREADS) s_thread_timers[i] = h;
    t_timer = h;
    return h;
}
#endif

/* Monotonicity guard. clock_gettime is monotonic by definition, but
 * QueryPerformanceCounter can — extremely rarely — go backwards by a few
 * ticks across cores in pathological cases. Keep the prior return and
 * clamp. Thread-local would be more correct, but openpete's pacer is
 * single-threaded and the worst this does in multi-threaded callers is
 * mask a tiny step backwards, which is harmless for pacing. */
static op_time_t s_last_now = 0;

static op_time_t parse_slack_env_us_to_ns(void) {
    const char* s = getenv("OPENPETE_PACER_SLACK_US");
    if (!s || !*s) {
#if defined(_WIN32)
        return 2500 * OP_NS_PER_US;
#else
        return 1500 * OP_NS_PER_US;
#endif
    }
    /* Accept negative-as-zero just to be safe. */
    long v = strtol(s, NULL, 10);
    if (v < 0) v = 0;
    return (op_time_t)v * OP_NS_PER_US;
}

void op_time_init(void) {
    if (s_initialized) return;
    s_slack_ns = parse_slack_env_us_to_ns();

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
    if (!s_hi_res_timer) {
        /* Fall back to coarse Sleep path with a 1 ms period request.
         * Document the perf hit by leaving timeEndPeriod paired in
         * op_time_shutdown. */
        if (timeBeginPeriod(1) == TIMERR_NOERROR) {
            s_timeperiod_set = 1;
        }
    }
#endif

    s_initialized = 1;
}

void op_time_wake_pending_sleeps(void) {
#if defined(_WIN32)
    if (!s_initialized) return;
    {
        /* SetWaitableTimer with a 0 due time fires immediately — a thread
         * in WaitForSingleObject(timer, INFINITE) wakes and returns. The
         * spin-tail then notices op_now() >= deadline (or its own caller's
         * exit predicate is true) and unwinds. CancelWaitableTimer would NOT
         * signal waiters, only the next SetWaitableTimer would — so we set
         * with a 0 time instead, on every thread's timer. */
        LARGE_INTEGER due;
        due.QuadPart = 0;
        LONG n = s_thread_timer_n; if (n > OP_TIMER_MAX_THREADS) n = OP_TIMER_MAX_THREADS;
        for (LONG i = 0; i < n; i++)
            if (s_thread_timers[i]) SetWaitableTimer(s_thread_timers[i], &due, 0, NULL, NULL, FALSE);
    }
#endif
}

void op_time_shutdown(void) {
    if (!s_initialized) return;
#if defined(_WIN32)
    if (s_hi_res_timer) {
        CloseHandle(s_hi_res_timer);
        s_hi_res_timer = NULL;
    }
    {
        LONG n = s_thread_timer_n; if (n > OP_TIMER_MAX_THREADS) n = OP_TIMER_MAX_THREADS;
        for (LONG i = 0; i < n; i++)
            if (s_thread_timers[i]) { CloseHandle(s_thread_timers[i]); s_thread_timers[i] = NULL; }
    }
    if (s_timeperiod_set) {
        timeEndPeriod(1);
        s_timeperiod_set = 0;
    }
#endif
    s_initialized = 0;
}

static void ensure_initialized(void) {
    if (!s_initialized) op_time_init();
}

/* Virtual clock (see the header). Advanced only by op_sleep_until. */
static int       s_virtual_on = 0;
static op_time_t s_virtual_ns = 0;

void op_virtual_clock_enable(op_time_t start_ns) {
    s_virtual_ns = (start_ns > 0) ? start_ns : OP_NS_PER_SEC;
    s_virtual_on = 1;
}

int op_virtual_clock_active(void) { return s_virtual_on; }

op_time_t op_pacer_slack_ns(void) {
    ensure_initialized();
    return s_slack_ns;
}

/* ---------------------------------------------------------------- *
 * op_now                                                            *
 * ---------------------------------------------------------------- */

op_time_t op_now(void) {
    if (s_virtual_on) return s_virtual_ns;
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
        return s_last_now;
    }
    ns = (op_time_t)ts.tv_sec * OP_NS_PER_SEC + (op_time_t)ts.tv_nsec;
#endif
    /* Monotonicity clamp — see s_last_now comment. */
    if (ns < s_last_now) ns = s_last_now;
    s_last_now = ns;
    return ns;
}

/* ---------------------------------------------------------------- *
 * Sleep helpers                                                     *
 * ---------------------------------------------------------------- */

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
        /* SetWaitableTimerEx with negative due time = relative, in 100 ns
         * units. Capped at INT64_MAX/2 to be safe. */
        LARGE_INTEGER due;
        int64_t hundreds = delay_ns / 100;
        if (hundreds < 1) return;
        due.QuadPart = -hundreds;
        if (SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(tm, INFINITE);
        }
    } else {
        /* Coarse Sleep with timeBeginPeriod(1). Sleep is in integer ms,
         * round DOWN so we always undershoot — the spin tail covers
         * the remainder. */
        DWORD ms = (DWORD)(delay_ns / OP_NS_PER_MS);
        if (ms > 0) Sleep(ms);
    }
}
#else
static void sleep_until_posix(op_time_t deadline_ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline_ns / OP_NS_PER_SEC);
    ts.tv_nsec = (long)(deadline_ns % OP_NS_PER_SEC);
#  if defined(CLOCK_MONOTONIC)
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
    /* Virtual clock: the deadline IS the next instant. Never block — the
     * caller's schedule advances, wall time doesn't enter into it. */
    if (s_virtual_on) {
        if (deadline_ns > s_virtual_ns) s_virtual_ns = deadline_ns;
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
     * precision isn't available and isn't needed — requestAnimationFrame
     * and the 60 Hz present cadence pace the actual frame output. */
    op_time_t now_em = op_now();
    int ms = (now_em < deadline_ns)
                 ? (int)((deadline_ns - now_em) / OP_NS_PER_MS)
                 : 0;
    emscripten_sleep(ms > 0 ? (unsigned)ms : 0u);
    return;
#else
    op_time_t slack = s_slack_ns;
    op_time_t now = op_now();
    if (now >= deadline_ns) return;

    /* Stage 1: OS-sleep until deadline - slack. */
    if (deadline_ns - now > slack) {
        op_time_t sleep_target = deadline_ns - slack;
#if defined(_WIN32)
        sleep_until_win(sleep_target);
#else
        sleep_until_posix(sleep_target);
#endif
    }

    /* Stage 2: spin until deadline. cpu_relax keeps the spin cheap on
     * SMT cores; sched_yield once early gives co-runners a chance, but
     * we don't yield inside the inner loop — yielding while the slack
     * budget is sub-ms tends to oversleep on Linux. */
#if !defined(_WIN32)
    sched_yield();
#endif
    while (op_now() < deadline_ns) {
        cpu_relax();
    }
#endif  /* __EMSCRIPTEN__ */
}
