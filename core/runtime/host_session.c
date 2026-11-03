// core/runtime/host_session.c - open and close one run of the host loop.
//
// Open runs the setup in its fixed order and records every resource it
// opened in the session. Close runs the teardown in its fixed order; each
// step is a release step, which runs on every path, or an output step,
// which runs only once the loop has entered (a setup error writes no
// movie, no verdict and no report).
#include "openpete_bugreport.h"
#include "openpete_bugreport_replay.h"
#include "host_loop_internal.h"
#include "host_state_ops.h"
#include "host_hotkeys.h"
#include "run_internal.h"
#include "host_input_tick.h"
#include "runahead.h"
#include "eager.h"

#include "openpete.h"
#include "openpete_fiber.h"
#include "openpete_host_input.h"
#include "openpete_host_mouse.h"
#include "openpete_host_trace.h"
#include "openpete_input_lock.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_log.h"
#include "openpete_menu.h"
#include "openpete_mods.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_present_thread.h"
#include "openpete_profile.h"
#include "openpete_func_trace.h"
#include "openpete_crash.h"
#include "openpete_psx.h"
#include "openpete_psxtrace.h"
#include "openpete_record.h"
#include "openpete_rewind.h"
#include "openpete_run_capture.h"
#include "openpete_settings_save.h"
#include "openpete_rebind.h"
#include "openpete_osd.h"
#include "openpete_vstream.h"
#include "openpete_freecam.h"
#include "openpete_devui.h"
#include "openpete_build_id.h"
#include "openpete_game_runtime.h"
#include "openpete_time.h"
#include "feature_registry.h"
#include "game_extras.h"
#include "bk2_feed.h"
#include "openpete_bugrec.h"
#include "bk2_record.h"
#include "../host/sfx_pack.h"
#include "../rhi/rhi_gpu_audit.h"
#include "PsyX/PsyX_public.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef OPENPETE_HAS_PSYCROSS

/* The --debug-triangle infra-triangle (core/renderer/debug_render.cpp). */
extern void openpete_debug_triangle_init(void);

/* ---- open --------------------------------------------------------------- */

/** @brief Query the primary display's refresh rate into the session; 60 Hz headless or when the query fails.
 *
 * The rate feeds the --render-fps default and the settings menu's FPS
 * ladder; the exact mHz value feeds the pace log, which keys pacing
 * baselines on the distinction between 59.94 and 60.
 * @param S The session. */
static void open_refresh_query(host_session_t* S) OP_REQUIRES(sim)
{
    S->refresh_hz  = 60;
    S->refresh_mhz = 60000;
    if (S->opts->headless) return;
    const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (dm && dm->refresh_rate > 0.0f) {
        S->refresh_hz  = (int)(dm->refresh_rate + 0.5f);
        S->refresh_mhz = (int)(dm->refresh_rate * 1000.0f + 0.5f);
    }
}

/** @brief Open --position-log and write its CSV header.
 *
 * The header is one line so two runs diff directly; the loop flushes after
 * each row, so a killed run leaves its progress on disk.
 * @param S The session.
 * @return 0, or -1 with S->exit_reason set when the file cannot be opened. */
static int open_position_log(host_session_t* S) OP_REQUIRES(sim)
{
    const char* path = S->opts->position_log_path;
    if (!path || !*path) return 0;
    S->out.position_log = fopen(path, "wb");
    if (!S->out.position_log) {
        OPL_ERROR_F("position-log", "fopen %s failed: %s", path, strerror(errno));
        S->exit_reason = "setup-position-log";
        return -1;
    }
    fputs("frame,vbl,x,y,z\n", S->out.position_log);
    OPL_INFO_F("position-log", "writing g_Spyro.m_Position each yield to %s", path);
    return 0;
}

/** @brief Open --gameplay-hash-log, the primary determinism gate, and write its header.
 *
 * Refused when the game registers no gameplay-state regions: every row
 * would log the FNV offset basis and two runs would compare equal on no
 * bytes.
 * @param S The session.
 * @return 0, or -1 with S->exit_reason set. */
static int open_hash_log(host_session_t* S) OP_REQUIRES(sim)
{
    const char* path = S->opts->gameplay_hash_log_path;
    if (!path || !*path) return 0;
    if (openpete_state_region_count == 0) {
        OPL_ERROR_F("gameplay-hash-log",
                    "this game registers no gameplay-state regions "
                    "(no games/<game>/manifest/gameplay_state_regions.toml); "
                    "a hash over zero regions is vacuous");
        S->exit_reason = "setup-hash-log-regions";
        return -1;
    }
    S->out.hash_log = fopen(path, "wb");
    if (!S->out.hash_log) {
        OPL_ERROR_F("gameplay-hash-log", "fopen %s failed: %s", path, strerror(errno));
        S->exit_reason = "setup-hash-log";
        return -1;
    }
    gameplay_hash_log_emit_header(S->out.hash_log);
    OPL_INFO_F("gameplay-hash-log", "FNV1a-64 over %zu state regions to %s",
               openpete_state_region_count, path);
    return 0;
}

/** @brief Begin --bk2-record, resolve the level-load delay, and load and open the --bk2 movie.
 *
 * The recorder buffers in memory and writes the file at close, so a
 * SIGTERM against the run still produces a usable movie; a hang-watchdog
 * exit skips close, so the crash exit-flush writes the movie there. The
 * load delay is constant for the run (the option, else the game's
 * default) and is resolved before the movie load so a movie recorded under
 * another value is refused up front.
 * @param S The session.
 * @return 0, or -1 with S->exit_reason set. */
static int open_bk2(host_session_t* S) OP_REQUIRES(sim)
{
    const openpete_options_t* opts = S->opts;
    if (opts->bk2_record_path && *opts->bk2_record_path) {
        if (bk2_record_begin(opts->bk2_record_path, opts->bk2_record_args) != 0) {
            S->exit_reason = "setup-bk2-record";
            return -1;
        }
        S->bk2.recording = true;
        openpete_crash_set_exit_flush(OP_EXIT_FLUSH_RECORD, bk2_record_exit_flush);
    }

    openpete_cd_load_delay = (opts->load_delay >= 0)
                               ? opts->load_delay
                               : (int)game_get_load_delay_default();
    OPL_INFO_F("cd", "load delay: %d VBL(s) per level load%s",
               openpete_cd_load_delay,
               opts->load_delay >= 0 ? " (--load-delay)" : " (game default)");

    S->bk2.active = (opts->bk2_path && *opts->bk2_path);
    openpete_bk2_set_active(S->bk2.active || S->bk2.recording);
    if (S->bk2.active && bk2_replay_load(opts, &S->bk2.movie, &S->bk2.offset) != 0) {
        S->exit_reason = "setup-bk2-load";
        return -1;
    }
    bk2_feed_open(S->bk2.active ? &S->bk2.movie : NULL, S->bk2.offset);
    openpete_bugrec_init();
    openpete_bugreport_init(opts);
    return 0;
}

/** @brief Open --frame-time-log, the per-present recorder of the pacing harness.
 *
 * Each record's deadline is the log's start plus one target period per
 * record already written: the actual present time against an ideal
 * cadence, independent of which pacer drives the loop. The target is the
 * launch --render-fps period when a render rate is set (explicit, or the
 * windowed default of the display refresh), else the tick period; whole
 * ns, since it is a measurement reference and not a deadline the pacer
 * steps.
 * @param S The session; the pacer holds the launch render rate and tick scale.
 * @return 0, or -1 with S->exit_reason set. */
static int open_frame_time_log(host_session_t* S) OP_REQUIRES(sim)
{
    const char* path = S->opts->frame_time_log_path;
    if (!path || !*path) return 0;
    const int64_t period_ns = (S->pacer.launch_fps > 0)
        ? (int64_t)((double)OP_NS_PER_SEC / (double)S->pacer.launch_fps / S->pacer.launch_scale)
        : (int64_t)g_openpete_tick_period_ns;
    if (openpete_frame_time_log_open(&S->out.ftl, path, period_ns) != 0) {
        S->exit_reason = "setup-frame-time-log";
        return -1;
    }
    return 0;
}

/** @brief Arm the always-on pace ring and open --pace-log, the unified pacing event log, both with one environment block.
 *
 * Done with the renderer already up, because the environment block
 * records the device and the present mode actually granted. A ring that
 * cannot be armed is logged and the run continues.
 * @param S The session.
 * @return 0, or -1 with S->exit_reason set when the log file fails. */
static int open_pace_log(host_session_t* S) OP_REQUIRES(sim)
{
    const char* path = S->opts->pace_log_path;
    char env[2048];
    host_pace_env_build(env, sizeof(env), S->opts, S->refresh_mhz,
                        host_pacer_render_fps(&S->pacer), S->pacer.launch_scale);
    (void)openpete_pace_ring_arm(env);
    if (!path || !*path) return 0;
    if (openpete_pace_log_open(path, env) != 0) {
        S->exit_reason = "setup-pace-log";
        return -1;
    }
    return 0;
}

/** @brief Open the run's outputs after the pacer: logs, bk2 record and replay, profile, latches, menu and eager production.
 *
 * On a failure S->exit_reason names the step and every output opened so
 * far stays recorded in @p S for host_session_close.
 * @param S The session; the pacer is initialized.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR. */
static int open_outputs(host_session_t* S) OP_REQUIRES(sim)
{
    const openpete_options_t* opts = S->opts;
    if (open_position_log(S) || open_hash_log(S) || open_bk2(S) ||
        open_frame_time_log(S) || open_pace_log(S))
        return OPENPETE_EXIT_SETUP_ERROR;
    /* Performance trace (--profile-out): CPU scope spans, GPU zones and the
     * census, keyed per present and canonical tick. */
    if (opts->profile_out_path && *opts->profile_out_path &&
        op_prof_open(opts->profile_out_path) != 0) {
        S->exit_reason = "setup-profile";
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    host_input_latch_init();
    host_hotkey_latch_init();
    /* Seed the in-game settings menu from the launch flags and build its FPS
     * ladder from the refresh queried at open. Applies nothing: the menu
     * reflects what the game launched with (the FPS row reads the live
     * pacer target, which already holds the resolved render-fps default). */
    openpete_menu_init(opts, S->refresh_hz);
    host_eager_start(&S->out.ftl, opts);
    return OPENPETE_EXIT_OK;
}

/* The open session, for the bug-report press's reports (the pacer). */
static host_session_t* s_press_session;

int host_session_open(host_session_t* S, const openpete_options_t* opts) OP_REQUIRES(sim)
{
    memset(S, 0, sizeof(*S));
    s_press_session = S;
    S->opts  = opts;
    S->split = openpete_split_applies(opts);
    /* Elapsed time for a fiber failure counts from here; a run that boots
     * re-stamps it once the fiber exists. */
    S->start = SDL_GetTicks();

    /* The device was created during init - under the thread split that was
     * main - but inline presents come from this thread; the display's seam
     * fence needs the ownership stated, not inferred from who created what.
     * Eager's engage/disengage re-claims at every later handoff. */
    openpete_native_display_claim();

    /* Last point at which guest RAM still holds exactly what the EXE loader
     * and the seed blocks put there: the fiber below is what first executes
     * guest code, and a --state-load-on-frame restore happens later still,
     * inside the loop. Mod code that may change guarded guest bytes runs on
     * this thread from here on: the guest-byte guard brackets it here and
     * nowhere else. */
    openpete_mods_guard_claim_thread();
    game_on_boot(&g_game.cpu);

    if (!run_game_fiber_create()) {
        OPL_ERROR_F("boot", "game fiber stack allocation failed (out of memory?)");
        S->exit_reason = "setup-fiber";
        return OPENPETE_EXIT_PANIC;
    }
    OPL_INFO_F("boot", "fibers ready: host=%p game=%p",
               openpete_host_fiber, openpete_game_fiber);

    if (opts->debug_triangle) {
        openpete_debug_triangle_init();
        OPL_INFO_F("boot",
                   "debug-triangle enabled - emitting hardcoded POLY_F3 each frame");
    }

    /* No scene is opened here: the game draws every other VBL, so the host
     * loop yields twice per DrawOTag. The DrawOTag thunk starts the scene on
     * demand, and only iterations where DrawOTag fired end the scene and
     * swap; empty iterations leave the previous game frame on screen. */

    S->start = SDL_GetTicks();  /* SDL3: SDL_GetTicks is 64-bit */
    run_deadline_arm(opts, S->start);
    open_refresh_query(S);
    host_pacer_init(&S->pacer, opts, S->refresh_hz);
    /* The getters report the seeded pacer state from here on. */
    host_live_knobs_publish(host_pacer_render_fps(&S->pacer),
                            host_pacer_tick_scale(&S->pacer));
    host_pacer_start(&S->pacer, opts);
    /* --rewind: the keyframe ring; its driver runs at the loop's quiescent point. */
    openpete_rewind_configure(opts->rewind_window);
    host_runahead_seed(opts);
    return open_outputs(S);
}

/* ---- close -------------------------------------------------------------- */

/** @brief Stop the present thread and the mods, flush settings, and release the bk2 movie and recorder.
 *
 * The present thread is joined before any other teardown: it presents
 * through the device the record flush drains, writes the frame-time log
 * and the pace log, and waits on this thread's clock, all of which close
 * tears down; stopping it hands the display back to this thread. Mods stop
 * their own threads before anything they call is torn down. The recorder
 * writes the movie only after a loop ran; on a setup error it is discarded,
 * so the file at the record path stays as it was.
 * @param S The session. */
static void close_threads_and_bk2(host_session_t* S) OP_REQUIRES(sim)
{
    sim_teardown_begin();
    openpete_present_thread_stop();
    host_trace_close_step(HT_S_PRESENT_THREAD_STOP);
    sim_teardown_progress();
    openpete_mods_shutdown(sim_teardown_progress);
    host_trace_close_step(HT_S_MODS_SHUTDOWN);
    sim_teardown_injected_stall();
    openpete_settings_save_flush("shutdown");   /* changes still inside the debounce */
    host_trace_close_step(HT_S_SETTINGS_FLUSH);
    if (S->loop_entered) {
        openpete_rebind_log_summary();
        host_trace_close_step(HT_S_REBIND_SUMMARY);
        bk2_feed_report();
        host_trace_close_step(HT_S_BK2_FEED_REPORT);
    }
    if (S->bk2.active) {
        bk2_movie_free(&S->bk2.movie);
        host_trace_close_step(HT_S_BK2_FREE);
        g_PsyX_Pad_InputOverride = 0xFFFF;
        g_PsyX_Pad_OverrideActive = 0;
        g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
        g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
    }
    if (S->bk2.recording) {
        openpete_crash_set_exit_flush(OP_EXIT_FLUSH_RECORD, NULL);
        if (S->loop_entered) {
            bk2_record_finalize();
            host_trace_close_step(HT_S_BK2_FINALIZE);
        } else {
            bk2_record_discard();
            host_trace_close_step(HT_S_BK2_DISCARD);
        }
        sim_teardown_progress();
    }
}

/** @brief Flush and shut down the video recorder, and close the position and hash logs.
 *
 * The native readback ring's queued tail frames drain to the encoder while
 * the Vulkan device is still alive, then the encoder flushes and writes the
 * container trailer; a SIGTERM reaching here still produces a playable
 * file. The recorder shutdown joins the encoder thread host init started.
 * @param S The session. */
static void close_record_and_logs(host_session_t* S) OP_REQUIRES(sim)
{
    if (S->loop_entered) {
        openpete_native_render_record_flush();
        host_trace_close_step(HT_S_RECORD_FLUSH);
        sim_teardown_progress();
    }
    openpete_record_shutdown();
    host_trace_close_step(HT_S_RECORD_SHUTDOWN);
    sim_teardown_progress();
    if (S->loop_entered) {
        /* --screenshot-presents-start/count engagement, reported whenever
         * the flag was set, because that fact is invisible in the images. */
        openpete_capture_report_presents(S->opts);
        host_trace_close_step(HT_S_CAPTURE_REPORT);
    }
    if (S->out.position_log) {
        fclose(S->out.position_log);
        S->out.position_log = NULL;
        host_trace_close_step(HT_S_POSITION_LOG_CLOSE);
    }
    if (S->out.hash_log) {
        fclose(S->out.hash_log);
        S->out.hash_log = NULL;
        host_trace_close_step(HT_S_HASH_LOG_CLOSE);
    }
}

/** @brief Report and finalize the --psxtrace-verify comparison, then close it.
 *
 * The summary and the PASS/FAIL verdict are output steps: on a setup
 * error the comparison never ran, and finalizing it would print a PASS.
 * A failed verdict turns an OK exit into OPENPETE_EXIT_DIVERGENCE.
 * @param S The session. */
static void close_psxtrace(host_session_t* S) OP_REQUIRES(sim)
{
    openpete_psxtrace_t* t = run_psxtrace();
    if (!t) return;
    if (S->loop_entered) {
        host_trace_close_step(HT_S_PSXTRACE_SUMMARY);
        openpete_psxtrace_exit_report(t);
        if (t->hard_failed && S->exit_code == OPENPETE_EXIT_OK)
            S->exit_code = OPENPETE_EXIT_DIVERGENCE;
    }
    run_psxtrace_release();
    host_trace_close_step(HT_S_PSXTRACE_CLOSE);
}

/** @brief Log the swapchain's image count and refusals; nothing without the native renderer. */
static void swapchain_report(void) OP_REQUIRES(sim)
{
    if (!openpete_native_render_enabled()) return;
    OPL_INFO_F("pace",
               "swapchain: %u image(s) granted; acquire refused %llu "
               "present(s) (%llu budget expiry, the rest suppressed by "
               "the back-off it opened); fence refused %llu present(s); "
               "%u present(s) never began",
               openpete_native_render_swapchain_images(),
               (unsigned long long)openpete_native_render_acquire_sheds(),
               (unsigned long long)openpete_native_render_acquire_timeouts(),
               (unsigned long long)openpete_native_render_fence_sheds(),
               openpete_native_render_begin_refused());
}

/** @brief Log the pacing census and the swapchain's refusals.
 *
 * Both print on every run that looped, with no flag: a run that was never
 * paced says so, and a run whose acquire budget expired has a display rate
 * that is not its present count, which the census cannot see. */
static void close_pace_reports(void) OP_REQUIRES(sim)
{
    char pace_summary[768];
    openpete_pace_census_format(pace_summary, sizeof(pace_summary));
    OPL_INFO_F("pace", "%s", pace_summary);
    host_trace_close_step(HT_S_PACE_CENSUS);
    if (!openpete_native_render_enabled()) return;
    swapchain_report();
    host_trace_close_step(HT_S_SWAPCHAIN_REPORT);
}

/** @brief Log the vertex-stream field census and the sfx pack census on stderr, where harnesses grep them.
 *
 * A non-zero `unresolved` in a run with no handle-writing mod means a
 * stock frame word lives below the handle bound. */
static void close_stream_censuses(void) OP_REQUIRES(sim)
{
    openpete_vstream_census_t vc;
    openpete_vstream_census(&vc);
    host_trace_close_step(HT_S_VSTREAM_CENSUS);
    if (vc.stock || vc.handle || vc.unresolved)
        openpete_log_rawf("[vstream] stock=%llu handle=%llu unresolved=%llu"
                " first=%#x\n", (unsigned long long)vc.stock,
                (unsigned long long)vc.handle,
                (unsigned long long)vc.unresolved,
                vc.first_unresolved);
    sfx_pack_census_t sc;
    sfx_pack_census(&sc);
    host_trace_close_step(HT_S_SFXPACK_CENSUS);
    if (sc.keyed || sc.active)
        openpete_log_rawf("[sfxpack] files=%d keyed=%llu resolved=%llu "
                "replaced=%llu rings_out=%llu verify_ok=%llu verify_bad=%llu "
                "cb_max_us=%llu cb_p99_us<=%llu sync_decodes=%llu\n",
                sc.files, (unsigned long long)sc.keyed,
                (unsigned long long)sc.resolved,
                (unsigned long long)sc.replaced,
                (unsigned long long)sc.rings_out,
                (unsigned long long)sc.verify_ok,
                (unsigned long long)sc.verify_bad,
                (unsigned long long)sc.cb_max_us,
                (unsigned long long)sc.cb_p99_us,
                (unsigned long long)sc.sync_decodes);
}

/** @brief Close the pace and frame-time logs, with the slip totals and the trace censuses between them.
 *
 * The clock itself is shut down by the caller once every thread that reads
 * it has stopped: the split's pump keeps calling op_now until the worker
 * is joined.
 * @param S The session. */
static void close_pacing_and_censuses(host_session_t* S) OP_REQUIRES(sim)
{
    if (S->loop_entered) close_pace_reports();
    openpete_pace_log_close();
    host_trace_close_step(HT_S_PACE_LOG_CLOSE);
    if (S->loop_entered && host_pacer_slip_report(&S->pacer))
        host_trace_close_step(HT_S_SLIP_TOTALS);
    openpete_frame_time_log_close(&S->out.ftl);
    host_trace_close_step(HT_S_FTL_CLOSE);
    if (S->loop_entered) {
        op_functrace_dump_aggregate();   /* per-function aggregate into the trace */
        host_trace_close_step(HT_S_FUNCTRACE);
        if (openpete_interp_enabled()) {
            host_trace_close_step(HT_S_INTERP_CENSUS);
            OPL_INFO_F("interp", "captured=%llu, dedupe-skipped=%llu",
                       (unsigned long long)openpete_interp_total_captures(),
                       (unsigned long long)openpete_interp_total_dedupe_skips());
        }
        if (runahead_predict_report())
            host_trace_close_step(HT_S_RA_PREDICT_REPORT);
        close_stream_censuses();
    }
}

/** @brief Shut down interpolation, delete the parked game fiber, and close the profile after the present-thread report.
 *
 * The game fiber is parked in its forever-yield loop, so deleting it is
 * safe. The profile closes after the present thread was joined: that
 * thread raises trace records while it renders, and closing the stream
 * under a live producer would flush a chunk it is still appending to.
 * @param S The session. */
static void close_fiber_and_profile(host_session_t* S) OP_REQUIRES(sim)
{
    openpete_interp_shutdown();
    host_trace_close_step(HT_S_INTERP_SHUTDOWN);
    if (openpete_game_fiber) {
        openpete_fiber_delete_guarded(openpete_game_fiber);
        openpete_game_fiber = NULL;
        host_trace_close_step(HT_S_FIBER_DELETE);
    }
    if (S->loop_entered) {
        openpete_present_thread_report();
        host_trace_close_step(HT_S_PRESENT_THREAD_REPORT);
    }
    op_prof_close();
    host_trace_close_step(HT_S_PROF_CLOSE);
}

/** @brief Log whether guest execution ever reached the Vulkan device.
 *
 * A present thread can run concurrently with a tick only when no call is
 * listed; a listed name is a call that would have to be moved, locked or
 * deferred first. Mapped-memory writes are reported even at zero calls:
 * they are host memcpys into device-visible buffers (the terrain sector
 * arenas, filled from hooks inside the guest's render walk), which no
 * Vulkan call names and which a present thread reading those arenas races
 * regardless of the call audit. */
static void gpu_audit_report(void) OP_REQUIRES(sim)
{
    host_trace_close_step(HT_S_GPU_AUDIT);
    const unsigned gpu_hits = op_gpu_audit_total();
    OPL_INFO_F("gpu-audit",
               "mapped-buffer writes during guest execution: %u (%llu bytes)",
               op_gpu_audit_map_writes(), (unsigned long long)op_gpu_audit_map_bytes());
    if (gpu_hits == 0) {
        OPL_INFO_F("gpu-audit", "%s", "no Vulkan calls during guest execution");
        return;
    }
    const char* names[32];
    const int n = op_gpu_audit_names(names, 32);
    char buf[1024];
    int at = 0;
    for (int i = 0; i < n && at < (int)sizeof(buf) - 1; i++)
        at += snprintf(buf + at, sizeof(buf) - (size_t)at, "%s%s", i ? " " : "", names[i]);
    if (!n) buf[0] = 0;
    OPL_WARN_F("gpu-audit", "%u Vulkan calls during guest execution (%d distinct): %s",
               gpu_hits, n, buf);
}

/** @brief Log the subsystem exit reports: native present, eager, disc, input, rumble, rewind, savestate, VBL delivery, OSD, version and gpu audit.
 *
 * The input parity report runs before PsyX_Shutdown, since it reads
 * PsyCross pad state. The OSD and version lines are engagement counters: a
 * capture that never composited a toast or drew the version line proves
 * nothing about it, whatever its pixels looked like. */
static void close_subsystem_reports(void) OP_REQUIRES(sim)
{
    openpete_native_present_census_report();
    host_trace_close_step(HT_S_NATIVE_PRESENT_CENSUS);
    host_present_exit_report();
    host_trace_close_step(HT_S_EAGER_REPORT);
    openpete_capture_report_intermediate();
    host_trace_close_step(HT_S_CAPTURE_INTER_REPORT);
    disc_cache_exit_census();
    host_trace_close_step(HT_S_DISC_CENSUS);
    openpete_host_input_parity_report();
    host_trace_close_step(HT_S_INPUT_PARITY);
    host_trace_close_step(HT_S_INPUT_LOG_REPORT);
    host_input_exit_report();
    rumble_census_report();
    host_trace_close_step(HT_S_RUMBLE_REPORT);
    openpete_rewind_report();
    host_trace_close_step(HT_S_REWIND_REPORT);
    host_trace_close_step(HT_S_SAVESTATE_REPORT);
    state_ops_report();
    if (psyq_vbl_delivery_report) psyq_vbl_delivery_report();
    host_trace_close_step(HT_S_VBL_DELIVERY);
    if (openpete_osd_enabled()) {
        OPL_INFO_F("osd", "census: %u toast(s) composited", openpete_native_osd_composited());
        host_trace_close_step(HT_S_OSD_CENSUS);
    }
    OPL_INFO_F("version", "census: %s (%s), %u line(s) drawn",
               openpete_build_version(), openpete_native_version_reason(),
               openpete_native_version_drawn());
    host_trace_close_step(HT_S_VERSION_CENSUS);
    gpu_audit_report();
    openpete_bugreport_report();
}

/** @brief Shut down the sfx pack and feature workers, and the renderer and PsyCross when this thread owns the window.
 *
 * The workers stop before SDL_Quit, which their SDL calls must not
 * overlap. Renderer and PsyCross teardown destroy the window and call
 * SDL_Quit, which belong to the thread that created them: under the split
 * that is main, which runs them after the join; tearing the window down
 * from the worker would hang the process on exit.
 * @param S The session. */
static void close_shutdown(const host_session_t* S) OP_REQUIRES(sim)
{
    sfx_pack_shutdown();
    host_trace_close_step(HT_S_SFX_SHUTDOWN);
    feature_registry_shutdown();
    host_trace_close_step(HT_S_FEATURE_SHUTDOWN);
    if (!S->split) {
        openpete_stall_monitor_stop();   /* its clock reads must not overlap SDL_Quit */
        openpete_native_render_shutdown();
        PsyX_Shutdown();
        host_trace_close_step(HT_S_RENDER_SHUTDOWN);
    }
}

/** @brief Print the host-input end state and the player's end pose on stdout.
 *
 * The input line prints while the input lock or the injection script is in
 * play: what the lock discarded and what a stray key or pointer reached.
 * The pose line uses --player-pose's grammar, so `--skip-to-level <level>
 * --player-pose "pos=(x, y, z) yaw=N"` starts the next run where this one
 * stopped; a title, demo, load or fly-in exit prints the reason instead. */
static void close_stdout_end_state(void) OP_REQUIRES(sim)
{
    if (openpete_input_lock_active() || openpete_host_mouse_inject_active()) {
        fprintf(stdout,
                "openpete-input: lock=%d dropped=%llu menu_open=%d freecam=%d "
                "freecam_engages=%u devui_open=%d mouse_motion=(%lld, %lld)\n",
                openpete_input_lock_active(), openpete_input_lock_dropped(),
                openpete_menu_is_open() ? 1 : 0,
                openpete_freecam_active() ? 1 : 0,
                openpete_freecam_engage_generation(),
                openpete_devui_is_open() ? 1 : 0,
                (long long)openpete_host_mouse_motion_total(0),
                (long long)openpete_host_mouse_motion_total(1));
        host_trace_close_step(HT_S_INPUT_STDOUT);
    }
    int32_t px, py, pz, pyaw, plevel;
    const int rc = game_get_player_pose(&g_game.cpu, &px, &py, &pz, &pyaw, &plevel);
    if (rc == 0) {
        if (pyaw >= 0) fprintf(stdout, "openpete-pose: level=%d pos=(%d, %d, %d) yaw=%d\n",
                               plevel, px, py, pz, pyaw);
        else           fprintf(stdout, "openpete-pose: level=%d pos=(%d, %d, %d) yaw=-\n",
                               plevel, px, py, pz);
    } else {
        fprintf(stdout, "openpete-pose: none reason=%s\n",
                rc == -1 ? "not-live" : "not-placeable");
    }
    host_trace_close_step(HT_S_POSE_STDOUT);
}

/* Documented at the declaration in host_loop_internal.h. */
void host_session_pace_env(char* env, unsigned env_bytes) OP_REQUIRES(sim)
{
    if (!env || !env_bytes) return;
    env[0] = '\0';
    host_session_t* S = s_press_session;
    if (!S || !S->opts) return;
    host_pace_env_build(env, env_bytes, S->opts, S->refresh_mhz,
                        host_pacer_render_fps(&S->pacer), S->pacer.launch_scale);
}

/* Documented at the declaration in host_loop_internal.h. */
bool host_session_paused(void) OP_REQUIRES(sim)
{
    return s_press_session && s_press_session->paused;
}

/* Documented at the declaration in host_loop_internal.h. */
void host_session_press_reports(void) OP_REQUIRES(sim)
{
    char pace_summary[768];
    openpete_pace_census_format(pace_summary, sizeof(pace_summary));
    OPL_INFO_F("pace", "%s", pace_summary);
    if (s_press_session) (void)host_pacer_slip_report(&s_press_session->pacer);
    if (openpete_interp_enabled())
        OPL_INFO_F("interp", "captured=%llu, dedupe-skipped=%llu",
                   (unsigned long long)openpete_interp_total_captures(),
                   (unsigned long long)openpete_interp_total_dedupe_skips());
    (void)runahead_predict_report();
    close_stream_censuses();
    openpete_capture_report_intermediate();
    disc_cache_exit_census();
    (void)openpete_host_input_parity_report();
    host_input_exit_report();
    rumble_census_report();
    openpete_rewind_report();
    state_ops_report();
    if (psyq_vbl_delivery_report) psyq_vbl_delivery_report();
    if (openpete_osd_enabled())
        OPL_INFO_F("osd", "census: %u toast(s) composited", openpete_native_osd_composited());
    OPL_INFO_F("version", "census: %s (%s), %u line(s) drawn",
               openpete_build_version(), openpete_native_version_reason(),
               openpete_native_version_drawn());
    gpu_audit_report();
}

/* Documented at the declaration in host_loop_internal.h. */
void host_session_display_reports(void) OP_REQUIRES(sim)
{
    swapchain_report();
    openpete_native_present_census_report();
    host_present_exit_report();
    openpete_present_thread_report();
}

int host_session_close(host_session_t* S, int rc) OP_REQUIRES(sim)
{
    S->exit_code = rc;
    openpete_bugreport_shutdown();
    close_threads_and_bk2(S);
    close_record_and_logs(S);
    close_psxtrace(S);
    S->exit_code = openpete_bugreport_replay_exit(S->exit_code);

    const Uint64 elapsed = SDL_GetTicks() - S->start;
    OPL_INFO_F("exit",
               "reason=%s frames=%u key=%u key_kind=%s elapsed_ms=%llu dispatch_ok=%d",
               S->exit_reason, openpete_frame, openpete_feed_key_now(), feed_key_kind_name(),
               (unsigned long long)elapsed, g_game.dispatch_ok);
    host_trace_close_step(HT_S_EXIT_LOG);

    close_pacing_and_censuses(S);
    close_fiber_and_profile(S);
    if (S->loop_entered) close_subsystem_reports();
    close_shutdown(S);

    fprintf(stdout,
            "openpete-exit: reason=%s frames=%u key=%u key_kind=%s ms=%llu code=%d\n",
            S->exit_reason, openpete_frame, openpete_feed_key_now(), feed_key_kind_name(),
            (unsigned long long)elapsed, S->exit_code);
    host_trace_close_step(HT_S_EXIT_STDOUT);
    if (S->loop_entered) close_stdout_end_state();
    return S->exit_code;
}

#endif /* OPENPETE_HAS_PSYCROSS */
