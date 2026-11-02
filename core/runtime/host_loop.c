// core/runtime/host_loop.c - the host loop's tick: the VBL an iteration runs
// in the game fiber, its pace-log bracket, and on a tick the runahead lead
// around the interp captures, stamped by the interp VBL clock.

#include "host_loop_internal.h"
#include "host_pacer.h"
#include "run_internal.h"
#include "runahead.h"

#include "openpete_fiber.h"
#include "openpete_host_input.h"
#include "openpete_host_trace.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_profile.h"
#include "openpete_rumble.h"
#include "openpete_time.h"
#include "../rhi/rhi_gpu_audit.h"
#include "bk2_feed.h"
#include "game_extras.h"

#include <stdlib.h>

#ifdef OPENPETE_HAS_PSYCROSS

/* The interp VBL clock: the stamps the interp captures carry, locked to the
 * ideal VBL grid. */
typedef struct {
    op_time_t vclk;     /* the smoothed stamp of the last capture, 0 before the first */
    unsigned  vclk_f;   /* openpete_frame at the last capture */
    op_time_t phase;    /* display-phase bias, -1 until OPENPETE_INTERP_PHASE_MS is read */
} host_vbl_stamp_t;

/* The run's interp VBL clock, on the sim thread. */
static host_vbl_stamp_t s_vbl_stamp = { 0, 0, -1 };

/* Set once the missing-latch ordering warning has been logged. */
static int s_latch_order_warned;

/** @brief Advance the PLL-smoothed VBL clock to a captured tick and return the stamp its interp capture carries.
 *
 * Stamp with a PLL-SMOOTHED VBL clock, not raw op_now(): the raw completion
 * time of a tick jitters by milliseconds with sim/render load, and every
 * snapshot ROLL resets the sub-tick alpha mapping against its stamp -
 * jittery stamps make shown interp time advance 0.05..1.4 ticks between
 * evenly-generated presents (measured on the titlescreen flyby: fast movers
 * judder ±half a tick at ANY --render-fps while the slow background looks
 * fine). Locking stamps to the ideal VBL grid (proportional 1/32 nudge
 * tracks real cadence; full resync on a stall/jump, e.g. CD loads) keeps
 * the present-time -> shown-time mapping continuous across rolls.
 * @param st The clock.
 * @param frame The captured tick's openpete_frame.
 * @param vbl_period_ns The VBL period in wall time, scaled by --tick-scale.
 * @return The stamp: the smoothed clock less the display-phase bias. */
static op_time_t host_vbl_stamp(host_vbl_stamp_t* st, unsigned frame, op_time_t vbl_period_ns)
{
    /* Ideal VBL step in WALL time - scaled so --tick-scale keeps
     * the stamps on the (stretched) grid; an unscaled step makes
     * the pred err exceed the resync bound every tick and stamps
     * degrade to raw jittery completion times. */
    const op_time_t vT = vbl_period_ns;
    op_time_t raw = op_now();
    unsigned df = frame - st->vclk_f;
    st->vclk_f = frame;
    if (st->vclk == 0 || df == 0 || df > 8) st->vclk = raw;
    else {
        op_time_t pred = st->vclk + (op_time_t)df * vT;
        int64_t err = (int64_t)raw - (int64_t)pred;
        if (err > (int64_t)vT || err < -(int64_t)vT) st->vclk = raw;  /* stall/jump resync */
        else st->vclk = pred + (op_time_t)(err / 32);
    }
    /* Display-phase bias: the canonical present fires a few ms
     * BEFORE this tick's grid stamp (present latency + PLL lock
     * to AVERAGE completion), making its alpha slightly NEGATIVE
     * - the [0,1] clamp then snaps shown time forward ~0.5 VBL
     * at every roll and the following intermediates crawl to
     * compensate (measured: ~24% of all motion delivered as
     * 30 Hz jumps - the flyby judder). Biasing the stamp back
     * by a fixed phase keeps every present's alpha inside
     * (0,1): the mapping now−->shown-time stays linear through
     * the roll. Costs that many ms of extra display latency.
     * OPENPETE_INTERP_PHASE_MS overrides (default 10). */
    if (st->phase < 0) {
        /* Default 0: the presents of one interval span exactly
         * one period, so ANY fixed bias clips an end (a +10ms
         * bias moved the clamp from alpha<0 at the canonical
         * present to alpha>1 on the last ~10ms of dense
         * intermediates). The canonical present firing a few
         * ms before the grid stamp is instead absorbed by the
         * consumers allowing slightly NEGATIVE alpha. */
        const char* e = getenv("OPENPETE_INTERP_PHASE_MS");
        double ms = e && *e ? atof(e) : 0.0;
        if (ms < 0.0) ms = 0.0; else if (ms > 16.0) ms = 16.0;
        st->phase = (op_time_t)(ms * 1e6);
    }
    return st->vclk - st->phase;
}

/** @brief Snapshot the transform-bearing regions into the interp ring while the game fiber is yielded (consistent RAM), stamped by the VBL clock.
 *
 * No-op inside the ring unless --render-fps exceeds the sim rate.
 * @param sim_period_ns The iteration's VBL period. */
static void tick_interp_capture(op_time_t sim_period_ns) OP_REQUIRES(sim)
{
    host_trace_bit(HT_F_INTERP_CAP, 1);
    op_time_t stamp = host_vbl_stamp(&s_vbl_stamp, openpete_frame, sim_period_ns);
    openpete_interp_capture(openpete_frame, stamp);
    /* Under deterministic capture the objs ring takes the VBL clock
     * the direct ring stamps its commits with, so per-object sub-tick
     * interps (moby positions/rotation, particles, sparkles, HUD
     * icons, dragon-stars) pair the pinned present clock against a
     * reproducible object timestamp instead of wall time. It advances
     * on a VBL the direct ring skips, as the wall-clock stamp does:
     * an object that changes there is stamped after the capture that
     * last saw its previous state, not equal to it. */
    g_openpete_tick_stamp = openpete_interp_deterministic()
                          ? openpete_interp_det_vbl_stamp(openpete_frame) : stamp;
}

/** @brief Run the post-tick work of a VBL that advanced openpete_frame: the game hook, the runahead lead, the interp captures at the led head and the runahead settle.
 * @param it The iteration; a speculation adds OP_PACE_F_SPECULATIVE to its VBL flags. */
static void tick_post(host_iter_t* it) OP_REQUIRES(sim)
{
    game_on_frame(&g_game.cpu, openpete_frame);

    /* Runahead: the tick above is the
     * COMMITTED timeline. Save it, speculate 2N more VBLs
     * with held input (audible SPU muted; external side
     * effects gated by openpete_speculative), let the interp
     * captures below snapshot the LED state, then restore
     * the real tick after them - everything downstream
     * (hash/position logs, input log, recorders, pacer,
     * canonical state) sees the real timeline while every
     * present interpolates one led by ~N ticks. Measured
     * L_submit = 2 ticks; N=2 collapses it.
     *
     * ONCE PER TICK, on the tick's DRAW VBL only (the real
     * yield produced a present): the led state only advances
     * once per game tick, so speculating on the update VBL
     * too costs a full extra speculation for a capture that
     * dedupes away - and that cost lands INSIDE the present-
     * critical iteration, dragging the virtual present clock
     * behind the capture stamps (measured: alpha drifting to
     * -0.27, motion delivered as 30 Hz steps = the "whole
     * world jittery" report). Non-speculated ticked
     * iterations skip the interp captures entirely - the
     * ring must never see a real-timeline (led-minus-N)
     * state or it would pair backward. */
    const runahead_lead_t ra = runahead_lead();
    if (ra.outcome != RA_NONE) it->vbl.flags |= OP_PACE_F_SPECULATIVE;

    if (!ra.skip_capture) tick_interp_capture(it->sim_period_ns);

    /* Per-object (indirect) snapshot - walks the game's
     * object pools (g_LevelMobys) into the keyed prev/curr
     * table for sub-tick lerp. Same read-only, same
     * --render-fps>sim gate. No-op if no game callback. */
    if (!ra.skip_capture) openpete_interp_objs_run_capture();

    /* Runahead: captures done at the led head - roll back to
     * the committed tick before anything else observes RAM. */
    runahead_settle(ra);
}

/** @brief Warn once when an iteration runs the game fiber with no latch decision made this iteration.
 *
 * Start-alignment invariant: an iteration that runs the game fiber must
 * have made its latch decision THIS iteration, before this point. The input
 * window that external injection measures - one VBL period, grid-aligned -
 * is a property of that ordering, so a refactor that moved the latch after
 * the switch, or skipped it on some path, would silently change the window
 * while every phase statistic kept reporting normally. Checked rather than
 * commented; warned rather than fatal, because a player's game must not die
 * for a diagnostic.
 * @param it The iteration. */
static void tick_latch_order_check(const host_iter_t* it)
{
    if (!openpete_host_input_legacy() &&
        it->vbl.latch_seq == OP_PACE_LATCH_NONE) {
        if (!s_latch_order_warned++)
            OPL_ERROR_F("input",
                        "ORDERING: VBL %u runs the fiber with no latch "
                        "decision - input phase is no longer defined "
                        "by the pump->decode->latch->switch order",
                        openpete_frame);
    }
}

/** @brief Run one VBL in the game fiber inside the pace-log bracket, then the step, present-owed, rumble and eager bookkeeping it settles.
 * @param S The session: the pause and step flags.
 * @param it The iteration: sets the VBL bracket and ticked. */
static void tick_run_vbl(host_session_t* S, host_iter_t* it) OP_REQUIRES(sim)
{
    const int pace_paced = !(it->vbl.flags & OP_PACE_F_UNLOCK);
    const bool stepping = step_begin_vbl(S->paused, &S->step_one);
    game_on_frame_pre(&g_game.cpu, openpete_frame);
    /* Calibration stall (OPENPETE_PACE_INJECT_US), off unless asked
     * for: a known lateness the log must then report, so the
     * instrument is checked against truth rather than trusted. */
    openpete_pace_inject(pace_paced);
    tick_latch_order_check(it);

    it->vbl.exec_start_ns = op_now();
    /* Bracket the VBL BEFORE the switch: the guest's pad read fires
     * inside it, and a record stamped against the previous VBL's
     * start would report a phase off by a whole period. */
    openpete_pace_vbl_begin(openpete_frame, it->vbl.exec_start_ns,
                             it->vbl.latch_ns);
    {
        uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
        /* The guest touches neither the device nor device-visible
         * memory while it runs (core/rhi/rhi_gpu_audit.h measures
         * this); under eager production the present thread keeps
         * presenting through this switch. */
        g_openpete_guest_running = 1;
        openpete_fiber_switch(openpete_game_fiber);
        g_openpete_guest_running = 0;
        if (pt0) openpete_phase_add(OPPT_SIM_TICK, openpete_phase_now_us() - pt0);
    }
    it->vbl.exec_end_ns = op_now();
    it->ticked = (openpete_frame != it->frame_before_yield);
    /* A VBL on which the screen changes without a DrawOTag (the
     * display swaps to a buffer holding a scene no present has
     * shown, or the game draws its fed scene into the buffer on
     * screen) owes that scene a present. */
    openpete_present_pending = openpete_native_scene_yield_owes_present(openpete_present_pending);
    step_after_vbl(stepping, &S->step_one);

    /* Sample the actuator buffer the tick just wrote. Here, rather
     * than beside the pad latch, because the guest fills that
     * buffer inside its own PadVSync: reading before the switch would
     * ship the previous tick's bytes and put a whole VBL of avoidable
     * latency on every rumble. Before the runahead speculation,
     * so only the committed timeline can reach a motor. */
    if (it->ticked)
        openpete_rumble_sample();
    eager_note_vbl(it->vbl.exec_end_ns - it->vbl.exec_start_ns,
                   it->vbl.deadline_ns ? it->vbl.exec_start_ns - it->vbl.deadline_ns : 0,
                   openpete_present_pending);
}

/* Documented at the declaration in host_loop_internal.h. */
void host_tick(host_session_t* S, host_iter_t* it) OP_REQUIRES(sim)
{
    /* Hand control to the game until the next VSync(0) yield. Gated so
     * --render-fps>0 outer iterations that don't have a sim tick due
     * skip the yield entirely (input is still pumped; the pacer
     * sleeps to the next deadline). Frame-step adds a second gate:
     * while paused the yield only fires while step_one is raised
     * (step key or overlay), which stays raised until the game draws. */
    it->frame_before_yield = openpete_frame;
    { unsigned e_, l_, j_; (void)bk2_feed_iter_take(&e_, &l_, &j_); }   /* writes outside this yield */
    it->yield_now = it->sim_due && (!S->paused || S->step_one)
                                && !it->rewound;
    host_trace_bit(HT_F_OVR_CLAIMED, it->ovr_claimed);
    host_trace_bit(HT_F_REWOUND, it->rewound);
    host_trace_bit(HT_F_YIELD, it->yield_now);
    /* Pacing log, per VBL. The deadline in force for this iteration is
     * next_sim.at as it stands BEFORE the tail advances it - the grid
     * point the pacer slept to at the end of the previous iteration - so
     * exec_start minus that is the tick's start-phase error.
     *
     * `openpete_unlock_fps` carries the previous iteration's verdict,
     * which is the one that governed the sleep this iteration started
     * from: the right question is whether a deadline was in force, not
     * what the next one will be. */
    it->vbl.deadline_ns = host_pacer_vbl_deadline(&S->pacer, &it->vbl.flags);
    if (!it->yield_now) return;
    tick_run_vbl(S, it);
    if (it->ticked) tick_post(it);
}

#endif /* OPENPETE_HAS_PSYCROSS */
