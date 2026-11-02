// core/runtime/host_pacer.h - the host loop's pacer: the sim and render
// periods, their phase-locked deadlines, the slip clamp, the unlock verdict
// and the sleep between iterations.
#pragma once

#include "openpete.h"
#include "openpete_time.h"

#include <stdbool.h>
#include <stdint.h>

/* One run's pacer. Every field is written by host_pacer.c only; the host
 * loop and the present code read it through the accessors below. */
typedef struct host_pacer {
    int           launch_fps;          /* --render-fps resolved at launch; the frame-time log's target */
    double        launch_scale;        /* --tick-scale at launch, 1 when unset */
    int           render_fps;          /* live target; 0 selects the legacy match-sim pacer */
    op_period_t   render_period;       /* live present period, tick-scaled */
    bool          render_fps_dirty;    /* a live change awaits the re-anchor */
    double        tick_scale;          /* live --tick-scale */
    bool          tick_scale_dirty;    /* a live change awaits the period re-derivation */
    op_period_t   sim_period;          /* the VBL period, tick-scaled */
    op_time_t     sim_period_ns;       /* sim_period in whole ns */
    op_period_t   tick_period;         /* OP_TICK_VBLS sim periods */
    op_time_t     slip_threshold_ns;   /* lateness past which a clamp discards whole periods */
    int           capture_mult;        /* --record-cfr presents per tick, 0 without the capture clock */
    op_deadline_t next_sim;            /* the next sim deadline */
    op_deadline_t next_render;         /* the next render-grid slot */
    bool          slip_pending;        /* a sim clamp fired; the next VBL record carries SLIPPED */
    bool          regrid_pending;      /* the deadlines were re-anchored; the next VBL records no deadline */
    bool          unlock_prev;         /* the previous iteration's unlock verdict */
    uint64_t      sim_slip_events;
    uint64_t      sim_slip_periods;
    uint64_t      render_slip_events;
    uint64_t      render_slip_periods;
} host_pacer_t;

/** @brief Resolve the launch render rate and tick scale into the periods, and arm the --record-cfr capture clock.
 *
 * An unset --render-fps (-1) defaults to @p refresh_hz windowed and to 0,
 * the legacy match-sim pacer, headless. Writes g_openpete_tick_period_ns
 * and the SPU feed's nominal rate.
 * @param P The pacer; every field is overwritten.
 * @param opts The parsed options.
 * @param refresh_hz The display refresh rate. */
void host_pacer_init(host_pacer_t* P, const openpete_options_t* opts, int refresh_hz);

/** @brief Seed the previous-iteration unlock state, anchor both deadlines at now, log the pacer mode and set the interpolation capture gate.
 *
 * Runs after host_pacer_init, once the live-knob getters report its rates.
 * @param P The pacer.
 * @param opts The parsed options. */
void host_pacer_start(host_pacer_t* P, const openpete_options_t* opts);

/** @brief Record a live --render-fps request; a value equal to the live target changes nothing.
 * @param P The pacer.
 * @param fps The requested rate, 0 for the legacy pacer. */
void host_pacer_request_fps(host_pacer_t* P, int fps);

/** @brief Record a live --tick-scale request; a value equal to the live scale changes nothing.
 * @param P The pacer.
 * @param scale The requested scale. */
void host_pacer_request_scale(host_pacer_t* P, double scale);

/** @brief Apply the pending live tick-scale and render-fps changes: re-derive the periods, re-anchor both deadlines and re-sync the interpolation gate.
 *
 * A tick-scale change re-derives the sim, tick and render periods and the
 * slip threshold; either change re-anchors the deadlines at now and marks
 * the next VBL record as regridded.
 * @param P The pacer. */
void host_pacer_apply_live(host_pacer_t* P);

/** @brief Return whether a headless run paces like a windowed one: OPENPETE_FORCE_PACING is set or the virtual clock is on.
 *
 * A headless run otherwise runs unlocked (fast-forward). The variable is
 * read once and logged. The virtual clock (--record-cfr) advances only
 * inside the pacer's op_sleep_until, which the unlocked branch never calls:
 * unlocked, every capture would carry the same timestamp and one CFR
 * ordinal. Pacing on the virtual clock never blocks, so it costs no speed.
 * @return true when pacing is forced. */
bool host_pacer_force_pacing(void);

/** @brief Return the iteration's unlock verdict, refused while the virtual clock (--record-cfr) is on.
 *
 * The virtual clock advances only inside the pacer's op_sleep_until, which
 * the unlocked branch never calls: an unlocked tick would run with a frozen
 * clock, its capture would repeat the previous tag and the recording would
 * drop it as an ordinal already written. Under the virtual clock the paced
 * branch never blocks, so turbo, --unlock-frames and the skip-to-level
 * fast-forward are already at full host speed; refusing them costs no speed.
 * The first refusal is logged once.
 * @param want The unlock the iteration asks for (turbo, --unlock-frames, skip, headless).
 * @return @p want, or false when the virtual clock is on. */
bool host_pacer_unlock_verdict(bool want);

/** @brief Return whether this iteration runs the sim: always under the legacy pacer or unlock, else once the sim deadline has passed.
 * @param P The pacer.
 * @param unlock The unlock verdict at the iteration's entry.
 * @return true when the sim is due. */
bool host_pacer_sim_due(const host_pacer_t* P, bool unlock);

/** @brief Return the deadline in force for this iteration's VBL record and its UNLOCK and SLIPPED flags, consuming the regrid and slip latches.
 *
 * The deadline is the sim deadline as it stands before the tail advances
 * it, 0 when the previous iteration ran unlocked or the grid was
 * re-anchored since. Called once per iteration whether or not it yields,
 * so a latch set on a non-yielding iteration is consumed there.
 * @param P The pacer.
 * @param flags Receives OP_PACE_F_UNLOCK when no deadline was in force, OR OP_PACE_F_SLIPPED when a sim clamp fired.
 * @return The deadline in ns, or 0. */
op_time_t host_pacer_vbl_deadline(host_pacer_t* P, uint8_t* flags);

/** @brief Return the deadline an intermediate present must beat.
 *
 * The sim deadline advances at the loop tail, so on a sim-due iteration it
 * still names the deadline this iteration already met; the next one is a
 * period later.
 * @param P The pacer.
 * @param sim_due The iteration ran the sim.
 * @return The deadline in ns. */
op_time_t host_pacer_present_deadline(const host_pacer_t* P, bool sim_due);

/** @brief Return the current render-grid slot, the instant eager planning steps its span from.
 * @param P The pacer.
 * @return The slot in ns. */
op_time_t host_pacer_render_slot(const host_pacer_t* P);

/** @brief Return the sim deadline @p k periods past the next one.
 * @param P The pacer.
 * @param k Whole sim periods to add.
 * @return The deadline in ns. */
op_time_t host_pacer_sim_deadline(const host_pacer_t* P, int k);

/** @brief Return the live sim (VBL) period in whole ns.
 * @param P The pacer.
 * @return The period. */
op_time_t host_pacer_sim_period_ns(const host_pacer_t* P);

/** @brief Return the live present period in whole ns.
 * @param P The pacer.
 * @return The period. */
op_time_t host_pacer_render_period_ns(const host_pacer_t* P);

/** @brief Return the live --render-fps target, 0 under the legacy pacer.
 * @param P The pacer.
 * @return The rate. */
int host_pacer_render_fps(const host_pacer_t* P);

/** @brief Return the live --tick-scale.
 * @param P The pacer.
 * @return The scale. */
double host_pacer_tick_scale(const host_pacer_t* P);

/** @brief Record the tail's unlock verdict: a pace-log window event on a change, and the state the next VBL record reads.
 * @param P The pacer.
 * @param unlock The verdict. */
void host_pacer_note_unlock(host_pacer_t* P, bool unlock);

/** @brief Advance the deadlines at the loop tail and sleep to the next one.
 *
 * Unlock or shutdown re-anchors both deadlines at now and does not sleep.
 * The legacy pacer steps the sim deadline on a ticked iteration and sleeps
 * to it, and does not sleep otherwise. The dual pacer steps the sim
 * deadline on a ticked sim-due iteration and the render deadline on every
 * iteration, and sleeps to the sooner; under eager production the render
 * grid is placed on its first slot after now and the sleep is to the sim
 * deadline alone. Each step is followed by the slip clamp.
 * @param P The pacer.
 * @param unlock The tail's unlock verdict.
 * @param sim_due The iteration ran the sim.
 * @param ticked The game fiber yielded with a frame bump.
 * @param eager_active Eager production owns the presents.
 * @param shutdown Shutdown was requested. */
void host_pacer_advance(host_pacer_t* P, bool unlock, bool sim_due, bool ticked,
                        bool eager_active, bool shutdown);

/** @brief Log the pacer's slip totals: deadline periods the sim and render clamps discarded.
 * @param P The pacer.
 * @return true when a slip occurred and the line was logged. */
bool host_pacer_slip_report(const host_pacer_t* P);
