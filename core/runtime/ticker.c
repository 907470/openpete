// core/runtime/ticker.c — high-resolution PSX-VBL pacer for the host loop.
//
// Phase 1a of notes/plans/openpete-frame-pacing.md. The old
// SDL_AddTimer(17 ms, ...) implementation gave us 1000/17 = 58.82 Hz
// (vs PSX NTSC 59.94 Hz) and integer-ms jitter; this thread sleeps to a
// phase-locked deadline using openpete_time's op_sleep_until, which
// gives sub-ms jitter on a quiet host.
//
// Determinism contract: the ticker changes the *timing* of host-loop
// iterations, NOT the count or order of game-fiber co_switches.
// openpete_vbl is still advanced synchronously by psyq_VSync(0) —
// exactly one increment per yield — because BK2 input replay relies
// on yields-per-logical-frame being deterministic regardless of pacing.

#include "openpete_ticker.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_time.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <stddef.h>

int            openpete_ticker_active     = 0;
int            openpete_unlock_fps        = 0;
SDL_Condition* openpete_ticker_wake_cond  = NULL;
SDL_Mutex*     openpete_ticker_wake_mutex = NULL;
SDL_AtomicInt  openpete_ticker_seq        = {0};

static SDL_Thread*   s_ticker_thread     = NULL;
static SDL_AtomicInt s_ticker_running    = {0};
static SDL_AtomicInt s_ticker_slip_count = {0};

/* NTSC PSX VBL: 1001/60000 sec = 16683333.33... ns. We keep this in
 * integer ns so the running deadline never accumulates floating-point
 * rounding error over a long session. Slip threshold = 4 periods
 * (~67 ms) — past that we treat the host as having stalled (load
 * spike, gdb breakpoint, swap-thrash) and snap forward by an integer
 * number of periods rather than firing N catch-up ticks back-to-back.
 *
 * Phase 1b will swap this for a runtime knob (--vbl-rate 59.94|60|50);
 * keep the constant in one place so the change is mechanical. */
#define OPENPETE_TICKER_PERIOD_NS    ((op_time_t)((OP_NS_PER_SEC * 1001) / 60000))
#define OPENPETE_TICKER_SLIP_THRESH  (4 * OPENPETE_TICKER_PERIOD_NS)

static int ticker_thread_fn(void* user) {
    (void)user;
    const op_time_t period_ns = OPENPETE_TICKER_PERIOD_NS;
    const op_time_t slip_ns   = OPENPETE_TICKER_SLIP_THRESH;

    /* Phase-lock the first deadline to op_now(). Subsequent iterations
     * step by exactly `period_ns` — never `op_now() + period` — so
     * cumulative drift can't sneak in via OS-sleep overshoot. */
    op_time_t deadline = op_now() + period_ns;

    while (SDL_GetAtomicInt(&s_ticker_running)) {
        /* Detect host stalls before sleeping. Without this the catch-up
         * loop would fire (now - deadline) / period extra ticks
         * back-to-back, triggering N condvar wakes inside the same
         * 100 us window — exactly the "multiply 16.67 ms by N stubs"
         * problem the legacy gate in run.c was added to avoid. */
        op_time_t now = op_now();
        if (now - deadline > slip_ns) {
            op_time_t behind   = now - deadline;
            op_time_t skip_n   = behind / period_ns;
            deadline += skip_n * period_ns;
            SDL_AddAtomicInt(&s_ticker_slip_count, (int)skip_n);
            OPL_WARN_F("ticker",
                       "slip: host stalled %.1f ms (skipped %lld periods)",
                       (double)behind / 1e6, (long long)skip_n);
        }

        op_sleep_until(deadline);
        deadline += period_ns;

        SDL_AddAtomicInt(&openpete_ticker_seq, 1);
        if (openpete_ticker_wake_cond && openpete_ticker_wake_mutex) {
            SDL_LockMutex(openpete_ticker_wake_mutex);
            SDL_BroadcastCondition(openpete_ticker_wake_cond);
            SDL_UnlockMutex(openpete_ticker_wake_mutex);
        }
    }
    return 0;
}

void openpete_ticker_start(void) {
#if defined(__EMSCRIPTEN__)
    /* Single-threaded WASM (no pthreads in this build): there is no
     * background thread to pace from. The host loop instead yields to the
     * browser event loop via op_sleep_until's Asyncify arm, and openpete_vbl
     * is advanced synchronously by psyq_VSync — exactly the --headless
     * pacing contract. Leave openpete_ticker_active = 0. */
    return;
#else
    if (s_ticker_thread != NULL) return; /* already running */

    /* Initialise the clock + Win32 timer-resolution boost up front so
     * the first tick doesn't pay startup latency. Idempotent. */
    op_time_init();

    /* SDL3 has no separate timer subsystem — SDL_GetTicks / SDL_Delay and
     * the perf counters are always available once SDL is initialised, so
     * the old SDL_InitSubSystem(SDL_INIT_TIMER) top-up is gone. */

    if (!openpete_ticker_wake_cond)  openpete_ticker_wake_cond  = SDL_CreateCondition();
    if (!openpete_ticker_wake_mutex) openpete_ticker_wake_mutex = SDL_CreateMutex();
    if (!openpete_ticker_wake_cond || !openpete_ticker_wake_mutex) {
        OPL_ERROR_F("ticker", "SDL_CreateCondition/Mutex failed: %s", SDL_GetError());
        return;
    }

    SDL_SetAtomicInt(&s_ticker_running, 1);
    s_ticker_thread = SDL_CreateThread(ticker_thread_fn, "openpete-ticker", NULL);
    if (!s_ticker_thread) {
        SDL_SetAtomicInt(&s_ticker_running, 0);
        OPL_ERROR_F("ticker", "SDL_CreateThread failed: %s", SDL_GetError());
        return;
    }

    openpete_ticker_active = 1;
    OPL_INFO_F("ticker",
               "started (high-res, %.4f ms / VBL = %.4f Hz NTSC, slack=%lld us)",
               (double)OPENPETE_TICKER_PERIOD_NS / 1e6,
               1e9 / (double)OPENPETE_TICKER_PERIOD_NS,
               (long long)(op_pacer_slack_ns() / OP_NS_PER_US));
#endif  /* !__EMSCRIPTEN__ */
}

void openpete_ticker_stop(void) {
    if (s_ticker_thread) {
        /* Signal the thread to exit, then force-wake any pending
         * op_sleep_until on Win32 so the join completes promptly even
         * if the high-res waitable timer is slow to fire on its
         * natural deadline. On POSIX clock_nanosleep already wakes at
         * the next deadline (~17 ms max), no force-wake needed. */
        SDL_SetAtomicInt(&s_ticker_running, 0);
        op_time_wake_pending_sleeps();
        SDL_WaitThread(s_ticker_thread, NULL);
        s_ticker_thread = NULL;

        int total_slips = SDL_GetAtomicInt(&s_ticker_slip_count);
        if (total_slips > 0) {
            OPL_INFO_F("ticker",
                       "stopped; %d slip events over session", total_slips);
        }
    }
    openpete_ticker_active = 0;
    if (openpete_ticker_wake_cond && openpete_ticker_wake_mutex) {
        /* Wake any host-loop iteration blocked on us before we tear down. */
        SDL_LockMutex(openpete_ticker_wake_mutex);
        SDL_BroadcastCondition(openpete_ticker_wake_cond);
        SDL_UnlockMutex(openpete_ticker_wake_mutex);
    }
}
