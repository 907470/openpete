// core/runtime/eager.h - eager production: the strategy that extracts a
// tick's sub-tick scenes on the sim thread right after the tick and hands
// them to the present thread through the scene ring. The host loop's
// present phase decides when it engages; this module owns its plan, its
// production and its cost estimates.
#pragma once

#include "openpete.h"
#include "openpete_frame_time_log.h"
#include "openpete_time.h"

/* The produce guard stops this long before each deadline it is given: the
 * update VBL's sim deadline, then the next tick's. */
#define EAGER_PRODUCE_GUARD_NS 500000

/** @brief Report whether eager production holds the display (the present thread presents).
 * @return 1 while engaged, else 0. */
int eager_active(void);

/** @brief Report the world-liveness verdict eager production last recorded.
 *
 * Set at each canonical plan and each idle production call; while 0 no
 * intermediate scene is produced and a cut span sheds nothing.
 * @return 1 when canonical presents were flowing at the last record. */
int eager_world_live(void);

/** @brief Engage (1) or disengage (0) eager production, handing the display to or taking it from the present thread.
 *
 * A call that does not change the mode does nothing. A change calls
 * openpete_present_thread_set_active (disengaging drains the present
 * thread) and logs the transition.
 * @param on 1 to engage, 0 to disengage. */
void eager_set_active(int on);

/** @brief Report whether eager production may engage under OPENPETE_EAGER_HOLD_S.
 *
 * OPENPETE_EAGER_HOLD_S=<seconds> (harness knob) keeps eager off for that
 * long from the first call, the first canonical present at which every
 * other engage term holds, so inline intermediate presents run and warm the
 * shed predictor before eager takes the display. Unset, always 1.
 * @return 1 when eager may engage. */
int eager_hold_done(void);

/** @brief Plan the span a canonical present opens and produce its canonical scene, then as much of the span as fits.
 *
 * The span's first display instant is the render-grid slot advanced past
 * now; it is anchored on the newest capture stamp unless that stamp is a
 * tick or more old, when it is anchored on now. A canonical one VBL after the
 * previous one predicts another: the span is then anchored on this VBL's stamp,
 * runs one VBL and is budgeted one VBL less the tick's mean cost.
 * The world-liveness verdict
 * is recorded after the plan (the plan's shed accounting reads the previous
 * one). Intermediate scenes are produced only while the world is live.
 * @param now The plan instant.
 * @param slot The pacer's render-grid slot.
 * @param period The render-grid period in nanoseconds.
 * @param prod_end The pacer grid instant production must finish before (two sim periods out).
 * @param produce_end The next sim deadline; intermediate production stops 0.5 ms before it.
 * @param world_live Nonzero when canonical presents are flowing. */
void eager_canonical(op_time_t now, op_time_t slot, op_time_t period,
                     op_time_t prod_end, op_time_t produce_end, int world_live);

/** @brief Record the world-liveness verdict on an iteration after the canonical block, and produce more of the plan when the iteration drew nothing and the world is live.
 *
 * @param world_live Nonzero when canonical presents are flowing.
 * @param may_produce Nonzero when no present was made this iteration and a re-extract is registered.
 * @param deadline The time production must stop by.
 * @param period The render-grid period in nanoseconds.
 * @return 1 when production ran, else 0. */
int eager_idle(int world_live, int may_produce, op_time_t deadline, op_time_t period);

/** @brief Fold one VBL's execution into the update-VBL and tick averages the eager planner reads.
 *
 * A VBL that queued no draw is the update VBL, whose mean the plan
 * subtracts from the span budget; one that did is the tick, whose
 * execution sets the lag. An execution of half a tick or more is a load or
 * a stall, not a VBL of either kind, and enters neither mean: it would plan
 * a lag of a hundred targets, or price every later span as if each update
 * VBL were a load. By the same line, a tick that starts half a tick or more
 * past its pacer deadline was delayed by a load or a stall, and its
 * lateness is kept in tick_late (0 for any later start, which is pacer
 * jitter or a production overrun the budget must keep seeing).
 * @param ex The VBL's execution time on this thread.
 * @param start_late Execution start minus the pacer deadline, or 0 when unpaced.
 * @param drawing Nonzero when the VBL queued a draw (a tick). */
void eager_note_vbl(op_time_t ex, op_time_t start_late, int drawing);

/** @brief Start eager production and its present thread on a windowed native display, with the run's frame-time log hooked to the present thread's presents.
 *
 * Eager production is on unless OPENPETE_EAGER=0 and needs the native
 * scene path and a window; otherwise, or when the present thread cannot
 * start, nothing starts. Registers eager's and the scene ring's guest
 * discontinuity handlers.
 * @param ftl The run's frame-time log; recorded through for every present
 *        the present thread makes.
 * @param opts The parsed options. */
void host_eager_start(openpete_frame_time_log_t* ftl, const openpete_options_t* opts);

/** @brief Log the eager-production totals and the count of sub-tick re-extracts presented, eager and inline.
 * @param inline_rex The sub-tick re-extracts the inline intermediate present made. */
void eager_exit_report(unsigned inline_rex);
