// core/runtime/run_internal.h - run.c symbols the host-loop modules
// (host_state_ops.c, host_session.c, host_input_tick.c, host_loop.c,
// host_present.c, host_hotkeys.c, eager.c, runahead.c) call.
#pragma once

#include "openpete.h"
#include "host_pacer.h"
#include "openpete_frame_time_log.h"
#include "openpete_input_log.h"
#include "openpete_psxtrace.h"
#include "openpete_time.h"
#include "bk2_movie.h"
#include "psxrt/psx_runtime.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

/** @brief Show or hide the OS cursor by who owns the mouse and how long it has been still.
 *
 * The cursor shows while the dev overlay is open or wants the mouse, while
 * the settings menu is open, and for CURSOR_IDLE_HIDE_MS after the last
 * mouse event; otherwise it hides over the game. The first call starts the
 * idle clock. Edge-triggered: one SDL call per transition. Must run on the
 * thread that owns the window: SDL's Win32 backend applies visibility with
 * SetCursor(), which affects only the calling thread's windows, so an
 * off-thread hide never takes effect until a WM_SETCURSOR, which only mouse
 * movement sends and which un-hides it again.
 * @param headless A headless run returns at once. */
void openpete_cursor_update(bool headless);

/** @brief Drain the SDL event queue, forwarding unconsumed events to PsyCross, then apply rumble.
 *
 * Each event goes through openpete_handle_event, and PsyX_Sys_HandleEvent
 * when it is not consumed; under OPENPETE_PUMP_PROBE the drain is timed
 * (openpete_pump_events_probed). The overlay's text-input state and rumble are
 * applied here because this thread owns the window and the SDL gamepad
 * handles. */
void openpete_pump_events(void);

/** @brief Restamp PsyCross's pad maps when a binding edit changed a player-1 pad action since this thread last stamped.
 *
 * Runs on the thread about to sample the pad (main's pump loop under the
 * thread split, the host loop unsplit and on the legacy path), so the
 * mapping structs have one reader and one writer on one thread. Counts the
 * restamp per thread for the exit summary.
 * @param on_main nonzero on main's pump loop. */
void pad_restamp_if_due(int on_main);

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

/** @brief Publish the applied render fps and tick scale, with the applied mute, as the values the live-knob getters report.
 * @param render_fps The applied --render-fps target.
 * @param tick_scale The applied --tick-scale. */
void host_live_knobs_publish(int render_fps, double tick_scale);

/** @brief Build the pace log's environment block from the launch options and the seeded pacer state.
 *
 * Reads the runahead request as session open seeded it, the interpolation
 * capture gate, the renderer's device, driver and granted present mode,
 * and the input path.
 * @param env Receives the block.
 * @param env_bytes Capacity of @p env.
 * @param opts The parsed options.
 * @param refresh_mhz The display refresh in mHz.
 * @param render_fps The seeded render-fps target.
 * @param tick_scale The launch --tick-scale. */
void host_pace_env_build(char* env, unsigned env_bytes, const openpete_options_t* opts,
                         int refresh_mhz, int render_fps, double tick_scale);

/** @brief Poll the fullscreen gestures (Alt+Enter edge, the `fullscreen` binding) and route the toggle to the window's owner.
 *
 * SDL_SetWindowFullscreen belongs to the thread that owns the window: under
 * the thread split this loop raises the request on its own edge and main
 * consumes it; otherwise this loop consumes every raise (its own edge, a
 * double-click, a request from another thread) and toggles.
 * @param split Whether the thread split is active.
 * @param prev_alt_enter Whether Alt+Enter was down at the previous poll.
 * @return Whether Alt+Enter is down now. */
bool host_fullscreen_poll(bool split, bool prev_alt_enter);

/** @brief Return the live mute baseline the pause exit restores: the boot or live --mute, or the skip-to-level transient mute.
 * @return Nonzero when either mute is set. */
int run_mute_baseline(void);

/** @brief Return whether a shutdown was requested (the pump's quit, the SIGINT handler, openpete_request_shutdown), without the --max-runtime-seconds deadline openpete_shutdown_requested adds.
 * @return true once a shutdown was requested. */
bool run_shutdown_flagged(void);

/** @brief Count one host-loop iteration on the sim heartbeat the pump's liveness check reads. */
void run_sim_heartbeat(void);

/** @brief Append a "frame,vbl,x,y,z" row of the player position to the position log.
 *
 * Writes nothing when game_get_player_position reports no position.
 * @param fp The log file.
 * @param frame The tick counter.
 * @param vbl The VBL counter. */
void position_log_emit(FILE* fp, unsigned frame, int vbl);

/** @brief Append one gameplay hash log row: frame, vbl, the total hash and each region hash in hex.
 *
 * Formats hashes gameplay_hash_compute_ex already computed (the bug-report
 * recorder's row for the tick). The row carries at most
 * OPENPETE_HASH_LOG_MAX_REGIONS region columns, matching the header.
 * @param fp The log file.
 * @param frame The tick counter.
 * @param vbl The VBL counter.
 * @param total The total hash.
 * @param per_region One hash per registered region. */
void gameplay_hash_log_emit(FILE* fp, unsigned frame, int vbl, uint64_t total,
                            const uint64_t* per_region);

/** @brief Apply the OPENPETE_TICK_SCALE_AT schedule: each `VBL:SCALE` entry calls openpete_set_tick_scale once the device VBL count reaches VBL.
 *
 * The harness form of a live tick-scale change (the devui row's setter, at
 * a reproducible VBL); entries are comma-separated and in VBL order. Read
 * once; a no-op when the variable is unset. */
void tick_scale_schedule_poll(void);

/** @brief Apply the queued live-knob requests on the host loop's thread, at its quiescent point.
 *
 * A render fps or tick scale request goes to the pacer, which applies a
 * change at host_pacer_apply_live; a changed runahead depth raises the
 * re-arm flag; a rewind window request resizes the keyframe ring here, off
 * every capture; a mute request sets the baseline and applies the gains
 * here. Requests that match the applied value change nothing.
 * @param P The run's pacer. */
void live_knob_drain(host_pacer_t* P);

/** @brief Mute audio while openpete_skip_present_until_level fast-forwards, and restore it after.
 *
 * Called wherever the flag can change (boot arming, each host-loop
 * iteration); applies the mute gains only on a change of the flag. */
void skip_mute_sync(void);

/** @brief Apply the device feed's run-mode policy for this host-loop iteration, then top the device queue up.
 *
 * The feed carries the SPU at the guest's real-time rate. In a window the
 * feed holds (the SPU still runs) through fast boot, which runs the guest
 * without host yields until the first scene, through a pause, a
 * skip-to-level fast-forward and any other unlocked stretch. The phase and
 * the VBL count tag the feed's per-phase counters and timeline log.
 * Headless runs unpaced by design: its feed (the synthetic device, when
 * armed) times itself from the frames pushed.
 * @param unlock_fps The iteration runs unpaced (fast-forward, skip, headless).
 * @param headless The run is headless.
 * @param paused The game is paused. */
void audio_feed_sync(int unlock_fps, int headless, int paused);
