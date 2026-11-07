// core/host/audio_consumer.c - the audio device feed's consumer thread.
//
// The guest's SPU render pushes frames into PsyCross's device feed on the
// sim thread; this thread drains them into the device. It owns the feed's
// resampler and the device queue: every PsyX_SPUFeed_Consume call (reclaim
// the buffers the device finished, apply a pending tick-scale step, update
// the rate fit and the ratio, queue resampled frames up to the target) runs
// here and nowhere else.
//
// Two clocks drive it:
//   - the wall clock (an OpenAL device): the thread wakes every
//     PsyX_SPUFeed_WakePeriodNs() and consumes;
//   - the virtual clock (the feed's synthetic device): the device and the
//     thread advance only inside the clock step. op_sleep_until calls
//     ac_clock_step after it moves the virtual time, which hands the new
//     instant to the thread and waits until the thread has run the
//     synthetic device and its wakes up to it (PsyX_SPUFeed_SimRunTo), so a
//     run's output depends on the schedule and never on host load or thread
//     timing.
#include "openpete_audio_consumer.h"

#include <stdatomic.h>

#include "openpete_log.h"
#include "openpete_thread_roles.h"
#include "openpete_time.h"

#include <SDL3/SDL.h>

/* Declared in PsyCross's private src/audio/PsyX_SPUFeed.h, which is not on
 * any public include path. */
extern void      PsyX_SPUFeed_Consume(void);
extern int       PsyX_SPUFeed_Simulated(void);
extern void      PsyX_SPUFeed_SimRunTo(long long now_ns);
extern long long PsyX_SPUFeed_WakePeriodNs(void);
extern double    PsyX_SPUFeed_SimProducerDelay(long long now_ns);

static SDL_Thread*    s_th OP_GUARDED_BY(sim);
static SDL_Mutex*     s_mx;
static SDL_Condition* s_cv;
static _Atomic(int)       s_quit;     /* the thread must exit */
static _Atomic(int)       s_running;  /* the thread serves clock steps */
static _Atomic(int)       s_virtual;  /* the thread runs on the virtual clock */
static _Atomic(op_time_t) s_want_ns;  /* virtual: run the device to this instant */
static _Atomic(op_time_t) s_done_ns;  /* virtual: the device has run to this instant */

/** @brief Consumer thread entry: consume on the wall clock every wake period, or run the synthetic device to each instant a clock step hands over.
 *
 * The wall-clock wait is a relative sleep (SDL_DelayNS), not a timed
 * condition wait: SDL's timed wait takes a CLOCK_REALTIME deadline, and a
 * host that steps its realtime clock back (WSL steps it about 0.6 s every
 * 32 s) stretches that wait by the step and starves the device.
 * @param ud Unused.
 * @return 0. */
static int SDLCALL ac_thread_main(void* ud)
{
    OP_ROLE_BIND(audio);
    (void)ud;
    const Uint64 wake_ns = (Uint64)PsyX_SPUFeed_WakePeriodNs();
    SDL_LockMutex(s_mx);
    while (!atomic_load(&s_quit)) {
        if (atomic_load(&s_virtual)) {
            const op_time_t want = atomic_load(&s_want_ns);
            if (want > atomic_load(&s_done_ns)) {
                SDL_UnlockMutex(s_mx);
                PsyX_SPUFeed_SimRunTo((long long)want);
                SDL_LockMutex(s_mx);
                atomic_store(&s_done_ns, want);
                SDL_BroadcastCondition(s_cv);
                continue;
            }
            SDL_WaitCondition(s_cv, s_mx);
            continue;
        }
        SDL_UnlockMutex(s_mx);
        PsyX_SPUFeed_Consume();
        if (!atomic_load(&s_quit))
            SDL_DelayNS(wake_ns);
        SDL_LockMutex(s_mx);
    }
    SDL_UnlockMutex(s_mx);
    return 0;
}

/** @brief Virtual-clock step hook: hand the new instant to the consumer thread and wait until the synthetic device and its wakes have run to it.
 * @param now_ns The virtual time after the step. */
static void ac_clock_step(op_time_t now_ns)
{
    if (!atomic_load(&s_running)) return;
    SDL_LockMutex(s_mx);
    if (now_ns > atomic_load(&s_want_ns)) {
        atomic_store(&s_want_ns, now_ns);
        SDL_BroadcastCondition(s_cv);
    }
    while (atomic_load(&s_done_ns) < now_ns && !atomic_load(&s_quit))
        SDL_WaitCondition(s_cv, s_mx);
    SDL_UnlockMutex(s_mx);
}

void openpete_audio_consumer_start(void) OP_REQUIRES(sim)
{
    if (s_th) return;
    if (!s_mx) s_mx = SDL_CreateMutex();
    if (!s_cv) s_cv = SDL_CreateCondition();
    if (!s_mx || !s_cv) {
        OPL_ERROR_F("audio", "consumer: no lock (%s); the device feed is not drained", SDL_GetError());
        return;
    }
    const int virt = PsyX_SPUFeed_Simulated() && op_virtual_clock_active();
    atomic_store(&s_quit, 0);
    atomic_store(&s_virtual, virt);
    atomic_store(&s_want_ns, op_now());
    atomic_store(&s_done_ns, op_now());
    s_th = SDL_CreateThread(ac_thread_main, "audio-consumer", NULL);
    if (!s_th) {
        OPL_ERROR_F("audio", "consumer: thread: %s; the device feed is not drained", SDL_GetError());
        return;
    }
    if (virt) {
        atomic_store(&s_running, 1);
        op_virtual_clock_set_step_hook(ac_clock_step);
    }
    OPL_INFO_F("audio", "consumer: started on the %s clock, wake period %.3f ms",
               virt ? "virtual" : "wall", (double)PsyX_SPUFeed_WakePeriodNs() / 1e6);
}

void openpete_audio_consumer_stop(void) OP_REQUIRES(sim)
{
    if (!s_th) return;
    op_virtual_clock_set_step_hook(NULL);
    atomic_store(&s_running, 0);
    SDL_LockMutex(s_mx);
    atomic_store(&s_quit, 1);
    SDL_BroadcastCondition(s_cv);
    SDL_UnlockMutex(s_mx);
    SDL_WaitThread(s_th, NULL);
    s_th = NULL;
}

void openpete_audio_consumer_producer_step(void) OP_REQUIRES(sim)
{
    if (!atomic_load(&s_virtual) || !atomic_load(&s_running)) return;
    const op_time_t now = op_now();
    const double delay_s = PsyX_SPUFeed_SimProducerDelay((long long)now);
    if (delay_s > 0.0)
        op_sleep_until(now + (op_time_t)(delay_s * 1e9 + 0.5));
}
