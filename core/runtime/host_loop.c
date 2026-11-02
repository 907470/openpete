// core/runtime/host_loop.c - the host loop's phases: the tick (the VBL an
// iteration runs in the game fiber, its pace-log bracket, and on a tick the
// runahead lead around the interp captures, stamped by the interp VBL
// clock), the quiescent ops, the RAM dump, the exit test and the sample log.

#include "host_loop_internal.h"
#include "host_pacer.h"
#include "run_internal.h"
#include "runahead.h"
#include "eager.h"
#include "host_hotkeys.h"
#include "host_input_tick.h"
#include "host_state_ops.h"

#include "openpete_fiber.h"
#include "openpete_host_input.h"
#include "openpete_host_trace.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_log.h"
#include "openpete_menu.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_profile.h"
#include "openpete_psx.h"
#include "openpete_psxtrace.h"
#include "openpete_record.h"
#include "openpete_rumble.h"
#include "openpete_time.h"
#include "rhi.h"
#include "../rhi/rhi_gpu_audit.h"
#include "bk2_feed.h"
#include "game_extras.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Documented at the declaration in host_loop_internal.h. */
void host_quiescent_ops(host_session_t* S, host_iter_t* it) OP_REQUIRES(sim)
{
    /* Save-state hotkeys (default 1 quick-save, 2 quick-load, 3 cycle
     * slot). Edge-triggered, one action per press. The game fiber is
     * yielded right now, so g_game.cpu and g_ram are quiescent - we can
     * snapshot or restore without racing the recompiled code.
     *
     * Run BEFORE PsyX_UpdateInput so a load that overwrites the pad
     * buffer (which lives in restored g_ram) is immediately followed by
     * a fresh write of current input - otherwise the game fiber reads
     * stale buttons from the saved RAM and feels like input has stopped
     * registering until the next iter.
     *
     * Skipped while the settings menu is open - it owns input. The
     * keymap latch keeps advancing underneath, so a key held across the
     * close is already latched and cannot misfire on the first iteration
     * after it, and a press made while the menu was up is swallowed
     * rather than replayed into the game. */
    if (openpete_menu_is_open()) return;
    it->rewound = state_ops_quiescent(S->opts, S->paused, S->bk2.active);

    /* Any op above may have re-anchored openpete_frame. */
    host_input_restamp_after_ops(S, it);

    host_pause_step(&S->paused, &S->step_one);

    host_hotkeys_toggles(S->split);
    host_hotkeys_fillpick();
    host_hotkeys_camera();
}

/** @brief Write the 528-byte CPU register block beside a RAM dump, in the halt-at-pc dump's format.
 *
 * Reads g_game.cpu directly: the game fiber is yielded inside VSync(0) and
 * the host fiber shares this static struct.
 * @param path The --halt-cpu-dump-output path. */
static void ram_dump_cpu(const char* path)
{
    FILE* cfp = fopen(path, "wb");
    if (!cfp) return;
    const CPUState* c = &g_game.cpu;
    uint32_t magic = 0x31765343u;
    fwrite(&magic, 1, 4, cfp);
    uint32_t gpr[32] = {
        0u,    c->at, c->v0, c->v1,
        c->a0, c->a1, c->a2, c->a3,
        c->t0, c->t1, c->t2, c->t3,
        c->t4, c->t5, c->t6, c->t7,
        c->s0, c->s1, c->s2, c->s3,
        c->s4, c->s5, c->s6, c->s7,
        c->t8, c->t9, c->k0, c->k1,
        c->gp, c->sp, c->fp, c->ra,
    };
    fwrite(gpr, 4, 32, cfp);
    /* PC at dump moment is wherever the game fiber
     * yielded - typically inside VSync's host yield
     * (PSYQ-side), which means the game's call site of
     * VSync(). Without a fiber-aware unwinder we can't
     * know precisely; report 0 so consumers don't
     * mis-interpret. sp is the load-bearing field for
     * the masked diff. */
    uint32_t pc = 0u;
    fwrite(&pc, 1, 4, cfp);
    fwrite(&c->hi, 1, 4, cfp);
    fwrite(&c->lo, 1, 4, cfp);
    static const uint32_t zero32[32] = {0};
    fwrite(zero32, 4, 32, cfp);  /* cop0  */
    fwrite(zero32, 4, 32, cfp);  /* gted  */
    fwrite(zero32, 4, 32, cfp);  /* gtec  */
    fclose(cfp);
    OPL_INFO_F("ram-dump",
               "cpu wrote 528 bytes to %s (sp=0x%08X gp=0x%08X)",
               path, c->sp, c->gp);
}

/* Documented at the declaration in host_loop_internal.h. */
void host_ram_dump_if_due(host_session_t* S) OP_REQUIRES(sim)
{
    const openpete_options_t* opts = S->opts;
    /* Runs outside the present_pending gate so a request for the very
     * first rendered frame (ram-dump-on-frame=0) still fires even on
     * iters where DrawOTag hasn't queued anything yet. Use >= with a
     * one-shot guard so the trigger is forgiving of skipped frame
     * numbers. */
    if (S->ram_dumped || opts->ram_dump_on_frame < 0 ||
        (int)openpete_feed_key_now() < opts->ram_dump_on_frame)
        return;
    S->ram_dumped = true;
    const char* path = opts->ram_dump_output;
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        OPL_ERROR_F("ram-dump", "fopen %s failed: %s",
                    path, strerror(errno));
    } else {
        const size_t ram_size = 2u * 1024u * 1024u;
        size_t n = fwrite(openpete_psx_get_ram(), 1, ram_size, fp);
        fclose(fp);
        if (n != ram_size) {
            OPL_ERROR_F("ram-dump",
                        "short write to %s: %zu of %zu bytes",
                        path, n, ram_size);
        } else {
            OPL_INFO_F("ram-dump",
                       "wrote %zu bytes to %s on frame %u (target=%d)",
                       ram_size, path, openpete_frame,
                       opts->ram_dump_on_frame);
        }
    }
    /* Optional CPU state dump alongside the RAM dump. */
    if (opts->halt_cpu_dump_output) ram_dump_cpu(opts->halt_cpu_dump_output);
}

/** @brief Return whether the run has reached --frame-limit and may exit there.
 *
 * The launcher hand-off trace (rhi_launcher_trace_pending) holds the exit
 * until it logs the game's first present, which the present thread can make
 * after the limit; the hold is logged once.
 * @param opts The parsed options.
 * @return true when the feed key reached a set limit and no trace holds it. */
static bool frame_limit_due(const openpete_options_t* opts) OP_REQUIRES(sim) {
    if (opts->frame_limit <= 0 || (int)openpete_feed_key_now() < opts->frame_limit) return false;
    if (!rhi_launcher_trace_pending()) return true;
    static bool told = false;
    if (!told) {
        told = true;
        OPL_INFO_F("exit", "%s", "frame-limit held until the launcher trace logs the game's first present");
    }
    return false;
}

/** @brief Observe the --psxtrace-verify verdict and return the exit it asks for, logging the eof summary or the divergence.
 *
 * The trace advances from the dispatch hook (per game-function entry, not
 * per host iteration), which sets terminal_status: 0 still running, 1 eof,
 * 2 divergence, 3 io-error. A divergence under --psxtrace-keep-going asks
 * for no exit.
 * @param opts The parsed options.
 * @return "psxtrace-eof", "psxtrace-divergence", "psxtrace-error", or NULL. */
static const char* psxtrace_exit_reason(const openpete_options_t* opts) OP_REQUIRES(sim)
{
    const openpete_psxtrace_t* psxtrace = run_psxtrace();
    if (!psxtrace) return NULL;
    switch (psxtrace->terminal_status) {
    case 1:
        OPL_INFO_F("psxtrace",
                   "trace exhausted (consumed %u records, "
                   "%llu compared, %llu hash mismatches)",
                   psxtrace->records_consumed,
                   (unsigned long long)psxtrace->frames_compared,
                   (unsigned long long)psxtrace->hashes_diverged);
        return "psxtrace-eof";
    case 2: {
        if (opts->psxtrace_keep_going) return NULL;
        int32_t r = psxtrace->hard_fail_region;
        const char* rname = (r >= 0 &&
            r < (int32_t)psxtrace->region_count)
            ? psxtrace->region_names[r] : "<total>";
        OPL_ERROR_F("psxtrace",
                    "DIVERGED (%s) at frame=%u region=%d (%s) "
                    "op=0x%016llx oracle=0x%016llx (record %u)",
                    psxtrace->hard_fail_reason
                        ? psxtrace->hard_fail_reason : "?",
                    psxtrace->hard_fail_frame,
                    (int)r, rname,
                    (unsigned long long)psxtrace->hard_fail_op,
                    (unsigned long long)psxtrace->hard_fail_oracle,
                    psxtrace->records_consumed);
        return "psxtrace-divergence";
    }
    case 3:
        return "psxtrace-error";
    default:
        return NULL;
    }
}

/* Documented at the declaration in host_loop_internal.h. */
const char* host_exit_reason(const host_session_t* S) OP_REQUIRES(sim)
{
    const openpete_options_t* opts = S->opts;
    const char* trace = psxtrace_exit_reason(opts);
    if (run_shutdown_flagged()) return "user-quit";
    if (trace) return trace;
    if (g_game.done) return "game-returned";
    if (frame_limit_due(opts)) return "frame-limit";
    /* --record-frames: the capture window has closed. Stop here rather
     * than at the next frame/tick boundary so the clip ends on exactly the
     * requested frame count and every rate covers the same game time. */
    if (openpete_record_finished()) return "record-frames";
    if (opts->max_runtime_seconds > 0 &&
        (SDL_GetTicks() - S->start)
            > (Uint64)opts->max_runtime_seconds * 1000)
        return "max-runtime-seconds";
    return NULL;
}

/* Documented at the declaration in host_loop_internal.h. */
void host_loop_sample(const host_session_t* S, const host_iter_t* it) OP_REQUIRES(sim)
{
    /* Sample log so the user can see the loop is alive. Gated on
     * sim_due so a render-only iter under --render-fps>0 doesn't
     * re-log the same frame number. Same gate clears
     * openpete_primitives_drawn after a tick consumed/presented its
     * draws, not on render-only iterations that didn't add any. */
    if (!it->sim_due) return;
    if ((openpete_frame & 0x3F) == 1u) {
        OPL_INFO_F("loop",
                   "frame=%u prims_last=%u elapsed_ms=%llu",
                   openpete_frame, openpete_primitives_drawn,
                   (unsigned long long)(SDL_GetTicks() - S->start));
    }
    openpete_primitives_drawn = 0u;
}

#endif /* OPENPETE_HAS_PSYCROSS */
