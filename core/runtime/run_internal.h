// core/runtime/run_internal.h - run.c symbols the host-loop modules
// (host_state_ops.c, host_session.c) call.
#pragma once

#include "openpete.h"
#include "openpete_frame_time_log.h"
#include "openpete_input_log.h"
#include "openpete_psxtrace.h"
#include "bk2_movie.h"
#include "psxrt/psx_runtime.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

extern unsigned long long s_synth_rewind_checks;
extern unsigned long long s_synth_rewind_mismatches;

/* Game fiber state. Static rather than capture so the libco trampoline (which
 * takes no args) can find it. */
struct run_game {
    CPUState cpu;
    uint32_t entry;
    uint32_t gp;
    int      done;
    int      dispatch_ok;
};
extern struct run_game g_game;

/** @brief Publish the --max-runtime-seconds deadline for openpete_shutdown_requested.
 *
 * Measured from the instant the host loop's own deadline test measures
 * from, so a fast-boot yield taken on the deadline meets a loop that quits
 * on it. No deadline is published when the option is 0.
 * @param opts The parsed options.
 * @param start The host loop's start, in SDL ticks. */
void run_deadline_arm(const openpete_options_t* opts, Uint64 start);

/** @brief Decide whether the host loop runs on a worker thread while main pumps the window.
 *
 * Every windowed play configuration splits: the vk renderer, and the
 * psycross renderer, where PsyCross rasterizes on the worker through the GL
 * context bring-up created on main (run_split_threaded hands it across).
 * Headless runs, screenshot captures (--screenshot-on-frame,
 * --screenshot-presents-start) and legacy host input stay single-threaded;
 * the gates run in those modes and no modal window loop reaches them.
 * Without the split the sim shares main with the message pump, and a Win32
 * move or size loop parks it for the whole gesture.
 * @param opts The parsed options.
 * @return true when the split is enabled (OPENPETE_THREAD_SPLIT) and the
 *         configuration qualifies. */
bool openpete_split_applies(const openpete_options_t* opts);

/** @brief Report teardown progress: one heartbeat for the pump's liveness check, and a re-arm of the quit backstop when a signal armed it. */
void sim_teardown_progress(void);

/** @brief Stall teardown for OPENPETE_TEARDOWN_STALL_MS milliseconds with no heartbeat.
 *
 * A fault-injection knob for the quit watchdog: it stands in for a
 * recorder flush that takes that long. Unset or 0 does nothing. */
void sim_teardown_injected_stall(void);

/** @brief Name the game's feed key, the counter --frame-limit and the one-shot triggers read.
 * @return "vbl" for openpete_vbl, "frame" for openpete_frame. */
const char* feed_key_kind_name(void);

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

/** @brief Write the CSV header of the gameplay hash log: frame, vbl, fnv1a64 and one column per region.
 *
 * A region name has commas, CR and LF replaced by '_'. More than
 * OPENPETE_HASH_LOG_MAX_REGIONS regions log an error and truncate the
 * columns to that many.
 * @param fp The log file. */
void gameplay_hash_log_emit_header(FILE* fp);

/** @brief Log the disc cache's exit census line, with misses per read path, and stop its loader.
 *
 * Logs nothing when the cache was never attached. */
void disc_cache_exit_census(void);

/** @brief Make the calling coroutine the host fiber and create the game fiber on an 8 MB guarded stack.
 *
 * The game fiber's body dispatches the game's entry point and parks the
 * fiber for good when it returns.
 * @return true when the game fiber was created; false when its stack
 *         allocation failed. */
bool run_game_fiber_create(void);

/** @brief Log the vibration census: PadSetAct registrations, motor commands, per-pad recorders, mod slots.
 *
 * Logged unconditionally because the counts are read together. A recorder
 * line appears only for a virtual pad with a callback or a live recorder,
 * and a mod line only for a slot that issued commands. */
void rumble_census_report(void);

/** @brief Raise the teardown flag the pump's liveness check reads, and report one step of progress. */
void sim_teardown_begin(void);

/** @brief Return the --psxtrace-verify comparison host init opened, or NULL.
 * @return The open trace, or NULL when none is open. */
openpete_psxtrace_t* run_psxtrace(void);

/** @brief Close the --psxtrace-verify comparison and clear the dispatch hook that advances it; no-op when none is open. */
void run_psxtrace_release(void);

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

/** @brief Log the eager-production totals and the count of sub-tick re-extracts presented, eager and inline. */
void host_eager_exit_report(void);

/** @brief Log the pacer's slip totals: deadline periods the sim and render clamps discarded.
 * @return true when a slip occurred and the line was logged. */
bool host_slip_report(void);

/** @brief Log the input-log census: slot-0 staging checks, per-slot presence and independence, the synthetic slots, and the first staging mismatch. */
void host_input_exit_report(void);

/** @brief Reset the overlay pad-stamp latch to its start-of-run state (no overlay held the pad). */
void host_input_latch_init(void);

/** @brief Reset the menu-close, pointer-owner and Alt+Enter latches to their start-of-run state. */
void host_hotkey_latch_init(void);

/** @brief Build the pace log's environment block from the launch options and the seeded pacer state.
 *
 * Reads the live render-fps target and the runahead request as the pacer
 * step seeded them, the interpolation capture gate, the renderer's
 * device, driver and granted present mode, and the input path.
 * @param env Receives the block.
 * @param env_bytes Capacity of @p env.
 * @param opts The parsed options.
 * @param refresh_mhz The display refresh in mHz.
 * @param tick_scale The launch --tick-scale. */
void host_pace_env_build(char* env, unsigned env_bytes, const openpete_options_t* opts,
                         int refresh_mhz, double tick_scale);
