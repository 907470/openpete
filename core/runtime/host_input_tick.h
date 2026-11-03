// core/runtime/host_input_tick.h - the host loop's input tick: override
// arbitration, the input latch and staging, the post-tick input-log
// commit and its census, the bk2 replay load and the record exit flush.
#pragma once

#include "host_loop_internal.h"
#include "openpete_input_log.h"
#include "bk2_movie.h"
#include "openpete_thread_roles.h"

#include <stdbool.h>

/** @brief Arbitrate the pad override channel for this iteration: the bk2 row on a sim-due iteration, the overlays' neutral stamp, the overlay hotkey suspension, then the cursor and mouse capture on the unsplit path.
 *
 * A bk2 row in range outranks the overlays' neutral stamp. With
 * --bk2-stop-on-end a sim-due iteration past the movie's last row stamps
 * nothing and returns true before any other step: the run must not tick a
 * row past the movie.
 * @param S The session.
 * @param it The iteration; reads sim_due, writes ovr_claimed and bk2_stamped_frame.
 * @return true when the run ends at the movie's end. */
bool host_input_arbitrate(const host_session_t* S, host_iter_t* it) OP_REQUIRES(sim);

/** @brief Restamp the bk2 row after a quiescent op re-anchored openpete_frame.
 *
 * A savestate load, stress rollback or handoff resume may move
 * openpete_frame; the stamp made by host_input_arbitrate was for the frame
 * before the jump, so the row is re-derived for the new one, or the next
 * tick runs with another frame's input. The override claim is decided
 * again for the new row on the same iteration (a movie row in range, else
 * an overlay's neutral hold, else live input), so the latch hands the pad
 * to the movie on the iteration the op commits. Silent: the transition
 * log and the mods' bk2 status stay with the arbitration stamp.
 * @param S The session.
 * @param it The iteration; reads bk2_stamped_frame, writes ovr_claimed. */
void host_input_restamp_after_ops(const host_session_t* S, host_iter_t* it) OP_REQUIRES(sim);

/** @brief Pump, sample and latch this iteration's input, then stage the tick's input record.
 *
 * Under the thread split main owns the pump, the SDL-state publish and the
 * pad sample, and this thread only latches what was published. The
 * staged record is published before the tick so a reader inside the tick
 * sees the bytes it consumes; the post-yield readback stays the truth the
 * log stores for slot 0.
 * @param S The session.
 * @param it The iteration; reads ovr_claimed, writes the latch record and p1_staged. */
void host_input_latch(const host_session_t* S, host_iter_t* it) OP_REQUIRES(sim);

/** @brief Commit the input log for the VBLs an iteration ticked: player 1's readback, the staging and slot census, and one log entry per yield, noted by the bk2 recorder while it records.
 *
 * Called only on an iteration that advanced openpete_frame.
 * @param S The session.
 * @param it The iteration; reads p1_staged and frame_before_yield. */
void host_input_commit(const host_session_t* S, const host_iter_t* it) OP_REQUIRES(sim);

/** @brief Read OPENPETE_SYNTH_PADS once: the synthetic pad-stream mode for slots 1-3.
 *
 * The synthetic stream lets the N-port input log be exercised without host
 * devices filling slots 1-3. Its bytes are a pure function of the VBL index
 * and the slot (synth_pad_for), so the append path checks that the staged
 * ports survived the tick and the rewind burst checks that the ports it
 * restaged from the log match what that VBL carried. Generate-and-verify
 * stages the stream over the live devices. Verify-only leaves the staged
 * slots as movie replay filled them and only checks them: replaying a
 * recording of the stream with the generator on would overwrite the bytes
 * read back from disk with the bytes they are compared against.
 * @return 0 off (unset, empty or starting with '0'), 2 verify-only (a value
 *         starting with 'v' or 'V'), else 1 generate-and-verify. */
int synth_pads_enabled(void);

/** @brief Test an input record against the synthetic stream over the slots the current mode fills.
 *
 * Generate-and-verify checks slots 1-3; verify-only checks slot 1 alone,
 * the one player-2 slot a two-player movie replay carries.
 * @param rec The input record.
 * @param frame The VBL index the stream is generated for.
 * @return 1 when every checked slot matches, else 0. */
int synth_pads_match(const openpete_input_rec_t* rec, unsigned frame);

/* The rewind burst's checks of restaged slots against the synthetic stream,
 * reported by host_input_exit_report. */
extern unsigned long long s_synth_rewind_checks;
extern unsigned long long s_synth_rewind_mismatches;

/** @brief Load the --bk2 movie and check that this run can replay it.
 *
 * Refuses a movie recorded under another load delay, and a consume-keyed
 * movie on a game whose descriptor names no pad-ring consumer. A movie's
 * rows are read at the row position its Header.txt names (bk2_feed_row): a
 * recorded movie at (openpete_consume, openpete_consume_write), a movie with
 * "OpenpeteFeedKey vbl" by openpete_vbl and one without the line by
 * openpete_frame. A movie on an older key is converted on load by replaying
 * it under that key, the rows tools/bk2_rekey.py rewrites to the consume
 * key. The movie is freed on every refusal.
 * @param opts The parsed options; bk2_path names the movie.
 * @param bk2 Receives the movie.
 * @param offset Receives the row counter's value at which row 0 plays.
 * @return 0 when the movie replays; -1 after logging the refusal. */
int bk2_replay_load(const openpete_options_t* opts, bk2_movie_t* bk2, int* offset);

/** @brief Write the movie being recorded on a fatal exit: commit the yield in flight, then finalize, with input-log appends stopped for good.
 *
 * Registered with openpete_crash_set_exit_flush while --bk2-record records;
 * a hang-watchdog trip runs it on the game fiber, or on the stall monitor
 * thread for the monitor's fallback. Takes input_log_lock and never
 * releases it: a flush on the monitor thread waits out a host-loop commit
 * in flight, and the host loop's next commit blocks until the process
 * ends. The yield the game fiber is inside has taken pad writes no commit
 * has noted; its player 1 readback is appended as one entry carrying them,
 * so the movie ends at the last pad write the guest took. Under a rewind
 * replay the log already holds that entry and none is appended. */
void bk2_record_exit_flush(void);

/** @brief Log the input-log census: slot-0 staging checks, per-slot presence and independence, the synthetic slots, and the first staging mismatch. */
void host_input_exit_report(void);

/** @brief Reset the input tick's latches to their start-of-run state: no overlay held the pad, and no movie bits logged. */
void host_input_latch_init(void);
