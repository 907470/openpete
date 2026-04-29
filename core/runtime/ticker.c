// core/runtime/ticker.c — 60 Hz ticker that drives `openpete_vbl`.
//
// See plans/openpete-free-running-vblank-counter.md and the header for
// rationale. Ticker is started by run.c before the host fiber loop in
// non-headless mode and stopped on shutdown. While running, it is the
// sole writer to `openpete_vbl`; psyq_VSync and openpete_yield_to_host
// only read.

#include "openpete_ticker.h"
#include "openpete_fiber.h"
#include "openpete_log.h"

#include <SDL.h>
#include <SDL_timer.h>
#include <stddef.h>

int          openpete_ticker_drives_vbl = 0;
SDL_cond*    openpete_ticker_wake_cond  = NULL;
SDL_mutex*   openpete_ticker_wake_mutex = NULL;

static SDL_TimerID s_ticker_id = 0;

static Uint32 ticker_callback(Uint32 interval, void* user) {
    (void)user;
    SDL_AtomicAdd(&openpete_vbl, 1);
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
     * (audio sync isn't keyed off this counter, and downstream display vsync
     * dominates jitter on the present path). Swap to a high-resolution
     * busy-wait fiber later if drift becomes audible. */
    s_ticker_id = SDL_AddTimer(17, ticker_callback, NULL);
    if (!s_ticker_id) {
        OPL_ERROR_F("ticker", "SDL_AddTimer failed: %s", SDL_GetError());
        return;
    }
    openpete_ticker_drives_vbl = 1;
    OPL_INFO_F("ticker", "started (17 ms ≈ 58.8 Hz)");
}

void openpete_ticker_stop(void) {
    if (s_ticker_id) {
        SDL_RemoveTimer(s_ticker_id);
        s_ticker_id = 0;
    }
    openpete_ticker_drives_vbl = 0;
    if (openpete_ticker_wake_cond && openpete_ticker_wake_mutex) {
        /* Wake any host-loop iteration blocked on us before we tear down. */
        SDL_LockMutex(openpete_ticker_wake_mutex);
        SDL_CondBroadcast(openpete_ticker_wake_cond);
        SDL_UnlockMutex(openpete_ticker_wake_mutex);
    }
}
