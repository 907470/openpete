/* The present thread: the consumer half of eager production.
 *
 * The thread running the guest extracts every scene of a span while the
 * guest is idle and publishes them into the scene ring (native_render.c),
 * each tagged with the instant its animation alpha was computed for. This
 * thread sleeps until the earliest pending target, picks the pending scene
 * nearest that instant, composes it and presents it -- through the next
 * tick's execution, which is the whole point: the display no longer waits
 * for the sim thread, and the sim thread never waits for the display.
 *
 * Ownership: from set_active(1) until set_active(0) returns, this thread is
 * the only one issuing RHI frames. The guest-side producer touches the
 * device only through persistently-mapped arena writes, which the terrain
 * cache's retirement window keeps clear of any frame in flight. Everything
 * else the compose reads was copied into the ring slot at publish time.
 *
 * Pacing: there is no grid here. The producer put the targets on the grid;
 * decimation shows up as targets that skip slots, and a late scene (its
 * target already passed when it was published) is shown at once rather
 * than held for a slot that is gone. Alpha binds at consume time: pick()
 * takes the nearest pending scene and retires the older ones unpresented.
 */
#include "openpete_present_thread.h"

#include "openpete.h"
#include "openpete_time.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_log.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

static struct {
    SDL_Thread    *thr;
    SDL_Mutex     *mx;
    SDL_Condition *cv;        /* activate / kick / quit */
    SDL_Condition *idle_cv;   /* a compose finished */

    int  active;              /* the thread owns the display */
    int  busy;                /* a compose is in flight right now */
    int  quit;

    openpete_present_thread_cb cb; void *cb_ctx;

    /* census */
    unsigned  presents;       /* scenes that went out */
    unsigned  refused;        /* compose ran but the swapchain declined */
    unsigned  late;           /* target already passed when picked */
    op_time_t late_ns;        /* summed lateness */
    op_time_t late_max_ns;
    op_time_t cost_ema;       /* compose + present, per scene */
    unsigned  drains;         /* set_active(0) calls */
    op_time_t drain_wait_ns;  /* time the caller spent waiting at a drain */
    op_time_t drain_wait_max_ns;
} s_pt;

static int s_enabled = -1;

int openpete_present_thread_enabled(void)
{
    if (s_enabled < 0) {
        const char *e = getenv("OPENPETE_EAGER");
        s_enabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return s_enabled;
}

/* Sleep until `t`, but wake early on a kick/deactivate/quit. Coarse
 * condvar waits for the bulk, a spin-capable hybrid for the last stretch:
 * at 480 Hz a slot is 2.08 ms and a millisecond timer is too blunt. Called
 * with the mutex held; returns with it held. */
static void wait_until_locked(op_time_t t)
{
    for (;;) {
        if (s_pt.quit || !s_pt.active) return;
        const op_time_t now = op_now();
        if (now >= t) return;
        const op_time_t left = t - now;
        /* A condvar timeout is a coarse timer (a millisecond or more of
         * oversleep on Windows), so it covers only the bulk, and only when
         * there is bulk: with less than 4 ms left the whole wait goes to the
         * hybrid sleep, which lands within tens of microseconds. Measured
         * before this split: ~10 % of presents 1.8 ms late, each followed by
         * one immediately -- the display saw pairs where the grid had two
         * slots. */
        if (left > 4000000) {
            SDL_WaitConditionTimeout(s_pt.cv, s_pt.mx, (Sint32)((left - 3000000) / 1000000));
            /* re-evaluate: a kick may have published an earlier target */
            return;
        }
        SDL_UnlockMutex(s_pt.mx);
        op_sleep_until(t);
        SDL_LockMutex(s_pt.mx);
        return;
    }
}

static int present_thread_main(void *unused)
{
    (void)unused;
    SDL_LockMutex(s_pt.mx);
    for (;;) {
        if (s_pt.quit) break;
        if (!s_pt.active) {
            SDL_WaitCondition(s_pt.cv, s_pt.mx);
            continue;
        }
        const op_time_t target = openpete_native_scene_next_target();
        if (!target) {
            /* Nothing pending: sleep until a publish kicks us. Bounded so a
             * lost kick cannot park the display for good. */
            SDL_WaitConditionTimeout(s_pt.cv, s_pt.mx, 4);
            continue;
        }
        op_time_t now = op_now();
        if (now < target) {
            wait_until_locked(target);
            continue;               /* re-read: the earliest target may have changed */
        }
        /* Due (or overdue). Pick against NOW: the nearest animation moment
         * to the instant this frame is actually leaving. */
        const int slot = openpete_native_scene_pick(now);
        if (slot < 0) continue;
        op_time_t t_ns = 0; double alpha = 0.0; int paired = 0, kind = OP_PACE_INTERMEDIATE;
        openpete_native_scene_slot_info(slot, &t_ns, &alpha, &paired, &kind);
        if (now - t_ns > 1000000) {         /* >1 ms past its target */
            s_pt.late++; s_pt.late_ns += now - t_ns;
            if (now - t_ns > s_pt.late_max_ns) s_pt.late_max_ns = now - t_ns;
        }
        s_pt.busy = 1;
        SDL_UnlockMutex(s_pt.mx);
        const op_time_t c0 = op_now();
        const int did = openpete_native_render_scene(slot, (uint64_t)(c0 / 1000));
        if (did) {
            const op_time_t c = op_now() - c0;
            s_pt.cost_ema = s_pt.cost_ema ? (s_pt.cost_ema * 7 + c) / 8 : c;
        }
        if (did) {
            openpete_pace_record_present((uint8_t)kind, alpha, paired);
            if (s_pt.cb) s_pt.cb(s_pt.cb_ctx, kind, alpha, paired);
            /* OPENPETE_PACE_FLIP_LOG: wait for the flip of the present just
             * queued, bounded by the time until the next target less a
             * compose, so the instrument never makes the next scene late.
             * This is the presenting thread, the only one that may wait. */
            static int flip_log = -1;
            if (flip_log < 0) { const char *e = getenv("OPENPETE_PACE_FLIP_LOG"); flip_log = (e && *e && *e != '0'); }
            if (flip_log) {
                const op_time_t nt = openpete_native_scene_next_target();
                const op_time_t budget = nt ? nt - op_now() - 400000 : 1000000;
                if (budget > 0) {
                    uint64_t id = 0, waited = 0;
                    if (openpete_native_render_present_wait((uint64_t)budget, &id, &waited) == 1)
                        openpete_pace_flip(id, (int64_t)op_now(), (int64_t)waited);
                }
            }
        } else {
            /* The refusal ledger is what turns a declined swapchain into
             * SHED records; run it even without a present. */
            openpete_pace_record_present((uint8_t)kind, alpha, paired);
        }
        SDL_LockMutex(s_pt.mx);
        s_pt.busy = 0;
        if (did) s_pt.presents++; else s_pt.refused++;
        SDL_SignalCondition(s_pt.idle_cv);
    }
    SDL_UnlockMutex(s_pt.mx);
    return 0;
}

int openpete_present_thread_start(void)
{
    if (!openpete_present_thread_enabled()) return 0;
    if (s_pt.thr) return 1;
    s_pt.mx      = SDL_CreateMutex();
    s_pt.cv      = SDL_CreateCondition();
    s_pt.idle_cv = SDL_CreateCondition();
    if (!s_pt.mx || !s_pt.cv || !s_pt.idle_cv) return 0;
    s_pt.thr = SDL_CreateThread(present_thread_main, "openpete-present", NULL);
    if (!s_pt.thr) {
        OPL_WARN_F("present-thread", "SDL_CreateThread failed: %s", SDL_GetError());
        return 0;
    }
    OPL_INFO_F("present-thread", "%s", "started: eager scenes present on their own targets");
    return 1;
}

void openpete_present_thread_set_callback(openpete_present_thread_cb cb, void *ctx)
{
    s_pt.cb = cb; s_pt.cb_ctx = ctx;
}

void openpete_present_thread_kick(void)
{
    if (!s_pt.thr) return;
    SDL_LockMutex(s_pt.mx);
    SDL_SignalCondition(s_pt.cv);
    SDL_UnlockMutex(s_pt.mx);
}

int openpete_present_thread_active(void) { return s_pt.thr && s_pt.active; }

void openpete_present_thread_set_active(int on)
{
    if (!s_pt.thr) return;
    SDL_LockMutex(s_pt.mx);
    if (on) {
        if (!s_pt.active) { s_pt.active = 1; SDL_SignalCondition(s_pt.cv); }
        SDL_UnlockMutex(s_pt.mx);
        return;
    }
    if (s_pt.active) {
        s_pt.active = 0;
        SDL_SignalCondition(s_pt.cv);
        s_pt.drains++;
        /* Nothing pending survives: a scene extracted for a moment the
         * inline path is about to present would be shown twice. */
        openpete_native_scene_ring_flush();
        const op_time_t t0 = op_now();
        while (s_pt.busy) SDL_WaitCondition(s_pt.idle_cv, s_pt.mx);
        const op_time_t w = op_now() - t0;
        s_pt.drain_wait_ns += w;
        if (w > s_pt.drain_wait_max_ns) s_pt.drain_wait_max_ns = w;
    }
    SDL_UnlockMutex(s_pt.mx);
}

void openpete_present_thread_stop(void)
{
    if (!s_pt.thr) return;
    openpete_present_thread_set_active(0);
    SDL_LockMutex(s_pt.mx);
    s_pt.quit = 1;
    SDL_SignalCondition(s_pt.cv);
    SDL_UnlockMutex(s_pt.mx);
    SDL_WaitThread(s_pt.thr, NULL);
    s_pt.thr = NULL;
    SDL_DestroyCondition(s_pt.idle_cv);
    SDL_DestroyCondition(s_pt.cv);
    SDL_DestroyMutex(s_pt.mx);
}

void openpete_present_thread_report(void)
{
    if (!s_pt.presents && !s_pt.refused) return;
    OPL_INFO_F("present-thread",
               "%u scene(s) presented, %u refused by the swapchain; %u picked late "
               "(%.3f ms mean, %.3f ms max); %u drain(s), %.3f ms waited total, %.3f ms max",
               s_pt.presents, s_pt.refused, s_pt.late,
               s_pt.late ? (double)s_pt.late_ns / s_pt.late / 1e6 : 0.0,
               (double)s_pt.late_max_ns / 1e6, s_pt.drains,
               (double)s_pt.drain_wait_ns / 1e6, (double)s_pt.drain_wait_max_ns / 1e6);
    openpete_native_scene_ring_report();
}

unsigned openpete_present_thread_presents(void) { return s_pt.presents; }
/* What one present costs this thread, compose to submit, EMA. With vsync
 * off this is the display's real throughput ceiling; the planner spaces
 * targets no denser than it, or most scenes are made only to be superseded. */
int64_t openpete_present_thread_cost_ns(void) { return s_pt.cost_ema; }
