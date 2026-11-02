// core/runtime/host_pacer.c - the host loop's pacer: periods, phase-locked
// deadlines, the slip clamp, the unlock verdict and the sleep between
// iterations.
//
// One time source (op_now / op_sleep_until) serves both pacer modes. The
// legacy match-sim pacer (render_fps 0) presents once per tick; the
// wall-clock dual pacer (render_fps > 0) drives the loop at render cadence
// and runs the sim when its deadline passes. Both step phase-locked
// deadlines in integer ns and clamp slips identically.
//
// Period choices:
//   sim_period    = the PSX 240p VBL, exactly 37609/2250000 s
//                   (59.8261054... Hz - see OP_VBL_* in openpete_time.h)
//   render_period = 1/render_fps when render_fps > 0, else sim_period (one
//                   present per sim tick).
//
// Both are op_period_t, so the fractional nanosecond each period carries (a
// VBL is 16715111 + 1/9 ns) accrues on the deadline instead of being
// truncated away every step. Deadlines land on floor(n * period) for all n:
// under a nanosecond off the true grid, and never drifting further.
//
// Invariant: the loop yields the game fiber at most once per iteration.
// Multi-VBL catch-up would race-complete the `pre - post < 2` GPU-pacing
// loops in draw.c and the 210-VBL Sony / Universal logo waits in
// initialization.c. The slip clamp moves the deadline forward without
// firing extra yields.
#include "host_pacer.h"

#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"

#include <stdlib.h>
#include <string.h>

/* Declared in PsyCross's private src/audio/PsyX_SPUFeed.h, which is not on
 * any public include path. */
extern void PsyX_SPUFeed_SetNominalRate(double rate);

/** @brief Return the present period for a render rate at a tick scale: 1/fps scaled, or the sim period at fps 0.
 * @param fps The render rate, 0 for the legacy pacer.
 * @param tick_scale The tick scale.
 * @param sim_period The tick-scaled sim period.
 * @return The period. */
static op_period_t pacer_render_period(int fps, double tick_scale, op_period_t sim_period)
{
    return (fps > 0)
        ? op_period_from_ns((op_time_t)((double)(OP_NS_PER_SEC / (op_time_t)fps)
                                        / tick_scale))
        : sim_period;   /* sim_period is already tick-scaled */
}

/** @brief Derive the sim period, its ns value, the tick period and the slip threshold from the live tick scale, and publish the tick period.
 *
 * One wall-time tick serves every sub-tick alpha denominator (the snapshot
 * ring and the per-object phase lerps). It stretches with --tick-scale:
 * alphas are wall-clock-derived, so an unscaled cap would sweep every alpha
 * 1/tick_scale times too fast and pin it at the clamp for the rest of the
 * tick. It is rounded to whole ns, since it is an alpha denominator and not
 * a deadline.
 * @param P The pacer. */
static void pacer_derive_sim(host_pacer_t* P)
{
    P->sim_period    = op_scaled_vbl_period(P->tick_scale);
    P->sim_period_ns = op_period_ns(P->sim_period);
    P->tick_period   = op_period_mul(P->sim_period, OP_TICK_VBLS);
    g_openpete_tick_period_ns = op_period_ns(P->tick_period);
    P->slip_threshold_ns = 4 * P->sim_period_ns;   /* ~67 ms (scaled) */
}

/** @brief Arm the --record-cfr offline capture clock: capture_mult presents per tick on a clock the pacer advances itself.
 *
 * The present schedule is then exact however long a present takes, so a
 * software rasterizer and a fast GPU emit the identical sequence and two
 * captures at different rates cover the same span of game time. The render
 * period divides the tick period rather than coming from an integer
 * --render-fps: 60 Hz against a 59.826 Hz VBL is a 2.0058 ratio, which
 * hands some ticks two intermediates and others three. op_period_div keeps
 * the division exact (a tick split three ways is 11143407 + 11/27 ns). A
 * multiplier of 1 wants no sub-tick presents, which is the legacy pacer.
 * @param P The pacer.
 * @param opts The parsed options. */
static void pacer_capture_clock(host_pacer_t* P, const openpete_options_t* opts)
{
    P->capture_mult = (opts->record_cfr > 0.0)
        ? (int)(opts->record_cfr / OP_TICK_HZ + 0.5) : 0;
    if (P->capture_mult <= 0) return;
    if (P->capture_mult > 1) {
        P->render_fps    = (int)(opts->record_cfr + 0.5);  /* nominal, for logs + the "paced" test */
        P->render_period = op_period_div(P->tick_period, P->capture_mult);
    } else {
        P->render_fps    = 0;
        P->render_period = P->sim_period;
    }
    op_virtual_clock_enable(OP_NS_PER_SEC);
    OPL_INFO_F("pacer",
               "--record-cfr %.5f: offline capture clock, %d present(s) per tick "
               "(%.4f ms), sim %.4f ms",
               opts->record_cfr, P->capture_mult,
               (double)op_period_ns(P->render_period) / 1e6,
               (double)P->sim_period_ns / 1e6);
}

void host_pacer_init(host_pacer_t* P, const openpete_options_t* opts, int refresh_hz)
{
    memset(P, 0, sizeof(*P));
    /* -1 = unset: windowed runs default to the display's refresh rate (the
     * dual pacer with transform interpolation); headless keeps the legacy
     * pacer, since those runs are unpaced and the oracle and determinism
     * harnesses expect one present per tick. */
    P->launch_fps = (opts->render_fps >= 0) ? opts->render_fps
                  : opts->headless          ? 0
                                            : refresh_hz;
    if (opts->render_fps < 0 && !opts->headless)
        OPL_INFO_F("pacer", "--render-fps unset: defaulting to display refresh %d Hz",
                   P->launch_fps);
    /* --tick-scale F divides both periods by F, so the count of sub-tick
     * presents per tick is invariant: a slow-motion view of the same
     * interpolation. F == 1 keeps the exact rational VBL; any other F
     * scales through a double and lands on a whole-ns period. */
    P->launch_scale = (opts->tick_scale > 0.0) ? opts->tick_scale : 1.0;
    P->tick_scale   = P->launch_scale;
    P->render_fps   = P->launch_fps;
    P->sim_period   = op_scaled_vbl_period(P->tick_scale);
    P->render_period = pacer_render_period(P->render_fps, P->tick_scale, P->sim_period);
    PsyX_SPUFeed_SetNominalRate(P->tick_scale);
    pacer_derive_sim(P);
    pacer_capture_clock(P, opts);
}

/** @brief Enable interpolation capture when the render rate exceeds the sim rate, at two or more capture presents per tick, or for intermediate screenshots.
 *
 * At parity there is nothing to interpolate between. Offline capture at 2
 * presents per tick puts the render period exactly on the VBL period, which
 * the rate test reads as parity; the game draws every other VBL, so one of
 * the two is a sub-tick present. Intermediate screenshot targets capture
 * the ring headless with deterministic stamps, one tick apart, so the
 * forced present lands alpha exactly at 0.5; the live sub-tick present
 * stays headless-blocked, so the canonical dump is unchanged.
 * @param P The pacer.
 * @param opts The parsed options. */
static void pacer_interp_gate(const host_pacer_t* P, const openpete_options_t* opts)
{
    const op_time_t render_period_ns = op_period_ns(P->render_period);
    /* The cadence counters judge a display hole against this. */
    openpete_pace_set_slot_ns((int64_t)render_period_ns);
    const op_time_t sim_hz_x1000 = (OP_NS_PER_SEC * 1000) / P->sim_period_ns;
    const op_time_t render_hz_x1000 = (render_period_ns > 0)
        ? (OP_NS_PER_SEC * 1000) / render_period_ns
        : 0;
    if (P->render_fps > 0 && render_hz_x1000 > sim_hz_x1000) {
        openpete_interp_set_enabled(1);
        OPL_INFO_F("interp", "capture enabled (render-fps=%d > sim ~30 Hz)",
                   P->render_fps);
    }
    if (P->capture_mult >= 2) {
        openpete_interp_set_enabled(1);
        OPL_INFO_F("interp", "capture enabled (offline capture, %d present(s) per tick)",
                   P->capture_mult);
    }
    if (opts->screenshot_intermediate_count > 0) {
        openpete_interp_set_enabled(1);
        openpete_interp_set_deterministic(1, g_openpete_tick_period_ns);
        OPL_INFO_F("interp",
                   "deterministic capture enabled (%d intermediate screenshot target(s), tick=%lld ns)",
                   opts->screenshot_intermediate_count, (long long)g_openpete_tick_period_ns);
    }
}

void host_pacer_start(host_pacer_t* P, const openpete_options_t* opts)
{
    /* Whether a deadline was in force for the iteration that just ended.
     * The loop's own verdict is not written until the tail, so iteration 1
     * has nothing to read, and an unlocked headless run would otherwise
     * record its first VBL as paced against the pre-boot anchor. A separate
     * field rather than seeding openpete_unlock_fps: that global gates
     * render paths mid-iteration. */
    P->unlock_prev = (opts->headless && !host_pacer_force_pacing());
    const op_time_t start_ns = op_now();
    op_deadline_anchor(&P->next_sim, start_ns);
    op_deadline_anchor(&P->next_render, start_ns);
    if (P->render_fps > 0) {
        OPL_INFO_F("pacer", "wall-clock dual-pacer: sim=%.6f ms (%.6f Hz) render=%.6f ms (render-fps=%d, --tick-scale=%.3f)",
                   (double)P->sim_period_ns / 1e6, op_period_hz(P->sim_period),
                   (double)op_period_ns(P->render_period) / 1e6,
                   P->render_fps, P->tick_scale);
    } else {
        OPL_INFO_F("pacer", "legacy pacer: sim=%.6f ms NTSC (%.6f Hz VBL, --render-fps=0, --tick-scale=%.3f)",
                   (double)P->sim_period_ns / 1e6, op_period_hz(P->sim_period), P->tick_scale);
    }
    /* Capture is a read-only PSX-RAM copy on the host fiber while the game
     * fiber is yielded inside VSync(0), so the ring comes up unconditionally. */
    if (openpete_interp_init() == 0)
        pacer_interp_gate(P, opts);
}

void host_pacer_request_fps(host_pacer_t* P, int fps)
{
    if (fps == P->render_fps) return;
    P->render_fps = fps;
    P->render_fps_dirty = true;
}

void host_pacer_request_scale(host_pacer_t* P, double scale)
{
    if (scale == P->tick_scale) return;
    P->tick_scale = scale;
    P->tick_scale_dirty = true;
}

void host_pacer_apply_live(host_pacer_t* P)
{
    if (P->tick_scale_dirty) {
        P->tick_scale_dirty = false;
        pacer_derive_sim(P);
        P->render_fps_dirty = true;   /* the render period is tick-scaled too */
        PsyX_SPUFeed_SetNominalRate(P->tick_scale);
        OPL_INFO_F("pacer", "tick-scale -> %.3f (sim %.6f ms, %.6f Hz)",
                   P->tick_scale, (double)P->sim_period_ns / 1e6,
                   op_period_hz(P->sim_period));
    }
    if (!P->render_fps_dirty) return;
    /* Re-anchor at now so a lower-to-higher switch fires no burst of
     * catch-up presents; keep the interpolation gate in step with the
     * startup rule (enabled only when the render rate exceeds the sim rate). */
    P->render_fps_dirty = false;
    P->render_period = pacer_render_period(P->render_fps, P->tick_scale, P->sim_period);
    const op_time_t render_period_ns = op_period_ns(P->render_period);
    openpete_pace_set_slot_ns((int64_t)render_period_ns);
    const op_time_t anchor = op_now();
    op_deadline_anchor(&P->next_sim, anchor);
    op_deadline_anchor(&P->next_render, anchor);
    P->regrid_pending = true;
    const op_time_t sim_hz_x1000  = (OP_NS_PER_SEC * 1000) / P->sim_period_ns;
    const op_time_t rend_hz_x1000 = (render_period_ns > 0)
        ? (OP_NS_PER_SEC * 1000) / render_period_ns : 0;
    openpete_interp_set_enabled(P->render_fps > 0 && rend_hz_x1000 > sim_hz_x1000 ? 1 : 0);
    OPL_INFO_F("pacer", "render-fps -> %d (%.4f ms/present)",
               P->render_fps, (double)render_period_ns / 1e6);
}

bool host_pacer_force_pacing(void)
{
    static int s_init = 0, s_on = 0;
    if (!s_init) {
        const char* v = getenv("OPENPETE_FORCE_PACING");
        s_on = (v && *v && *v != '0') ? 1 : 0;
        s_init = 1;
        OPL_INFO_F("pacer", "OPENPETE_FORCE_PACING=%d (env=%s)",
                   s_on, v ? v : "<unset>");
    }
    return s_on || op_virtual_clock_active();
}

bool host_pacer_unlock_verdict(bool want)
{
    static int s_logged = 0;
    if (!want || !op_virtual_clock_active()) return want;
    if (!s_logged) {
        s_logged = 1;
        OPL_INFO_F("pacer", "--record-cfr: unlock request (turbo, --unlock-frames or "
                            "skip-to-level) ignored; every tick advances the capture clock");
    }
    return false;
}

bool host_pacer_sim_due(const host_pacer_t* P, bool unlock)
{
    return (P->render_fps == 0) || unlock || (op_now() >= P->next_sim.at);
}

op_time_t host_pacer_vbl_deadline(host_pacer_t* P, uint8_t* flags)
{
    const bool paced = !P->unlock_prev;
    op_time_t deadline = paced ? P->next_sim.at : 0;
    if (P->regrid_pending) { deadline = 0; P->regrid_pending = false; }
    *flags = paced ? 0u : OP_PACE_F_UNLOCK;
    if (P->slip_pending) { *flags |= OP_PACE_F_SLIPPED; P->slip_pending = false; }
    return deadline;
}

op_time_t host_pacer_present_deadline(const host_pacer_t* P, bool sim_due)
{
    return sim_due ? P->next_sim.at + P->sim_period_ns : P->next_sim.at;
}

op_time_t host_pacer_render_slot(const host_pacer_t* P)      { return P->next_render.at; }
op_time_t host_pacer_sim_deadline(const host_pacer_t* P, int k) { return P->next_sim.at + k * P->sim_period_ns; }
op_time_t host_pacer_sim_period_ns(const host_pacer_t* P)    { return P->sim_period_ns; }
op_time_t host_pacer_render_period_ns(const host_pacer_t* P) { return op_period_ns(P->render_period); }
int       host_pacer_render_fps(const host_pacer_t* P)       { return P->render_fps; }
double    host_pacer_tick_scale(const host_pacer_t* P)       { return P->tick_scale; }

void host_pacer_note_unlock(host_pacer_t* P, bool unlock)
{
    if (unlock != P->unlock_prev)
        openpete_pace_window(OP_PACE_W_UNLOCK, unlock ? 1 : 0);
    P->unlock_prev = unlock;
}

/** @brief Read OPENPETE_PACE_FLIP_LOG once: whether the pacer's idle wait measures the display flip.
 *
 * When on, pacer_sleep_until spends the sleep before the next deadline in a
 * bounded vkWaitForPresentKHR on the present just queued, which returns at
 * the flip, a timestamp the engine cannot otherwise observe. The wait runs
 * on the presenting thread only, since the swapchain is externally
 * synchronized. It stays a measurement and off by default: a driver whose
 * wait misbehaves would stall the sim clock.
 * @return 1 when the variable is set to a value not starting with '0'. */
static int pace_flip_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("OPENPETE_PACE_FLIP_LOG");
        on = (e && *e && *e != '0');
    }
    return on;
}

/** @brief Sleep to a deadline and record the requested and actual wake instants.
 *
 * Recording both instants splits a late tick into an OS wake that returned
 * late and work after waking that ran slow. With pace_flip_log_on() and no
 * eager production, the first part of the sleep is a bounded wait for the
 * last queued present's flip (logged through openpete_pace_flip); the wait
 * ends no later than the sim deadline minus the pacer slack, and
 * op_sleep_until then runs the remainder, so the pacing is unchanged.
 * @param deadline_ns The instant to wake at.
 * @param sim_deadline_ns The next sim tick's deadline, or 0 for none; bounds
 *        the flip wait.
 * @param eager_active Eager production owns the presents; its present
 *        thread does its own flip logging, which a wait here would race. */
static void pacer_sleep_until(op_time_t deadline_ns, op_time_t sim_deadline_ns,
                              bool eager_active)
{
    const op_time_t req = op_now();
    if (pace_flip_log_on() && !eager_active) {
        /* Never wait into the pacer's slack ahead of a SIM deadline. The sleep
         * is not idle right up to it: op_sleep_until parks until the deadline
         * minus slack and the loop uses what remains to be ready on time, so
         * a wait into that window makes ticks late.
         *
         * Bounding by the RENDER target instead would disable the wait
         * wherever the slack exceeds a present slot, which at --render-fps 480
         * (2.08 ms slots, 2.5 ms slack) is always. Only the tick's own margin
         * is protected. */
        const op_time_t guard = sim_deadline_ns
            ? sim_deadline_ns - op_pacer_slack_ns() : deadline_ns;
        const op_time_t until = (guard < deadline_ns) ? guard : deadline_ns;
        const op_time_t budget = until - op_now();
        if (budget > 0) {
            uint64_t id = 0, waited = 0;
            if (openpete_native_render_present_wait((uint64_t)budget, &id,
                                                    &waited) == 1)
                openpete_pace_flip(id, (int64_t)op_now(), (int64_t)waited);
        }
    }
    op_sleep_until(deadline_ns);
    openpete_pace_wake(req, deadline_ns, op_now());
}

/* The two deadline kinds the slip clamp serves. A render slip is
 * display-only: a present was late, nothing about the game moved. A sim slip
 * means wall-clock time passed that the game clock did not account for, so
 * RTA time and tick time have diverged by that much. Sim slips are warned
 * per event and mark the next VBL record; render slips are counted and
 * reported once, because at high --render-fps a struggling host can produce
 * them continuously. */
typedef enum { PACER_SLIP_SIM, PACER_SLIP_RENDER } pacer_slip_kind_t;

/** @brief Clamp a deadline that has fallen more than the slip threshold behind @p now forward by whole periods, and account the discarded time.
 *
 * Catching up instead would fire iterations back to back; a stall is never
 * repaid as a burst. The clamp discards real time, so it is never silent:
 * every event is counted and written to the pace log.
 * @param P The pacer.
 * @param kind Which deadline: the sim deadline or the render slot.
 * @param now The instant the lateness is measured at. */
static void pacer_clamp_slip(host_pacer_t* P, pacer_slip_kind_t kind, op_time_t now)
{
    const bool sim = (kind == PACER_SLIP_SIM);
    op_deadline_t* d = sim ? &P->next_sim : &P->next_render;
    const op_period_t period = sim ? P->sim_period : P->render_period;
    const op_time_t period_ns = op_period_ns(period);
    if (period_ns <= 0 || now - d->at <= P->slip_threshold_ns) return;
    /* Captured before the advance: afterwards d->at is the new grid point
     * and the lateness is gone. */
    const op_time_t behind = now - d->at;
    const op_time_t skip = behind / period_ns;
    op_deadline_advance(d, period, skip);
    if (!sim) {
        P->render_slip_events++;
        P->render_slip_periods += (uint64_t)skip;
        openpete_pace_slip(OP_PACE_SLIP_RENDER, (int64_t)skip, (int64_t)behind);
        return;
    }
    P->sim_slip_events++;
    P->sim_slip_periods += (uint64_t)skip;
    openpete_pace_slip(OP_PACE_SLIP_SIM, (int64_t)skip, (int64_t)behind);
    P->slip_pending = true;
    OPL_WARN_F("pacer",
               "sim slip: host stalled %.1f ms (dropped %lld VBL period(s) "
               "of wall time)", (double)behind / 1e6, (long long)skip);
}

/** @brief Step the dual pacer at the loop tail and sleep to its sooner deadline.
 *
 * The sim deadline advances on sim-due iterations that ticked; the render
 * deadline on every iteration. A dispatch.c no-tick stub yield returns
 * without bumping openpete_frame, and charging it a VBL period of wall time
 * would slow the guest in proportion to how many such yields a tick takes.
 * Under eager production the render grid is the present thread's: the loop
 * only borrows its phase when it plans a span, so the slot is placed on the
 * grid's first instant after now (nothing was missed, so no slip is
 * accounted) and the sleep is to the sim deadline alone. It never steps
 * while already ahead of now, so back-to-back catch-up iterations cannot
 * push it more than one period past now.
 * @param P The pacer.
 * @param sim_due The iteration ran the sim.
 * @param ticked The game fiber yielded with a frame bump.
 * @param eager_active Eager production owns the presents. */
static void pacer_advance_dual(host_pacer_t* P, bool sim_due, bool ticked, bool eager_active)
{
    const op_time_t now = op_now();
    if (sim_due && ticked) {
        op_deadline_advance(&P->next_sim, P->sim_period, 1);
        pacer_clamp_slip(P, PACER_SLIP_SIM, now);
    }
    if (eager_active) {
        const op_time_t rp = op_period_ns(P->render_period);
        if (rp > 0 && now >= P->next_render.at)
            op_deadline_advance(&P->next_render, P->render_period,
                                (now - P->next_render.at) / rp + 1);
        pacer_sleep_until(P->next_sim.at, P->next_sim.at, eager_active);
        return;
    }
    op_deadline_advance(&P->next_render, P->render_period, 1);
    pacer_clamp_slip(P, PACER_SLIP_RENDER, now);
    const op_time_t next = (P->next_sim.at < P->next_render.at)
                         ? P->next_sim.at : P->next_render.at;
    pacer_sleep_until(next, P->next_sim.at, eager_active);
}

void host_pacer_advance(host_pacer_t* P, bool unlock, bool sim_due, bool ticked,
                        bool eager_active, bool shutdown)
{
    /* Unlock (turbo, headless, a scheduled fast-forward): re-anchor at now
     * so the release does not stall. Shutdown skips the sleep too, so a
     * misbehaving OS timer cannot hold the quit. */
    if (unlock || shutdown) {
        const op_time_t anchor = op_now();
        op_deadline_anchor(&P->next_sim, anchor);
        op_deadline_anchor(&P->next_render, anchor);
        P->regrid_pending = true;
        return;
    }
    if (P->render_fps > 0) {
        pacer_advance_dual(P, sim_due, ticked, eager_active);
        return;
    }
    /* Legacy match-sim pacer: one step per tick, taken only when the game
     * fiber yielded with a frame bump, so a tick full of no-tick stub yields
     * does not park the loop for a period per stub. */
    if (!ticked) return;
    op_deadline_advance(&P->next_sim, P->sim_period, 1);
    pacer_clamp_slip(P, PACER_SLIP_SIM, op_now());
    pacer_sleep_until(P->next_sim.at, P->next_sim.at, eager_active);
}

bool host_pacer_slip_report(const host_pacer_t* P)
{
    if (!P->sim_slip_events && !P->render_slip_events) return false;
    OPL_INFO_F("pacer",
               "slip totals: sim %llu event(s) / %llu period(s) discarded; "
               "render %llu event(s) / %llu period(s)",
               (unsigned long long)P->sim_slip_events,
               (unsigned long long)P->sim_slip_periods,
               (unsigned long long)P->render_slip_events,
               (unsigned long long)P->render_slip_periods);
    return true;
}
