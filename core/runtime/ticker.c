// core/runtime/ticker.c — 60 Hz wall-clock pacer for the host loop.
//
// See header for rationale. The ticker only signals a condvar at ~58.8 Hz
// — it does NOT touch `openpete_vbl`. vbl is yield-driven (psyq_VSync(0))
// so BK2 replay state is independent of pacing.

#include "openpete_ticker.h"
#include "openpete_fiber.h"
#include "openpete_log.h"

#include <SDL.h>
#include <SDL_timer.h>
#include <stddef.h>

int          openpete_ticker_active     = 0;
int          openpete_unlock_fps        = 0;
SDL_cond*    openpete_ticker_wake_cond  = NULL;
SDL_mutex*   openpete_ticker_wake_mutex = NULL;
SDL_atomic_t openpete_ticker_seq        = {0};

static SDL_TimerID s_ticker_id = 0;

static Uint32 ticker_callback(Uint32 interval, void* user) {
    (void)user;
    SDL_AtomicAdd(&openpete_ticker_seq, 1);
    if (openpete_ticker_wake_cond && openpete_ticker_wake_mutex) {
        SDL_LockMutex(openpete_ticker_wake_mutex);
        SDL_CondBroadcast(openpete_ticker_wake_cond);
        SDL_UnlockMutex(openpete_ticker_wake_mutex);
    }
    return interval;
}

void openpete_ticker_start(void) {
    if (s_ticker_id != 0) return;  /* already running */
    /* PsyCross only initialises SDL_INIT_VIDEO; SDL_AddTimer needs the
     * timer subsystem. Idempotent — SDL ref-counts subsystem inits. */
    if (SDL_InitSubSystem(SDL_INIT_TIMER) != 0) {
        OPL_ERROR_F("ticker", "SDL_InitSubSystem(TIMER) failed: %s", SDL_GetError());
        return;
    }
    if (!openpete_ticker_wake_cond)  openpete_ticker_wake_cond  = SDL_CreateCond();
    if (!openpete_ticker_wake_mutex) openpete_ticker_wake_mutex = SDL_CreateMutex();
    /* 17 ms ≈ 58.8 Hz. SDL_AddTimer is integer-ms; tolerance is acceptable
     * — vbl semantics don't ride on this anymore (yield-driven), the
     * ticker only governs wall-clock pacing of the host loop. */
    s_ticker_id = SDL_AddTimer(17, ticker_callback, NULL);
    if (!s_ticker_id) {
        OPL_ERROR_F("ticker", "SDL_AddTimer failed: %s", SDL_GetError());
        return;
    }
    openpete_ticker_active = 1;
    OPL_INFO_F("ticker", "started (17 ms ≈ 58.8 Hz wall-clock pacer)");
}

void openpete_ticker_stop(void) {
    if (s_ticker_id) {
        SDL_RemoveTimer(s_ticker_id);
        s_ticker_id = 0;
    }
    openpete_ticker_active = 0;
    if (openpete_ticker_wake_cond && openpete_ticker_wake_mutex) {
        /* Wake any host-loop iteration blocked on us before we tear down. */
        SDL_LockMutex(openpete_ticker_wake_mutex);
        SDL_CondBroadcast(openpete_ticker_wake_cond);
        SDL_UnlockMutex(openpete_ticker_wake_mutex);
    }
}
