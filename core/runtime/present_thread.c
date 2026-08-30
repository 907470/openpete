/* Presenting the last frame again while the guest executes.
 *
 * The sub-tick present grid loses one slot per tick. The slot that would land
 * inside the tick's protective margin is refused, not because the GPU is busy
 * (a present costs ~430 us against a 2.08 ms slot at 480 Hz) but because
 * issuing it would push the tick late: presenting and executing the guest
 * share a thread. At 480 Hz that is 0.8 slots per tick, 24 presents a second,
 * exactly the 480 -> 456 gap.
 *
 * This thread takes the refused slots. While the guest is executing it
 * re-presents the LAST composed frame -- offscreen to swapchain, no
 * recompose, no scene walk, no read of guest RAM or GTE state. That is safe
 * to run concurrently only because the guest's own execution touches neither
 * the Vulkan device nor device-visible memory, which core/rhi/rhi_gpu_audit.h
 * measures rather than assumes.
 *
 * What this can prove: that presents issued during a tick reach the display
 * on the refresh grid instead of serialising against the sim thread in the
 * driver. What it CANNOT prove: anything perceptual. A re-presented frame is
 * a DUPLICATE -- under FIFO the display was already showing those pixels for
 * the whole hold. Judge it on the flip histogram and on what the join costs
 * the sim, never on how it looks.
 *
 * Exclusion is a handshake, not a device lock: the sim opens the window
 * immediately before switching to the game fiber and closes it immediately
 * after, waiting out any present still in flight. Outside that window the sim
 * owns the device exactly as before, so no RHI call site changes.
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
    SDL_Condition *cv;        /* open / close / quit */
    SDL_Condition *idle_cv;   /* a present finished */

    int  open;                /* the guest is executing; presenting permitted */
    int  busy;                /* a present is in flight right now */
    int  quit;

    op_time_t next_at;        /* next grid slot to aim at */
    op_time_t period;         /* present grid period */

    /* census */
    unsigned  presents;       /* re-presents that went out */
    unsigned  slots;          /* grid slots stepped this window (presented or not) */
    unsigned  refused;        /* begin_frame declined (acquire/recreate) */
    unsigned  windows;        /* open/close pairs */
    unsigned  joins;          /* closes that had to wait for a present */
    op_time_t join_ns;        /* total time the sim spent waiting at close */
    op_time_t join_max_ns;
} s_pt;

static int s_enabled = -1;

int openpete_present_thread_enabled(void)
{
    if (s_enabled < 0) {
        const char *e = getenv("OPENPETE_PRESENT_THREAD");
        s_enabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return s_enabled;
}

static int present_thread_main(void *unused)
{
    (void)unused;
    SDL_LockMutex(s_pt.mx);
    for (;;) {
        if (s_pt.quit) break;
        if (!s_pt.open) {
            SDL_WaitCondition(s_pt.cv, s_pt.mx);
            continue;
        }
        const op_time_t now = op_now();
        if (now < s_pt.next_at) {
            /* Wait out the slot, but wake on a close so the sim never waits
             * on a sleep it could have interrupted. */
            Sint32 ms = (Sint32)((s_pt.next_at - now) / 1000000);
            SDL_WaitConditionTimeout(s_pt.cv, s_pt.mx, ms > 0 ? ms : 1);
            continue;
        }
        s_pt.busy = 1;
        SDL_UnlockMutex(s_pt.mx);
        const int did = openpete_native_render_repeat_last();
        if (did) {
            /* Logged as its own kind. It carries the alpha of the frame it
             * repeats, so the analysis can see it is the same animation
             * moment arriving twice -- a hole closed in the display, not
             * motion added. */
            openpete_pace_present(OP_PACE_REPEAT, op_now(), 0, 0, 0, 0);
        }
        SDL_LockMutex(s_pt.mx);
        s_pt.busy = 0;
        if (did) s_pt.presents++; else s_pt.refused++;
        /* Step the grid rather than re-basing on now, and step it in whole
         * periods: re-basing on the current time silently re-phases this
         * thread's grid away from the sim's, after which the two interleave
         * and present MORE often than the grid has slots (measured: 529/s
         * against a 480 Hz target). Every slot stepped is counted, presented
         * or not -- the sim advances its own deadline by this count when it
         * takes the window back, and counting only the presents leaves its
         * grid lagging by the difference. */
        s_pt.next_at += s_pt.period;
        s_pt.slots++;
        {
            const op_time_t t = op_now();
            if (s_pt.next_at < t) {
                const op_time_t behind = t - s_pt.next_at;
                const op_time_t skip   = behind / s_pt.period + 1;
                s_pt.next_at += skip * s_pt.period;
                s_pt.slots   += (unsigned)skip;
            }
        }
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
    OPL_INFO_F("present-thread", "%s",
               "started: re-presenting the last frame during guest execution");
    return 1;
}

void openpete_present_thread_open(op_time_t first_at, op_time_t period)
{
    if (!s_pt.thr || period <= 0) return;
    SDL_LockMutex(s_pt.mx);
    s_pt.open    = 1;
    s_pt.next_at = first_at;
    s_pt.period  = period;
    s_pt.slots   = 0;
    s_pt.windows++;
    SDL_SignalCondition(s_pt.cv);
    SDL_UnlockMutex(s_pt.mx);
}

op_time_t openpete_present_thread_close(unsigned *out_slots)
{
    if (out_slots) *out_slots = 0;
    if (!s_pt.thr) return 0;
    SDL_LockMutex(s_pt.mx);
    s_pt.open = 0;
    SDL_SignalCondition(s_pt.cv);
    op_time_t waited = 0;
    if (s_pt.busy) {
        const op_time_t t0 = op_now();
        while (s_pt.busy) SDL_WaitCondition(s_pt.idle_cv, s_pt.mx);
        waited = op_now() - t0;
        s_pt.joins++;
        s_pt.join_ns += waited;
        if (waited > s_pt.join_max_ns) s_pt.join_max_ns = waited;
    }
    if (out_slots) *out_slots = s_pt.slots;
    SDL_UnlockMutex(s_pt.mx);
    return waited;
}

void openpete_present_thread_stop(void)
{
    if (!s_pt.thr) return;
    SDL_LockMutex(s_pt.mx);
    s_pt.quit = 1;
    s_pt.open = 0;
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
    if (!s_pt.windows) return;
    OPL_INFO_F("present-thread",
               "%u re-present(s) over %u tick window(s), %u refused; "
               "sim waited at close %u time(s), %.3f ms total, %.3f ms max",
               s_pt.presents, s_pt.windows, s_pt.refused, s_pt.joins,
               (double)s_pt.join_ns / 1e6, (double)s_pt.join_max_ns / 1e6);
}

unsigned openpete_present_thread_presents(void) { return s_pt.presents; }
