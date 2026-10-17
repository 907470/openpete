// core/runtime/run.c - openpete_run: boot the PSX EXE, optionally smoke-test
// one recompiled function, then run the host loop that drives the game fiber
// (input, tick, present, pacing, logs and captures). Under the thread split
// main pumps the window and a worker runs the loop; a build without PsyCross
// opens a window that only clears and presents.

#include "openpete.h"
#include "openpete_crash.h"
#include "openpete_fatal.h"
#include "openpete_mutex_once.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_knobs.h"
#include "openpete_host_input.h"
#include "openpete_host_mouse.h"
#include "openpete_input_lock.h"
#include "openpete_rumble.h"
#include "openpete_psx.h"
#include "openpete_discontinuity.h"
#include "openpete_recomp_module.h"
#include "openpete_ticker.h"
#include "openpete_time.h"
#include "openpete_present_thread.h"
#include "openpete_workpool.h"
#include "openpete_frame_time_log.h"
#include "openpete_pace_log.h"
#include "openpete_boot_timing.h"
#include "openpete_disc_trace.h"
#include "openpete_disc_image.h"
#include "rhi.h"
#include "../rhi/rhi_gpu_audit.h"
#include "openpete_profile.h"
#include "openpete_func_trace.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "screenshot.h"
#include "game_extras.h"
#include "bk2_feed.h"
#include "bk2_movie.h"
#include "bk2_record.h"
#include "openpete_input_log.h"
#include "openpete_rewind.h"
#include "openpete_record.h"
#include "openpete_native_render.h"
#include "openpete_build_id.h"
#include "game_layout.h"
#include "openpete_game_runtime.h"
#include "openpete_terrain_cache.h"
#include "openpete_fb_snapshot.h"
#include "openpete_ident_audit.h"
#include "openpete_run_capture.h"
#include "openpete_menu.h"
#include "openpete_settings_save.h"
#include "openpete_devui.h"
#include "openpete_camlock.h"
#include "openpete_glitch_mark.h"
#include "openpete_freecam.h"
#include "openpete_fillpick.h"
#include "openpete_keymap.h"  /* [keys] - every host hotkey below */
#include "openpete_osd.h"     /* toast the changes the player makes */
#include "openpete_rebind.h"  /* the binding page: drain, pad restamp */
#include "feature_registry.h"
#include "openpete_mods.h"
#include "openpete_vstream.h"
#include "../host/sfx_pack.h"
#include "no_lod.h"
#include "openpete_state.h"
#include "openpete_fnv1a64.h"
#include "openpete_psxtrace.h"
#include "music_override.h"
#include "openpete_memcard.h"
#include "openpete_handoff.h"

#include "PsyX/PsyX_public.h"  /* g_PsyX_Pad_InputOverride */

#include <SDL3/SDL.h>
#include <libco.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* _exit */
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>  /* emscripten_sleep (Asyncify) */
#endif

/* The default window comes from the game's own display environment
 * (game_default_window_w/h, game_layout.h): its height is the visible
 * DRAWENV clip height at two window rows per VRAM row, not the full DISPENV
 * height, because the game never draws into the overscan bands and PsyCross
 * crops them at present time. A game whose clip is shorter than Spyro 1's
 * therefore opens a shorter window instead of letterboxing inside a fixed
 * one. */

#define CLEAR_R 0x10
#define CLEAR_G 0x14
#define CLEAR_B 0x20

/* 1 while the canonical present's lagged re-extract (lag_re) is running -
 * lets feeder-side diagnostics tag which present leg produced a sample. */
int g_openpete_canon_lag_leg = 0;

/** @brief Advance the virtual present clock and pass it to openpete_interp_set_present_target as this present's instant.
 *
 * Off unless OPENPETE_PRESENT_TARGET_CLOCK is set to a value not starting
 * with '0'; while off the function returns at once and interpolation phase
 * comes from op_now(). While on, the clock steps by an EMA (weight 0.05) of
 * the measured present-to-present interval (samples under 100 ms), pulls
 * 1/16 of the remaining distance toward op_now(), and is clamped to within
 * 6 ms of op_now(); the first present, or a clock more than 50 ms from
 * op_now(), resyncs it to op_now(). Each present's content is then timed for
 * an evenly spaced display slot instead of the instant it was generated at,
 * which removes submit-time jitter. It also gives a present held on screen
 * through a tick the same slice of motion as one shown for one refresh,
 * which is why it is off by default. */
static void vp_set_present_target(void) {
    static int s_on = -1;
    if (s_on < 0) {
        /* Off by default: the clock spaces animation time evenly per present,
         * but display time is not evenly spaced. The tick executes on this
         * thread, so one image is held on screen for the length of it, and an
         * evenly spaced alpha gives that image the same slice of motion as an
         * image shown for one refresh: a systematic animation error each tick
         * (about 5 ms of animation at --render-fps 480) plus a small mid-tick
         * bias of the opposite sign. Letting the phase follow wall time
         * removes that term.
         *
         * What the clock buys is removal of submit-time jitter; without it
         * the error tail widens (p99 0.6-1.7 ms -> about 2.2 ms, roughly one
         * refresh at 480 Hz). A periodic deficit every tick is more visible
         * than random error of that size on a small share of presents, so the
         * systematic term is the one to remove. The [-6, +6] ms clamp below
         * keeps the clock inside the range the consumers accept, since they
         * allow slightly negative alpha rather than snapping shown time
         * forward at every interval roll. */
        const char* e = getenv("OPENPETE_PRESENT_TARGET_CLOCK");
        s_on = (e && *e && *e != '0');
    }
    if (!s_on) return;
    static op_time_t s_clk = 0, s_last = 0;
    static double s_sp = 0.0;
    op_time_t now = op_now();
    if (s_last != 0) {
        double d = (double)(now - s_last);
        if (d > 0.0 && d < 100.0 * (double)OP_NS_PER_MS) {
            if (s_sp <= 0.0) s_sp = d;
            else s_sp += (d - s_sp) * 0.05;
        }
    }
    s_last = now;
    if (s_clk == 0 || s_sp <= 0.0 ||
        now > s_clk + 50 * OP_NS_PER_MS || now + 50 * OP_NS_PER_MS < s_clk) {
        s_clk = now;                      /* first present / stall / jump: resync */
    } else {
        s_clk += (op_time_t)s_sp;         /* even step at the measured cadence */
        s_clk += (now - s_clk) / 16;      /* bounded pull toward reality */
        /* Hard drift bound (+-6 ms): the capture stamps live on their own
         * PLL-smoothed grid anchored to wall time; this clock smooths the
         * present cadence, so the two are independent oscillators. When the
         * present cadence shifts under load (runahead speculation inside a
         * tick iteration), the 1/16 pull is too weak and the standing offset
         * becomes an interp-alpha bias: presents clamp at the interval's
         * start and its tail is never shown, so motion arrives as 30 Hz
         * roll-jumps. Smoothing within +-6 ms keeps the jitter rejection;
         * beyond that the wall clock wins. */
        const op_time_t cap = 6 * OP_NS_PER_MS;
        if (s_clk < now - cap) s_clk = now - cap;
        else if (s_clk > now + cap) s_clk = now + cap;
    }
    openpete_interp_set_present_target(s_clk);
}

/* PCSX-Redux trace state - file-static so the dispatch hook (which has
 * to be a plain function pointer with no closure) can find it. There's
 * only ever one open trace per run. */
static openpete_psxtrace_t* s_psxtrace_active = NULL;
/* Set during host init, consumed by the host loop - which run on different
 * threads under the split, so this cannot stay a local. Written once before
 * the worker exists and read-only thereafter. */
static openpete_psxtrace_t* psxtrace = NULL;

/* The record index the next advance() consumes, and a skip on the active
 * trace (the load-window re-alignment), for the thunk layer's anchor hook. */
uint32_t openpete_psxtrace_current_record(void) {
    return s_psxtrace_active ? s_psxtrace_active->records_consumed : 0u;
}
int openpete_psxtrace_skip_current_to(uint32_t rec) {
    return s_psxtrace_active ? openpete_psxtrace_skip_to(s_psxtrace_active, rec) : -1;
}

/** @brief Advance the active psxtrace by one record at a guest anchor and update its terminal status.
 *
 * Installed as openpete_psxtrace_dispatch_hook. Does nothing without an
 * active trace or once its status is 1 (end of file) or 3 (error). An
 * advance error sets status 3, end of file status 1, and a hard failure
 * with status still 0 sets status 2, which the host loop reads to exit.
 * @param addr The anchor address, unused. */
static void openpete_psxtrace_on_anchor(uint32_t addr) {
    (void)addr;
    if (!s_psxtrace_active) return;
    /* EOF (1) and error (3) are terminal - stop advancing. Divergence (2)
     * is sticky for the host loop's exit-decision but the verifier keeps
     * comparing every subsequent frame so --psxtrace-keep-going can
     * surface ALL leaks across the trace, not just the first. Only a HARD
     * failure (non-transient mismatch, or a CD-load transient that didn't
     * heal in the window) trips status 2 - a healing CD-load transient
     * leaves hard_failed clear so the run continues to EOF. */
    if (s_psxtrace_active->terminal_status == 1 ||
        s_psxtrace_active->terminal_status == 3) return;
    int rc = openpete_psxtrace_advance(s_psxtrace_active);
    if (rc < 0)      s_psxtrace_active->terminal_status = 3;
    else if (rc == 0) s_psxtrace_active->terminal_status = 1;
    else if (s_psxtrace_active->hard_failed &&
             s_psxtrace_active->terminal_status == 0) {
        s_psxtrace_active->terminal_status = 2;
    }
}

/* Does the PsyCross GL renderer still rasterize the ordering table this run?
 * Computed once at boot (after the native renderer's bring-up). When the native
 * Vulkan renderer is the sole display (pure --renderer vk, windowed) PsyCross
 * does NOT walk/submit the OT - psyq_DrawOTag skips the GL rasterization (keeping
 * only the sim-tick timing pin) and PsyX_EndScene auto-no-ops via begin_scene_flag,
 * so there's no redundant GL draw and no second vsync swap on the hidden window.
 * It STAYS true (PsyCross renders) whenever the native renderer isn't the display
 * or its output is still needed: headless / psycross mode (where the native
 * window DISPLAYS the PsyCross frame, so the rasterization is the picture) /
 * native bring-up failure / --record (taps the PsyCross present FBO) /
 * --screenshot (the -og reference). Read by psyq_DrawOTag. */
int openpete_psycross_renders = 1;

/* Master interpolation/frame-gen toggle ('I' key). Gates the native
 * sub-tick intermediate presents; 1 = smooth (default), 0 = canonical
 * ~29.913 fps only. (Moved here from the deleted screen-space render
 * matcher, whose substitute path it also used to gate.) */
int g_openpete_interp_enabled = 1;

/* Cooperative shutdown flag. Set by the event pump (window close, SDL quit,
 * the quit binding) and by our SIGINT handler. The host-fiber loop checks this
 * once per iteration and drains cleanly, instead of PsyCross's PsyX_Exit()
 * calling exit(0) from inside PsyX_BeginScene (which can hang in WSL/GL with
 * the game fiber and vblank thread still mid-flight). Written by the pump,
 * the SIGINT handler and any thread calling openpete_request_shutdown, read by
 * the host loop and the pump's liveness check: a lock-free atomic, which is
 * both async-signal-safe and ordered across threads, where a volatile
 * sig_atomic_t is only the former. */
static atomic_int g_shutdown_requested = 0;

/* Sets g_shutdown_requested; documented at the declaration. */
void openpete_request_shutdown(void) { g_shutdown_requested = 1; }

/* Set by the event pump on a window double-click, by
 * openpete_native_request_fullscreen_toggle from any thread, and by the
 * host loop on its own key edge under the thread split; consumed with an
 * atomic exchange by the thread that owns the window, so a raise between a
 * read and a clear is never lost and a consumed request is never raised
 * again by an observer. Kept as a flag so the heavy SDL_SetWindowFullscreen
 * call runs in a loop, not in the event handler. */
static SDL_AtomicInt g_native_fs_toggle_request;

/* Player screenshot pending (the `screenshot` binding). Raised where the
 * bindings are sampled, consumed at the capture site below in the same
 * host-loop iteration - both on this thread, so a plain int is enough. */
static int s_screenshot_request = 0;

/* Raises g_native_fs_toggle_request; documented at the declaration. */
void openpete_native_request_fullscreen_toggle(void)
{
    SDL_SetAtomicInt(&g_native_fs_toggle_request, 1);
}

/* The process's single event pump.
 *
 * openpete owns the SDL_PollEvent loop; every dequeued event is routed on to
 * PsyCross's handlers (controller hotplug, resize, its debug keys, text input,
 * Alt tracking) so those keep working without PsyCross draining the queue
 * itself. SDL_PollEvent drains one shared queue, so a second drain does not
 * split the work - it steals events from whichever loop polls second.
 *
 * Each event goes to openpete_handle_event first, which consumes the ones
 * openpete owns outright - quit, window close, Escape's overlay dismiss, the
 * quit binding - and forwards the rest. Consuming quit/close here is what
 * keeps PsyX_Exit (and its exit(0), which hangs on static teardown under
 * WSL+GL+libco) unreachable. */
/* Guest-execution GPU audit (core/rhi/rhi_gpu_audit.h). Set around every
 * switch to the game fiber; the RHI records any Vulkan call made while it is
 * non-zero. Declared rather than included so run.c does not inherit the
 * header's vk* redirection macros. */
extern int g_openpete_guest_running;
extern unsigned op_gpu_audit_total(void);
extern int      op_gpu_audit_names(const char **out, int max);
extern unsigned op_gpu_audit_map_writes(void);
extern unsigned long long op_gpu_audit_map_bytes(void);

/** @brief Return 1 once per process when OPENPETE_GPU_AUDIT_SELFTEST asks to audit one real present.
 *
 * The caller sets g_openpete_guest_running around that native present, so
 * the GPU audit records its Vulkan calls. A reading of zero Vulkan calls
 * during guest execution is evidence only from a build shown to catch
 * calls; a mis-wired macro set reports the same zero as a clean tick.
 * @return 1 on the first call with the variable set to a value not starting
 *         with '0', else 0. */
static int gpu_audit_selftest_arm(void)
{
    static int state = -1;
    if (state < 0) {
        const char *e = getenv("OPENPETE_GPU_AUDIT_SELFTEST");
        state = (e && *e && *e != '0') ? 1 : 0;
    }
    if (state != 1) return 0;
    state = 0;          /* one present only */
    return 1;
}

/** @brief Read OPENPETE_THREAD_SPLIT once: whether the host loop may move to a worker thread while main pumps the window.
 *
 * Win32 runs a modal loop inside DefWindowProc for a title-bar drag or a
 * resize, which blocks the thread that owns the window. With the window and
 * the pump on main and the host loop on a worker (run_split_threaded), a
 * drag blocks main while the worker keeps ticking. OPENPETE_THREAD_SPLIT=0
 * keeps the host loop on main, the A/B for any defect suspected of coming
 * from the split. openpete_split_applies adds the configuration conditions.
 * @return 1 when the variable is unset, empty, or does not start with '0';
 *         else 0. */
static int openpete_thread_split_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_THREAD_SPLIT");
        v = (e && *e) ? (*e != '0') : 1;   /* default ON */
    }
    return v;
}

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
static bool openpete_split_applies(const openpete_options_t* opts)
{
    return openpete_thread_split_enabled()
        && !opts->headless
        && (opts->renderer == OPENPETE_RENDERER_VK
            || opts->renderer == OPENPETE_RENDERER_PSYCROSS)
        && opts->screenshot_frame_count <= 0
        && opts->screenshot_presents_start < 0
        && !openpete_host_input_legacy();
}

/* Set once the worker has finished, so main can leave its pump loop. */
static SDL_AtomicInt g_worker_done;
/* Incremented once per host-loop iteration on the sim thread. The pump
 * thread reads it to tell a hung sim (a runaway guest loop, a deadlocked
 * feeder) from a busy one, and to force the process down when a quit
 * request goes unanswered. */
static SDL_AtomicInt g_sim_heartbeat;
/* Set by the worker when its loop has exited and teardown (recorder
 * flush, movie finalize, joins) is running; the pump then allows the
 * longer teardown bound between heartbeats. */
static SDL_AtomicInt g_sim_teardown;

/** @brief Report teardown progress to the pump's liveness check: one heartbeat. */
static void sim_teardown_progress(void) { SDL_AddAtomicInt(&g_sim_heartbeat, 1); }

/** @brief Take a key event while a binding capture owns the keyboard.
 *
 * From the capture's start until the inputs it latched are released,
 * Escape posts the cancel (the key is latched so the poll does not also
 * fire Escape's binding), the quit test is skipped, and no key event or
 * repeat reaches PsyCross's debug keys, so Backspace stays capturable.
 * @param ev the event.
 * @return true when the event is a key event the capture owns. */
static bool capture_owns_key_event(const SDL_Event* ev) {
    if ((ev->type != SDL_EVENT_KEY_DOWN && ev->type != SDL_EVENT_KEY_UP) ||
        !openpete_keymap_capture_active())
        return false;
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat &&
        ev->key.scancode == SDL_SCANCODE_ESCAPE &&
        KM_CAPWORD_STATE(openpete_keymap_capture_word()) != KM_CAP_IDLE) {
        openpete_keymap_consume_key(ev->key.scancode);
        openpete_rebind_request_cancel("escape");
    }
    return true;
}

/** @brief Stall teardown for OPENPETE_TEARDOWN_STALL_MS milliseconds with no heartbeat.
 *
 * A fault-injection knob for the quit watchdog: it stands in for a
 * recorder flush that takes that long. Unset or 0 does nothing. */
static void sim_teardown_injected_stall(void)
{
    const char* e = getenv("OPENPETE_TEARDOWN_STALL_MS");
    const int ms = (e && *e) ? atoi(e) : 0;
    if (ms <= 0) return;
    OPL_WARN_F("host", "OPENPETE_TEARDOWN_STALL_MS: stalling teardown %d ms", ms);
    SDL_Delay((Uint32)ms);
}

static bool openpete_handle_event(SDL_Event* ev);

/* Idle-hide timer for the OS cursor: last mouse activity, in SDL ticks.
 * Written by the event pump (main thread) and read by the host loop (the
 * worker, once the thread split applies), hence the atomic. Unsigned 32-bit
 * differences stay correct across the ~49-day wrap. Seeded to 0 rather than
 * "now" would hide the cursor for the first two seconds of a session; the
 * host loop seeds it from the current tick on its first pass instead. */
static SDL_AtomicU32 g_mouse_last_activity_ms;

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
static void openpete_cursor_update(bool headless)
{
    enum { CURSOR_IDLE_HIDE_MS = 2000 };
    if (headless) return;

    Uint32 now_ms = (Uint32)SDL_GetTicks();
    static int cursor_shown = -1;   /* -1 = not yet applied */
    if (cursor_shown < 0)           /* first pass: start the idle clock */
        SDL_SetAtomicU32(&g_mouse_last_activity_ms, now_ms);

    Uint32 idle_ms = now_ms - SDL_GetAtomicU32(&g_mouse_last_activity_ms);
    bool want_cursor = openpete_devui_is_open() ||
                       openpete_devui_wants_mouse() ||
                       openpete_menu_is_open() ||
                       idle_ms < CURSOR_IDLE_HIDE_MS;
    if ((int)want_cursor == cursor_shown) return;

    if (want_cursor) SDL_ShowCursor();
    else             SDL_HideCursor();
    cursor_shown = (int)want_cursor;
}

/** @brief Read OPENPETE_PUMP_PROBE once: whether the event pump times its steps.
 *
 * A diagnostic for a pump that stalls: with it set, each drain first runs
 * the joystick update and the platform message pump as separate timed
 * calls, and each step or event handler that takes 20 ms or longer is
 * logged with its duration and event type.
 * @return 1 when the variable is set to a value not starting with '0'. */
static int pump_probe_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_PUMP_PROBE");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

/** @brief Log one pump step that took 20 ms or longer, under OPENPETE_PUMP_PROBE.
 *
 * @param step The step's name.
 * @param t0 op_now at the step's start.
 * @param type The event type the step dequeued or handled, or 0. */
static void pump_probe_note(const char* step, op_time_t t0, unsigned type)
{
    const op_time_t ns = op_now() - t0;
    if (ns >= 20000000)
        OPL_INFO_F("pump", "slow %s: %.3f ms (event 0x%x)", step,
                   (double)ns / 1e6, type);
}

/** @brief Drain the SDL event queue with each step timed, under OPENPETE_PUMP_PROBE.
 *
 * SDL_UpdateJoysticks and SDL_PumpEvents run first as separate calls, so a
 * stall is attributed to joystick detection and polling or to the
 * platform message pump; the drain then times each SDL_PollEvent (which
 * pumps again) and each handler. The summary line counts the drain's
 * events and its duration when it took 20 ms or longer. */
static void openpete_pump_events_probed(void) {
    SDL_Event ev;
    const op_time_t d0 = op_now();
    op_time_t t0 = d0;
    SDL_UpdateJoysticks();
    pump_probe_note("joystick update", t0, 0);
    t0 = op_now();
    SDL_PumpEvents();
    pump_probe_note("message pump", t0, 0);
    unsigned n = 0;
    for (;;) {
        t0 = op_now();
        if (!SDL_PollEvent(&ev)) break;
        pump_probe_note("poll", t0, ev.type);
        t0 = op_now();
        if (openpete_handle_event(&ev))
            PsyX_Sys_HandleEvent(&ev);
        pump_probe_note("handler", t0, ev.type);
        n++;
    }
    const op_time_t drain = op_now() - d0;
    if (drain >= 20000000)
        OPL_INFO_F("pump", "slow drain: %.3f ms, %u event(s)",
                   (double)drain / 1e6, n);
}

/** @brief Drain the SDL event queue, forwarding unconsumed events to PsyCross, then apply rumble.
 *
 * Each event goes through openpete_handle_event, and PsyX_Sys_HandleEvent
 * when it is not consumed; under OPENPETE_PUMP_PROBE the drain is timed
 * (openpete_pump_events_probed). The overlay's text-input state and rumble are
 * applied here because this thread owns the window and the SDL gamepad
 * handles. */
static void openpete_pump_events(void) {
    SDL_Event ev;
    if (pump_probe_enabled())
        openpete_pump_events_probed();
    else
        while (SDL_PollEvent(&ev)) {
            if (openpete_handle_event(&ev))
                PsyX_Sys_HandleEvent(&ev);
        }
    /* SDL text input is a window op of this thread; the overlay decides on
     * the present thread whether a field wants it. */
    openpete_devui_pump_text_input();
    /* Actuators are driven from here because this thread owns the SDL gamepad
     * handles: PsyCross opens and closes them in the controller-added /
     * -removed handlers just above, so applying on any other thread would race
     * a close. It also puts the focus gate where focus is knowable. */
    openpete_rumble_apply();
}

/** @brief Handle one event dequeued by the event pump and decide whether PsyCross also sees it.
 *
 * In order: the input lock drops physical-device events; the mouse channel
 * and the dev overlay see every remaining event; a joystick arrival is
 * reported against the mapping databases; mouse activity re-arms the
 * cursor idle timer. Quit and window close set the shutdown flag and are
 * consumed, so PsyX_Exit stays unreachable. A left double-click raises the
 * fullscreen toggle request unless the overlay, the menu, a mouse capture
 * or a mouse:left binding claims it. Escape closes an open dev overlay or
 * settings menu and is consumed until release; the quit binding shuts
 * down; an Escape left over is consumed so it cannot reach PsyCross's debug
 * keys.
 * @param ev The event.
 * @return true to forward the event to PsyCross, false when consumed. */
static bool openpete_handle_event(SDL_Event* ev) {
    /* The input lock discards an event from a physical input device before
     * any consumer below sees it. Quit and window events are not input and
     * fall through, so closing the window still exits. */
    if (openpete_input_lock_drops_event(ev)) return false;
    /* The mouse channel derives position, mask and the running totals from
     * the event stream (openpete_host_mouse.h); every consumer reads those. */
    openpete_host_mouse_handle_event(ev);
    /* Dev overlay sees every event (imgui io bookkeeping; frames run only
     * while openpete_devui_frame_wanted: overlay open, fill-tag inspector
     * active, or an enabled always-visible mod section), including ones
     * consumed below. */
    openpete_devui_process_event(ev);
    /* A device arriving after the mapping databases loaded still resolves
     * against them; say which one supplied it, or that none did. */
    if (ev->type == SDL_EVENT_JOYSTICK_ADDED)
        openpete_host_input_pad_arrived(ev->jdevice.which);
    /* Any mouse activity re-arms the idle-hide timer applied in the host
     * loop. Buttons and the wheel count as well as motion: a user clicking
     * without moving is still using the pointer. */
    if (ev->type == SDL_EVENT_MOUSE_MOTION ||
        ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        ev->type == SDL_EVENT_MOUSE_BUTTON_UP ||
        ev->type == SDL_EVENT_MOUSE_WHEEL) {
        SDL_SetAtomicU32(&g_mouse_last_activity_ms, (Uint32)SDL_GetTicks());
    }
    if (ev->type == SDL_EVENT_QUIT) {
        g_shutdown_requested = 1;
        return false;  /* never reaches PsyCross, so PsyX_Exit stays unreachable */
    }
    /* Double-click toggles native-window fullscreen (see the host loop) -
     * unless the click landed on the dev overlay, the settings menu is open,
     * a camera or mod has requested capture, or some action binds
     * mouse:left (two quick presses of it are not a window gesture). */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT && ev->button.clicks == 2 &&
        !openpete_devui_wants_mouse() &&
        openpete_host_mouse_double_click_allowed()) {
        openpete_host_mouse_double_click_engaged();
        SDL_SetAtomicInt(&g_native_fs_toggle_request, 1);
    }
    if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        g_shutdown_requested = 1;
        return false;
    }
    /* The pump runs on the window's owner, the one thread that may read its
     * flags; the published state serves every other reader. */
    if (ev->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
        ev->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
        openpete_native_publish_fullscreen();
    /* Quit-by-key, and Escape's overlay dismiss.
     *
     * Handled here rather than through the polled hotkey block because the
     * pump sees one key event at a time: polling would have to re-derive
     * "was this press already consumed by closing the menu?" from held
     * state, and would quit the game on the press that only closes the menu.
     *
     * Quitting is not gated on gamepad presence, so a synthetic keystroke
     * from a controller-emulation layer can quit; `quit = "none"` unbinds
     * it (see km_sdl_key_down in keymap_sdl.c). The window-close path above
     * quits regardless of any binding. */
    if (capture_owns_key_event(ev)) return false;
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        const bool esc = (ev->key.scancode == SDL_SCANCODE_ESCAPE);

        /* Escape dismisses an open overlay whatever `quit` is bound to -
         * a UI convention rather than a binding, and it keeps unbinding
         * quit from stranding a panel open. (The open flags are plain
         * cross-thread reads; the menu close is routed through a request
         * the host loop consumes at its poll point.)
         *
         * Consuming the key is what stops the dismiss fighting a polled
         * binding on the same key: with `menu` bound to Escape the poll
         * would otherwise still see it held and immediately reopen what
         * this just closed. Consumed until release, so pressing Escape
         * again reopens - the dismiss and the toggle compose. */
        if (esc && openpete_devui_is_open()) {
            openpete_devui_close();
            openpete_keymap_consume_key(ev->key.scancode);
            return false;
        }
        if (esc && openpete_menu_is_open()) {
            /* Requested, not toggled: under the thread split this runs on
             * the pump thread, and the host loop's pad-override clear latch
             * reads the open flag twice per iteration. The request lands
             * between those reads. */
            openpete_host_mouse_request_menu(0);
            openpete_keymap_consume_key(ev->key.scancode);
            return false;
        }

        if (openpete_keymap_key_binds(OPK_QUIT, ev->key.scancode)) {
            g_shutdown_requested = 1;
            return false;
        }
        /* Escape with quit unbound or rebound elsewhere: swallow it anyway
         * so it can't reach PsyCross's debug-key dispatch. */
        if (esc) return false;
    }
    return true;  /* everything else goes on to PsyCross */
}

/** @brief SIGINT and SIGTERM handler: request a graceful shutdown, and exit at once on a second signal.
 *
 * The first signal sets g_shutdown_requested; a second calls _exit(130).
 * @param sig The signal number, unused. */
static void openpete_on_sigint(int sig) {
    (void)sig;
    if (g_shutdown_requested) {
        /* Second signal: something is hung in graceful shutdown - escape
         * hatch so the user isn't stuck. _exit is async-signal-safe. */
        _exit(130);  /* 128 + SIGINT */
    }
    g_shutdown_requested = 1;
}

/** @brief Test whether a frame index lies in any of the options' unlock ranges, bounds inclusive.
 *
 * @param opts The parsed options.
 * @param frame The feed key's value (openpete_feed_key_now).
 * @return 1 when inside a range, else 0. */
static int unlock_frame_in_ranges(const openpete_options_t* opts, unsigned frame) {
    int f = (int)frame;
    for (int i = 0; i < opts->unlock_ranges_count; i++) {
        if (f >= opts->unlock_ranges[i].lo && f <= opts->unlock_ranges[i].hi) {
            return 1;
        }
    }
    return 0;
}

/** @brief Return the name of the counter a movie's rows are indexed by.
 * @param key The movie's row key.
 * @return "openpete_consume", "openpete_vbl" or "openpete_frame". */
static const char* bk2_row_key_name(bk2_row_key_t key) {
    if (key == BK2_ROW_KEY_CONSUME) return "openpete_consume";
    return key == BK2_ROW_KEY_VBL ? "openpete_vbl" : "openpete_frame";
}

/** @brief Name the game's feed key, the counter --frame-limit and the one-shot triggers read.
 * @return "vbl" for openpete_vbl, "frame" for openpete_frame. */
static const char* feed_key_kind_name(void) {
    return openpete_feed_key() == OPENPETE_FEED_KEY_VBL ? "vbl" : "frame";
}

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
static int bk2_replay_load(const openpete_options_t* opts, bk2_movie_t* bk2,
                           int* offset) {
    if (bk2_movie_load(opts->bk2_path, bk2) != 0) {
        return -1;
    }
    if (bk2->load_delay != openpete_cd_load_delay) {
        OPL_ERROR_F("bk2", "%s was recorded with a load delay of %d VBL(s); "
                    "this run uses %d. Pass --load-delay %d to replay it.",
                    opts->bk2_path, bk2->load_delay, openpete_cd_load_delay,
                    bk2->load_delay);
        bk2_movie_free(bk2);
        return -1;
    }
    if (bk2->row_key == BK2_ROW_KEY_CONSUME && !openpete_game_runtime()->consume_pc) {
        OPL_ERROR_F("bk2", "%s is keyed on consume; %s names no pad-ring consumer",
                    opts->bk2_path, game_get_display_name());
        bk2_movie_free(bk2);
        return -1;
    }
    if (bk2->row_key != BK2_ROW_KEY_CONSUME)
        OPL_INFO_F("bk2", "%s is keyed on %s: converted on load (replayed under "
                   "that key)", opts->bk2_path, bk2_row_key_name(bk2->row_key));
    /* Default offset is 0: both counters start at 0 on record and
     * replay, so the row numbers line up 1:1 and replay sees the same
     * game state the player saw when they pressed the button.
     * Auto-skipping to first_input_frame would collapse the player's
     * idle wait and fire input before the game is ready. Use
     * --bk2-frame-offset to opt into a manual skip. */
    *offset = opts->bk2_frame_offset_set ? opts->bk2_frame_offset : 0;
    const char* clock = bk2_row_key_name(bk2->row_key);
    OPL_INFO_F("bk2",
               "loaded %d frames from %s, first input at bk2-frame=%d, "
               "%s=%d -> bk2-frame=%s-(%d)%s",
               bk2->frame_count, opts->bk2_path, bk2->first_input_frame,
               clock, *offset, clock, *offset,
               opts->bk2_frame_offset_set ? " (explicit)" : "");
    return 0;
}

/** @brief Append the staged input record once per yield of the iteration and note each entry's movie rows while recording.
 *
 * A recorded movie carries one row per pad write of a yield, grouped by
 * openpete_consume: the iteration's writes (bk2_feed_iter_take) are noted on
 * its last entry.
 * @param ticks Yields the iteration ran (openpete_frame's advance).
 * @param recording True while --bk2-record is recording.
 * @param bits Player 1 pad bits the guest read, for the recorder's log. */
static void input_log_commit_yields(unsigned ticks, bool recording, uint16_t bits) {
    unsigned e = 0u, e_last = 0u, j = 0u;
    const unsigned writes = bk2_feed_iter_take(&e, &e_last, &j);
    for (unsigned t = 0; t < ticks; t++) {
        const int idx = input_log_append_staged();
        if (recording) bk2_record_note(bits, idx, t + 1 == ticks ? writes : 0u, e, e_last, j);
    }
}

/** @brief Stamp a movie row into PsyCross's pad-override slot.
 *
 * A row outside the movie stamps no override (0xFFFF, inactive), which
 * hands the pad to live input.
 * @param m The replayed movie.
 * @param row The row, as bk2_feed_row returned it.
 * @return 1 when a movie row was stamped, else 0. */
static int bk2_stamp_row(const bk2_movie_t* m, int row) {
    const int in = row >= 0 && row < m->frame_count;
    unsigned short bits = 0xFFFF;
    if (in) {
        const bk2_frame_t* fr = &m->frames[row];
        bits = fr->buttons;
        /* bk2 axis order is LX,LY,RX,RY; PSX pad->analog is RX,RY,LX,LY. */
        g_PsyX_Pad_AnalogOverride[0] = fr->axes[2]; /* RStick X */
        g_PsyX_Pad_AnalogOverride[1] = fr->axes[3]; /* RStick Y */
        g_PsyX_Pad_AnalogOverride[2] = fr->axes[0]; /* LStick X */
        g_PsyX_Pad_AnalogOverride[3] = fr->axes[1]; /* LStick Y */
    }
    g_PsyX_Pad_InputOverride = bits;
    g_PsyX_Pad_OverrideActive = in;
    bk2_feed_stamped(row);
    return in;
}

/** @brief Append a "frame,vbl,x,y,z" row of the player position to the position log.
 *
 * Writes nothing when game_get_player_position reports no position.
 * @param fp The log file.
 * @param frame The tick counter.
 * @param vbl The VBL counter. */
static void position_log_emit(FILE* fp, unsigned frame, int vbl) {
    int32_t x = 0, y = 0, z = 0;
    if (game_get_player_position(&x, &y, &z) != 0) return;
    fprintf(fp, "%u,%d,%d,%d,%d\n", frame, vbl, x, y, z);
}

/* Default-empty fallback for the gameplay-state regions table. Per-game
 * TUs call openpete_state_regions_register() from game main (explicit
 * registration, NOT weak-symbol override: MinGW's PE/COFF linker
 * mis-resolves weak overrides in this configuration - count resolved
 * strong while the pointer resolved weak-NULL, a NULL deref the moment
 * --psxtrace-verify computed the first hash on Windows). Without
 * registration the hash log is the FNV1a-64 offset basis on every
 * frame - useless but link-safe. */
const OpenpeteStateRegion* openpete_state_regions = NULL;
size_t openpete_state_region_count = 0;
uint64_t openpete_state_regions_table_hash = 0;

void openpete_state_regions_register(const OpenpeteStateRegion* regions,
                                     size_t count, uint64_t table_hash) {
    openpete_state_regions = regions;
    openpete_state_region_count = count;
    openpete_state_regions_table_hash = table_hash;
}

uint64_t gameplay_hash_compute_ex(uint64_t* out_per_region);
/** @brief Compute the FNV-1a-64 hash over every registered gameplay-state region, and optionally one hash per region.
 *
 * Covers every region the per-game TU registered in openpete_state_regions.
 * Any change to gameplay state appears as a hash divergence at the first
 * tick the change reaches a registered region; the per-region hashes name
 * that region without a RAM dump. Indirect regions read their base pointer
 * from RAM and are skipped when it is NULL or its slot lies outside RAM;
 * END_PTR, COUNT and SENTINEL kinds clamp the hashed length to the live
 * part of the pool. Bytes at a region's mask offsets are excluded from both
 * the per-region and the total hash.
 * @param out_per_region NULL, or an array of at least
 *        openpete_state_region_count slots receiving each region's hash;
 *        0 marks a skipped region.
 * @return The total hash over all regions in registration order. */
uint64_t gameplay_hash_compute_ex(uint64_t* out_per_region) {
    uint64_t total = openpete_fnv1a64_init();
    const uint8_t* ram = openpete_psx_get_ram();
    const uint32_t RAM_BYTES = 2u * 1024u * 1024u;
    for (size_t r = 0; r < openpete_state_region_count; r++) {
        const OpenpeteStateRegion* reg = &openpete_state_regions[r];
        if (out_per_region) out_per_region[r] = 0;
        uint32_t base_off;
        uint32_t live_len = reg->length;
        const int is_indirect = (reg->kind == OPENPETE_REGION_INDIRECT_ARRAY
                              || reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_END_PTR
                              || reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_COUNT
                              || reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_SENTINEL);
        uint32_t base_vaddr = 0;
        /* Gameplay-state regions are canonical-2 MB by contract (regions
         * registered via psx_runtime_register_region are presentation-
         * only and explicitly excluded from the determinism gate).
         * psx_runtime_canonicalize is `addr & 0x1FFFFF` in release; in
         * debug it asserts the input doesn't fall in a registered
         * region's band - catching a future state-region manifest
         * mistake before it silently mirrors into g_ram. */
        if (is_indirect) {
            uint32_t ptr_off = psx_runtime_canonicalize(reg->start);
            if (ptr_off + 4u > RAM_BYTES) continue;
            base_vaddr =
                  (uint32_t)ram[ptr_off]
                | ((uint32_t)ram[ptr_off + 1] << 8)
                | ((uint32_t)ram[ptr_off + 2] << 16)
                | ((uint32_t)ram[ptr_off + 3] << 24);
            if (base_vaddr == 0) continue;
            base_off = psx_runtime_canonicalize(base_vaddr);
        } else {
            base_off = psx_runtime_canonicalize(reg->start);
        }
        if (reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_END_PTR) {
            uint32_t aux_off = psx_runtime_canonicalize(reg->aux);
            if (aux_off + 4u <= RAM_BYTES) {
                uint32_t end_vaddr =
                      (uint32_t)ram[aux_off]
                    | ((uint32_t)ram[aux_off + 1] << 8)
                    | ((uint32_t)ram[aux_off + 2] << 16)
                    | ((uint32_t)ram[aux_off + 3] << 24);
                /* Allocator end-pointer < base means the pool is empty
                 * (alloc_ptr reset to NULL). The deallocated residue
                 * isn't game-readable; without this clamp we'd hash
                 * stale bytes in [base..base+reg->length) that vary
                 * across emulator runs without affecting game state. */
                if (end_vaddr >= base_vaddr) {
                    uint32_t live = end_vaddr - base_vaddr;
                    if (live < live_len) live_len = live;
                } else {
                    live_len = 0;
                }
            }
        } else if (reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_COUNT) {
            uint32_t aux_off = psx_runtime_canonicalize(reg->aux);
            if (aux_off + 4u <= RAM_BYTES && reg->elem_size > 0u) {
                uint32_t cnt =
                      (uint32_t)ram[aux_off]
                    | ((uint32_t)ram[aux_off + 1] << 8)
                    | ((uint32_t)ram[aux_off + 2] << 16)
                    | ((uint32_t)ram[aux_off + 3] << 24);
                uint64_t live = (uint64_t)cnt * (uint64_t)reg->elem_size;
                if (live < (uint64_t)live_len) live_len = (uint32_t)live;
            }
        } else if (reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_SENTINEL) {
            const uint32_t es = reg->elem_size;
            const uint32_t s_off = reg->aux & 0xFFu;
            const uint8_t  s_val = (uint8_t)((reg->aux >> 8) & 0xFFu);
            if (es > 0u && s_off < es) {
                uint32_t n = 0;
                while ((uint64_t)(n + 1u) * es <= live_len &&
                       base_off + (uint64_t)n * es + s_off < RAM_BYTES &&
                       ram[base_off + n * es + s_off] != s_val)
                    n++;
                live_len = n * es;
            }
        }
        uint32_t end = base_off + live_len;
        if (end > RAM_BYTES) end = RAM_BYTES;
        if (end <= base_off) continue;
        size_t len = end - base_off;
        const uint8_t* p = ram + base_off;
        /* Mask-aware hash: for regions tagged with mask_offsets, skip
         * the masked bytes during FNV update for BOTH the per-region
         * and total hashes. Both absolute and per-element variants
         * supported (see openpete_psx.h). When mask_count==0, falls
         * through to the existing bulk-update fast path so non-masked
         * regions retain identical hashes to pre-mask-aware code. */
        if (reg->mask_count == 0) {
            if (out_per_region) out_per_region[r] = openpete_fnv1a64(p, len);
            total = openpete_fnv1a64_update(total, p, len);
        } else {
            const uint16_t* m  = reg->mask_offsets;
            const uint16_t mc  = reg->mask_count;
            const uint32_t es  = reg->elem_size; /* 0 if not per-element */
            const int per_el   = (reg->mask_per_element != 0u && es != 0u);
            uint64_t per = openpete_fnv1a64_init();
            for (size_t i = 0; i < len; i++) {
                uint32_t off = per_el ? (uint32_t)(i % es) : (uint32_t)i;
                int masked = 0;
                for (uint16_t mi = 0; mi < mc; mi++) {
                    if ((uint32_t)m[mi] == off) { masked = 1; break; }
                }
                if (masked) continue;
                per   ^= (uint64_t)p[i];
                per   *= 0x100000001B3ULL;
                total ^= (uint64_t)p[i];
                total *= 0x100000001B3ULL;
            }
            if (out_per_region) out_per_region[r] = per;
        }
    }
    return total;
}

#define OPENPETE_HASH_LOG_MAX_REGIONS 128

/** @brief Read OPENPETE_RA_PREDICT_CHECK once: the runahead prediction-exactness check level.
 *
 * The check tests that with unchanged input the led state is the future
 * committed state bit for bit. A thunk that reads host state behaving
 * differently under speculation than in the real future (the wall clock, a
 * host static outside the snapshot) breaks that while every
 * state-roundtrip determinism gate stays green. After each speculation the
 * host loop hashes the led guest RAM, scratchpad and mod arena, keyed by
 * the led tick; when the committed timeline reaches that tick the hashes
 * must match. An input edge invalidates the pending predictions (the branch
 * legitimately changed), and so does a guest discontinuity (rewind and
 * state-load replays run under live input, not the predicted branch).
 * @return 0 off, 1 hash check, 2 hash check plus a full copy of the led RAM
 *         so a mismatch dumps the differing ranges. */
static int ra_pc_on(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_RA_PREDICT_CHECK");
        v = (e && *e && *e != '0') ? atoi(e) : 0;
        if (v < 0) v = 0;
    }
    return v;   /* 1 = hash check; 2 = + full led copy, dump diff ranges */
}
#define RA_PC_PENDING 8
static struct { uint32_t target; uint64_t hash; int valid; uint8_t* led; } s_ra_pc[RA_PC_PENDING];
static unsigned s_ra_pc_checked = 0, s_ra_pc_mismatches = 0, s_ra_pc_invalidated = 0;
/* Wall time of the last runahead snapshot save, added to its restore's for
 * the Mods panel's snapshot cost. */
static op_time_t s_ra_save_ns = 0;

/** @brief Take runahead's per-tick snapshot, timing the save for the snapshot cost the Mods panel shows.
 *
 * @param snap The runahead snapshot.
 * @param cpu The guest register file.
 * @param frame The committed frame the snapshot records.
 * @return openpete_state_mem_save's result, 0 on success. */
static int ra_snapshot_save(openpete_state_mem_t* snap, const CPUState* cpu,
                            unsigned frame) {
    const op_time_t t0 = op_now();
    const int rc = openpete_state_mem_save(snap, cpu, frame);
    s_ra_save_ns = op_now() - t0;
    return rc;
}

/** @brief Roll back to runahead's snapshot quietly, recording that tick's save plus restore time.
 *
 * @param snap The runahead snapshot.
 * @param cpu The guest register file, restored.
 * @param frame Receives the snapshot's frame.
 * @return openpete_state_mem_load's result, 0 on success. */
static int ra_snapshot_rollback(const openpete_state_mem_t* snap, CPUState* cpu,
                                unsigned* frame) {
    const op_time_t t0 = op_now();
    const int rc = openpete_state_mem_load(snap, cpu, frame, 1);
    if (rc == 0)
        openpete_mods_runahead_snapshot_note(
            (uint64_t)(s_ra_save_ns + (op_now() - t0)) / 1000u);
    return rc;
}
/** @brief Drop every pending runahead prediction, counting each one dropped. */
static void ra_pc_invalidate_all(void) {
    for (int i = 0; i < RA_PC_PENDING; i++)
        if (s_ra_pc[i].valid) { s_ra_pc[i].valid = 0; s_ra_pc_invalidated++; }
}
/** @brief Hash guest RAM, the scratchpad and the mod arena's three used ranges with FNV-1a-64.
 *
 * @return The hash the runahead prediction check compares. */
static uint64_t ra_pc_hash(void) {
    uint64_t h = openpete_fnv1a64_init();
    h = openpete_fnv1a64_update(h, openpete_psx_get_ram(), OPENPETE_PSX_RAM_SIZE);
    h = openpete_fnv1a64_update(h, openpete_psx_get_scratch(), OPENPETE_PSX_SCRATCH_SIZE);
    /* The mod arena is inside the rollback boundary: a led/committed
     * split there is the same class of misprediction as one in RAM. */
    uint32_t used[OPENPETE_ARENA_RANGES];
    (void)openpete_mods_arena_used(used);
    for (unsigned r = 0; r < OPENPETE_ARENA_RANGES; r++)
        if (used[r])
            h = openpete_fnv1a64_update(h, openpete_mods_arena_range_bytes(r), used[r]);
    return h;
}

/** @brief Record the led prediction in a free pending slot while RAM is the speculation head, keyed by the led tick.
 *
 * Nothing when the check is off or every slot is pending. At level 2 the
 * slot also keeps a copy of the led RAM and scratchpad for the mismatch
 * dump. */
static void ra_pc_record_led(void) {
    if (!ra_pc_on()) return;
    for (int i = 0; i < RA_PC_PENDING; i++) {
        if (s_ra_pc[i].valid) continue;
        s_ra_pc[i].target = openpete_frame;
        s_ra_pc[i].hash   = ra_pc_hash();
        s_ra_pc[i].valid  = 1;
        if (ra_pc_on() < 2) return;
        if (!s_ra_pc[i].led)
            s_ra_pc[i].led = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE + OPENPETE_PSX_SCRATCH_SIZE);
        if (!s_ra_pc[i].led) return;
        memcpy(s_ra_pc[i].led, openpete_psx_get_ram(), OPENPETE_PSX_RAM_SIZE);
        memcpy(s_ra_pc[i].led + OPENPETE_PSX_RAM_SIZE, openpete_psx_get_scratch(),
               OPENPETE_PSX_SCRATCH_SIZE);
        return;
    }
}

/** @brief Write the CSV header of the gameplay hash log: frame, vbl, fnv1a64 and one column per region.
 *
 * A region name has commas, CR and LF replaced by '_'. More than
 * OPENPETE_HASH_LOG_MAX_REGIONS regions log an error and truncate the
 * columns to that many.
 * @param fp The log file. */
static void gameplay_hash_log_emit_header(FILE* fp) {
    fputs("frame,vbl,fnv1a64", fp);
    /* One column per registered region, headed by its name. The
     * harness uses these to pinpoint which region first diverged.
     * Capped at OPENPETE_HASH_LOG_MAX_REGIONS to keep the per-frame
     * line bounded; over the cap we still emit the total hash and
     * skip extra columns (an error is logged). */
    size_t n = openpete_state_region_count;
    if (n > OPENPETE_HASH_LOG_MAX_REGIONS) {
        OPL_ERROR_F("hash-log",
                    "too many regions (%zu > %d); per-region columns truncated",
                    n, OPENPETE_HASH_LOG_MAX_REGIONS);
        n = OPENPETE_HASH_LOG_MAX_REGIONS;
    }
    for (size_t r = 0; r < n; r++) {
        const char* name = openpete_state_regions[r].name;
        fputc(',', fp);
        /* Sanitize: strip commas / newlines defensively. */
        for (const char* c = name; *c; c++) {
            fputc((*c == ',' || *c == '\n' || *c == '\r') ? '_' : *c, fp);
        }
    }
    fputc('\n', fp);
}

/** @brief Append one gameplay hash log row: frame, vbl, the total hash and each region hash in hex.
 *
 * The per-region array holds OPENPETE_HASH_LOG_MAX_REGIONS entries and the
 * row carries at most that many region columns, matching the header, while
 * gameplay_hash_compute_ex writes one entry per registered region.
 * @param fp The log file.
 * @param frame The tick counter.
 * @param vbl The VBL counter. */
static void gameplay_hash_log_emit(FILE* fp, unsigned frame, int vbl) {
    uint64_t per_region[OPENPETE_HASH_LOG_MAX_REGIONS];
    size_t n = openpete_state_region_count;
    if (n > OPENPETE_HASH_LOG_MAX_REGIONS) n = OPENPETE_HASH_LOG_MAX_REGIONS;
    /* gameplay_hash_compute_ex always fills the prefix [0..region_count); */
    uint64_t total = gameplay_hash_compute_ex(per_region);
    fprintf(fp, "%u,%d,%016llx", frame, vbl, (unsigned long long)total);
    for (size_t r = 0; r < n; r++) {
        fprintf(fp, ",%016llx", (unsigned long long)per_region[r]);
    }
    fputc('\n', fp);
}

/* Implemented in core/psx/exe_loader.cpp. */
extern uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                                const char* exe_filename,
                                                size_t* out_size);
extern uint8_t* openpete_psx_read_file(const char* path, size_t* out_size);

extern void openpete_psx_set_diag_cpu(CPUState* cpu);

/* CPUState seed file format. Little-endian throughout. Total 528 bytes:
 *   uint32_t magic            = 'CSv1' (LE 0x31765343)
 *   uint32_t gpr[32]          = zero, at, v0, v1, a0..a3, t0..t7, s0..s7,
 *                               t8, t9, k0, k1, gp, sp, fp, ra
 *   uint32_t pc, hi, lo
 *   uint32_t cop0[32]
 *   uint32_t gte_data[32]
 *   uint32_t gte_ctrl[32]
 * Written by scripts/pcsx-redux-ram-dump.lua when PCSXR_DUMP_CPU is set. */
#define OPENPETE_CPU_SEED_MAGIC 0x31765343u   /* 'CSv1' little-endian */
#define OPENPETE_CPU_SEED_SIZE  528u

/** @brief Load a CPUState seed file into cpu: the GPRs, pc, hi, lo, cop0 and both GTE register files.
 *
 * The format is described above (528 bytes, magic 'CSv1'). Bytes past the
 * 528th are ignored and no other CPUState field is touched. A failure is
 * logged and leaves cpu unchanged.
 * @param cpu The CPU state to fill.
 * @param path The seed file.
 * @return 0 on success, -1 when the file cannot be opened, is shorter than
 *         528 bytes, or has a wrong magic. */
static int load_cpu_seed(CPUState* cpu, const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        OPL_ERROR_F("seed-cpu", "fopen %s failed: %s", path, strerror(errno));
        return -1;
    }
    uint32_t buf[OPENPETE_CPU_SEED_SIZE / 4];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n != sizeof(buf)) {
        OPL_ERROR_F("seed-cpu", "%s: short read %zu of %u",
                    path, n, OPENPETE_CPU_SEED_SIZE);
        return -1;
    }
    if (buf[0] != OPENPETE_CPU_SEED_MAGIC) {
        OPL_ERROR_F("seed-cpu", "%s: bad magic 0x%08X (expected 0x%08X 'CSv1')",
                    path, buf[0], OPENPETE_CPU_SEED_MAGIC);
        return -1;
    }
    /* GPRs: 32 contiguous uint32s in CPUState declaration order. */
    memcpy(&cpu->zero,    &buf[1],    32 * sizeof(uint32_t));
    cpu->pc = buf[33];
    cpu->hi = buf[34];
    cpu->lo = buf[35];
    memcpy(cpu->cop0,     &buf[36],   32 * sizeof(uint32_t));
    memcpy(cpu->gte_data, &buf[68],   32 * sizeof(uint32_t));
    memcpy(cpu->gte_ctrl, &buf[100],  32 * sizeof(uint32_t));
    return 0;
}

#ifdef OPENPETE_HAS_PSYCROSS
/* PsyCross window/GL initialiser. Must be called before any libgpu/libgte
 * call (ResetGraph, SetGeomScreen, etc.) - those touch internal SDL/GL
 * state set up here. */
/* Returns 0 if the window, core or PSX GPU state failed to come up. Ignoring
 * that leaves PsyCross's GL entry points unresolved, and the first libgpu call
 * jumps through a null proc pointer. */
extern int PsyX_Initialise(char* windowName, int screenWidth,
                           int screenHeight, int fullscreen, int startHidden);
extern void PsyX_Shutdown(void);
extern void PsyX_SetDithering(int enable);

/** @brief Set PsyCross's ordered dither: off in a run that writes -og screenshots, on otherwise.
 *
 * The PsyCross raster adds the 4x4 dither matrix to every shaded polygon
 * whose draw state asks for it. A run that writes the -og reference
 * (--screenshot-on-frame/-tick or --screenshot-presents-start without
 * --screenshot-native-only) draws it undithered, so the reference compares
 * with the native render, which is undithered unless --dither is set. Every
 * other run keeps the dither: PsyCross's framebuffer is copied back into the
 * VRAM mirror that StoreImage reads into guest RAM, so those bytes stay as
 * they were. OPENPETE_PSYX_DITHER=0|1 overrides either default. Read once at
 * boot.
 * @param opts Resolved launch options. */
static void apply_psyx_dither_env(const openpete_options_t* opts)
{
    const bool og_written = !opts->screenshot_native_only
        && (opts->screenshot_frame_count > 0 || opts->screenshot_presents_start >= 0);
    const char* v = getenv("OPENPETE_PSYX_DITHER");
    int on = !og_written;
    if (v && (strcmp(v, "0") == 0 || strcmp(v, "1") == 0)) on = v[0] == '1';
    PsyX_SetDithering(on);
}
#include "openpete_disc_cache.h"
extern char PsyX_BeginScene(void);
extern void PsyX_EndScene(void);
extern void PsyX_UpdateInput(void);
/* Declared in PsyCross's private src/audio/PsyX_SPUAL.h and PsyX_SPUFeed.h,
 * which are not on any public include path. Runahead and rewind replay run
 * the SPU speculatively (its frames are committed or dropped, never fed
 * twice); pause, fast-forward and replay hold the device feed. */
extern int PsyX_SPUAL_SetMute(int on_off);
extern void PsyX_SPUAL_Speculate(int on, int commit);
extern void PsyX_SPUAL_SetCapture(int on);
extern void PsyX_SPUFeed_SetHold(int on);
extern void PsyX_SPUFeed_Pump(void);
extern void PsyX_SPUFeed_Simulate(double drift, unsigned int seed, double start_stall);
extern void PsyX_SPUFeed_SetNominalRate(double rate);
#define PSYX_SPUFEED_PHASES 8
typedef struct PsyX_SPUFeed_PhaseStats {
    unsigned long long pushed, held, overflow, underruns, primes;
} PsyX_SPUFeed_PhaseStats;
typedef struct PsyX_SPUFeed_Stats {
    unsigned long long pushed, held, overflow, consumed, pulls, underruns, primes;
    int target, depth_min, depth_p01, depth_p50, depth_p99;
    double ratio, rate;
    unsigned long long taken, discarded;
    unsigned int fifo;
    unsigned long long data_out, silence_out;
    PsyX_SPUFeed_PhaseStats phase[PSYX_SPUFEED_PHASES];
    char phase_name[PSYX_SPUFEED_PHASES][16];
} PsyX_SPUFeed_Stats;
extern void PsyX_SPUFeed_SetPhase(int phase, const char* name, int tag);
extern void PsyX_SPUFeed_SetLog(const char* path);
extern void PsyX_SPUFeed_GetStats(PsyX_SPUFeed_Stats* out);
extern void PsyX_SPUAL_FeatureStats(unsigned long long* reverb_cycles, unsigned long long* reverb_in,
                                    unsigned long long* noise, unsigned long long* pmon,
                                    unsigned long long* sweep);
extern void PsyX_SPUAL_HostStats(unsigned long long* plays, unsigned long long* clip_frames,
                                 unsigned long long* stream_frames);

/** @brief Log the SPU's feature engagement and host-input counters, then the device feed's counters (frames pushed, held and consumed, device periods, underruns, primes, the depth target and percentiles, the rate estimate), its frame accounting, and its per-phase counters, at exit. */
static void audio_feed_exit_summary(void) {
    unsigned long long rc = 0, ri = 0, nz = 0, pm = 0, sw = 0, hp = 0, hf = 0, hs = 0;
    PsyX_SPUAL_FeatureStats(&rc, &ri, &nz, &pm, &sw);
    PsyX_SPUAL_HostStats(&hp, &hf, &hs);
    OPL_INFO_F("audio", "spu: reverb_cycles=%llu reverb_in=%llu noise=%llu pmon=%llu sweep=%llu "
               "host_plays=%llu host_clip_frames=%llu host_stream_frames=%llu",
               rc, ri, nz, pm, sw, hp, hf, hs);
    PsyX_SPUFeed_Stats st;
    PsyX_SPUFeed_GetStats(&st);
    if (!st.pulls && !st.pushed) return;
    OPL_INFO_F("audio", "feed: pushed=%llu held=%llu overflow=%llu consumed=%llu periods=%llu "
               "underruns=%llu primes=%llu target=%d depth_min=%d p01=%d p50=%d p99=%d "
               "ratio=%.5f rate=%.5f",
               st.pushed, st.held, st.overflow, st.consumed, st.pulls, st.underruns, st.primes,
               st.target, st.depth_min, st.depth_p01, st.depth_p50, st.depth_p99,
               st.ratio, st.rate);
    OPL_INFO_F("audio", "feed: accounting: pushed=%llu = taken=%llu + overflow=%llu + discarded=%llu "
               "+ fifo=%u (residual %lld); consumed=%llu = data_out=%llu + silence_out=%llu",
               st.pushed, st.taken, st.overflow, st.discarded, st.fifo,
               (long long)(st.pushed - st.taken - st.overflow - st.discarded - st.fifo),
               st.consumed, st.data_out, st.silence_out);
    for (int i = 0; i < PSYX_SPUFEED_PHASES; i++) {
        const PsyX_SPUFeed_PhaseStats* p = &st.phase[i];
        if (!p->pushed && !p->held) continue;
        OPL_INFO_F("audio", "feed: phase %s: pushed=%llu held=%llu overflow=%llu underruns=%llu primes=%llu",
                   st.phase_name[i][0] ? st.phase_name[i] : "?", p->pushed, p->held, p->overflow,
                   p->underruns, p->primes);
    }
}

/* The --debug-triangle infra-triangle (core/renderer/debug_render.cpp). */
extern void openpete_debug_triangle_init(void);
extern void openpete_debug_triangle_emit(void);

/* PsyCross internal: set non-zero to bypass SDL_GL_SetSwapInterval at the
 * start of each scene, which uncaps the GL present and lets the host loop
 * actually run as fast as the pipeline allows. Without this Tab still
 * skips our wall-clock pacer, but GL vsync re-locks us to monitor refresh. */
extern int g_skipSwapInterval;

/* PsyCross's GL window - created by PsyX_Initialise. We need it so --headless
 * can hide the window post-init (we keep a real GL driver for the context;
 * SDL's offscreen driver gives back a context with NULL function pointers
 * and we segfault on the first gl* call from GR_InitialisePSX). */
extern SDL_Window* g_window;

/** @brief Write the resolved [keys.pad] bindings into PsyCross's g_cfg_keyboardMapping and g_cfg_controllerMapping.
 *
 * PsyCross reads both exported structs on every internal pad update and
 * fills them only from its hardcoded table in PsyX_Sys_InitialiseInput, so
 * the boot stamp runs right after PsyX_Initialise; a binding edit restamps
 * on the thread that samples the pad (pad_restamp_if_due), the structs'
 * only reader. Each field holds one keyboard scancode or one gamepad button
 * or axis; the field shape of each action is the keymap's table
 * (openpete_keymap_pad_shape), which the binding page admits by: one key
 * and one pad source per button, a key only for the four WASD stick
 * directions (they share the gc_axis_left_* fields), and one gamepad axis
 * for the four stick axes. A source the shape has no field for is ignored.
 * An unbound field is set to PsyCross's unbound value
 * (SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_INVALID,
 * SDL_GAMEPAD_AXIS_INVALID), so a removed source unbinds live. The stamp
 * covers player 1: players 2-4 decode through the shared controller
 * mapping.
 * @param warn nonzero at boot: warn about ignored sources and [keys.pad2..4]
 *        rows. */
static void stamp_psx_pad_bindings(int warn) {
    /* Every field of both structs, paired with the action that feeds it;
     * SIZE_MAX marks the half an action's shape has no field for. */
    static const struct {
        openpete_action_t action;
        size_t kbd_off;    /* offset into PsyXKeyboardMapping */
        size_t pad_off;    /* offset into PsyXControllerMapping */
        const char* name;
    } map[] = {
#define KM_PAD_FIELD(act, kfield, gfield, nm) \
        { act, offsetof(PsyXKeyboardMapping, kfield), \
               offsetof(PsyXControllerMapping, gfield), nm }
#define KM_PAD_KEY(act, kfield, nm) \
        { act, offsetof(PsyXKeyboardMapping, kfield), SIZE_MAX, nm }
#define KM_PAD_AXIS(act, gfield, nm) \
        { act, SIZE_MAX, offsetof(PsyXControllerMapping, gfield), nm }
        KM_PAD_FIELD(OPK_PAD_CROSS,      kc_cross,      gc_cross,      "cross"),
        KM_PAD_FIELD(OPK_PAD_CIRCLE,     kc_circle,     gc_circle,     "circle"),
        KM_PAD_FIELD(OPK_PAD_SQUARE,     kc_square,     gc_square,     "square"),
        KM_PAD_FIELD(OPK_PAD_TRIANGLE,   kc_triangle,   gc_triangle,   "triangle"),
        KM_PAD_FIELD(OPK_PAD_L1,         kc_l1,         gc_l1,         "l1"),
        KM_PAD_FIELD(OPK_PAD_L2,         kc_l2,         gc_l2,         "l2"),
        KM_PAD_FIELD(OPK_PAD_L3,         kc_l3,         gc_l3,         "l3"),
        KM_PAD_FIELD(OPK_PAD_R1,         kc_r1,         gc_r1,         "r1"),
        KM_PAD_FIELD(OPK_PAD_R2,         kc_r2,         gc_r2,         "r2"),
        KM_PAD_FIELD(OPK_PAD_R3,         kc_r3,         gc_r3,         "r3"),
        KM_PAD_FIELD(OPK_PAD_START,      kc_start,      gc_start,      "start"),
        KM_PAD_FIELD(OPK_PAD_SELECT,     kc_select,     gc_select,     "select"),
        KM_PAD_FIELD(OPK_PAD_DPAD_UP,    kc_dpad_up,    gc_dpad_up,    "dpad_up"),
        KM_PAD_FIELD(OPK_PAD_DPAD_DOWN,  kc_dpad_down,  gc_dpad_down,  "dpad_down"),
        KM_PAD_FIELD(OPK_PAD_DPAD_LEFT,  kc_dpad_left,  gc_dpad_left,  "dpad_left"),
        KM_PAD_FIELD(OPK_PAD_DPAD_RIGHT, kc_dpad_right, gc_dpad_right, "dpad_right"),
        KM_PAD_KEY(OPK_PAD_STICK_UP,     kc_axis_left_up,    "stick_up"),
        KM_PAD_KEY(OPK_PAD_STICK_DOWN,   kc_axis_left_down,  "stick_down"),
        KM_PAD_KEY(OPK_PAD_STICK_LEFT,   kc_axis_left_left,  "stick_left"),
        KM_PAD_KEY(OPK_PAD_STICK_RIGHT,  kc_axis_left_right, "stick_right"),
        KM_PAD_AXIS(OPK_PAD_LEFT_X,  gc_axis_left_x,  "left_x"),
        KM_PAD_AXIS(OPK_PAD_LEFT_Y,  gc_axis_left_y,  "left_y"),
        KM_PAD_AXIS(OPK_PAD_RIGHT_X, gc_axis_right_x, "right_x"),
        KM_PAD_AXIS(OPK_PAD_RIGHT_Y, gc_axis_right_y, "right_y"),
#undef KM_PAD_AXIS
#undef KM_PAD_KEY
#undef KM_PAD_FIELD
    };

    unsigned char* kbd = (unsigned char*)&g_cfg_keyboardMapping;
    unsigned char* pad = (unsigned char*)&g_cfg_controllerMapping;

    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        const km_pad_shape_t shape = openpete_keymap_pad_shape(map[i].action);
        km_source_t src[KM_MAX_SOURCES];
        int n = openpete_keymap_sources(map[i].action, src, KM_MAX_SOURCES);
        int have_kbd = 0, have_pad = 0, ignored = 0;

        for (int s = 0; s < n; s++) {
            const int is_key = src[s].kind == KM_SRC_KEY;
            const int fits = is_key ? shape != KM_PADSHAPE_AXIS_ONLY
                           : shape == KM_PADSHAPE_KEY_AND_PAD ||
                             (shape == KM_PADSHAPE_AXIS_ONLY && src[s].kind == KM_SRC_PADAXIS);
            if (!fits) { ignored++; continue; }
            if (is_key) {
                if (have_kbd++) { ignored++; continue; }
                *(int*)(kbd + map[i].kbd_off) = src[s].code;
                continue;
            }
            if (have_pad++) { ignored++; continue; }
            int code = src[s].code;
            if (src[s].kind == KM_SRC_PADAXIS) {
                code |= CONTROLLER_MAP_FLAG_AXIS;
                if (src[s].invert < 0) code |= CONTROLLER_MAP_FLAG_INVERSE;
            }
            *(int*)(pad + map[i].pad_off) = code;
        }
        /* An unbound half must be cleared, not left at PsyCross's default -
         * otherwise `cross = "pad:south"` would silently keep C bound too.
         * The sentinels are PsyCross's own "NONE" values: scancode 0
         * indexes a key that is never down, and -1 (button or axis) reads
         * 0 in GetControllerButtonState. */
        if (!have_kbd && map[i].kbd_off != SIZE_MAX)
            *(int*)(kbd + map[i].kbd_off) = SDL_SCANCODE_UNKNOWN;
        if (!have_pad && map[i].pad_off != SIZE_MAX)
            *(int*)(pad + map[i].pad_off) = shape == KM_PADSHAPE_AXIS_ONLY
                                              ? (int)SDL_GAMEPAD_AXIS_INVALID
                                              : (int)SDL_GAMEPAD_BUTTON_INVALID;

        if (warn && ignored)
            OPL_WARN_F("keys", "pad.%s: PsyCross holds %s; %d source%s ignored",
                       map[i].name,
                       shape == KM_PADSHAPE_KEY_ONLY ? "one keyboard key here"
                       : shape == KM_PADSHAPE_AXIS_ONLY ? "one gamepad axis here"
                       : "one keyboard and one gamepad binding per button",
                       ignored, ignored > 1 ? "s" : "");
    }

    /* Players 2-4 have working pads without any binding here, but their
     * decode still runs the shared controller merge, so a row bound under
     * [keys.pad2..4] does not yet change what that player's buttons do.
     * Only say so if the user actually bound something there. */
    for (int a = OPK_PAD2_CROSS; warn && a < OPK_ACTION_COUNT; a++) {
        km_source_t src[KM_MAX_SOURCES];
        if (openpete_keymap_sources((openpete_action_t)a, src, KM_MAX_SOURCES)) {
            OPL_WARN_F("keys", "[keys.pad2..4] rows are parsed but do not "
                       "re-map that player yet: players 2-4 decode through "
                       "the shared controller mapping");
            break;
        }
    }
}

/** @brief Restamp PsyCross's pad maps when a binding edit changed a player-1 pad action since this thread last stamped.
 *
 * Runs on the thread about to sample the pad (main's pump loop under the
 * thread split, the host loop unsplit and on the legacy path), so the
 * mapping structs have one reader and one writer on one thread. Counts the
 * restamp per thread for the exit summary.
 * @param on_main nonzero on main's pump loop. */
static void pad_restamp_if_due(int on_main) {
    static unsigned s_stamped_main, s_stamped_host;
    unsigned* stamped = on_main ? &s_stamped_main : &s_stamped_host;
    const unsigned gen = openpete_rebind_restamp_generation();
    if (gen == *stamped) return;
    *stamped = gen;
    stamp_psx_pad_bindings(0);
    openpete_rebind_count_restamp(on_main);
    OPL_INFO_F("keys", "pad restamp on %s (generation %u)", on_main ? "main" : "host", gen);
}
#else
/** @brief Without PsyCross there are no pad maps to restamp. */
static void pad_restamp_if_due(int on_main) { (void)on_main; }
#endif

/** @brief Open the sector image behind --disc as the session's disc and attach the disc cache to it.
 *
 * The path may be the cue or the bin; the resolver covers both and the
 * error names every candidate it tried. With --disc-cache the cache starts
 * loading here, before the EXE is read, so every later read of the image
 * (the EXE, PsyCross's libcd and XA, the host paths) goes through it. A
 * second call changes nothing.
 * @param opts The parsed options; disc_path must be set.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR when no readable
 *         image resolves or disc_image refuses it (a 2048-byte image). */
static int open_game_disc(const openpete_options_t* opts) {
    if (disc_image_game()) return OPENPETE_EXIT_OK;
    char bin[4096], tried[4096];
    if (openpete_psx_resolve_disc_bin(opts->disc_path, bin, sizeof bin,
                                      tried, sizeof tried) != 0) {
        OPL_ERROR_F("boot",
                    "--disc %s names no readable disc image (tried: %s). "
                    "Pass the .cue, or the .bin itself.",
                    opts->disc_path, tried[0] ? tried : "(nothing)");
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    disc_image_t* img = disc_image_open(bin, DISC_READ_CACHED, OPENPETE_DISC_TAG_NONE);
    if (!img) return OPENPETE_EXIT_SETUP_ERROR;
    disc_image_set_game(img);
    if (opts->disc_cache) (void)openpete_disc_cache_attach(disc_image_bin_path(img));
    return OPENPETE_EXIT_OK;
}

/** @brief Point PsyCross's CD reads at the session's disc image.
 *
 * Opens the image first when open_game_disc has not, installs the trace
 * observer, and binds PsyCross's image to the handle, which reads through
 * the disc cache when it is attached and the file otherwise.
 * @param opts The parsed options; disc_path must be set.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR when no readable
 *         image resolves or it cannot be bound. */
static int attach_disc_image(const openpete_options_t* opts) {
    int rc = open_game_disc(opts);
    if (rc != OPENPETE_EXIT_OK) return rc;
    openpete_disc_cache_trace_install();
    if (!openpete_disc_psycross_bind(disc_image_game())) {
        OPL_ERROR_F("boot", "--disc %s: the image cannot be bound to PsyCross (larger than 2 GiB)",
                    opts->disc_path);
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    OPL_INFO_F("boot", "PsyCross CD image: %s (%s)", disc_image_bin_path(disc_image_game()),
               openpete_disc_cache_active() ? "disc cache" : "file");
    return OPENPETE_EXIT_OK;
}

/** @brief Log the disc cache's exit census line, with misses per read path, and stop its loader.
 *
 * Logs nothing when the cache was never attached. */
static void disc_cache_exit_census(void) {
    uint64_t m; int64_t w, ln; size_t b;
    openpete_disc_cache_census(&m, &w, &b, &ln);
    if (openpete_disc_cache_active()) {
        char tags[256] = "";
        size_t at = 0;
        for (int t = 0; t < OPENPETE_DISC_TAG_COUNT && at < sizeof tags; t++) {
            uint64_t n = openpete_disc_cache_tag_misses(t);
            if (n) at += (size_t)snprintf(tags + at, sizeof tags - at, " %s=%llu",
                                          openpete_disc_trace_tag_name(t), (unsigned long long)n);
        }
        OPL_INFO_F("cd", "disc cache: %zu MB, loaded in %.2f s, %llu miss(es)%s, %.2f ms read from the file",
                   b >> 20, (double)ln / 1e9, (unsigned long long)m, tags, (double)w / 1e6);
    }
    openpete_disc_cache_shutdown();
}

/** @brief Read the PSX EXE from --exe-path, else from the disc image under the game's EXE filename.
 *
 * The disc route first opens the session's disc (open_game_disc), which
 * attaches the disc cache under --disc-cache.
 * @param opts The parsed options; exe_path takes precedence over disc_path.
 * @param out_bytes Receives a malloc'd copy of the EXE (the caller frees
 *        it), or NULL on failure.
 * @param out_size Receives the EXE size in bytes.
 * @return 0 on success, 1 when neither path is set, the disc image cannot
 *         be opened, or the read fails. */
static int try_load_exe(const openpete_options_t* opts,
                        uint8_t** out_bytes, size_t* out_size) {
    *out_bytes = NULL;
    *out_size  = 0;

    if (opts->exe_path) {
        OPL_INFO_F("boot", "loading EXE from --exe-path: %s", opts->exe_path);
        *out_bytes = openpete_psx_read_file(opts->exe_path, out_size);
        return *out_bytes ? 0 : 1;
    }
    if (opts->disc_path) {
        /* The session's disc, and the cache behind it, open before the EXE
         * read so that read goes through the cache too. */
        if (open_game_disc(opts) != OPENPETE_EXIT_OK) return 1;
        OPL_INFO_F("boot", "loading EXE from --disc: %s (%s)",
                   opts->disc_path, game_get_exe_filename());
        *out_bytes = openpete_psx_load_exe_from_disc(
            opts->disc_path, game_get_exe_filename(), out_size);
        return *out_bytes ? 0 : 1;
    }
    return 1;
}

/** @brief Boot the EXE, dispatch one recompiled function, check its v0, and exit.
 *
 * Runs instead of the host loop when --test-recomp-call or --test-segv is
 * given. Under PsyCross it initialises PsyCross, stamps the pad bindings
 * and attaches the disc image (optional here) first. --test-segv raises
 * SIGSEGV to exercise the crash handler. The entry is --test-recomp-call,
 * else the EXE's entry pc; --test-recomp-gp overrides $gp; --probe logs the
 * word at a PSX address after the call.
 * @param opts The parsed options.
 * @return OPENPETE_EXIT_OK; OPENPETE_EXIT_SETUP_ERROR when no EXE or disc
 *         loads; OPENPETE_EXIT_PANIC on a boot or PsyCross init failure or
 *         an address with no recompiled function; OPENPETE_EXIT_DIVERGENCE
 *         when v0 differs from --test-recomp-expect-v0. */
static int run_recomp_smoke_test(const openpete_options_t* opts) {
    CPUState cpu;
    openpete_psx_runtime_init(&cpu);
    openpete_psx_set_diag_cpu(&cpu);

    uint8_t* bytes = NULL;
    size_t   len   = 0;
    if (try_load_exe(opts, &bytes, &len) != 0) {
        OPL_ERROR_F("boot", "could not load PSX EXE (need --exe-path or --disc)");
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    int rc = openpete_psx_boot(&cpu, bytes, len);
    free(bytes);
    if (rc != 0) return OPENPETE_EXIT_PANIC;

#ifdef OPENPETE_HAS_PSYCROSS
    /* libgpu/libgte calls touch internal SDL/GL state that PsyX_Initialise
     * sets up. For the smoke-test path we still want PsyCross alive. */
    OPL_INFO_F("boot", "PsyX_Initialise: opening %dx%d window",
               game_default_window_w(), game_default_window_h());
    int psyx_ok = PsyX_Initialise((char*)opts->display_name,
                                  game_default_window_w(),
                                  game_default_window_h(),
                                  /*fullscreen=*/0,
                                  /*startHidden=*/opts->headless ? 1 : 0);
    /* --headless still opens a GL context: PsyCross rasterizes the OT for the
     * oracle, so g_psyx_null_gpu stays 0 and SDL_INIT_VIDEO must succeed. A
     * session with no display reachable fails here. Stopping now names the
     * cause; continuing would crash at the first ResetGraph with a null GL
     * proc and no explanation. */
    if (!psyx_ok) {
        OPL_ERROR_F("boot",
                    "PsyCross init failed (SDL: %s). No display is reachable; "
                    "even --headless needs one, because PsyCross rasterizes "
                    "for the oracle. Over ssh, export XDG_RUNTIME_DIR and "
                    "WAYLAND_DISPLAY (or DISPLAY) from the login session.",
                    SDL_GetError());
        return OPENPETE_EXIT_PANIC;
    }

    /* PsyX_Sys_InitialiseInput just wrote its hardcoded pad maps; replace
     * them with the resolved [keys.pad] bindings. */
    stamp_psx_pad_bindings(1);
    apply_psyx_dither_env(opts);

    /* Wire PsyCross's CDFS to the disc image so CdRead has real data.
     * Without --disc CdRead returns failure, which the leaf smoke test
     * tolerates. */
    if (opts->disc_path) {
        int disc_rc = attach_disc_image(opts);
        if (disc_rc != OPENPETE_EXIT_OK) return disc_rc;
    } else {
        OPL_WARN_F("boot", "no --disc; CdRead will return no data");
    }
#endif

    if (opts->test_recomp_segv) {
        OPL_INFO_F("boot", "deliberately raising SIGSEGV to verify crash handler");
        volatile int* p = (int*)(uintptr_t)0;
        *p = 0;
        return OPENPETE_EXIT_PANIC;
    }

    uint32_t entry = opts->test_recomp_call_addr;
    if (entry == 0) entry = cpu.pc;

    if (opts->test_recomp_gp != 0) {
        OPL_INFO_F("boot", "overriding cpu.gp 0x%08X -> 0x%08X (skipping PSYQ _96_init)",
                   cpu.gp, opts->test_recomp_gp);
        cpu.gp = opts->test_recomp_gp;
    }

    OPL_INFO_F("boot", "dispatching recompiled function at 0x%08X", entry);
    int matched = openpete_recomp_module_get()->dispatch(&cpu, entry);
    if (!matched) {
        OPL_ERROR_F("boot", "no recompiled function at 0x%08X", entry);
        return OPENPETE_EXIT_PANIC;
    }
    OPL_INFO_F("boot", "function returned: v0=0x%08X v1=0x%08X", cpu.v0, cpu.v1);

    /* Optional post-dispatch probe: read 4 bytes at a PSX vaddr and log
     * them. Used as a portable exit-criteria assertion (e.g. Spyro
     * confirms `g_WadHeader.m_LevelEntry[0].m_Offset` is non-zero). */
    if (opts->probe_addr != 0) {
        uint8_t* ram = openpete_psx_get_ram();
        uint32_t v = 0;
        memcpy(&v, ram + (opts->probe_addr & 0x1FFFFFFFu), 4);
        OPL_INFO_F("boot", "probe[0x%08X] = 0x%08X", opts->probe_addr, v);
    }

    if (opts->test_recomp_expected_v0 != 0 &&
        cpu.v0 != opts->test_recomp_expected_v0) {
        OPL_ERROR_F("boot",
                    "v0 mismatch: got=0x%08X expected=0x%08X",
                    cpu.v0, opts->test_recomp_expected_v0);
        return OPENPETE_EXIT_DIVERGENCE;
    }
    fprintf(stdout, "openpete-exit: reason=recomp-smoke-test v0=0x%08X code=0\n",
            cpu.v0);
    return OPENPETE_EXIT_OK;
}

#ifdef OPENPETE_HAS_PSYCROSS
/* Main-loop driver: dispatch the recompiled function at
 * `opts->run_recomp_addr` on a dedicated fiber. PSYQ thunks (psyq_VSync) yield
 * back to the host fiber on VSync(0); the host ends the current PsyCross scene,
 * advances the frame counter, runs exit checks, begins the next scene, and
 * resumes the game. Returns when frame_limit/runtime exits or the dispatched
 * function returns (which `main()` shouldn't, but guards against premature
 * returns from any other entry point we point this at). */

/* Game fiber state. Static rather than capture so the libco trampoline (which
 * takes no args) can find it. */
static struct {
    CPUState cpu;
    uint32_t entry;
    uint32_t gp;
    int      done;
    int      dispatch_ok;
} g_game;

/* ---- eager production --------------------------------------------------
 *
 * The pull model presented one scene at a time, extracting it on the sim
 * thread at the moment the pacer decided it was due -- and refused the
 * slots that fell inside the tick's protective margin, because extracting
 * and executing the guest share this thread. Nothing forces extraction to
 * happen at present time. Every present between tick N and tick N+1
 * interpolates the span [N-1, N], whose both endpoints are already in the
 * interp ring the moment tick N lands; so at that moment the whole span is
 * extractable, and this thread is about to be idle for most of a tick.
 *
 * So: at the canonical tick, PLAN which grid instants of the coming span
 * get a scene; then PRODUCE them one at a time whenever this thread is
 * idle, publishing each into the scene ring for the present thread, which
 * shows them on their instants -- through the next tick's execution. The
 * deadline check sits BETWEEN scenes, never inside one, so the guest never
 * resumes on interpolated RAM: production is scene-atomic. A scene whose
 * instant has already passed is skipped (except the canonical, which also
 * carries the tick boundary for mods).
 *
 * Running out of CPU inverts the failure mode. The pull model found out at
 * the boundary, one present at a time, and lost a contiguous run at the END
 * of the span. Here the budget (span length minus what this thread must
 * still do in it) and the per-scene cost are known before anything is
 * produced, so the shape of the degradation is chosen up front: a divisor
 * ladder (every 2nd slot, every 3rd ...) keeps the cadence uniform on a
 * fixed refresh. The trigger is a division, not a threshold on a curve.
 * Hysteresis is asymmetric -- drop a level the moment a span is cut short,
 * climb only after several consecutive spans with clear headroom -- because
 * a rate that oscillates is worse than a steady lower one.
 *
 * The lag. The spine shows span [N-1, N] from the moment tick N lands, at
 * alpha = (t - stamp_N) / period, so alpha reaches 1 when tick N+1 is DUE --
 * and tick N+1 does not land until it has executed, ~6 ms later. The slots
 * inside that execution sit at alpha 1.0-1.2: they need pose N+1, which does
 * not exist yet. No amount of extraction ahead of time fills them; the
 * timeline itself is one tick-execution too short. So an eager scene for
 * display instant t animates the instant t - lag, where lag tracks the tail
 * of the tick's execution time: every slot through the execution then sits
 * at alpha < 1 of a span whose endpoints are both known, no extrapolation,
 * and the price is a CONSTANT few milliseconds of display latency in place
 * of a hold of the same length thirty times a second.
 *
 * OPENPETE_EAGER=0 disables (on by default). OPENPETE_EAGER_POLICY=greedy
 * swaps the divisor ladder for "as many scenes as fit, evenly spaced across
 * the span" -- the A/B partner for the question whether uniform-on-grid
 * beats more-frames-off-grid. OPENPETE_EAGER_LEVEL=n pins the divisor;
 * OPENPETE_EAGER_LAG_US=n pins the lag. */
#define EAGER_MAX_TARGETS 128
#define EAGER_COST_WARMUP 8
#define EAGER_HEADROOM_RUNS 8
static struct {
    int       active;                 /* the present thread owns the display */
    int       n, i;                   /* planned targets, next to produce */
    op_time_t target[EAGER_MAX_TARGETS];
    int       level;                  /* divisor: produce every level-th slot */
    int       pinned;                 /* OPENPETE_EAGER_LEVEL */
    int       afford_pin;             /* OPENPETE_EAGER_AFFORD: force the budget's verdict (A/B) */
    int       policy;                 /* 0 divisor, 1 greedy */
    int       headroom_runs;
    op_time_t cost_ema, cost_dev; int cost_n;   /* per-scene production cost */
    op_time_t vbl_ema;                /* cost of a non-drawing VBL on this thread */
    op_time_t tick_ema, tick_dev;     /* execution time of a drawing VBL (the tick) */
    op_time_t tick_peak;              /* decaying peak of the tick's execution time */
    op_time_t lag;                    /* animation lag behind the display instant */
    op_time_t lag_pin;                /* OPENPETE_EAGER_LAG_US */
    int       lag_headroom;
    int       dbg_level;
    unsigned  trimmed;                /* spans whose render-grid slot was pulled back to within a period of now */
    op_time_t anchor_age;             /* now - the span anchor at the last plan: 0 when anchored on now */
    op_time_t last_target;            /* the previous span's final display instant */
    int       world_live;             /* canonical presents are flowing (see the inline gate) */
    unsigned  spans, produced, skipped_late, cut, refused, level_up, level_down;
    unsigned  produced_inter;         /* of `produced`, the sub-tick re-extracts */
    int       span_cut;               /* this span ended with targets unproduced */
} s_eager = { .level = 1 };

/* Sub-tick re-extracts driven by the host loop's inline intermediate present,
 * the path taken whenever eager production is not active. Counted beside
 * s_eager.produced_inter so one exit line states how many presents in a run
 * were built from interpolated gamestate rather than from a tick. */
static unsigned s_inline_rex = 0;
/** @brief Read the OPENPETE_EAGER_* tuning variables into s_eager, once.
 *
 * OPENPETE_EAGER_POLICY=greedy selects policy 1 (evenly spaced targets),
 * else policy 0 (every level-th grid slot). OPENPETE_EAGER_LEVEL, when
 * nonzero, disables the adaptive level ladder, and a positive value also
 * sets the level. OPENPETE_EAGER_AFFORD pins the per-span scene budget and
 * OPENPETE_EAGER_LAG_US (microseconds) pins the lag. Later calls do nothing. */
static void eager_init(void)
{
    static int inited;
    if (inited) return;
    inited = 1;
    const char *p = getenv("OPENPETE_EAGER_POLICY");
    s_eager.policy = (p && !strcmp(p, "greedy")) ? 1 : 0;
    const char *l = getenv("OPENPETE_EAGER_LEVEL");
    s_eager.pinned = (l && *l) ? atoi(l) : 0;
    if (s_eager.pinned > 0) s_eager.level = s_eager.pinned;
    const char *f = getenv("OPENPETE_EAGER_AFFORD");
    s_eager.afford_pin = (f && *f) ? atoi(f) : 0;
    const char *g = getenv("OPENPETE_EAGER_LAG_US");
    s_eager.lag_pin = (g && *g) ? (op_time_t)atoll(g) * 1000 : 0;
}

/** @brief Return the first instant of the lattice v(n) = base + n * period at or after t.
 *
 * An instant up to a millionth of a period before t counts as at t, so a t
 * already on the lattice does not round up a whole period.
 * @param t Time on the op_now() clock.
 * @param base Time of lattice instant 0.
 * @param period Lattice period in nanoseconds.
 * @return The lattice instant in nanoseconds. */
static op_time_t eager_lattice_ceil(op_time_t t, int64_t base, double period)
{
    const double n = ceil((double)(t - base) / period - 1e-6);
    return base + (op_time_t)(n * period + 0.5);
}

/** @brief Return the tail bound of one eager scene's production cost.
 *
 * The mean plus three deviations of the per-scene production time that
 * eager_produce_one measures. eager_produce stops before a scene whose tail
 * cost would pass its deadline, and eager_plan floors the render-grid
 * period at it when presenting is uncapped.
 * @return The bound in nanoseconds, or 0 before EAGER_COST_WARMUP scenes
 *         have been measured. */
static op_time_t eager_tail_cost(void)
{
    return s_eager.cost_n >= EAGER_COST_WARMUP
        ? s_eager.cost_ema + 3 * s_eager.cost_dev : 0;
}

/** @brief Record one intermediate scene the eager producer dropped as a SHED record.
 *
 * The record is the one the inline path writes for a present it does not
 * make: kind OP_PACE_INTERMEDIATE, the per-scene tail cost as the predicted
 * cost, and the time left before the scene's deadline as the margin.
 * @param reason PACE_SHED_NO_BUDGET or PACE_SHED_INSTANT_PASSED.
 * @param margin_ns Time left before the deadline; negative once it passed. */
static void eager_shed(uint16_t reason, op_time_t margin_ns)
{
    openpete_pace_shed(OP_PACE_INTERMEDIATE, reason,
                       (int64_t)eager_tail_cost(), (int64_t)margin_ns);
}

/** @brief Close the previous span: count it cut when targets were left unproduced and shed each one.
 *
 * Every target from s_eager.i to s_eager.n is an intermediate scene (the
 * canonical scene, index 0, is produced when its span is planned) that the
 * produce guard held back for the deadline or a full scene ring, and the new
 * plan discards it. Each is written as one PACE_SHED_NO_BUDGET record while
 * the world is live; while it is not, production was withheld for the
 * liveness gate and the display holds the last image, which is not a shed.
 * Sets span_cut for the plan's level ladder. */
static void eager_close_span(void)
{
    if (!s_eager.n || s_eager.i >= s_eager.n) return;
    s_eager.cut++;
    s_eager.span_cut = 1;
    if (!s_eager.world_live) return;
    for (int q = s_eager.i; q < s_eager.n; q++) eager_shed(PACE_SHED_NO_BUDGET, 0);
}

/** @brief Shed the display-grid slots of a new span that the plan leaves without a scene for cost.
 *
 * Two plan verdicts leave a slot of the display grid unproduced because
 * scenes cost more than the span affords: the cost floor that coarsens the
 * grid period to the scene or present-thread cost, and the decimation level
 * (or the greedy policy's afford) that targets fewer slots than the grid
 * holds. Each such slot is the eager form of the inline present the host
 * loop sheds with PACE_SHED_NO_BUDGET, and is written as one. A level pinned
 * by OPENPETE_EAGER_LEVEL under the divisor policy is a configuration, not a
 * cost verdict, and sheds nothing; nor does any slot while the world is not
 * live, when no scene is produced and the display holds the last image.
 * @param first The span's first display instant.
 * @param span_end The span's last admissible display instant.
 * @param grid_period The display grid period before the cost floor.
 * @param period The planned grid period, at least grid_period.
 * @param ns The number of slots enumerated at period. */
static void eager_shed_unplanned(op_time_t first, op_time_t span_end,
                                 op_time_t grid_period, op_time_t period, int ns)
{
    if (!s_eager.world_live) return;
    int grid_slots = ns;
    if (grid_period > 0 && grid_period < period && span_end >= first) {
        const op_time_t g = (span_end - first) / grid_period + 1;
        grid_slots = g < EAGER_MAX_TARGETS ? (int)g : EAGER_MAX_TARGETS;
    }
    const int ladder = s_eager.pinned == 0 || s_eager.policy == 1;
    const int planned = ladder ? s_eager.n : ns;
    for (int q = planned; q < grid_slots; q++) eager_shed(PACE_SHED_NO_BUDGET, 0);
}

/* Room for the canonical scene between its plan and its vblank: production
 * (~0.25 ms), compose (0.3 ms on a light scene, 1.2 ms on a 48k-prim vista)
 * and the driver's own lead to the flip (a submit 0.33 ms before a vblank
 * still made it; one 0.13 ms before did not). */
#define EAGER_PUBLISH_LEAD_NS 2000000
/* The present thread's fitted vblank lattice, when targets go on it. */
typedef struct { int on; int64_t base; double period; } eager_lattice_t;
/* One span's display instants, as eager_span_slots enumerates them. */
typedef struct { op_time_t first, end; int ns; op_time_t slots[EAGER_MAX_TARGETS]; } eager_span_t;

/** @brief Enumerate the display instants of one span from its first render-grid slot.
 *
 * Raises span_end to first, puts first on the vblank lattice (no earlier than
 * EAGER_PUBLISH_LEAD_NS past now) when one is fitted, extends span_end by the
 * lag and, when the previous span's last target plus a period lies past
 * first, continues on the previous span's grid (unless it drifted a whole
 * period from this one). The slots step period from first through span_end;
 * a span holds at least the slot at first. Reads s_eager.lag and
 * s_eager.last_target and writes nothing else.
 * @param now The plan instant.
 * @param first The span's first render-grid slot.
 * @param span_end The instant the next tick lands.
 * @param period The planned grid period.
 * @param lat The vblank lattice.
 * @param out Receives first, the extended span_end, the slot count and the slots. */
static void eager_span_slots(op_time_t now, op_time_t first, op_time_t span_end,
                             op_time_t period, const eager_lattice_t *lat, eager_span_t *out)
{
    if (span_end < first) span_end = first;
    if (lat->on) {
        /* The canonical scene is produced right after this plan and
         * shown at once; its vblank must leave room for that. */
        op_time_t f = now + EAGER_PUBLISH_LEAD_NS;
        if (f < first) f = first;
        first = eager_lattice_ceil(f, lat->base, lat->period);
    }
    /* Display instants whose animation instant (t - lag) is inside the span.
     * The previous span already covers the instants up to its last target
     * (that is what the lag is for: its late scenes show through this tick's
     * execution), so this span begins at the slot after it -- a scene for an
     * instant the old span covers would sit at negative alpha of the new
     * bracket, a backward extrapolation the old bracket renders exactly. */
    /* The extension below moves the SLOT ENUMERATION past the instant
     * production stops at, which is correct - those late instants are shown
     * through the next tick's execution - but it is not time in which to
     * produce them. eager_plan's `prod_end` is that separate quantity. */
    span_end += s_eager.lag;
    if (s_eager.last_target && s_eager.last_target + period > first) {
        op_time_t f = s_eager.last_target + period;
        /* the old span's grid, unless it drifted a whole slot from ours */
        if (f - first > period) f = first + ((f - first) / period) * period;
        /* re-snapped: the lattice fit moves by microseconds between spans */
        if (lat->on) f = eager_lattice_ceil(f - period / 2, lat->base, lat->period);
        first = f;
    }
    int ns = 0;
    for (op_time_t t = first; t <= span_end && ns < EAGER_MAX_TARGETS; t += period) out->slots[ns++] = t;
    if (ns == 0) out->slots[ns++] = first;
    out->first = first; out->end = span_end; out->ns = ns;
}

/** @brief Write eager_plan's OPENPETE_EAGER_DBG lines for the span just planned.
 *
 * OPENPETE_EAGER_DBG=1 writes the plan line on a cut span or a level change.
 * 2 or more also writes one span line per plan: the offsets from now of the
 * render-grid slot passed in (in), the span's first display instant, its end
 * after the lag extension and the previous span's last target, the lag, the
 * anchor source (cap: the newest capture stamp, now: the plan instant) with
 * the capture stamp's age, the slot count, the level and the count of spans
 * whose slot was pulled back. 3 or more also writes the slot list beside the
 * one the untrimmed slot and the caller-side span_end clamp enumerate, and
 * whether the two are equal. Runs before the plan stores its targets, so
 * last_target is still the previous span's.
 * @param now The plan instant.
 * @param first_in The render-grid slot eager_plan was called with.
 * @param end_in The span_end eager_plan was called with.
 * @param period The planned grid period.
 * @param lat The vblank lattice.
 * @param sp The planned span.
 * @param budget The production budget in nanoseconds.
 * @param afford The number of scenes the budget affords. */
static void eager_plan_dbg(op_time_t now, op_time_t first_in, op_time_t end_in, op_time_t period,
                           const eager_lattice_t *lat, const eager_span_t *sp,
                           op_time_t budget, int afford)
{
    static int dbg = -1;
    if (dbg < 0) { const char *e = getenv("OPENPETE_EAGER_DBG"); dbg = (e && *e) ? atoi(e) : 0; }
    if (dbg <= 0) return;
    if (s_eager.span_cut || s_eager.level != s_eager.dbg_level) {
        fprintf(stderr, "[eager f=%u] plan: slots=%d budget=%.2fms tail=%.3fms afford=%d level=%d cut=%d lag=%.2fms vbl=%.2fms\n",
                openpete_frame, sp->ns, (double)budget / 1e6, (double)eager_tail_cost() / 1e6, afford,
                s_eager.level, s_eager.span_cut, (double)s_eager.lag / 1e6, (double)s_eager.vbl_ema / 1e6);
        s_eager.dbg_level = s_eager.level;
    }
    if (dbg < 2) return;
    const double last = s_eager.last_target ? (double)(s_eager.last_target - now) / 1e6 : 0.0;
    const op_time_t curr = openpete_interp_curr_tick_ns();
    const double age = curr ? (double)(now - curr) / 1e6 : -1.0;
    fprintf(stderr, "[eager f=%u] span: now=%.6f in=%.3f first=%.3f end=%.3f last=%.3f lag=%.3f anchor=%s age=%.3f ns=%d level=%d trimmed=%u\n",
            openpete_frame, (double)now / 1e9, (double)(first_in - now) / 1e6,
            (double)(sp->first - now) / 1e6, (double)(sp->end - now) / 1e6, last,
            (double)s_eager.lag / 1e6, s_eager.anchor_age ? "cap" : "now",
            age, sp->ns, s_eager.level, s_eager.trimmed);
    if (dbg < 3) return;
    eager_span_t old;
    eager_span_slots(now, first_in, end_in > first_in ? end_in : first_in, period, lat, &old);
    const int same = old.ns == sp->ns && old.first == sp->first && old.end == sp->end &&
                     memcmp(old.slots, sp->slots, sizeof(op_time_t) * (size_t)sp->ns) == 0;
    char buf[1024]; int o = snprintf(buf, sizeof(buf), "[eager f=%u] slots: same=%d new", openpete_frame, same);
    for (int q = 0; q < sp->ns && o < 900; q++) o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %.3f", (double)(sp->slots[q] - now) / 1e6);
    o += snprintf(buf + o, sizeof(buf) - (size_t)o, " | old");
    for (int q = 0; q < old.ns && o < 1000; q++) o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %.3f", (double)(old.slots[q] - now) / 1e6);
    fprintf(stderr, "%s\n", buf);
}

/** @brief Plan the display instants of the span that starts now and adjust the decimation level.
 *
 * Counts the previous span as cut when it left targets unproduced. Pulls
 * first back by whole render periods to the grid's earliest instant at or
 * after now, counting each span it pulls back in s_eager.trimmed. Moves the
 * lag (unless pinned) up to the decaying tick-time peak plus 0.3 ms at once
 * and down only after 30 spans of headroom. Chooses the grid: on the
 * present thread's locked lattice a whole number of refreshes per render
 * slot with the first target on a vblank; otherwise, on a vsynced display
 * slower than the render grid, the refresh period; the period never falls
 * below the scene production cost, the present thread's cost, or 250 us
 * before the cost is measured. Enumerates the slots with eager_span_slots
 * (span_end raised to first and extended by the lag, continuing after the
 * previous span's last target). Estimates how many scenes fit
 * before prod_end (afford), raises the level at once when the slots need it
 * or the last span was cut, and lowers it after a run of spans with a
 * quarter margin. Stores every level-th slot (policy 0) or afford evenly
 * spaced instants (policy 1) in s_eager.target, sets last_target and counts
 * the span.
 * @param now Current time on the op_now() clock.
 * @param first A render-grid instant at or after now.
 * @param period The render grid period in nanoseconds.
 * @param span_end The instant the next tick lands (alpha reaches 1).
 * @param prod_end The pacer grid instant production must finish before. */
static void eager_plan(op_time_t now, op_time_t first, op_time_t period,
                       op_time_t span_end, op_time_t prod_end)
{
    eager_close_span();   /* the previous span's books first */
    /* Precondition check: under eager the host loop keeps the render grid's
     * next slot within one period of now, so this trims nothing. A slot that
     * led by more would leave this tick's span one slot, or none past the
     * clamp to it; its earliest instant at or after now, on the same phase,
     * is the span's first, and s_eager.trimmed counts the breach. */
    const op_time_t first_in = first;
    if (first - now > period) {
        first -= ((first - now) / period) * period;
        s_eager.trimmed++;
    }
    /* The lag follows the tick's execution tail: up at once (a slot at
     * alpha > 1 is a lost frame), down only after a second of headroom (a
     * lag that flickers warps animation time). */
    if (s_eager.lag_pin > 0) s_eager.lag = s_eager.lag_pin;
    else {
        /* The peak over roughly the last second, decaying toward the centre:
         * a tail bound that a rare long tick raises at once and that lets go
         * of it again, unlike a deviation term that a bimodal tick inflates
         * for good. */
        s_eager.tick_peak -= (s_eager.tick_peak - s_eager.tick_ema) / 32;
        const op_time_t want = s_eager.tick_peak + 300000;
        if (want > s_eager.lag) { s_eager.lag = want; s_eager.lag_headroom = 0; }
        else if (++s_eager.lag_headroom >= 30) { s_eager.lag = want; s_eager.lag_headroom = 0; }
    }
    /* Under FIFO the display shows at most one image per refresh, and the
     * window may sit on a slower monitor than --render-fps was chosen for
     * (480 on a 60 Hz panel): a scene per grid slot would be produced 8x
     * faster than any can be shown and superseded unpresented. Plan on the
     * coarser of the two grids, phase-preserving. */
    eager_lattice_t lat = { 0, 0, 0.0 };
    op_time_t grid_period;            /* the display grid before the cost floor */
    lat.on = openpete_native_display_vsync() &&
             openpete_present_thread_lattice(&lat.base, &lat.period);
    {
        const float hz = openpete_native_display_refresh_hz();
        if (lat.on) {
            /* The present thread has fitted the display's vblank lattice
             * from observed flips: targets go ON it, a whole number of
             * refreshes apart, so each target is a vblank and the
             * animation instant (target - lag) is exact. The nominal mode
             * rate is not good enough for this -- it was 2e-5 off the
             * measured one, a full slot of phase every ~50 s. */
            const op_time_t lp = (op_time_t)(lat.period + 0.5);
            op_time_t k = (period + lp / 2) / lp;             /* refreshes per render slot */
            if (k < 1) k = 1;
            period = (op_time_t)(lat.period * (double)k + 0.5);
        } else
        if (hz > 1.0f && openpete_native_display_vsync()) {
            const op_time_t rp = (op_time_t)(1e9 / hz);
            if (rp > period + period / 8) {
                const op_time_t k = (rp + period / 2) / period;   /* whole slots per refresh */
                if (k > 1) period *= k;
            }
        }
        /* Uncapped presenting (vsync off, --render-fps far above the display):
         * targets denser than scenes can be made would plan a span the ring
         * cannot hold and the burst cannot finish. Floor the grid at the
         * production cost; the ring's back-pressure below does the rest, and
         * the present thread shows the newest scene at whatever rate the GPU
         * allows. */
        grid_period = period;
        op_time_t floor_ns = eager_tail_cost() ? eager_tail_cost() : 250000;
        const op_time_t pc = openpete_present_thread_cost_ns();
        if (pc > floor_ns) floor_ns = pc;
        if (period < floor_ns) period = floor_ns;
    }
    eager_span_t sp;
    eager_span_slots(now, first, span_end, period, &lat, &sp);
    const int ns = sp.ns;

    /* Budget on the tail of the per-scene cost, against what this thread
     * has left of the span once the update VBL has run.
     *
     * Against `prod_end` (the pacer grid instant the produce deadlines are
     * derived from), never the span endpoint. The span is anchored on the
     * capture stamp, which PLL-locks to average tick completion, so it lands
     * about one tick execution past the moment production actually stops;
     * the lag extension above adds more. Budgeting against either prices
     * time this thread never has, so `afford` would exceed the slot count
     * even at a level that cannot keep up, the proactive step below would
     * never fire, and the level would fall only after a span was cut short,
     * which is the oscillation the divisor ladder exists to prevent. */
    /* How many scenes fit is a question about their SUM, not about one of
     * them. `eager_tail_cost()` is a per-scene p99.7 bound, which is the right
     * question between scenes (the produce guard below) and the wrong one
     * here: pricing all N at once at p99.7 charges every scene the worst
     * scene's cost, ~2x the true sum, since independent costs average out.
     * The sum's own spread grows as sqrt(N), so bound it that way -- with
     * N <= EAGER_MAX_TARGETS the sqrt term is a constant. Being wrong is also
     * cheap in one direction only: the produce guard still stops at the
     * deadline, and a cut span is observed and answered by the ladder. */
    const op_time_t tail = eager_tail_cost();
    op_time_t budget = prod_end - now - s_eager.vbl_ema - 1000000;   /* 1 ms safety */
    if (budget < 0) budget = 0;
    int afford;
    if (s_eager.cost_n >= EAGER_COST_WARMUP && s_eager.cost_ema > 0) {
        const op_time_t spread = 8 * s_eager.cost_dev;      /* 2 sigma at N = 16 */
        op_time_t room = budget > spread ? budget - spread : 0;
        afford = (int)(room / s_eager.cost_ema);
    } else {
        afford = tail > 0 ? (int)(budget / tail) : ns;
    }
    if (s_eager.afford_pin > 0) afford = s_eager.afford_pin;   /* a forced budget, for the policy A/B */
    if (afford < 1) afford = 1;

    if (!s_eager.pinned) {
        int need = (ns + afford - 1) / afford;      /* ceil(ns / afford) */
        if (need < 1) need = 1;
        if (s_eager.span_cut && need <= s_eager.level) need = s_eager.level + 1;
        if (need > s_eager.level) {
            s_eager.level = need; s_eager.level_down++; s_eager.headroom_runs = 0;
        } else if (s_eager.level > 1) {
            /* Climb only with a quarter margin over the next level's demand,
             * held for several spans: a level that flickers judders the RATE. */
            const int next_need = (ns + s_eager.level - 2) / (s_eager.level - 1);
            if (afford * 4 >= next_need * 5) {
                if (++s_eager.headroom_runs >= EAGER_HEADROOM_RUNS) {
                    s_eager.level--; s_eager.level_up++; s_eager.headroom_runs = 0;
                }
            } else s_eager.headroom_runs = 0;
        }
    }
    eager_plan_dbg(now, first_in, span_end, period, &lat, &sp, budget, afford);
    s_eager.span_cut = 0;

    s_eager.n = 0; s_eager.i = 0;
    if (s_eager.policy == 1) {
        int k = afford < ns ? afford : ns;
        const op_time_t len = sp.end - sp.first;
        for (int q = 0; q < k; q++) s_eager.target[s_eager.n++] = sp.first + (len * q) / k;
    } else {
        for (int q = 0; q < ns; q += s_eager.level) s_eager.target[s_eager.n++] = sp.slots[q];
    }
    s_eager.last_target = s_eager.n ? s_eager.target[s_eager.n - 1] : 0;
    eager_shed_unplanned(sp.first, sp.end, grid_period, period, ns);
    s_eager.spans++;
}

/** @brief Extract one scene for display instant t and publish it into the scene ring.
 *
 * Syncs the VRAM mirror (the guest is idle), sets the interpolation present
 * target to the animation instant t - lag, runs the re-extract, flushes the
 * mods' present work and publishes the scene tagged with t, its kind and
 * alpha; a published scene kicks the present thread, a refused one counts
 * as refused. The canonical scene, first of the span, also raises
 * g_openpete_canon_lag_leg for the re-extract. An intermediate scene first
 * spends the OPENPETE_PACE_INJECT_PRESENT_US fault injection (none unless
 * set) inside the measured span. The scene's production time updates the
 * cost mean and deviation that eager_tail_cost reads and is logged as its
 * PRESENT_SPAN.
 * @param t Display instant on the op_now() clock.
 * @param canonical Nonzero for the span's canonical scene. */
static void eager_produce_one(op_time_t t, int canonical)
{
    extern int g_openpete_canon_lag_leg;
    openpete_native_reextract_fn re = openpete_native_get_reextract();
    const op_time_t t0 = op_now();
    if (!canonical) openpete_pace_inject_present();   /* fault injection, off by default */
    uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
    openpete_native_vram_sync();                 /* guest idle: the mirror cannot tear */
    openpete_interp_set_present_target(t - s_eager.lag);   /* the animation instant this display instant shows */
    if (canonical) g_openpete_canon_lag_leg = 1;
    openpete_native_scene_eager_begin();
    const op_time_t rex0 = op_now();
    re(&g_game.cpu);
    const op_time_t rex_ns = op_now() - rex0;
    openpete_native_scene_sig_report(canonical);
    g_openpete_canon_lag_leg = 0;
    openpete_mods_present_flush(&g_game.cpu, canonical);
    openpete_interp_set_present_target(0);
    const int kind = canonical ? OP_PACE_CANONICAL : OP_PACE_INTERMEDIATE;
    int paired = 0;
    const double alpha = openpete_pace_extract_alpha(&paired);
    const int slot = openpete_native_scene_publish(t, kind, alpha, paired);
    if (slot >= 0) {
        openpete_present_thread_kick();
        s_eager.produced++;
        if (!canonical) s_eager.produced_inter++;
    }
    else s_eager.refused++;
    if (pt0) openpete_phase_add(OPPT_REX_TOTAL, openpete_phase_now_us() - pt0);
    if (op_prof_on())
        op_prof_present((uint32_t)openpete_canon_tick,
                        canonical ? OP_PROF_CANONICAL : OP_PROF_INTERMEDIATE);
    const op_time_t span = op_now() - t0;
    openpete_pace_present_span((uint8_t)kind, t0, t0 + span, rex_ns);
    if (s_eager.cost_n) {
        const op_time_t d = span > s_eager.cost_ema ? span - s_eager.cost_ema : s_eager.cost_ema - span;
        /* Slower than the mean: the deviation feeds the plan's capacity
         * verdict, and an 8-sample window is half a tick - short enough that a
         * single costly scene moves the level and starts a limit cycle. */
        s_eager.cost_dev = (s_eager.cost_dev * 31 + d) / 32;
    }
    s_eager.cost_ema = s_eager.cost_n ? (s_eager.cost_ema * 7 + span) / 8 : span;
    if (s_eager.cost_n < EAGER_COST_WARMUP) s_eager.cost_n++;
}

/** @brief Produce the span's planned scenes in order while they fit before deadline.
 *
 * The canonical scene, first of the span, is always produced. A later scene
 * is skipped, and recorded as a PACE_SHED_INSTANT_PASSED shed, when its
 * instant is more than half a period past; the walk
 * stops when the scene ring has one free slot or less, or when the tail
 * cost of one more scene would pass the deadline, and resumes from the same
 * scene on the next call. The checks run between scenes only.
 * @param deadline Time by which production must stop.
 * @param period Render-grid period in nanoseconds.
 * @return The number of scenes produced. */
static int eager_produce(op_time_t deadline, op_time_t period)
{
    int made = 0;
    const op_time_t tail = eager_tail_cost();
    while (s_eager.i < s_eager.n) {
        const op_time_t now = op_now();
        const op_time_t t = s_eager.target[s_eager.i];
        const int canonical = s_eager.i == 0;
        if (!canonical && t < now - period / 2) {   /* its instant is gone */
            eager_shed(PACE_SHED_INSTANT_PASSED, t - now);
            s_eager.i++; s_eager.skipped_late++; continue;
        }
        if (!canonical && openpete_native_scene_ring_free() <= 1) break;   /* ring full: resume next iteration */
        if (!canonical && now + tail > deadline) {
            static int dbg = -1;
            if (dbg < 0) { const char *e = getenv("OPENPETE_EAGER_DBG"); dbg = (e && *e && *e != '0'); }
            if (dbg) fprintf(stderr, "[eager f=%u] stop: %d/%d produced, deadline in %.2fms, tail %.3fms\n",
                             openpete_frame, s_eager.i, s_eager.n, (double)(deadline - now) / 1e6, (double)tail / 1e6);
            break;
        }
        eager_produce_one(t, canonical);
        s_eager.i++; made++;
    }
    return made;
}

/** @brief Log the eager-production totals at shutdown.
 *
 * One `eager` info line: spans, scenes produced, skipped because their
 * instant passed, refused because the ring was full, and cut short; the
 * divisor level and its moves (shown as n/a under the greedy policy, where
 * the level does not decide the plan); scene cost mean and deviation; the
 * lag and the tick execution-time mean and deviation; the policy; the spans
 * whose render-grid slot was pulled back to within a period of now. Silent
 * when no span ran. */
static void eager_report(void)
{
    if (!s_eager.spans) return;
    /* Under the greedy policy the divisor is bookkeeping only (the plan
     * comes from `afford`), so it is reported as such. */
    OPL_INFO_F("eager", "%u span(s): %u scene(s) produced, %u skipped (instant passed), "
               "%u refused (ring full), %u cut short; level now %s%d (%u down, %u up); "
               "scene cost %.3f ms ema, %.3f ms dev; lag %.3f ms (tick %.3f ms ema, %.3f dev); policy %s; "
               "%u first slot(s) pulled back",
               s_eager.spans, s_eager.produced, s_eager.skipped_late, s_eager.refused,
               s_eager.cut, s_eager.policy ? "n/a " : "", s_eager.level, s_eager.level_down, s_eager.level_up,
               (double)s_eager.cost_ema / 1e6, (double)s_eager.cost_dev / 1e6,
               (double)s_eager.lag / 1e6, (double)s_eager.tick_ema / 1e6, (double)s_eager.tick_dev / 1e6,
               s_eager.policy ? "greedy" : "divisor", s_eager.trimmed);
}

/** @brief Drop the eager scene plan on a guest discontinuity (savestate load, rewind, level load).
 *
 * Clears the planned targets and the last target, and resets the decaying
 * tick-time peak to the tick mean. The scene ring flushes its own pending
 * scenes. Registered with openpete_on_guest_discontinuity. */
static void eager_on_discontinuity(void) { s_eager.n = s_eager.i = 0; s_eager.last_target = 0; s_eager.tick_peak = s_eager.tick_ema; }

/* Frame-time-log hook for presents made on the present thread. */
static struct { openpete_frame_time_log_t *log; int64_t *start_ns, *period_ns; } s_eager_ftl;
static void ftl_record_present(openpete_frame_time_log_t* log, int64_t start_ns,
                               int64_t period_ns, uint32_t frame);
/** @brief Record a present made on the present thread in the frame-time log.
 *
 * Does nothing when no log was registered. The log start and period are read
 * through the pointers registered at startup, and the frame is
 * openpete_frame at the call. The arguments are unused.
 * @param ctx Unused.
 * @param kind Unused.
 * @param alpha Unused.
 * @param paired Unused. */
static void eager_present_cb(void *ctx, int kind, double alpha, int paired)
{
    (void)ctx; (void)kind; (void)alpha; (void)paired;
    if (s_eager_ftl.log)
        ftl_record_present(s_eager_ftl.log, *s_eager_ftl.start_ns, *s_eager_ftl.period_ns,
                           openpete_frame);
}

/** @brief Body of the game fiber: dispatch the game's entry point and park the fiber for good when it returns.
 *
 * Applies the --test-recomp-gp override to cpu.gp first, and gives
 * OPENPETE_CRASH_TEST its chance to fault on the fiber's stack. Sets
 * g_game.dispatch_ok and g_game.done; sets openpete_game_running at entry
 * and clears it before the permanent yield. */
static void game_fiber_entry(void) {
    SDL_SetAtomicInt(&openpete_game_running, 1);
    openpete_crash_test();
    if (g_game.gp != 0u) {
        OPL_INFO_F("boot",
                   "game fiber: overriding cpu.gp 0x%08X -> 0x%08X "
                   "(skipping PSYQ _96_init)",
                   g_game.cpu.gp, g_game.gp);
        g_game.cpu.gp = g_game.gp;
    }
    OPL_INFO_F("boot", "game fiber: dispatching 0x%08X", g_game.entry);
    g_game.dispatch_ok = openpete_recomp_module_get()->dispatch(&g_game.cpu, g_game.entry);
    if (!g_game.dispatch_ok) {
        OPL_ERROR_F("boot", "game fiber: no compiled body at 0x%08X", g_game.entry);
    } else {
        OPL_INFO_F("boot",
                   "game fiber: dispatched function returned (v0=0x%08X)",
                   g_game.cpu.v0);
    }
    g_game.done = 1;
    SDL_SetAtomicInt(&openpete_game_running, 0);
    /* Permanent yield - never resume. */
    while (1) co_switch((cothread_t)openpete_host_fiber);
}

/** @brief Write one frame-time-log record for a present, canonical or sub-tick.
 *
 * The record's deadline is start_ns plus one period per record already
 * written, so under --render-fps N the log carries the N Hz present
 * cadence; its slack is the deadline minus op_now(). No-op when the log
 * is not open.
 * @param log The frame-time log.
 * @param start_ns Time of the log's first deadline.
 * @param period_ns Target present period in nanoseconds.
 * @param frame openpete_frame, cross-reference only: it repeats across a
 *        tick's sub-tick presents, and measure-pacing.py keys on the
 *        timestamps. */
static void ftl_record_present(openpete_frame_time_log_t* log,
                               int64_t start_ns, int64_t period_ns,
                               uint32_t frame)
{
    if (!log->fp) return;
    int64_t now_ns      = op_now();
    int64_t deadline_ns = start_ns + (int64_t)log->records_written * period_ns;
    openpete_frame_time_log_record(log, frame, 0u, now_ns,
                                   deadline_ns, deadline_ns - now_ns);
}

/* Live render-fps pacer state (in-game settings menu). openpete_set_render_fps
 * updates s_render_fps and flags a re-anchor; the host loop recomputes the
 * period (it needs the loop's tick_scale) and resets the deadlines to avoid a
 * catch-up burst. Display-only: pacing, never gameplay. Initialised from
 * the RESOLVED launch target just before the loop (an unset --render-fps
 * defaults to the display refresh rate, windowed; 0, headless). */
static int         s_render_fps       = 0;
static op_period_t s_render_period    = { 0, 0, 1 };
static int         s_render_fps_dirty = 0;

/** @brief Return 1 when OPENPETE_FORCE_PACING makes a headless run pace like a windowed one.
 *
 * A headless run otherwise runs unlocked (fast-forward). The variable is
 * read once and logged; the result is readable before the main loop starts,
 * because the loop seeds its previous-iteration unlock state from it before
 * the first iteration, else the boot VBL would be recorded as paced against
 * a grid nothing slept to. */
static int pace_force_pacing(void)
{
    static int s_init = 0, s_on = 0;
    if (!s_init) {
        const char* v = getenv("OPENPETE_FORCE_PACING");
        s_on = (v && *v && *v != '0') ? 1 : 0;
        s_init = 1;
        OPL_INFO_F("pacer", "OPENPETE_FORCE_PACING=%d (env=%s)",
                   s_on, v ? v : "<unset>");
    }
    return s_on;
}

/** @brief Read OPENPETE_PACE_FLIP_LOG once: whether the pacer's idle wait measures the display flip.
 *
 * When on, pace_sleep_until spends the sleep before the next deadline in a
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
 *        the flip wait. */
static void pace_sleep_until(op_time_t deadline_ns, op_time_t sim_deadline_ns)
{
    const op_time_t req = op_now();
    /* Under eager production the presenting thread is the present thread,
     * which does its own flip logging; a wait from here would race it. */
    if (pace_flip_log_on() && !s_eager.active) {
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

/* One PRESENT record, carrying the block time the present path imposed on
 * this thread since the previous present. Read-and-clear, so each present
 * owns its own waits rather than a running total: under the thread split the
 * presenting thread is the sim thread, which makes a blocked present a
 * candidate cause for the next tick's lateness. */

/* Rolling cost of an intermediate present, and how many have been measured.
 * The predicate above will not act until it has seen this many, so a cold
 * start cannot shed on an estimate of zero. */
#define PACE_COST_WARMUP 16
/* Consecutive sheds before one present is admitted anyway, purely to
 * re-measure. Without it the estimator is one-way. */
#define PACE_SHED_PROBE_EVERY 32
static op_time_t s_present_cost_ema;
/* EMA of |cost - ema|: the WIDTH of the cost distribution, not just its
 * centre. The predicate needs an upper bound on what the next present will
 * cost, and an EMA alone is a centre estimate - on a right-skewed cost
 * distribution it is exceeded constantly. Measured over 49k recorded presents,
 * the shipped `ema + ema/4` bound was exceeded by **18-19 % of presents**, in
 * the attract demo and in a level alike. */
static op_time_t s_present_cost_dev;
static int       s_present_cost_n;
static int       s_present_shed_run;
/* Largest remaining-budget seen at an intermediate opportunity, decayed so it
 * tracks the machine instead of latching on one lucky moment. The probe below
 * needs "the moment in the cycle with the most budget"; expressed as a FIXED
 * fraction of a tick that moment does not exist on every machine - measured on
 * an Arc 140V laptop, the budget at 11 119 consecutive sheds never once
 * exceeded 9.6 ms against a 16.7 ms requirement, so the probe could not fire
 * and interpolation stayed off for the whole run. */
static op_time_t s_present_budget_peak;

/** @brief Read OPENPETE_PACE_LATE_BUDGET_US once: how much tick lateness an intermediate present may spend.
 *
 * The host loop admits an intermediate present that overruns the next sim
 * deadline by up to this budget when the tail cost bound (EMA plus three
 * deviations) fits in it. 0, the default, keeps the strict rule: never start
 * a present that could overrun the tick's start. The cap of 2000 us keeps
 * the budget under the slip threshold, so it costs phase, never the tick
 * grid.
 * @return The budget in nanoseconds, 0 to 2 000 000. */
static op_time_t pace_late_budget_ns(void)
{
    static op_time_t v = -1;
    if (v < 0) {
        const char *e = getenv("OPENPETE_PACE_LATE_BUDGET_US");
        long us = (e && *e) ? atol(e) : 0;
        if (us < 0) us = 0;
        if (us > 2000) us = 2000;
        v = (op_time_t)us * 1000;
    }
    return v;
}

/** @brief Read OPENPETE_PACE_SHED_K once: the multiplier on the cost deviation in the present-shed margin.
 *
 * The host loop sheds an intermediate present whose predicted cost does
 * not fit before the sim deadline; the predicted cost is the EMA plus k
 * deviations, or EMA plus EMA/4 at k = 0, the default. The two are A/B'd on
 * one machine through this variable.
 * @return k, 0 or greater. */
static int pace_shed_margin_k(void)
{
    static int k = -1;
    if (k < 0) {
        const char* e = getenv("OPENPETE_PACE_SHED_K");
        /* Default 0 = the centre-only bound (EMA + EMA/4). k=3 predicts better
         * (miss rate 19 % -> 5 % over 49k recorded presents) without a
         * measurable end-to-end difference, and k=4 collapses interpolation
         * (about 16 intermediate presents in a run instead of ~16000) because
         * a bound that wide almost never fits the budget. */
        k = (e && *e) ? atoi(e) : 0;
        if (k < 0) k = 0;
    }
    return k;
}

/** @brief Record a present of the given kind in the pace log with the last alpha and its pairing flag.
 *
 * @param kind The pace kind, OP_PACE_CANONICAL or OP_PACE_INTERMEDIATE. */
static void pace_record_present(uint8_t kind)
{
    openpete_pace_record_present(kind, openpete_pace_last_alpha(),
                                 openpete_pace_last_alpha_paired());
}

/* Slip ledger for the pacer's deadline clamps below. A clamp fires when the
 * loop has fallen more than slip_threshold_ns behind and snaps the deadline
 * forward by whole periods instead of firing catch-up iterations back to
 * back. That is the correct behaviour - a stall must never be repaid as a
 * burst - but it DISCARDS real time, so it must never be silent.
 *
 * The two kinds are not equally serious. A render slip is display-only: a
 * present was late, nothing about the game moved. A SIM slip means wall-clock
 * time passed that the game clock did not account for, so RTA time and tick
 * time have diverged by that much. Sim slips are therefore warned per event;
 * render slips are counted and reported once, because at high --render-fps a
 * struggling host can produce them continuously. */
static uint64_t s_sim_slip_events    = 0;
static uint64_t s_sim_slip_periods   = 0;
static uint64_t s_render_slip_events = 0;
static uint64_t s_render_slip_periods = 0;

/* Live-knob command queue. The dev overlay and the settings menu set the
 * render fps, tick scale, runahead depth, rewind window and mute from the
 * presenting or pump thread; the values they drive (s_render_fps,
 * s_tick_scale, s_runahead_req, the rewind keyframe ring, s_mute and their
 * dirty flags) belong to the host loop. A setter records its
 * request in s_knob_cmds under s_knob_mx and raises s_knob_pending; the host
 * loop drains the requests at the top of its iteration (live_knob_drain), its
 * quiescent point, so every write of those values and every mute gain change
 * happens on one thread. s_knob_shown carries the values the getters report
 * to other threads. */
typedef struct {
    int    fps, fps_set;
    double scale; int scale_set;
    int    ra, ra_set;
    int    rw, rw_set;
    int    mute, mute_set;
} live_knob_cmds_t;
typedef struct { int fps; double scale; int mute; } live_knob_shown_t;
static void            *s_knob_mx;           /* SDL_Mutex, openpete_mutex_once */
static live_knob_cmds_t s_knob_cmds;         /* under s_knob_mx */
static live_knob_shown_t s_knob_shown = { 0, 1.0, 0 };   /* under s_knob_mx */
static SDL_AtomicInt    s_knob_pending;

/** @brief Lock the live-knob queue. */
static void knob_lock(void)   { SDL_LockMutex(openpete_mutex_once(&s_knob_mx)); }
/** @brief Unlock the live-knob queue. */
static void knob_unlock(void) { SDL_UnlockMutex(openpete_mutex_once(&s_knob_mx)); }

void openpete_set_render_fps(int fps) {
    if (fps < 0) fps = 0;
    knob_lock();
    s_knob_cmds.fps = fps; s_knob_cmds.fps_set = 1;
    s_knob_shown.fps = fps;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

/* Live tick-scale (dev overlay). Same dirty-flag shape as render-fps: the
 * host loop re-derives every scaled period (sim, render, tick, slip) at its
 * top-of-iteration quiescent point and re-anchors the deadlines. Pacing
 * only - the sim executes identical ticks, just on a stretched wall clock
 * (headless/unlock ignores the deadlines entirely). */
static double s_tick_scale       = 1.0;
static int    s_tick_scale_dirty = 0;

void openpete_set_tick_scale(double scale) {
    if (scale < 0.05) scale = 0.05;
    if (scale > 8.0)  scale = 8.0;
    knob_lock();
    s_knob_cmds.scale = scale; s_knob_cmds.scale_set = 1;
    s_knob_shown.scale = scale;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

double openpete_get_tick_scale(void) {
    knob_lock();
    const double v = s_knob_shown.scale;
    knob_unlock();
    return v;
}

/* Live runahead depth (dev overlay). Same dirty-flag shape: the host loop
 * re-arms at its top-of-iteration quiescent point - speculation is fully
 * contained inside one ticked iteration (save -> speculate -> restore), so
 * there is never in-flight state to tear. s_runahead_live is the applied
 * depth after the mod-capability gate (may be forced 0). */
static int s_runahead_req   = 0;
static int s_runahead_dirty = 0;
static SDL_AtomicInt s_runahead_live;   /* written by the host loop, read anywhere */

void openpete_runahead_set(int n) {
    if (n < 0) n = 0;
    if (n > 3) n = 3;
    knob_lock();
    s_knob_cmds.ra = n; s_knob_cmds.ra_set = 1;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

int openpete_runahead_get(void) { return SDL_GetAtomicInt(&s_runahead_live); }

/* Pause and step requests from the dev overlay, read by the host loop where it
 * reads the pause and step keys: s_pause_req holds -1 (none), 0 or 1;
 * s_step_req is nonzero while a step waits. s_paused_shown publishes the
 * loop's paused state. */
static SDL_AtomicInt s_pause_req = { -1 };
static SDL_AtomicInt s_step_req;
static SDL_AtomicInt s_paused_shown;

void openpete_sim_pause_request(int on) { SDL_SetAtomicInt(&s_pause_req, on ? 1 : 0); }
void openpete_sim_step_request(void)    { SDL_SetAtomicInt(&s_step_req, 1); }
int  openpete_sim_paused(void)          { return SDL_GetAtomicInt(&s_paused_shown); }

void openpete_rewind_window_set(int seconds) {
    knob_lock();
    s_knob_cmds.rw = seconds < 0 ? 0 : seconds; s_knob_cmds.rw_set = 1;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

/* Live engine-level mute (dev overlay). Same three gain clamps the boot
 * apply uses; s_mute replaces opts->mute as the baseline the F7 pause
 * exit restores, so a live toggle survives a pause/resume cycle. Host
 * audio only - never gameplay state. (Toggling OFF while paused unmutes
 * the pause silence until resume; harmless dev-tool edge.) */
static int s_mute = 0;
/* Transient mute held while the skip-to-level present fast-forward is up:
 * the logos, title and loading the fast-forward hides also play their audio,
 * so the same gains stay clamped until presents resume. Layered over s_mute
 * (the baseline), never replacing it. */
static int s_skip_mute = 0;

/* The device feed's phases: its hold policy and per-phase counters. Boot is
 * the phase until the host loop's first iteration names another. */
enum { PH_BOOT, PH_SKIP, PH_UNLOCKED, PH_PAUSED, PH_LIVE, PH_HEADLESS };
static const char* const s_feed_phase_names[] = { "boot", "skip", "unlocked", "paused", "live", "headless" };

/** @brief Set up the SPU render's output sinks before any SPU init: the capture ring, the audio dump, the feed's synthetic device, and the exit summary.
 *
 * --record (with audio) arms the capture ring the recorder drains: every
 * frame the SPU renders, the frames the device plays. OPENPETE_AUDIO_DUMP
 * =PATH arms the same ring, drained into a raw s16 stereo 44.1 kHz file
 * every VSync (psyq_VSync), so a headless run yields the SPU's output as a
 * measurable signal; the recorder owns the ring when both are asked for.
 * OPENPETE_AUDIO_FEED_SIM=DRIFT[,SEED[,STALL]] replaces the audio device
 * with the feed's synthetic device, whose clock runs DRIFT (a fraction,
 * e.g. -0.024) off the producer's and which consumes nothing for STALL
 * seconds after the feed first leaves hold, so a headless run measures the
 * feed's underruns and depth. OPENPETE_AUDIO_FEED_LOG=PATH writes the
 * feed's timeline (holds, primes, underruns, overflows, and its counters
 * every 250 ms of device time).
 * @param opts The parsed options. */
static void audio_sinks_setup(const openpete_options_t* opts) {
    const int record_audio = opts->record_path && !opts->record_no_audio;
    PsyX_SPUAL_SetCapture(record_audio);
    const char* dump = getenv("OPENPETE_AUDIO_DUMP");
    if (dump && *dump && opts->record_path) {
        OPL_WARN_F("audio", "OPENPETE_AUDIO_DUMP ignored: --record owns the capture ring");
    } else if (dump && *dump) {
        extern int openpete_audio_dump_armed;
        openpete_audio_dump_armed = 1;
        PsyX_SPUAL_SetCapture(1);
    }
    const char* feed_log = getenv("OPENPETE_AUDIO_FEED_LOG");
    if (feed_log && *feed_log)
        PsyX_SPUFeed_SetLog(feed_log);
    PsyX_SPUFeed_SetPhase(PH_BOOT, s_feed_phase_names[PH_BOOT], 0);
    const char* sim = getenv("OPENPETE_AUDIO_FEED_SIM");
    if (sim && *sim) {
        const char* comma = strchr(sim, ',');
        const unsigned seed = comma ? (unsigned)strtoul(comma + 1, NULL, 10) : 1u;
        const char* comma2 = comma ? strchr(comma + 1, ',') : NULL;
        const double stall = comma2 ? atof(comma2 + 1) : 0.0;
        PsyX_SPUFeed_Simulate(atof(sim), seed, stall);
        OPL_INFO_F("audio", "feed: synthetic device, drift %+.4f seed %u start stall %.3f s",
                   atof(sim), seed, stall);
    }
    atexit(audio_feed_exit_summary);
}

/** @brief Apply the device feed's run-mode policy for this host-loop iteration, then top the device queue up.
 *
 * The feed carries the SPU at the guest's real-time rate. In a window the
 * feed holds (the SPU still runs) through fast boot, which runs the guest
 * without host yields until the first scene, through a pause, a
 * skip-to-level fast-forward and any other unlocked stretch. The phase and
 * the VBL count tag the feed's per-phase counters and timeline log. Headless runs unpaced by design: its feed (the synthetic device,
 * when armed) times itself from the frames pushed.
 * @param unlock_fps The iteration runs unpaced (fast-forward, skip, headless).
 * @param headless The run is headless.
 * @param paused The game is paused. */
static void audio_feed_sync(int unlock_fps, int headless, int paused) {
    int ph = PH_LIVE;
    if (headless)                                ph = PH_HEADLESS;
    else if (openpete_fast_boot_active)          ph = PH_BOOT;
    else if (paused)                             ph = PH_PAUSED;
    else if (openpete_skip_present_until_level)  ph = PH_SKIP;
    else if (unlock_fps)                         ph = PH_UNLOCKED;
    PsyX_SPUFeed_SetPhase(ph, s_feed_phase_names[ph], SDL_GetAtomicInt(&openpete_vbl_device));
    PsyX_SPUFeed_SetHold(ph != PH_LIVE && ph != PH_HEADLESS);
    PsyX_SPUFeed_Pump();
}

/** @brief Apply the combined mute state (baseline or skip-to-level) to every host audio path.
 *
 * With either s_mute or s_skip_mute set it mutes the SPU layer, forces the XA
 * and music-override gains to 0, and mutes the mod one-shot sources (which
 * are outside the SPU mixer); otherwise it clears the SPU mute and releases
 * the forced gains. Host audio only. */
static void apply_mute_gains(void) {
    extern int  PsyX_SPUAL_SetMute(int on_off);
    extern void PsyX_XA_ForceGain(float linear_gain);
    int on = s_mute || s_skip_mute;
    PsyX_SPUAL_SetMute(on);
    PsyX_XA_ForceGain(on ? 0.0f : -1.0f);
    music_override_set_forced_gain(on ? 0.0f : -1.0f);
    /* The SPU mute silences the render's output, host clips included;
     * the mod-sound mute also stops clips in flight and refuses new ones. */
    openpete_mod_sound_set_mute(on);
}

void openpete_set_mute(int on) {
    knob_lock();
    s_knob_cmds.mute = on ? 1 : 0; s_knob_cmds.mute_set = 1;
    s_knob_shown.mute = on ? 1 : 0;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

/** @brief Publish the host loop's applied render fps, tick scale and mute as the values the getters report. */
static void live_knob_publish_shown(void)
{
    knob_lock();
    s_knob_shown.fps = s_render_fps;
    s_knob_shown.scale = s_tick_scale;
    s_knob_shown.mute = s_mute;
    knob_unlock();
}

/** @brief Apply the queued live-knob requests on the host loop's thread, at its quiescent point.
 *
 * A changed render fps or tick scale raises the dirty flag the loop's
 * re-derivation reads; a changed runahead depth raises the re-arm flag; a
 * rewind window request resizes the keyframe ring here, off every capture; a
 * mute request sets the baseline and applies the gains here. Requests that
 * match the applied value change nothing. */
static void live_knob_drain(void)
{
    if (!SDL_GetAtomicInt(&s_knob_pending)) return;
    knob_lock();
    const live_knob_cmds_t c = s_knob_cmds;
    memset(&s_knob_cmds, 0, sizeof s_knob_cmds);
    SDL_SetAtomicInt(&s_knob_pending, 0);
    knob_unlock();
    if (c.fps_set && c.fps != s_render_fps) { s_render_fps = c.fps; s_render_fps_dirty = 1; }
    if (c.scale_set && c.scale != s_tick_scale) { s_tick_scale = c.scale; s_tick_scale_dirty = 1; }
    if (c.ra_set && c.ra != s_runahead_req) { s_runahead_req = c.ra; s_runahead_dirty = 1; }
    if (c.rw_set) openpete_rewind_configure(c.rw);
    if (c.mute_set) { s_mute = c.mute; apply_mute_gains(); }
}

/** @brief Mute audio while openpete_skip_present_until_level fast-forwards, and restore it after.
 *
 * Called wherever the flag can change (boot arming, each host-loop
 * iteration); applies the mute gains only on a change of the flag. */
static void skip_mute_sync(void) {
    int want = openpete_skip_present_until_level ? 1 : 0;
    if (want == s_skip_mute) return;
    s_skip_mute = want;
    apply_mute_gains();
}

int openpete_get_mute(void) {
    knob_lock();
    const int v = s_knob_shown.mute;
    knob_unlock();
    return v;
}

/* --sfx / --music: the requested in-game slider levels, 0..10, or -1 for
 * "leave the game's own slider alone". These are a host-side REQUEST, not a
 * write: the per-game audio-volume feature reads them from its tick hook and
 * stamps the game's volume globals there. Keeping the guest write on the sim's
 * schedule is what lets the dev overlay edit them safely (north-star: nothing
 * outside the tick touches gameplay-readable RAM). */
static int s_sfx_volume   = -1;
static int s_music_volume = -1;

/** @brief Clamp a slider level to 0..10, mapping every negative value to -1 (leave the setting alone). */
static int clamp_volume(int v) {
    if (v < 0)  return -1;
    if (v > 10) return 10;
    return v;
}

void openpete_set_sfx_volume(int v)   { s_sfx_volume   = clamp_volume(v); }
int  openpete_get_sfx_volume(void)    { return s_sfx_volume; }
void openpete_set_music_volume(int v) { s_music_volume = clamp_volume(v); }
int  openpete_get_music_volume(void)  { return s_music_volume; }

/* --vibration: one request feeds the guest enable flag and the motor amplitude. */
void openpete_set_vibration(int v) { openpete_rumble_set_strength(clamp_volume(v)); }
int  openpete_get_vibration(void)  { return openpete_rumble_get_strength(); }

int openpete_get_render_fps(void) {
    knob_lock();
    const int v = s_knob_shown.fps;
    knob_unlock();
    return v;
}

/** @brief Report whether PsyCross runs without a GPU because nothing in the run reads what it rasterizes.
 *
 * PsyCross's raster reaches a consumer through the display under
 * --renderer psycross, the -og screenshot leg, the present-sequence
 * capture (-og per present), and a headless --record, which encodes
 * PsyCross's present FBO because the native display is off under
 * --headless. A windowed --record reads the native offscreen and needs no
 * PsyCross raster. A windowed run with any screenshot request keeps the GL
 * path, --screenshot-native-only included. A headless
 * --screenshot-native-only run reads only the native capture, whose scene
 * is composed from game state. A windowed run whose launcher found no
 * Vulkan device (opts->native_unavailable) keeps the GL path: PsyCross's
 * GL window is then its display. In every run the CPU VRAM mirror stays
 * live, so StoreImage reads what LoadImage, MoveImage and ClearImage
 * wrote; null-GPU mode stops only the copy-back of the PsyCross
 * framebuffer into the mirror, and a run that rasterizes no OT never
 * stores a framebuffer rect for that copy-back to read.
 * @param opts Resolved launch options.
 * @return true when PsyCross is to open no window, no GL context and no
 *         PSX GPU state. */
static bool psycross_raster_unread(const openpete_options_t* opts) {
    if (opts->renderer != OPENPETE_RENDERER_VK || opts->screenshot_presents_start >= 0
        || opts->native_unavailable)
        return false;
    if (!opts->headless)
        return opts->screenshot_frame_count == 0;
    return opts->screenshot_native_only && !opts->record_path;
}

void openpete_game_window_size(const openpete_options_t* opts, int* out_w, int* out_h)
{
    int w = (opts->window_w > 0) ? opts->window_w : game_default_window_w();
    int h = (opts->window_h > 0) ? opts->window_h : game_default_window_h();
    /* If --aspect is set but the user didn't pin --window, scale the
     * default window so the *cropped* (post-overscan) src exactly fills
     * it - otherwise the present blit letterboxes the un-rendered
     * disp/clip difference. The cropped aspect = requested_aspect ×
     * game_display_crop_factor(), the game's own disp_h/clip_h. The
     * PsyCross reference renderer has no Hor+ path and draws canonical 4:3
     * only, so a widened window would stretch it; native modes keep
     * --aspect (window-driven Hor+). An explicit --window always wins. */
    if (opts->aspect_w_over_h > 0.0f && opts->renderer != OPENPETE_RENDERER_PSYCROSS &&
        opts->window_w <= 0 && opts->window_h <= 0) {
        h = game_default_window_h();
        w = (int)((float)h * opts->aspect_w_over_h * game_display_crop_factor() + 0.5f);
    }
    *out_w = w;
    *out_h = h;
}

/** @brief Bring up the host side of a run: guest CPU and PSX runtime, knobs, mods, PsyCross, audio, input, windows and the native renderer.
 *
 * Runs before the host loop, on the thread that will pump SDL events (main
 * under the thread split, else the single thread). In order it initialises
 * the guest CPU and PSX runtime, seeds the CLI knobs (volumes, vibration,
 * mouse, input lock), boots the EXE and applies --seed-cpu / --seed-ram,
 * applies feature-module knobs and loads mods, resolves the window size
 * from --aspect, sets up PsyCross (null-GPU when nothing reads its raster),
 * input maps and icons, arms --record, brings up the native Vulkan renderer
 * for --renderer vk (a windowed run left with no renderer stops with a
 * message to the player), arms the pad-read probe, halt-at-pc and the psxtrace
 * verifier, configures the memory card, and starts the VBL ticker unless
 * headless without OPENPETE_FORCE_PACING.
 * @param opts Resolved launch options.
 * @return 0 on success, else an OPENPETE_EXIT_* code for the caller to
 *         return. */
static int run_host_init(const openpete_options_t* opts) {
    /* True when main owns the window, the pump and the SDL-state publish and
     * the host loop runs on a worker. This function itself runs on main in
     * that case, before the worker exists. Currently unused below. */
    const bool split = openpete_split_applies(opts);

    memset(&g_game, 0, sizeof(g_game));
    openpete_psx_runtime_init(&g_game.cpu);
    openpete_psx_set_diag_cpu(&g_game.cpu);

    /* --sfx / --music: seed the slider presets the per-game audio-volume
     * feature stamps into the game's own volume globals (-1 = leave the
     * game's slider alone, the default). Seeded here rather than beside the
     * --mute apply below because the knob registry stamps its boot values
     * from the live getters during renderer init, which happens first. */
    openpete_set_sfx_volume(opts->sfx_volume);
    openpete_set_music_volume(opts->music_volume);

    /* --vibration: the host-side strength (0..10, -1 = parity), plus the
     * context gates. Headless is a refusal rather than a default - a run with
     * no window has no player, and every gate battery, capture and
     * scripted launch is one.
     *
     * Installing the actuator-registration hook is what moves the per-vsync
     * half of libpad's PadSetAct contract onto the host; left uninstalled
     * (the OPENPETE_INPUT_LEGACY A/B) PsyCross keeps its historical one-shot
     * behaviour, which is the "before" side of the vibration A/B. */
    openpete_set_vibration(opts->vibration);
    openpete_rumble_set_headless(opts->headless ? 1 : 0);

    /* --mouse: the host mouse channel switch, seeded before the knob registry
     * stamps its boot values from the live getters. -1 (unset) is on. */
    openpete_host_mouse_set_enabled(opts->mouse != 0);
    /* --input-lock: seeded here for the same reason. -1 (unset) is off. */
    openpete_input_lock_set(opts->input_lock > 0);
    /* --pad-unfocused: seeded here for the same reason; -1 (unset) is off. */
    openpete_host_input_set_pad_unfocused(opts->pad_unfocused > 0);
    if (opts->input_lock > 0)
        OPL_INFO_F("input", "input lock on: keyboard, mouse and gamepad "
                   "devices are ignored");
    {
        /* OPENPETE_RUMBLE_LEGACY=1 leaves the hook uninstalled, so PadSetAct
         * keeps PsyCross's historical one-shot behaviour. That is the "before"
         * side of the vibration A/B, and it is deliberately its OWN knob
         * rather than a lean on OPENPETE_INPUT_LEGACY: the input-legacy path
         * also moves pumping and decode back to PsyCross, which changes
         * several things at once. An A/B that moves more than one variable
         * cannot attribute what it measures. */
        const char* e = getenv("OPENPETE_RUMBLE_LEGACY");
        const int rumble_legacy = (e && *e && *e != '0');
        if (!openpete_host_input_legacy() && !rumble_legacy)
            g_PsyX_Pad_ActuatorRegister = openpete_rumble_register;
        else
            OPL_INFO_F("rumble", "actuator registration hook NOT installed "
                                 "(%s) - PadSetAct keeps its one-shot behaviour",
                       rumble_legacy ? "OPENPETE_RUMBLE_LEGACY"
                                     : "OPENPETE_INPUT_LEGACY");
    }

    uint8_t* bytes = NULL;
    size_t   len   = 0;
    if (try_load_exe(opts, &bytes, &len) != 0) {
        OPL_ERROR_F("boot", "could not load PSX EXE (need --exe-path or --disc)");
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    int rc = openpete_psx_boot(&g_game.cpu, bytes, len);
    free(bytes);
    if (rc != 0) return OPENPETE_EXIT_PANIC;

    /* seed-cpu / seed-ram block. Both run AFTER openpete_psx_boot so the
     * seeds win over the EXE-loader's writes. seed-cpu also short-circuits
     * the --test-recomp-gp override below - if the user asked us to seed
     * the full register set, we do not want a stale --test-recomp-gp value
     * clobbering the seeded gp inside game_fiber_entry. */
    if (opts->seed_cpu_path) {
        if (load_cpu_seed(&g_game.cpu, opts->seed_cpu_path) != 0) {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        OPL_INFO_F("seed-cpu",
                   "primed from %s: pc=0x%08X gp=0x%08X sp=0x%08X ra=0x%08X",
                   opts->seed_cpu_path,
                   g_game.cpu.pc, g_game.cpu.gp,
                   g_game.cpu.sp, g_game.cpu.ra);
    }
    if (opts->seed_ram_path) {
        FILE* fp = fopen(opts->seed_ram_path, "rb");
        if (!fp) {
            OPL_ERROR_F("seed-ram", "fopen %s failed: %s",
                        opts->seed_ram_path, strerror(errno));
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        const size_t ram_size = 2u * 1024u * 1024u;
        size_t n = fread(openpete_psx_get_ram(), 1, ram_size, fp);
        fclose(fp);
        if (n != ram_size) {
            OPL_ERROR_F("seed-ram",
                        "short read from %s: %zu of %zu bytes",
                        opts->seed_ram_path, n, ram_size);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        OPL_INFO_F("seed-ram", "g_ram primed from %s (%zu bytes)",
                   opts->seed_ram_path, ram_size);
    }

    /* Feature modules apply their CLI-derived knobs; a game with no
     * feature list in its descriptor makes this a no-op. */
    feature_registry_apply_options(opts);

    /* Modding framework: discover, build and load mods. No mods directory
     * means zero work and a byte-identical modless run; a broken mod is
     * disabled with a log, never a failed launch. */
    openpete_mods_init(opts);
    openpete_boot_mark(OP_BOOT_MODS_INIT);

    int win_w, win_h;
    openpete_game_window_size(opts, &win_w, &win_h);
    openpete_knob_default_window_set(game_default_window_w(), game_default_window_h());
    if (opts->aspect_w_over_h > 0.0f &&
        opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        OPL_WARN_F("aspect", "--aspect is native-renderer-only (the PsyCross "
                   "Hor+ substitution stack was retired); the PsyCross "
                   "reference renders canonical 4:3 - flag ignored");
    } else if (opts->aspect_w_over_h > 0.0f &&
        opts->window_w <= 0 && opts->window_h <= 0) {
        OPL_INFO_F("aspect", "auto window size %dx%d (--aspect=%.4f × crop %.4f)",
                   win_w, win_h, (double)opts->aspect_w_over_h,
                   (double)game_display_crop_factor());
    }

    /* --fov and --aspect are native-renderer display policies: every
     * renderer mode runs the game's verdict producers pure canonical 4:3,
     * the native feeders project through the effective H and widen Hor+
     * from the window aspect (core/renderer/native_render.c). PsyCross
     * reference mode has neither. */
    if (opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        if (opts->fov_deg > 0.0f)
            OPL_WARN_F("fov", "--fov is a native-renderer policy; inert under "
                       "--renderer psycross");
    } else {
        if (opts->fov_deg > 0.0f)
            openpete_native_set_fov(opts->fov_deg);
        if (opts->aspect_w_over_h > 0.0f) {
            OPL_INFO_F("aspect", "--aspect %.4f sizes the window; the native "
                       "renderer widens Hor+ at the display layer from "
                       "canonical 4:3 producers",
                       (double)opts->aspect_w_over_h);
        }
    }

    /* --no-lod: native-renderer display policy (sets openpete_no_lod and
     * resolves the fog scale; warns and is ignored in PsyCross reference
     * mode, which has no extended-LOD producer). */
    if (no_lod_apply_options(opts) != 0) {
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    audio_sinks_setup(opts);

    /* --psycross-native-raster: size the reference renderer's present FBO to
     * the console display grid instead of the window. Read per scene begin, so
     * setting it before init is enough. */
    extern int g_psyx_present_native_raster;
    g_psyx_present_native_raster = opts->psycross_native_raster ? 1 : 0;

    /* PsyCross opens a GPU exactly when something in the run reads what it
     * rasterizes (psycross_raster_unread names the readers). Otherwise it
     * runs as a library in null-GPU mode: SDL, the vblank/interrupt core,
     * input, SPU and CDFS come up; no window, GL context or PSX GPU state is
     * created, and every GL-touching GR entry point returns early, so the
     * libgpu thunks pay no texture upload or framebuffer readback. The
     * oracle and the determinism runs (headless, no --screenshot-native-only)
     * keep the GL path: PsyCross rasterizes every OT there. */
    extern int g_psyx_null_gpu;
    g_psyx_null_gpu = psycross_raster_unread(opts) ? 1 : 0;
    OPL_INFO_F("boot", "PsyX_Initialise: %s (headless=%d)",
               g_psyx_null_gpu ? "null-GPU (library-only: no window/GL)"
                               : "opening window",
               (int)opts->headless);
    /* The dev overlay exists only in the native-vk window path, so a run
     * without one has nothing for --devui-open to open. Saying so beats a
     * flag that silently does nothing. */
    if (openpete_devui_open_requested(opts)
        && (opts->headless || opts->renderer != OPENPETE_RENDERER_VK))
        OPL_INFO_F("devui", "%s", "--devui-open is inert in this run: the "
                   "dev config overlay exists only in a windowed native-vk "
                   "session");
    /* A headless run that keeps the GL path uses the real GL driver - SDL's
     * "offscreen" video driver hands back a context with NULL gl* function
     * pointers, so the first call from GR_InitialisePSX segfaults. A
     * GPU/display backend (X/Wayland/WSLg/WGL) must therefore be present;
     * pure-console CI needs Xvfb. The window is created unmapped via
     * startHidden so it never flashes or steals focus - GL/FBO rendering
     * works without it ever being shown. Null-GPU mode creates no window or
     * context. */
    /* PsyCross renders to its FBO only; the native Vulkan window is the
     * display in every mode. What reaches the screen is either the native
     * scene (--renderer vk) or PsyCross's read-back frame (--renderer
     * psycross), and either way this GL window is not it. Created unmapped
     * and shown again below only if native bring-up fails. */
    int psyx_ok = PsyX_Initialise((char*)opts->display_name, win_w, win_h,
                                  /*fullscreen=*/0,
                                  /*startHidden=*/1);
    /* PsyX_Initialise runs SDL_INIT_VIDEO in every mode, and a headless run
     * that keeps the GL path (the oracle: PsyCross rasterizes every OT) opens
     * a GL context. A session with no display reachable fails here. Stopping
     * now names the
     * cause; continuing would crash at the first ResetGraph with a null GL
     * proc and no explanation. */
    if (!psyx_ok) {
        OPL_ERROR_F("boot",
                    "PsyCross init failed (SDL: %s). No display is reachable; "
                    "even --headless needs one, because PsyCross rasterizes "
                    "for the oracle. Over ssh, export XDG_RUNTIME_DIR and "
                    "WAYLAND_DISPLAY (or DISPLAY) from the login session.",
                    SDL_GetError());
        return OPENPETE_EXIT_PANIC;
    }

    /* PsyX_Sys_InitialiseInput just wrote its hardcoded pad maps; replace
     * them with the resolved [keys.pad] bindings. */
    stamp_psx_pad_bindings(1);
    apply_psyx_dither_env(opts);

    /* The gamepad subsystem is up now, which is the earliest point a mapping
     * can be added and the latest one at which it still reaches a pad that
     * was already attached. */
    openpete_host_input_load_mappings(opts->controller_db);
    openpete_host_input_drop_unread_events();

    /* App icon: the registered descriptor's RGBA8 image, emitted at build
     * time by the game's own icon step; a descriptor without one sets
     * none. The Win32 .rc resource handles taskbar / .exe shortcut /
     * alt-tab on Windows; SDL_SetWindowIcon covers the live window on
     * every platform. */
    const openpete_game_icon_t* game_icon = openpete_game_runtime()->icon;
    if (game_icon && game_icon->w <= 0) game_icon = NULL;
    if (g_window && game_icon) {
        SDL_Surface* icon = SDL_CreateSurfaceFrom(
            game_icon->w, game_icon->h, SDL_PIXELFORMAT_RGBA32,
            (void*)game_icon->rgba, game_icon->w * 4);
        if (icon) {
            SDL_SetWindowIcon(g_window, icon);
            SDL_DestroySurface(icon);
        }
    }
    /* Same icon for the native Vulkan window, which is the visible one in the
     * default path (PsyCross runs null-GPU and creates no window at all).
     * Handed over rather than applied: that window does not exist yet. */
    if (game_icon)
        openpete_native_set_icon(game_icon->rgba, game_icon->w, game_icon->h);

    if (opts->headless && g_window) {
        /* Window was created unmapped (PsyX_Initialise startHidden=1) - no
         * hide needed here; just record the driver SDL picked. */
        OPL_INFO_F("boot", "headless: PsyCross window created hidden (driver=%s)",
                   SDL_GetCurrentVideoDriver());
    }

    /* --record: arm gameplay video capture. The encoder is built lazily on
     * the first captured frame (once the present FBO's native dimensions are
     * known); this only stashes the parameters. Capture fires before
     * PsyX_EndScene, reading the present FBO directly. */
    if (openpete_record_init(opts) != 0)
        return OPENPETE_EXIT_SETUP_ERROR;

    /* --renderer vk: bring up the native Vulkan renderer's window. SDL
     * is already initialized by PsyX_Initialise. Pass the same resolved window
     * size handed to PsyCross so the native window matches the game's. A
     * bring-up failure is non-fatal - we log and run PsyCross-only. The
     * native path is a pure display consumer; it never touches PSX RAM, so
     * the oracle gate / determinism are unaffected. */
    /* Create the window before bring-up so it belongs to this thread - the
     * one that pumps. Bring-up adopts it exactly as it adopts the splash's. */
    openpete_native_precreate_window(opts, win_w, win_h);
    rhi_set_clock(op_now);   /* boot facts on the pace log's clock */
    (void)openpete_native_render_init(opts, win_w, win_h);
    /* Bring-up failed in a windowed run: PsyCross's own GL window is the only
     * display left, so map the window that was created hidden above. Without
     * this the run would present to nothing and look like a hang. */
    if (!opts->headless && !openpete_native_render_enabled() && g_window) {
        SDL_ShowWindow(g_window);
        OPL_WARN_F("boot", "%s", "native display unavailable - showing "
                   "PsyCross's own GL window instead");
    }
    openpete_native_set_perf_stats(opts->perf_stats);   /* stats overlay level (O / M menus move it live) */
    openpete_native_set_perf_anchor(opts->perf_position);  /* which corner it anchors to (--perf-position) */
    openpete_osd_set_enabled(opts->osd);
    openpete_osd_set_anchor(opts->osd_position);
    openpete_osd_set_default_timeout_ms(opts->osd_timeout_ms);
    openpete_native_set_version_osd(opts->version_osd);   /* build-version line (--version-osd) */
    if (opts->mouse_sensitivity >= 0.0f)
        openpete_freecam_set_mouse_sensitivity(opts->mouse_sensitivity);
    /* OPENPETE_OSD_DEMO=N seeds N sample toasts at startup. The OSD is
     * otherwise driven entirely by live user action - a menu row, a hotkey, a
     * mod - which a headless or scripted capture cannot perform, so a gate
     * that wants to SEE a toast has no other way to make one exist. The
     * timeout is the maximum so the stack is still up whenever the capture
     * frame lands, and the strings are deliberately different lengths: the
     * box is content-sized inside a fixed slot, and equal-length samples
     * would never exercise that. Diagnostic only; unset in normal play. */
    {
        const char *e = getenv("OPENPETE_OSD_DEMO");
        int demo_n = (e && *e) ? atoi(e) : 0;
        static const char *const demo[] = {
            "VSYNC: ON", "LOD: EXTENDED", "SSAA: 4X",
            "DISPLAY: FULLSCREEN", "FPS: UNLIMITED", "TEXTURES: AFFINE",
            "DITHER: 15 BIT", "FREECAM: ON",
        };
        if (demo_n > (int)(sizeof demo / sizeof demo[0]))
            demo_n = (int)(sizeof demo / sizeof demo[0]);
        for (int i = 0; i < demo_n; i++) {
            /* One bucket each: the per-source rate limit exists to stop a
             * runaway producer, and it would otherwise cap this seed at the
             * burst of 4 - an eight-toast gate that silently captured four. */
            char src[16];
            snprintf(src, sizeof src, "demo%d", i);
            openpete_osd_notify(src, NULL, OPENPETE_OSD_TIMEOUT_MAX_MS,
                                "%s", demo[i]);
        }
    }
    /* OPENPETE_MENU_DEMO=1 opens the settings menu at startup, the menu's
     * counterpart of the toast seed above: an unattended run has no key to
     * open it with. The open menu holds the game pad released, so the run
     * needs no pad input of its own (--skip-to-level, not a movie).
     * Diagnostic only. */
    {
        const char *e = getenv("OPENPETE_MENU_DEMO");
        if (e && *e && *e != '0' && !openpete_menu_is_open()) openpete_menu_toggle();
    }
    openpete_native_set_fps_in_record(opts->record_fps_overlay);  /* --record-fps-overlay */

    /* Decide whether PsyCross still rasterizes the OT (see the global's comment).
     * Only a pure, successfully-brought-up native vk display with no capture
     * consumer lets us skip the redundant GL submission. Everything else
     * (headless, PsyCross mode, native bring-up failure, --screenshot) keeps
     * PsyCross rendering. NOTE: --record does NOT force it on for the native display - the
     * native --record path reads the native offscreen directly (rhi_read_offscreen,
     * self-synchronizing), so forcing PsyCross to ALSO render every frame just
     * doubled the GPU work and throttled the capture to ~8 fps. (Headless --record
     * still uses PsyCross - native is disabled there, so the first term is true.) */
    openpete_psycross_renders =
        (!openpete_native_render_enabled()                 /* headless / psycross / bring-up failed */
         /* PsyCross mode: PsyCross IS the rasterizer, so it always renders.
          * Redundant with the term above only while PsyCross mode creates no
          * native device - keep it stated in its own right, because it is the
          * whole rasterization decision the moment that changes. */
         || opts->renderer == OPENPETE_RENDERER_PSYCROSS
         /* --screenshot-native-only discards the PsyCross reference, so don't pay
          * the GL OT walk to build it - the native -vk dump reads gamestate. */
         || (opts->screenshot_frame_count > 0 && !opts->screenshot_native_only) /* -og leg */
         || opts->screenshot_presents_start >= 0)          /* present-sequence capture (-og leg) */
        /* null-GPU hard override: no GL context exists, so PsyCross MUST NOT
         * rasterize regardless of the terms above (the reachable case is a
         * native bring-up failure in a windowed pure-vk run, which stops
         * below instead of reaching a NULL GL proc). */
        && !g_psyx_null_gpu;
    /* PsyCross skipped its GL init for this run, so a failed native bring-up
     * leaves no renderer: the game would run with sound and no picture. */
    if (g_psyx_null_gpu && !openpete_native_render_enabled()) {
        OPL_ERROR_F("boot", "%s", "native renderer unavailable and PsyCross is "
                    "in null-GPU mode: no display path, stopping");
        openpete_fatal_message("OpenPete could not start graphics",
            "No usable Vulkan graphics device was found.\n\n"
            "Install the latest driver for your graphics card.\n"
            "To run without Vulkan, add renderer = \"psycross\" to openpete.toml.");
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    OPL_INFO_F("boot", "PsyCross OT rasterization: %s (renderer=%d native=%d)",
               openpete_psycross_renders ? "ON" : "OFF (the native scene is the picture)",
               (int)opts->renderer, (int)openpete_native_render_enabled());

    signal(SIGINT,  openpete_on_sigint);
#ifdef SIGTERM
    signal(SIGTERM, openpete_on_sigint);
#endif

    if (opts->disc_path) {
        int disc_rc = attach_disc_image(opts);
        if (disc_rc != OPENPETE_EXIT_OK) return disc_rc;
    }

    /* The header's entry is the crt0, which sets sp, fp and gp, stores the
     * BIOS return address and calls main; an explicit OPENPETE_RUN_RECOMP bypasses
     * it and is a diagnostic, since main entered directly runs 8 bytes lower
     * on the stack with the heap words unwritten. */
    g_game.entry = opts->run_recomp_addr ? opts->run_recomp_addr : g_game.cpu.pc;
    if (g_game.entry != g_game.cpu.pc) {
        OPL_WARN_F("boot", "OPENPETE_RUN_RECOMP 0x%08X bypasses the EXE entry 0x%08X: "
                   "stack, heap words and gp differ from hardware",
                   g_game.entry, g_game.cpu.pc);
    }
    /* When --seed-cpu is supplied, suppress the --test-recomp-gp override
     * so it can't clobber the freshly-seeded gp inside game_fiber_entry. */
    g_game.gp    = opts->seed_cpu_path ? 0u : opts->test_recomp_gp;

    /* Pad-read probe: arm from the per-game address before the first
     * dispatch, so tick zero's read is seen like every other. */
    {
        extern uint32_t openpete_pad_read_pc;
        /* OPENPETE_PAD_READ_PC overrides the per-game address. It is how the
         * probe is falsified - pointed at an address the game never calls,
         * the census must report NEVER - and how a second game's read site is
         * found before it is written down. */
        const char* pad_pc_env = getenv("OPENPETE_PAD_READ_PC");
        openpete_pad_read_pc = (pad_pc_env && *pad_pc_env)
            ? (uint32_t)strtoul(pad_pc_env, NULL, 0)
            : game_get_pad_read_pc();
        if (openpete_pad_read_pc) {
            openpete_pace_pad_probe_arm();
            OPL_DEBUG_F("pacing", "pad-read probe armed at 0x%08X",
                        openpete_pad_read_pc);
        }
    }

    /* Wire halt-at-pc before dispatch so psx_override_dispatch sees it on
     * the very first recompiled function-entry. dispatch.c owns the slot. */
    if (opts->halt_at_pc != 0u) {
        extern uint32_t    openpete_halt_at_pc;
        extern uint32_t    openpete_halt_hit_target;
        extern const char* openpete_halt_dump_path;
        extern const char* openpete_halt_cpu_dump_path;
        openpete_halt_at_pc         = opts->halt_at_pc;
        openpete_halt_hit_target    = opts->halt_hit_target ? opts->halt_hit_target : 1u;
        openpete_halt_dump_path     = opts->ram_dump_output;
        openpete_halt_cpu_dump_path = opts->halt_cpu_dump_output;
        OPL_INFO_F("halt-at-pc",
                   "armed: PC=0x%08X hit=%u dump=%s%s%s",
                   opts->halt_at_pc, openpete_halt_hit_target,
                   opts->ram_dump_output,
                   opts->halt_cpu_dump_output ? " cpu=" : "",
                   opts->halt_cpu_dump_output ? opts->halt_cpu_dump_output : "");
    }

    /* PCSX-Redux oracle verifier: open the trace before the game loop and
     * register the dispatch hook so advance() ticks at every dispatch of
     * the trace's anchor PC (read from the trace itself unless the user
     * overrides via --psxtrace-anchor-pc), matching the BP
     * scripts/pcsx-redux-trace.lua used to record the trace. */
    static openpete_psxtrace_t s_psxtrace;
    if (opts->psxtrace_verify_path) {
        if (openpete_psxtrace_open(&s_psxtrace,
                                   opts->psxtrace_verify_path) == 0) {
            psxtrace = &s_psxtrace;
            s_psxtrace_active = &s_psxtrace;
            extern uint32_t openpete_psxtrace_anchor_pc;
            extern uint32_t openpete_psxtrace_anchor_hit_target;
            extern void (*openpete_psxtrace_dispatch_hook)(uint32_t);
            openpete_psxtrace_anchor_pc =
                opts->psxtrace_anchor_pc ? opts->psxtrace_anchor_pc
                                         : s_psxtrace.inferred_anchor_pc;
            if (openpete_psxtrace_anchor_pc == 0u) {
                OPL_ERROR_F("psxtrace",
                            "no anchor PC: trace had no first record pc, "
                            "pass --psxtrace-anchor-pc HEX");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            openpete_psxtrace_anchor_hit_target =
                opts->psxtrace_anchor_hit ? opts->psxtrace_anchor_hit : 1u;
            openpete_psxtrace_dispatch_hook = openpete_psxtrace_on_anchor;
            OPL_INFO_F("psxtrace",
                       "anchor armed: PC=0x%08X hit=%u",
                       openpete_psxtrace_anchor_pc,
                       openpete_psxtrace_anchor_hit_target);
        } else {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (opts->psxtrace_anchor_pc) {
        /* No trace: the anchor still counts hits, for the CD read log's
         * hit stamps and OPENPETE_ANCHOR_VBL_LOG. */
        extern uint32_t openpete_psxtrace_anchor_pc;
        openpete_psxtrace_anchor_pc = opts->psxtrace_anchor_pc;
        OPL_INFO_F("psxtrace", "anchor counting: PC=0x%08X (no trace)",
                   openpete_psxtrace_anchor_pc);
    }

    /* Engine-level mute: clamp the SPU AL sources and the XA stream's AL
     * gain. Bypasses the game's volume sliders entirely. The music WAV
     * override mirrors the XA forced gain so --mute covers it too.
     * Routed through the live setter (dev overlay toggles it too). */
    music_override_init();  /* logs the assets/music/ search path */
    sfx_pack_set_dir_override(opts->sfxpack_dir);
    sfx_pack_init();        /* assets/sfx/ drop-in replacement */
    /* Boot: written before the host loop starts, so no other thread reads it. */
    s_mute = opts->mute;
    apply_mute_gains();
    live_knob_publish_shown();

    /* Host-backed memory card: apply CLI/env config before the game's
     * MemCardInit thunk fires (during the game's Initialize()). */
    openpete_memcard_set_dir_override(opts->card_dir);
    openpete_memcard_set_paths(opts->card1_path, opts->card2_path);
    openpete_memcard_set_presence(0, opts->card_a_present);
    openpete_memcard_set_presence(1, opts->card_b_present);
    openpete_memcard_set_unformatted(opts->card_unformatted);
    openpete_memcard_set_adopt(opts->adopt_saves);
    if (openpete_memcard_startup_check() != 0) return OPENPETE_EXIT_SETUP_ERROR;
    openpete_handoff_init(opts->resume_handoff, opts->handoff_argc,
                          opts->handoff_argv);
    /* Savestate slots every game shared are copied into this game's own
     * folder; the newest per-game overlay ini is adopted as the one
     * openpete-devui.ini every game shares. */
    (void)openpete_state_seed_game_dir();
    (void)openpete_devui_seed_ini();

    /* Skip fast-forward is windowed-only - headless is already unlocked and
     * has nothing to show. Published before the setter below so the boot
     * CLI skip (and every later runtime warp) arms correctly. */
    openpete_skip_ffwd_windowed = opts->headless ? 0 : 1;

    /* Forward CLI overrides to the per-game impl. game_on_frame_pre
     * applies them when the relevant in-game globals are ready. The setter
     * itself arms the present fast-forward
     * (openpete_skip_present_until_level). */
    game_set_skip_to_level(opts->skip_to_level);
    game_set_skip_to_cutscene(opts->skip_to_cutscene);
    game_set_skip_to_area(opts->skip_to_area_level, opts->skip_to_area);
    skip_mute_sync();

    /* Fast-boot gate is on by default (see fiber.c). psyq_DrawOTag clears it
     * the first time real draw commands hit, so host yields engage exactly
     * when the game starts rendering - no CLI knob needed. */
    OPL_INFO_F("boot", "fast-boot: skipping host yields until first DrawOTag");

    /* Start the free-running vblank ticker. Skipped under --headless to keep
     * that path deterministic: psyq_VSync advances `openpete_vbl`
     * synchronously when no ticker is running. */
    {
        /* The pacing measurement harness needs the ticker active even under
         * --headless to capture a paced baseline; OPENPETE_FORCE_PACING=1
         * opts in. */
        const char* fp = getenv("OPENPETE_FORCE_PACING");
        bool force_pacing = (fp && *fp && *fp != '0');
        if (!opts->headless || force_pacing) {
            openpete_ticker_start();
        }
    }

    /* The mouse channel: event-derived state, totals, the injection seam.
     * Headless keeps PsyCross's hidden window, so the channel initialises
     * there too; the native window, when there is one, is found lazily. */
    openpete_host_mouse_init(g_window);

    return 0;
}

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
static int synth_pads_enabled(void) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = getenv("OPENPETE_SYNTH_PADS");
        if (!v || !*v || *v == '0')       s_on = 0;
        else if (v[0] == 'v' || v[0] == 'V') s_on = 2;
        else                              s_on = 1;
    }
    return s_on;
}

/** @brief Compute the synthetic pad bits and axes for a VBL index and slot.
 *
 * With k = frame * 7 + slot * 1361, bits has bit (k % 16) cleared (buttons
 * are active low) and the axes are k, k / 3, k / 7 and k / 11, each modulo
 * 251. A pure function of its inputs.
 * @param frame The VBL index.
 * @param slot The pad slot.
 * @param bits Receives the button bits.
 * @param axes Receives the four axis bytes. */
static void synth_pad_for(unsigned frame, int slot,
                          uint16_t* bits, unsigned char axes[4]) {
    /* Distinct per slot and per VBL, and never 0xFFFF-with-centred-sticks,
     * so a dropped port is not mistaken for a legitimately idle one. */
    unsigned k = frame * 7u + (unsigned)slot * 1361u;
    *bits   = (uint16_t)~(1u << (k % 16));      /* one button held */
    axes[0] = (unsigned char)(k % 251u);
    axes[1] = (unsigned char)((k / 3u) % 251u);
    axes[2] = (unsigned char)((k / 7u) % 251u);
    axes[3] = (unsigned char)((k / 11u) % 251u);
}

/** @brief Test whether slots 1 to last_slot of an input record carry exactly the synthetic stream for a VBL.
 *
 * @param rec The input record.
 * @param frame The VBL index the stream is generated for.
 * @param last_slot The highest slot checked.
 * @return 1 when every checked port is present with the synthetic bits and
 *         axes, else 0. */
static int synth_pads_match_upto(const openpete_input_rec_t* rec, unsigned frame,
                                 int last_slot) {
    for (int sl = 1; sl <= last_slot; sl++) {
        uint16_t b; unsigned char ax[4];
        synth_pad_for(frame, sl, &b, ax);
        const openpete_input_port_t* pt = &rec->port[sl];
        if (!pt->present || pt->bits != b || memcmp(pt->axes, ax, 4) != 0)
            return 0;
    }
    return 1;
}

/** @brief Test an input record against the synthetic stream over the slots the current mode fills.
 *
 * Generate-and-verify checks slots 1-3; verify-only checks slot 1 alone,
 * the one player-2 slot a two-player movie replay carries.
 * @param rec The input record.
 * @param frame The VBL index the stream is generated for.
 * @return 1 when every checked slot matches, else 0. */
static int synth_pads_match(const openpete_input_rec_t* rec, unsigned frame) {
    const int last = (synth_pads_enabled() == 2) ? 1 : OPENPETE_PAD_SLOTS - 1;
    return synth_pads_match_upto(rec, frame, last);
}

/* VBLs each pad slot was recorded present for. Slot 0 is every logged VBL by
 * construction; a zero on slots 1-3 is what separates "the extra players were
 * carried" from "nothing was ever plugged in", which no downstream comparison
 * can distinguish on its own. */
static unsigned long long s_slot_present_vbls[OPENPETE_PAD_SLOTS] = {0};

/* VBLs on which two slots carried different bytes. Presence alone cannot
 * catch a routing bug that fed one device into several slots: crossed slots
 * are present every VBL and identical every VBL. A pair that never differs
 * over a whole run was never independently routed. */
static unsigned long long s_slot_differed[OPENPETE_PAD_SLOTS][OPENPETE_PAD_SLOTS] = {{0}};

static unsigned long long s_synth_stage_checks = 0;
static unsigned long long s_synth_stage_mismatches = 0;
static unsigned long long s_synth_rewind_checks = 0;
static unsigned long long s_synth_rewind_mismatches = 0;

/* Pre-yield-staging equality instrument. The input record is staged before
 * the tick so an in-tick reader can see it, but the bytes the log stores for
 * slot 0 remain the post-yield guest readback. These count how often the two
 * were compared and how often they disagreed; a nonzero mismatch means an
 * in-tick slot-0 read would see bytes the log never recorded. */
static unsigned long long s_p1_stage_checks = 0;
static unsigned long long s_p1_stage_mismatches = 0;
/* First disagreement, kept for the report: a count alone cannot say whether
 * the cause is a one-off boot transient or a live decode path. */
static unsigned s_p1_stage_first_frame = 0;
static uint16_t s_p1_stage_first_staged_bits = 0;
static uint16_t s_p1_stage_first_read_bits = 0;
static unsigned char s_p1_stage_first_staged_axes[4] = {0};
static unsigned char s_p1_stage_first_read_axes[4] = {0};

/** @brief Return the actions that keep reading through the dev overlay's keyboard and pad suspensions.
 *
 * imgui's nav focus covers the whole panel while it is focused, so
 * suspending every action would leave the source that opened the panel
 * unable to close it, and the pad unable to navigate it: `devui` and every
 * KM_CTX_DEVUI action are exempt.
 * @param n receives the number of actions.
 * @return the actions, a static list. */
static const openpete_action_t* devui_suspend_exempt(int* n) {
    static const openpete_action_t kExempt[] = {
        OPK_DEVUI, OPK_DEVUI_UP, OPK_DEVUI_DOWN, OPK_DEVUI_LEFT,
        OPK_DEVUI_RIGHT, OPK_DEVUI_ACTIVATE, OPK_DEVUI_CANCEL, OPK_DEVUI_MENU,
        OPK_DEVUI_SLOWER, OPK_DEVUI_FASTER,
    };
    *n = (int)(sizeof kExempt / sizeof kExempt[0]);
    return kExempt;
}

/** @brief Write player 1's override channel as all buttons released and sticks centred.
 *
 * @param active 1 to make the neutral record the pad's input, 0 to hand the
 *        channel back to live decode (the overlays' clear-once latch). */
static void pad_override_neutral(int active) {
    g_PsyX_Pad_InputOverride  = 0xFFFF;
    g_PsyX_Pad_OverrideActive = active;
    g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
    g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
}

/** @brief Return whether a bk2 movie holds a row for this VBL.
 *
 * @param active a movie is loaded for replay.
 * @param row the movie row at the current row position (bk2_feed_row).
 * @param count the movie's row count.
 * @return true when row indexes a movie row. */
static bool bk2_frame_in_range(bool active, int row, int count) {
    return active && row >= 0 && row < count;
}

/** @brief Hold player 1's pad neutral while an overlay owns it, through the override channel.
 *
 * The settings menu owns the pad while open; a binding capture from its
 * start until the inputs it latched are released
 * (openpete_keymap_capture_neutral); the dev overlay while imgui
 * owns the keyboard (a text field or navigation focus, so typed characters
 * cannot reach the PSX pad through PsyCross's keyboard map) or navigates
 * with the pad (an overlay window holds navigation focus, so the buttons
 * that move through the panel do not also move Spyro; a click outside
 * every overlay window drops the focus and the pad returns to the game).
 * The sim keeps running behind either, so settings preview live. A bk2
 * frame in range outranks both: the movie's input stays the pad's input.
 * On the iteration an overlay lets go the channel is handed back to live
 * decode once, unless a movie is loaded, whose stamp rewrites it.
 * @param bk2_active a movie is loaded for replay.
 * @param bk2_in_range the movie holds a frame for this VBL.
 * @param prev_hold the previous iteration's hold, updated.
 * @return true when this call claimed the override channel. */
static bool overlay_pad_stamp(bool bk2_active, bool bk2_in_range, bool* prev_hold) {
    const bool hold = openpete_menu_is_open() ||
                      openpete_devui_wants_keyboard() ||
                      openpete_devui_padnav_engaged() ||
                      openpete_keymap_capture_neutral();
    const bool was = *prev_hold;
    *prev_hold = hold;
    if (hold && !bk2_in_range) {
        pad_override_neutral(1);
        return true;
    }
    if (!hold && was && !bk2_active) pad_override_neutral(0);
    return false;
}

/** @brief Suspend the hotkey sources the dev overlay owns this iteration.
 *
 * While imgui owns the keyboard every keyboard-sourced action goes inert;
 * while the overlay navigates with the pad every live pad-sourced action
 * does too, so the buttons that move through the panel fire no savestate,
 * freecam or other pad-bound action, and the freecam axes are zeroed as
 * the settings menu zeroes them. `devui` and the KM_CTX_DEVUI actions are
 * exempt (devui_suspend_exempt), except from the keyboard suspension while
 * a text field takes input, which swallows every key. While the settings
 * menu is open every live pad source outside the menu's own actions and
 * the overlay toggles (openpete_keymap_menu_pad_exempt) is suspended the
 * same way, so a hold: pad hotkey sharing a button with menu navigation
 * never fires in the menu. A binding capture zeroes the freecam axes too.
 * Esc-close works through the event pump. */
static void overlay_input_suspend(void) {
    int n_exempt = 0;
    const openpete_action_t* exempt = devui_suspend_exempt(&n_exempt);
    const bool padnav = openpete_devui_padnav_engaged() != 0;
    openpete_keymap_suspend_keyboard(
        openpete_devui_wants_keyboard() ? 1 : 0,
        openpete_devui_wants_text_input() ? NULL : exempt, n_exempt);
    if (padnav) {
        openpete_keymap_suspend_pad(1, exempt, n_exempt);
    } else if (openpete_menu_is_open()) {
        int n_menu = 0;
        const openpete_action_t* menu_ex = openpete_keymap_menu_pad_exempt(&n_menu);
        openpete_keymap_suspend_pad(1, menu_ex, n_menu);
    } else {
        openpete_keymap_suspend_pad(0, NULL, 0);
    }
    const bool zero = padnav || openpete_keymap_capture_neutral();
    if (!zero || !(openpete_freecam_active() || openpete_camlock_active()))
        return;
    openpete_freecam_axes ax = {0};
    openpete_freecam_set_axes(&ax);
}

/* Snapshots (savestate, rewind keyframe, runahead) and loads happen only
 * while the game fiber is parked at a psyq_VSync wait's final yield
 * (openpete_yield_final). Below any other yield the fiber's host call stack
 * is mid-delivery (an intermediate VBL of VSync(n > 1), a polling-loop or
 * spin-tick VBL, a schedule charge, a flushed VBLANK, a stub-miss yield);
 * the stack is not snapshotted, so a state taken there or loaded there
 * resumes the wrong continuation.
 *
 * A load also needs the right final yield: each records its park site (the
 * psyq_VSync call's guest ra and sp, openpete_yield_site), every snapshot
 * stores the site it was taken at, and a load applies only while the fiber
 * is parked at a final yield with that site. At another site the loaded
 * registers would run the parked call's continuation (its caller's code
 * with the saved call's s-registers). A load that finds another site waits:
 * a queued file or slot load up to STATE_LOAD_SITE_WAIT final yields and
 * is refused after them; the rewind seek takes the newest keyframe saved
 * at the current site; the stress rollback waits up to its bound and then
 * re-arms; the runahead rollback continues the speculation to the
 * snapshot's site, through a transition when the speculation leaves
 * gameplay, and adopts it at the bound.
 *
 * One-shot requests (the save and load hotkeys, a mod's request, the
 * scheduled --state-save-on-frame / --state-load-on-frame) enter a queue
 * whenever they are raised; state_ops_drain runs the queue in arrival order
 * at a final yield and keeps it otherwise. The periodic state work (the
 * stress harnesses, the rewind keyframe cadence and seek, the process
 * handoff, the runahead snapshot) runs only on an iteration whose drain
 * reported a final yield, and sits out the others. */
typedef enum {
    STATE_OP_SAVE_SLOT,   /* openpete_state_save to a slot */
    STATE_OP_LOAD_SLOT,   /* openpete_state_load from a slot */
    STATE_OP_SAVE_FILE,   /* openpete_state_save_file to a path */
    STATE_OP_LOAD_FILE,   /* openpete_state_load_file from a path */
} state_op_kind;

typedef struct {
    state_op_kind kind;
    unsigned slot;        /* the slot of a slot op */
    const char* path;     /* the path of a file op */
    int deferred;         /* counted once in s_state_ops_deferred */
    unsigned site_waits;  /* final yields a load spent at another park site */
} state_op_t;

/* Final yields a queued load waits for the fiber to park at its site. A
 * game's tick parks at each of its frame-wait sites every tick, so a site
 * absent this long belongs to a mode the run has left (a pause menu, a
 * transition): loading at another site corrupts the timeline, so the load
 * is refused instead. 1800 final yields is at least 30 s of play. */
#define STATE_LOAD_SITE_WAIT 1800u

/* Park-site deferrals by kind: a load whose snapshot was saved at another
 * park site than the current final yield's. */
enum { SITE_DEF_LOAD, SITE_DEF_REWIND, SITE_DEF_STRESS, SITE_DEF_RUNAHEAD, SITE_DEF_N };
static unsigned long long s_site_deferred[SITE_DEF_N];
static unsigned long long s_site_refused = 0;    /* queued loads refused at STATE_LOAD_SITE_WAIT */
static unsigned long long s_site_wait_yields = 0; /* final yields queued loads spent at another site */
static unsigned long long s_site_abandoned = 0;  /* stress rollbacks re-armed at their bound */

#define STATE_OP_QUEUE_LEN 8
static state_op_t s_state_ops[STATE_OP_QUEUE_LEN];
static int s_state_op_n = 0;
static unsigned long long s_state_ops_deferred[2];  /* requests that waited: [0] saves, [1] loads */
static unsigned long long s_state_yields_skipped = 0;  /* drains at an intermediate yield */
/* Runahead's share of s_state_ops_deferred: [0] snapshots owed to a later
 * yield (ra_owed), [1] rollbacks whose speculation ended below a non-final
 * yield. */
static unsigned long long s_ra_deferred[2];
static unsigned long long s_ra_adopted = 0;  /* speculations adopted: no rollback at the snapshot's park site */
static unsigned long long s_ra_left[2];  /* speculations that left gameplay: [0] rolled back, [1] adopted */

/** @brief Count one runahead snapshot (kind 0) or rollback (kind 1) deferred to a final yield.
 * @param kind 0 for a snapshot, 1 for a rollback. */
static void ra_count_deferral(int kind) {
    s_ra_deferred[kind]++;
    s_state_ops_deferred[kind]++;
}

/** @brief Carry a runahead snapshot owed to a later yield, counting the request once when it first waits.
 * @param owe Nonzero when this yield owes the snapshot (due, not at a final yield).
 * @param owed The value carried from the previous yield.
 * @return The value to carry to the next yield. */
static int ra_note_owed(int owe, int owed) {
    if (owe && !owed) ra_count_deferral(0);
    return owe;
}

/** @brief Queue one savestate request for the next final yield.
 * @param kind The operation.
 * @param slot The slot of a slot operation.
 * @param path The path of a file operation. */
static void state_op_push(state_op_kind kind, unsigned slot, const char* path) {
    if (s_state_op_n == STATE_OP_QUEUE_LEN) {
        OPL_WARN_F("savestate", "state-op queue full (%d): request dropped", STATE_OP_QUEUE_LEN);
        return;
    }
    s_state_ops[s_state_op_n++] = (state_op_t){kind, slot, path, 0, 0u};
}

/** @brief Run one queued savestate request; a load that succeeds re-anchors the host frame counter.
 *
 * The re-anchor makes the one-shot triggers (screenshot-on-frame,
 * ram-dump-on-frame, frame-limit) reason about time from the load as on a
 * fresh run.
 * @param op The request.
 * @return 1 when a load found the fiber parked at another site than its
 *         state's (nothing restored), else 0. */
static int state_op_run(const state_op_t* op) {
    unsigned lf = openpete_frame;
    int rc = -1;
    switch (op->kind) {
    case STATE_OP_SAVE_SLOT:
        (void)openpete_state_save(op->slot, &g_game.cpu, openpete_frame);
        return 0;
    case STATE_OP_SAVE_FILE:
        (void)openpete_state_save_file(op->path, &g_game.cpu, openpete_frame);
        return 0;
    case STATE_OP_LOAD_SLOT:
        rc = openpete_state_load(op->slot, &g_game.cpu, &lf);
        break;
    case STATE_OP_LOAD_FILE:
        rc = openpete_state_load_file(op->path, &g_game.cpu, &lf);
        break;
    }
    if (rc == 0) openpete_frame = lf;
    return rc == 1;
}

/** @brief Keep a load that found another park site, or refuse it once it has waited STATE_LOAD_SITE_WAIT final yields.
 * @param op The load request.
 * @return 1 when the request stays queued, 0 when it was refused. */
static int state_op_site_wait(state_op_t* op) {
    s_site_wait_yields++;
    if (op->site_waits++ == 0u) s_site_deferred[SITE_DEF_LOAD]++;
    if (op->site_waits < STATE_LOAD_SITE_WAIT) return 1;
    s_site_refused++;
    OPL_ERROR_F("savestate", "load of %s refused: the fiber did not park at the state's "
                "site within %u final yields (parked at ra=0x%08X sp=0x%08X); running "
                "state untouched", op->kind == STATE_OP_LOAD_FILE ? op->path : "the slot",
                STATE_LOAD_SITE_WAIT, openpete_yield_site.ra, openpete_yield_site.sp);
    return 0;
}

/** @brief Run every queued savestate request at a final yield, or keep the queue and count the wait.
 *
 * Requests run in arrival order. A load whose state was saved at another
 * park site stops the drain there: it and every later request stay queued
 * for the next final yield (state_op_site_wait).
 * @return 1 at a final yield (the queue ran and the periodic state work may run), else 0. */
static int state_ops_drain(void) {
    if (!openpete_yield_final) {
        s_state_yields_skipped++;
        for (int i = 0; i < s_state_op_n; i++) {
            state_op_t* op = &s_state_ops[i];
            if (op->deferred) continue;
            op->deferred = 1;
            s_state_ops_deferred[op->kind == STATE_OP_LOAD_SLOT || op->kind == STATE_OP_LOAD_FILE]++;
        }
        return 0;
    }
    int done = 0;
    while (done < s_state_op_n) {
        state_op_t* op = &s_state_ops[done];
        if (state_op_run(op) && state_op_site_wait(op)) break;
        done++;
    }
    memmove(s_state_ops, s_state_ops + done, (size_t)(s_state_op_n - done) * sizeof s_state_ops[0]);
    s_state_op_n -= done;
    return 1;
}

/** @brief Continue a runahead speculation until the game fiber parks at a final yield at the snapshot's park site, so the rollback loads only there.
 *
 * The rollback restores a snapshot taken at a final yield and the fiber then
 * resumes wherever it is parked; below any other yield, or below a final
 * yield of another psyq_VSync call site, its host call stack is not the one
 * the snapshot continues from. The speculation loop ends at a final yield
 * when it breaks on the tick count, but that yield can be another site
 * than the snapshot's (a game with two frame-wait sites), and its yield
 * bound, its no-advance break (the no-tick yield after a stubbed dispatch)
 * and its break on leaving gameplay can leave the fiber at an intermediate
 * yield or at another site. Speculation then continues with the same held
 * input, the rollback counted as deferred (and as a park-site deferral when
 * the fiber was at a final yield), until the site or the bound. A
 * speculation that has left gameplay waits only to its first final yield:
 * a transition that waits at the snapshot's psyq_VSync site rolls back
 * there, and one that waits at another site has moved its frame wait, so
 * the speculation is adopted at once rather than after further held-input
 * VBLs. A site that does not recur within the bound leaves the speculation
 * adopted: no rollback can be applied at another site.
 * @param site The snapshot's park site.
 * @param cap Further yields allowed.
 * @param left In: nonzero when the speculation already left gameplay. Out:
 *        also set when the continuation leaves it.
 * @return 0 when parked at the site (the rollback may load), 1 when the
 *         speculation is adopted. */
static int ra_speculate_to_final(openpete_park_site_t site, int cap, int* left) {
    if (openpete_park_site_matches(&site)) return 0;
    ra_count_deferral(1);
    if (openpete_yield_final) s_site_deferred[SITE_DEF_RUNAHEAD]++;
    OPL_DEBUG_F("runahead", "speculation ended at f=%u %s; rollback deferred to the "
                "snapshot's park site", openpete_frame,
                openpete_yield_final ? "at another park site" : "below an intermediate yield");
    for (int v = 0; v < cap && !(*left && openpete_yield_final); v++) {
        game_on_frame_pre(&g_game.cpu, openpete_frame);
        g_openpete_guest_running = 1;
        co_switch((cothread_t)openpete_game_fiber);
        g_openpete_guest_running = 0;
        if (!game_is_gameplay_live(&g_game.cpu)) *left = 1;
        if (openpete_park_site_matches(&site)) return 0;
    }
    s_ra_adopted++;
    return 1;
}

/** @brief Log the end of a speculation that left gameplay or was adopted, and count it.
 * @param left Nonzero when the speculation left gameplay.
 * @param adopted Nonzero when it became the timeline (its park site did not
 *        recur within the bound); zero when it was rolled back. */
static void ra_end_log(int left, int adopted) {
    if (left) s_ra_left[adopted ? 1 : 0]++;
    if (!adopted)
        OPL_INFO_F("runahead", "speculation left gameplay; rolled back at the snapshot's "
                   "park site (led f=%u)", openpete_frame);
    else if (left)
        OPL_INFO_F("runahead",
                   "speculation left gameplay and parked at another site than the "
                   "snapshot's; adopted the led timeline through f=%u (runahead idle "
                   "until gameplay resumes)", openpete_frame);
    else
        OPL_WARN_F("runahead",
                   "speculation reached no final yield at the snapshot's park site within "
                   "its bound; adopted the led timeline through f=%u", openpete_frame);
}

/** @brief Queue the savestate requests raised this iteration: the hotkeys, a mod's request and the scheduled one-shots.
 *
 * The scheduled save is queued before the scheduled load, so a save and a load
 * armed for the same tick capture and then restore (a same-tick roundtrip).
 * @param opts Run options (the --state-*-on-frame one-shots).
 * @param slot The active hotkey slot. */
static void state_ops_collect(const openpete_options_t* opts, unsigned slot) {
    static bool sched_saved = false, sched_loaded = false;
    if (openpete_key_pressed(OPK_STATE_SAVE)) state_op_push(STATE_OP_SAVE_SLOT, slot, NULL);
    if (openpete_key_pressed(OPK_STATE_LOAD)) state_op_push(STATE_OP_LOAD_SLOT, slot, NULL);
    unsigned mod_slot = 0;
    const int mod_op = openpete_mods_pending_state_op(&mod_slot);
    if (mod_op) state_op_push(mod_op == 2 ? STATE_OP_LOAD_SLOT : STATE_OP_SAVE_SLOT, mod_slot, NULL);
    const int key = (int)openpete_feed_key_now();
    if (!sched_saved && opts->state_save_on_frame >= 0 && key >= opts->state_save_on_frame) {
        sched_saved = true;
        state_op_push(STATE_OP_SAVE_FILE, 0, opts->state_save_output);
    }
    if (!sched_loaded && opts->state_load_on_frame >= 0 && key >= opts->state_load_on_frame) {
        sched_loaded = true;
        if (!opts->state_load_input)
            OPL_ERROR_F("savestate", "--state-load-on-frame armed but no --state-load-input");
        else
            state_op_push(STATE_OP_LOAD_FILE, 0, opts->state_load_input);
    }
}

static unsigned s_stress_it = 0, s_stress_ops = 0;  /* iterations in the current phase; operations done */

/** @brief --state-stress N: every N iterations, an in-memory save and an immediate quiet restore; a stressed run must be hash-identical to a clean one.
 * @param sm The stress snapshot.
 * @param every N. */
static void state_stress_roundtrip(openpete_state_mem_t* sm, unsigned every) {
    if (s_stress_it % every != 0) return;
    if (openpete_state_mem_save(sm, &g_game.cpu, openpete_frame) != 0 ||
        openpete_state_mem_load(sm, &g_game.cpu, NULL, 1) != 0)
        return;
    if ((++s_stress_ops & 0x3FFu) == 1u)
        OPL_INFO_F("savestate", "stress: %u roundtrips (f=%u)", s_stress_ops, openpete_frame);
}

/** @brief --state-stress-rewind K: save, run K iterations, restore, replay K (the runahead rollback shape); the replayed window must self-match.
 *
 * Three phases so the timeline still advances: 0 arms (save at F), 1 runs
 * K and rolls back to F, 2 replays K and re-arms at F+K. Armed only in
 * steady-state gameplay: the mem snapshot excludes the boot and load-window
 * sections (VRAM, SPU sample RAM) by contract, so a pending rollback whose
 * window crosses into a transition is abandoned, as runahead is gated off
 * around loads. The rollback applies only at the snapshot's park site: at
 * another site it waits (counted once per rollback), up to 8K + 16
 * iterations past K, the runahead speculation's bound; a site that does not
 * recur by then abandons the rollback and re-arms at the current site.
 * @param sm The stress snapshot.
 * @param k K. */
static void state_stress_rewind(openpete_state_mem_t* sm, unsigned k) {
    static int armed = 0;
    const int live = game_is_gameplay_live(&g_game.cpu);
    if (armed == 1 && !live) armed = 0;
    if (armed == 0) {
        if (live && openpete_state_mem_save(sm, &g_game.cpu, openpete_frame) == 0) armed = 1;
        s_stress_it = 0;
        return;
    }
    if (s_stress_it < k) return;
    if (armed == 2) { armed = 0; return; }   /* replay done: re-arm here */
    unsigned lf = openpete_frame;
    const int rc = openpete_state_mem_load(sm, &g_game.cpu, &lf, 1);
    if (rc == 1) {
        if (s_stress_it == k) s_site_deferred[SITE_DEF_STRESS]++;
        if (s_stress_it >= 9u * k + 16u) {
            s_site_abandoned++;
            armed = 0;
        }
        return;
    }
    if (rc == 0) {
        openpete_frame = lf;
        if ((++s_stress_ops & 0xFFu) == 1u)
            OPL_INFO_F("savestate", "stress-rewind: %u rollbacks (back to f=%u)", s_stress_ops, lf);
    }
    armed = 2;
    s_stress_it = 0;
}

/** @brief Step the savestate stress harnesses (restore-at-yield checks) by one quiescent iteration.
 * @param opts Run options (--state-stress, --state-stress-rewind). */
static void state_stress_step(const openpete_options_t* opts) {
    static openpete_state_mem_t* sm = NULL;
    if (opts->state_stress <= 0 && opts->state_stress_rewind <= 0) return;
    if (!sm) sm = openpete_state_mem_new();
    if (!sm) return;
    s_stress_it++;
    if (opts->state_stress > 0) state_stress_roundtrip(sm, (unsigned)opts->state_stress);
    if (opts->state_stress_rewind > 0) state_stress_rewind(sm, (unsigned)opts->state_stress_rewind);
}

/** @brief Stage the input log's record for the tick being re-executed, every slot as recorded, with player 1 as the pad override.
 *
 * A re-executed tick reads every slot exactly as the log recorded it, or
 * the burst replays player 1 faithfully and the other players from stale
 * bytes. Synthetic pads are checked against the log, their only carrier.
 * @param r The record, or NULL when the log has none (nothing staged). */
static void rewind_stage_logged_input(const openpete_input_rec_t* r) {
    if (!r) return;
    input_log_stage_reset(openpete_frame);
    for (int sl = 0; sl < OPENPETE_PAD_SLOTS; sl++) {
        const openpete_input_port_t* pt = &r->port[sl];
        if (pt->present) input_log_stage_port(sl, pt->bits, pt->axes);
    }
    if (synth_pads_enabled()) {
        s_synth_rewind_checks++;
        if (!synth_pads_match(input_log_staged(), openpete_frame)) s_synth_rewind_mismatches++;
    }
    const openpete_input_port_t* p1 = &r->port[0];
    g_PsyX_Pad_InputOverride = p1->bits;
    g_PsyX_Pad_AnalogOverride[0] = p1->axes[2];
    g_PsyX_Pad_AnalogOverride[1] = p1->axes[3];
    g_PsyX_Pad_AnalogOverride[2] = p1->axes[0];
    g_PsyX_Pad_AnalogOverride[3] = p1->axes[1];
    g_PsyX_Pad_OverrideActive = 1;
}

/** @brief Step the timeline back one tick: quiet-load the covering keyframe, replay forward to the target with the logged inputs, truncate the future.
 *
 * Interp stores are reset (cheap memsets) so presents snap backward; the
 * full discontinuity broadcast (texture-pack rescan) is not fired per held
 * iteration. Replayed ticks run the main loop's pre-yield hook (per-tick
 * guest mutations are part of the timeline) with presentation
 * announcements and pad-read pacing suspended.
 * The keyframe is the newest at or before the target saved at the park
 * site the fiber is parked at: only that keyframe can be loaded here, and
 * the logged-input replay reaches the target from it whatever its age. With
 * no such keyframe in the window the step is deferred (counted) and the
 * tick runs forward, so a held rewind resumes at the next final yield whose
 * site a keyframe carries.
 * @param bk2_active A movie is loaded for replay (it keeps the pad override).
 * @return true when the timeline moved. */
static bool rewind_seek_one_tick(bool bk2_active) {
    const unsigned target = openpete_frame > 2u ? openpete_frame - 2u : 0u;
    const openpete_rewind_kf_t* kf = openpete_rewind_pick(target, &openpete_yield_site);
    if (!kf) {
        if (openpete_rewind_pick(target, NULL)) {
            s_site_deferred[SITE_DEF_REWIND]++;
            OPL_DEBUG_F("rewind", "no keyframe at or before f=%u was saved at the park site "
                        "ra=0x%08X sp=0x%08X; step deferred", target,
                        openpete_yield_site.ra, openpete_yield_site.sp);
            return false;
        }
        OPL_WARN_F("rewind", "no keyframe covers f=%u (window exhausted or none captured)", target);
        return false;
    }
    unsigned lf = openpete_frame;
    if (openpete_rewind_restore(kf, &g_game.cpu, &lf) != 0) return false;
    openpete_frame = lf;
    openpete_interp_reset();
    openpete_interp_objs_reset();
    openpete_replaying = 1;
    PsyX_SPUAL_Speculate(1, 0);
    while (openpete_frame < target) {
        rewind_stage_logged_input(input_log_get(kf->log_idx + (int)(openpete_frame - kf->frame)));
        const unsigned before = openpete_frame;
        game_on_frame_pre(&g_game.cpu, openpete_frame);
        openpete_pace_vbl_suspend(1);
        g_openpete_guest_running = 1;
        co_switch((cothread_t)openpete_game_fiber);
        g_openpete_guest_running = 0;
        openpete_pace_vbl_suspend(0);
        if (openpete_frame == before) break;   /* the fiber made no tick */
    }
    openpete_replaying = 0;
    PsyX_SPUAL_Speculate(0, 0);
    input_log_truncate(kf->log_idx + (int)(target - kf->frame));
    openpete_rewind_drop_after(openpete_frame);
    if (!bk2_active) g_PsyX_Pad_OverrideActive = 0;
    return true;
}

/** @brief Report whether every enabled code mod declares its host-state posture (mod.toml state=), warning once when one does not.
 *
 * An undeclared mod's host state desyncs silently when the world moves
 * under it, so a timeline jump is refused while one is enabled.
 * @return true when a timeline jump is allowed. */
static bool rewind_mods_allow(void) {
    static bool hinted = false;
    const char* offender = NULL;
    if (openpete_mods_state_safe(&offender)) return true;
    if (!hinted) {
        hinted = true;
        OPL_WARN_F("rewind", "refused: mod \"%s\" declares no state posture (add state = \"none\" "
                   "or \"rebuildable\" to its mod.toml)", offender ? offender : "?");
    }
    return false;
}

/** @brief Report whether the rewind key is held this iteration: the key, or the --rewind-hold A:N driver once feed key A is reached.
 *
 * The driver latches at A, or at the first VBL from A on that meets
 * OPENPETE_REWIND_HOLD_PHASE (a held key does not release when the frame
 * drops below the trigger), and holds for N iterations.
 * @param opts Run options (--rewind-hold).
 * @param live The game is in live gameplay.
 * @param paused The host is paused.
 * @return true when held. */
static bool rewind_key_held(const openpete_options_t* opts, int live, bool paused) {
    static int held = 0, armed = 0;
    bool k = openpete_key_down(OPK_REWIND);
    if (opts->rewind_hold_frame >= 0 && !armed &&
        (int)openpete_feed_key_now() >= opts->rewind_hold_frame &&
        openpete_rewind_hold_phase_ready(openpete_frame))
        armed = 1;
    if (armed && held < opts->rewind_hold_iters) {
        k = true;
        held++;
        OPL_INFO_F("rewind", "hold %d/%d at f=%u (live=%d paused=%d)",
                   held, opts->rewind_hold_iters, openpete_frame, live, (int)paused);
    }
    return k;
}

/** @brief Run one quiescent iteration of rewind: the keyframe cadence, then one tick back while the rewind key is held in live, unpaused gameplay.
 * @param opts Run options.
 * @param paused The host is paused.
 * @param bk2_active A movie is loaded for replay.
 * @return true when the timeline moved (the iteration skips its forward tick). */
static bool rewind_step(const openpete_options_t* opts, bool paused, bool bk2_active) {
    if (!openpete_rewind_enabled()) {
        static bool hinted = false;
        if (!hinted && openpete_key_down(OPK_REWIND)) {
            hinted = true;
            OPL_WARN_F("rewind", "the rewind key was pressed but rewind is disabled - launch with "
                       "--rewind 30 to enable a 30 s window");
        }
        return false;
    }
    const int live = game_is_gameplay_live(&g_game.cpu);
    openpete_rewind_on_iter(&g_game.cpu, openpete_frame, live);
    const bool held = rewind_key_held(opts, live, paused);
    if (!held || !live || paused || !rewind_mods_allow()) return false;
    return rewind_seek_one_tick(bk2_active);
}

/** @brief Log the vibration census: PadSetAct registrations, motor commands, per-pad recorders, mod slots.
 *
 * Logged unconditionally because the counts are read together. A recorder
 * line appears only for a virtual pad with a callback or a live recorder,
 * and a mod line only for a slot that issued commands. */
static void rumble_census_report(void) {
    /* Vibration census. Emitted unconditionally because the numbers only mean
     * something together, and every one of them is a zero in some legitimate
     * configuration:
     *
     *   registered/nonzero  the diagnosis. libpad's PadSetAct is a
     *                       registration, so the expected reading is a small
     *                       number of calls all carrying {0,0} - a NON-zero
     *                       registration would mean the game got its buffer
     *                       filled before handing it over, and the one-shot
     *                       reading of PadSetAct was not the whole story.
     *   commands            SDL rumble calls issued. Zero is correct headless,
     *                       unfocused, at --vibration 0, or with no pad.
     *   recorder            the virtual-pad oracle: `live` says the self-probe
     *                       arrived, and without that a zero motor count is
     *                       indistinguishable from a recorder that was never
     *                       wired. Never read the counts without it. */
    {
        uint16_t low = 0, high = 0;
        openpete_rumble_last_pair(&low, &high);
        OPL_INFO_F("rumble",
                   "census: PadSetAct %u call(s), %u carrying non-zero bytes; "
                   "%u motor command(s) issued, last pair {low %u, high %u}",
                   openpete_rumble_register_calls(),
                   openpete_rumble_register_nonzero(),
                   openpete_rumble_commands(), (unsigned)low, (unsigned)high);
        /* Per virtual pad, because "which device received it" is the claim a
         * per-slot actuator path has to answer and a total cannot. */
        for (int v = 0; v < OPENPETE_PAD_SLOTS; v++) {
            if (!openpete_host_input_vpad_rumble_calls(v) &&
                !openpete_host_input_vpad_rumble_live(v))
                continue;
            unsigned vlow = 0, vhigh = 0;
            openpete_host_input_vpad_rumble_last(v, &vlow, &vhigh);
            OPL_INFO_F("rumble",
                       "recorder: pad %d live=%d, %d callback(s), %d non-zero, "
                       "last pair {low %u, high %u}", v,
                       openpete_host_input_vpad_rumble_live(v),
                       openpete_host_input_vpad_rumble_calls(v),
                       openpete_host_input_vpad_rumble_nonzero(v),
                       vlow, vhigh);
        }
        for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++) {
            if (!openpete_rumble_mod_commands(sl)) continue;
            uint16_t mlow = 0, mhigh = 0;
            openpete_rumble_mod_pair(sl, &mlow, &mhigh);
            OPL_INFO_F("rumble",
                       "mod slot %d: %u motor command(s), pair {low %u, high %u}",
                       sl, openpete_rumble_mod_commands(sl),
                       (unsigned)mlow, (unsigned)mhigh);
        }
    }
}

/** @brief Poll the fullscreen gestures (Alt+Enter edge, the `fullscreen` binding) and route the toggle to the window's owner.
 *
 * SDL_SetWindowFullscreen belongs to the thread that owns the window: under
 * the thread split this loop raises the request on its own edge and main
 * consumes it; otherwise this loop consumes every raise (its own edge, a
 * double-click, a request from another thread) and toggles.
 * @param split Whether the thread split is active.
 * @param prev_alt_enter Whether Alt+Enter was down at the previous poll.
 * @return Whether Alt+Enter is down now. */
static bool fullscreen_poll(bool split, bool prev_alt_enter)
{
    const bool alt_enter = openpete_keymap_scancode_down(SDL_SCANCODE_RETURN) &&
                           (openpete_host_input_modstate() & SDL_KMOD_ALT) != 0;
    const bool own_edge = (alt_enter && !prev_alt_enter) ||
                          openpete_key_pressed(OPK_FULLSCREEN);
    if (split) {
        if (own_edge) SDL_SetAtomicInt(&g_native_fs_toggle_request, 1);
    } else if (SDL_SetAtomicInt(&g_native_fs_toggle_request, 0) || own_edge) {
        openpete_native_toggle_fullscreen();
    }
    return alt_enter;
}

/** @brief Commit the canonical present's 2D capture, then resolve the framebuffer windows it names.
 *
 * Runs once per canonical present, after the fade commit and before any
 * re-extract or present of the tick. The 2D-UI overlay (fade, letterbox,
 * menu panel, 2D sprites and lines) is captured from the canonical render's
 * scene before a re-extract wipes it; sub-ticks only re-apply it, since their
 * scene already holds the re-applied set. With the spine off (unlock, paused,
 * interpolation disabled, headless) the capture still commits, so the carry
 * always holds the last canonical present's 2D set for the headless
 * intermediate leg and for the first sub-tick present after unlock. A pending
 * framebuffer-window copy is then composed from that scene, which by now
 * holds the tick's 2D, and cropped into its window before the tick's
 * re-extract and present, so the present that first draws the window draws
 * its image. The device belongs to the present thread while eager production
 * is engaged, so the compose takes it back and returns it. Both steps are a
 * no-op without a native scene. */
static void run_canonical_capture(void)
{
    if (!openpete_native_scene_enabled()) return;
    openpete_native_scene_capture_overlay();
    if (!openpete_fb_snapshot_pending()) return;
    const int pt_was = openpete_present_thread_active();
    if (pt_was) openpete_present_thread_set_active(0);
    openpete_native_render_snapshot_fb_window();
    if (pt_was) openpete_present_thread_set_active(1);
}

/* Scripted pause for windowed harnesses, OPENPETE_PAUSE_AT_VBL=N[,M[,K]]:
 * pause at VBL N through the dev overlay's pause request (the pause key's
 * path), then, after every M paused idle iterations (each one repaints the
 * held scene where a repaint runs), request one step, K steps in all (M
 * defaults to 30, K to 1), and after M more request the resume. Each transition logs a frame-step line the harness parses.
 * Unset or malformed leaves it disarmed. */
static struct {
    int      armed;      /* -1 unread, 0 off, 1 on */
    unsigned vbl;        /* N */
    int      every;      /* M */
    int      steps;      /* K */
    int      stage;      /* 0 waiting for N, 1 paused, 2 done */
    int      idle;       /* paused idle iterations since the last transition */
    int      stepped;    /* steps requested so far */
} s_pause_at = { -1, 0, 30, 1, 0, 0, 0 };

/* Set when a paused step finished; host_pause_step logs it on the next
 * iteration, after that iteration's canonical present counted its tick. */
static int s_step_done_log;
/* VBLs the pending step has run. A step with no draw for STEP_MAX_VBLS
 * VBLs (about 10 s) ends with a warning and the sim stays paused. */
static unsigned s_step_vbls;
#define STEP_MAX_VBLS 600u

/** @brief Drive the OPENPETE_PAUSE_AT_VBL schedule for one host iteration, before the pause and step requests are read.
 *
 * Raises the same requests the dev overlay's Pause sim row and Step tick
 * button raise, so the run takes the pause key's path. A step is requested
 * only once the previous one has finished (no step pending).
 * @param paused The host loop's paused flag.
 * @param step_one The host loop's pending-step flag. */
static void pause_at_vbl_drive(bool paused, bool step_one)
{
    if (s_pause_at.armed < 0) {
        const char *e = getenv("OPENPETE_PAUSE_AT_VBL");
        unsigned n = 0; int m = 30, k = 1;
        const int got = (e && *e) ? sscanf(e, "%u,%d,%d", &n, &m, &k) : 0;
        s_pause_at.armed = got >= 1 && m > 0 && k >= 0;
        s_pause_at.vbl = n; s_pause_at.every = m; s_pause_at.steps = k;
    }
    if (!s_pause_at.armed || s_pause_at.stage == 2) return;
    if (s_pause_at.stage == 0) {
        if (openpete_feed_key_now() >= s_pause_at.vbl && !paused) {
            openpete_sim_pause_request(1);
            s_pause_at.stage = 1; s_pause_at.idle = 0;
        }
        return;
    }
    if (!paused || step_one || s_pause_at.idle < s_pause_at.every) return;
    s_pause_at.idle = 0;
    if (s_pause_at.stepped < s_pause_at.steps) {
        s_pause_at.stepped++;
        openpete_sim_step_request();
    } else {
        openpete_sim_pause_request(0);
        s_pause_at.stage = 2;
    }
}

/** @brief Report whether this poll is the menu_close release that closes the open settings menu.
 *
 * The close fires on the release edge of a menu_close source. A source held
 * while the menu was still closed (Start held through the hold that opens
 * the menu, which binds both `menu` as hold:pad:start and `menu_close` as
 * pad:start) is stale: its release clears the stale flag instead of closing
 * the menu, so only a press that began with the menu open closes it.
 * @param close_k Whether menu_close reads held on this poll.
 * @param prev_close Whether menu_close read held on the previous open poll.
 * @param owner_changed Whether the mouse owner changed on this poll; a
 *        release on such a poll is the arbitrated view going neutral.
 * @param stale In: a menu_close source has been held since before the menu
 *        opened. Cleared once menu_close reads released.
 * @return True when the menu closes on this poll. */
static bool menu_close_released(bool close_k, bool prev_close,
                                bool owner_changed, bool *stale)
{
    const bool edge = !close_k && prev_close && !owner_changed && !*stale;
    if (!close_k) *stale = false;
    return edge;
}

/** @brief Decide whether the VBL about to run belongs to a paused step, clearing the step flag when it does not.
 *
 * @param paused The host loop's paused flag.
 * @param step_one The host loop's pending-step flag.
 * @return True when the VBL runs for a pending paused step. */
static bool step_begin_vbl(bool paused, bool *step_one)
{
    const bool stepping = paused && *step_one;
    if (!stepping) *step_one = false;
    return stepping;
}

/** @brief End a paused step on the VBL the game drew, its tick boundary.
 *
 * A VBL with no draw (the first VBL of a two-VBL tick, or a load) keeps the
 * step pending into the next; after STEP_MAX_VBLS VBLs with no draw the step
 * ends with a warning and the sim stays paused.
 * @param stepping Whether the VBL ran for a paused step (step_begin_vbl); false returns at once.
 * @param step_one The host loop's pending-step flag, lowered when the step ends. */
static void step_after_vbl(bool stepping, bool *step_one)
{
    if (!stepping) return;
    s_step_vbls++;
    if (openpete_present_pending) {
        *step_one = false;
        s_step_done_log = 1;
    } else if (s_step_vbls >= STEP_MAX_VBLS) {
        *step_one = false;
        OPL_WARN_F("frame-step", "no tick completed in %u VBLs; step ended "
                   "at openpete_frame=%u", s_step_vbls, openpete_frame);
        s_step_vbls = 0;
    }
}

/* Hold time after which a held step key repeats, one tick at a time. */
#define STEP_REPEAT_DELAY_NS ((op_time_t)300 * 1000 * 1000)

/** @brief Apply the pause and step keys and the dev overlay's pause and step requests for one host iteration.
 *
 * Pause is an edge-triggered toggle. Step raises the pending-step flag on
 * the key's press edge while paused; the flag stays raised across VBLs until
 * the game draws (its tick boundary), so one press is one tick whether the
 * game's tick spans one VBL or two. A key held for STEP_REPEAT_DELAY_NS
 * re-raises the flag each time the previous step finishes, so holding steps
 * tick after tick. The overlay's requests take the same paths: a pause
 * request that differs from the state toggles it, a step request steps one
 * tick. Pausing mutes the audio and silences the motors; resuming restores
 * the live mute baseline. Publishes the paused state for
 * openpete_sim_paused.
 * @param paused The host loop's paused flag, toggled here.
 * @param step_one The host loop's one-tick flag, raised here while paused. */
static void host_pause_step(bool *paused, bool *step_one)
{
    if (s_step_done_log) {
        s_step_done_log = 0;
        OPL_INFO_F("frame-step", "stepped to openpete_frame=%u canon_tick=%u vbls=%u",
                   openpete_frame, openpete_canon_tick, s_step_vbls);
        s_step_vbls = 0;
    }
    pause_at_vbl_drive(*paused, *step_one);
    static op_time_t step_held_since;
    const bool step_edge = openpete_key_pressed(OPK_FRAME_STEP);
    if (step_edge) step_held_since = op_now();
    const bool step_repeat = !step_edge && openpete_key_down(OPK_FRAME_STEP) &&
                             op_now() - step_held_since >= STEP_REPEAT_DELAY_NS;
    const bool step_k = step_edge || step_repeat ||
                        SDL_SetAtomicInt(&s_step_req, 0) != 0;
    const int pause_req = SDL_SetAtomicInt(&s_pause_req, -1);
    if (openpete_key_pressed(OPK_PAUSE) ||
        (pause_req >= 0 && (pause_req != 0) != *paused)) {
        *paused = !*paused;
        /* Enter: mute. Exit: restore the live mute baseline (boot --mute,
         * possibly toggled since via the dev overlay). */
        extern int PsyX_SPUAL_SetMute(int on_off);
        PsyX_SPUAL_SetMute(*paused ? 1 : (s_mute || s_skip_mute));
        /* A paused game is not producing actuator bytes, but the last pair
         * it published would keep being re-issued; silence the motors for the
         * same reason the audio mutes. */
        openpete_rumble_set_paused(*paused ? 1 : 0);
        OPL_INFO_F("frame-step", "%s at openpete_frame=%u canon_tick=%u",
                   *paused ? "paused" : "resumed", openpete_frame, openpete_canon_tick);
    }
    if (!*paused) { *step_one = false; s_step_vbls = 0; }
    else if (step_k && !*step_one) {
        *step_one = true;
        OPL_INFO_F("frame-step", "step requested at openpete_frame=%u canon_tick=%u",
                   openpete_frame, openpete_canon_tick);
    }
    SDL_SetAtomicInt(&s_paused_shown, *paused ? 1 : 0);
}

/** @brief Present the held scene on a paused host iteration that presented nothing else.
 *
 * While the sim is paused with no step pending no tick runs, so neither the
 * canonical nor the sub-tick present fires, and the window would hold its
 * last image with the dev overlay, the toasts and the cursor frozen inside
 * it - the overlay could not even be used to resume. This presents the held
 * scene instead, once per call (the paused loop sleeps 16 ms per iteration),
 * without a re-extract, since nothing advanced: the hold the intermediate
 * path applies while the world is not live. Windowed only, never during a
 * skip fast-forward, and never while eager production holds the display
 * (it is disengaged while paused). Every call counts one paused idle
 * iteration for the OPENPETE_PAUSE_AT_VBL schedule, presented or not.
 * @param opts The parsed options. */
static void host_paused_repaint(const openpete_options_t *opts)
{
    s_pause_at.idle++;
    if (opts->headless || openpete_skip_present_until_level || s_eager.active)
        return;
    openpete_mods_present_flush(&g_game.cpu, 0);
    openpete_native_render_frame((uint64_t)(op_now() / 1000));
}

/** @brief Run the worker half of the host: create the game fiber and drive the tick and present loop until exit.
 *
 * Split from run_host_init because SDL brings its video, gamepad and haptic
 * subsystems up on one thread and pumps them on that thread. With init on
 * the worker, main's pump raced it: a GAMEPAD_ADDED delivered on main
 * reaches PsyX_Pad_Event_ControllerAdded, which re-creates the haptic
 * subsystem while the worker is inside PsyX_Pad_InitSystem. Init on main
 * before main starts pumping serialises the two with no barrier. libco
 * fibers are created and switched here, on the one thread that owns them
 * for their whole life.
 * @param opts The parsed options.
 * @return The process exit code. */
static int run_host_loop(const openpete_options_t* opts) {
    /* Which half of the input seam this thread owns (see run_host_init). */
    const bool split = openpete_split_applies(opts);

    /* The device was created during init - under the thread split that was
     * main - but inline presents come from THIS thread; the display's seam
     * fence needs the ownership stated, not inferred from who created what.
     * Eager's engage/disengage re-claims at every later handoff. */
    openpete_native_display_claim();

    /* Last point at which guest RAM still holds exactly what the EXE
     * loader and the seed blocks put there: the fiber below is what first
     * executes guest code, and a --state-load-on-frame restore happens
     * later still, inside the frame loop. */
    /* Mod code that may change guarded guest bytes runs on this thread
     * from here on: the guest-byte guard brackets it here and nowhere else. */
    openpete_mods_guard_claim_thread();
    game_on_boot(&g_game.cpu);

    openpete_host_fiber = co_active();
    /* 8 MB fiber stack - Spyro's deepest reasonable call chain is well under
     * 1 MB but PSYQ + recompiler shadow stacks add up; budget generously. */
    openpete_game_fiber = openpete_fiber_create_guarded(8u * 1024u * 1024u, game_fiber_entry);
    if (!openpete_game_fiber) {
        OPL_ERROR_F("boot", "game fiber stack allocation failed (out of memory?)");
        return OPENPETE_EXIT_PANIC;
    }
    OPL_INFO_F("boot", "fibers ready: host=%p game=%p",
               openpete_host_fiber, openpete_game_fiber);

    if (opts->debug_triangle) {
        openpete_debug_triangle_init();
        OPL_INFO_F("boot",
                   "debug-triangle enabled - emitting hardcoded POLY_F3 each frame");
    }

    /* Don't open a scene here - Spyro is a 30fps game (two PSX vblanks per
     * logical frame), so the host loop yields twice per DrawOTag. We let the
     * DrawOTag thunk start the scene on demand via its own PsyX_BeginScene()
     * call, and only EndScene + SwapWindow on host iterations where DrawOTag
     * actually fired. Empty iterations leave the front buffer untouched, so
     * the previous game frame stays visible until the next DrawOTag.
     *
     * Net effect: GL output is naturally 30fps and there's no yellow flash
     * between game frames. */

    Uint64 start = SDL_GetTicks();  /* SDL3: SDL_GetTicks is 64-bit */
    int    exit_code = OPENPETE_EXIT_OK;
    const char* exit_reason = "frame-limit";
    int    quit = 0;

    /* Monitor refresh rate (primary display - the window spawns there; 60
     * when headless / query failure). Feeds the --render-fps default below
     * and the settings menu's FPS ladder before the loop. */
    int refresh_hz = 60;
    /* Exact rate for the pacing log: rounding 59.94 to 60 erases precisely the
     * distinction a pacing baseline is keyed on. */
    int refresh_mhz = 60000;
    if (!opts->headless) {
        const SDL_DisplayMode* dm =
            SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        if (dm && dm->refresh_rate > 0.0f) {
            refresh_hz  = (int)(dm->refresh_rate + 0.5f);
            refresh_mhz = (int)(dm->refresh_rate * 1000.0f + 0.5f);
        }
    }

    /* Resolve the launch --render-fps target. -1 = unset: windowed runs
     * default to the display's refresh rate - the wall-clock dual-pacer + transform-interp
     * path - while headless keeps the legacy match-sim pacer (0): those runs
     * are unpaced (unlock) and the oracle/determinism harnesses expect one
     * present per tick. An explicit --render-fps 0 selects the legacy pacer.
     * opts stays const; everything below reads the resolved local, and the
     * live pacer state (s_render_fps) is seeded from it. */
    const int render_fps = (opts->render_fps >= 0) ? opts->render_fps
                         : opts->headless          ? 0
                                                   : refresh_hz;
    if (opts->render_fps < 0 && !opts->headless)
        OPL_INFO_F("pacer", "--render-fps unset: defaulting to display refresh %d Hz",
                   render_fps);

    /* Pacer state: a single time source (op_now / op_sleep_until) serves
     * both the legacy --render-fps=0 path and the --render-fps>0 wall-clock
     * pacer. Both branches step phase-locked deadlines in integer ns and
     * slip-clamp identically.
     *
     * Period choices:
     *   sim_period    = the PSX 240p VBL, exactly 37609/2250000 s
     *                   (59.8261054... Hz - see OP_VBL_* in openpete_time.h)
     *   render_period = 1/render_fps when --render-fps>0, else
     *                   sim_period (one present per sim tick).
     *
     * Both are op_period_t, so the fractional nanosecond each period carries
     * (a VBL is 16715111 + 1/9 ns) accrues on the deadline instead of being
     * truncated away every step. Deadlines land on floor(n * period) for all
     * n: under a nanosecond off the true grid, and never drifting further.
     *
     * Invariant: the inner sim block yields the game fiber at most once
     * per outer iteration. Multi-vbl catch-up would race-complete the
     * `pre - post < 2` GPU-pacing loops in draw.c and the 210-vbl Sony /
     * Universal logo waits in initialization.c. The slip-clamp resets
     * the deadline forward without firing extra yields. */
    /* --tick-scale F slows/speeds the WHOLE engine by dividing both the sim and
     * the render period by F: sim_rate = F*29.913 Hz, present_rate = F*render_fps,
     * so the count of sub-tick presents PER tick (present_rate/sim_rate) is
     * invariant - a slow-motion view of the SAME interpolation. Pacing only: the
     * sim still executes identical ticks (the interp alpha is wall-clock-derived
     * from the actual tick timestamps, so it auto-stretches), and the deadlines
     * are reset to now under unlock/headless, so this is inert in the oracle.
     *
     * F == 1 keeps the exact rational VBL; any other F is a debug slow-mo that
     * scales through a double and lands on a whole-ns period, so it leaves the
     * exact grid by design. */
    double tick_scale = (opts->tick_scale > 0.0) ? opts->tick_scale : 1.0;
    op_period_t sim_period = op_scaled_vbl_period(tick_scale);
    op_time_t   sim_period_ns = op_period_ns(sim_period);
    const op_period_t render_period = (render_fps > 0)
        ? op_period_from_ns((op_time_t)((double)(OP_NS_PER_SEC / (op_time_t)render_fps)
                                        / tick_scale))
        : sim_period;   /* sim_period is already tick-scaled */
    /* Seed the live pacer state (openpete_set_render_fps /
     * openpete_set_tick_scale mutate it; the loop reads s_render_fps /
     * s_render_period / s_tick_scale instead of the values above). */
    s_render_fps       = render_fps;
    s_render_period    = render_period;
    s_render_fps_dirty = 0;
    s_tick_scale       = tick_scale;
    s_tick_scale_dirty = 0;
    PsyX_SPUFeed_SetNominalRate(tick_scale);
    /* One WALL-time tick for every sub-tick alpha denominator (ring +
     * per-object phase lerps). Must stretch with --tick-scale: interp alphas
     * are wall-clock-derived, so an unscaled (NTSC) cap makes every sweep run
     * 1/tick_scale× too fast and pin at the clamp for the rest of the tick.
     * Rounded to whole ns: it is an alpha denominator, not a deadline, so the
     * carried ninth would be noise below the resolution of any alpha. */
    op_period_t tick_period = op_period_mul(sim_period, OP_TICK_VBLS);
    g_openpete_tick_period_ns = op_period_ns(tick_period);
    op_time_t slip_threshold_ns = 4 * sim_period_ns; /* ~67 ms (scaled) */

    /* --record-cfr: offline capture. `capture_mult` presents are produced per
     * sim tick, on a clock the pacer advances itself, so the present schedule
     * is exact no matter how long a frame takes to render - a software
     * rasterizer and a fast GPU emit the identical frame sequence. That is what
     * makes two captures at different rates cover the same span of game time.
     *
     * The render period is derived from the tick period rather than from an
     * integer --render-fps: 60 Hz against a 59.826 Hz VBL is a 2.0058 ratio,
     * which hands some ticks two intermediates and others three. Dividing the
     * tick period gives exactly `capture_mult` evenly spaced presents per tick.
     * op_period_div keeps that division exact - a tick split three ways is
     * 11143407 + 11/27 ns, which no whole-ns period can express - so the
     * capture grid stays even however long the capture runs.
     *
     * A multiplier of 1 wants no sub-tick presents at all, which is precisely
     * the legacy match-sim pacer (s_render_fps == 0). */
    const int capture_mult = (opts->record_cfr > 0.0)
        ? (int)(opts->record_cfr / OP_TICK_HZ + 0.5) : 0;
    if (capture_mult > 0) {
        if (capture_mult > 1) {
            s_render_fps    = (int)(opts->record_cfr + 0.5);  /* nominal, for logs + the "paced" test */
            s_render_period = op_period_div(tick_period, capture_mult);
        } else {
            s_render_fps    = 0;
            s_render_period = sim_period;
        }
        op_virtual_clock_enable(OP_NS_PER_SEC);
        OPL_INFO_F("pacer",
                   "--record-cfr %.5f: offline capture clock, %d present(s) per tick "
                   "(%.4f ms), sim %.4f ms",
                   opts->record_cfr, capture_mult,
                   (double)op_period_ns(s_render_period) / 1e6,
                   (double)sim_period_ns / 1e6);
    }
    live_knob_publish_shown();   /* the getters report the seeded pacer state */

    /* Was a deadline in force for the iteration that just ended? The loop's
     * own unlock verdict is not written until the tail, so iteration 1 has
     * nothing to read - and an unlocked headless run would record its first
     * VBL as paced against the pre-boot anchor, printing a paced census for a
     * run that never paced anything and making the refuse-at-zero guard
     * unreachable.
     *
     * A separate variable rather than seeding `openpete_unlock_fps` itself:
     * that global gates render paths mid-iteration, and an instrument that
     * changes iteration 1's rendering to measure iteration 1 is not an
     * instrument. */
    int pace_unlock_prev = (opts->headless && !pace_force_pacing()) ? 1 : 0;

    op_time_t     start_ns = op_now();
    /* Set when a clamp discards wall time at the loop tail; consumed by the
     * NEXT VBL record, which is the first tick to run against the re-anchored
     * grid - the slip belongs to the tick that inherited it. */
    int           pace_slip_pending = 0;
    /* Set when the deadline chain is deliberately re-anchored (a live
     * --render-fps / --tick-scale change, an unlock exit). The next VBL's
     * sleep targeted the OLD grid, so subtracting the NEW anchor would
     * manufacture a near-zero phase error - flattering, and untrue. That VBL
     * records no deadline instead. */
    int           pace_regrid_pending = 0;
    op_deadline_t next_sim, next_render;
    op_deadline_anchor(&next_sim, start_ns);
    op_deadline_anchor(&next_render, start_ns);
    if (s_render_fps > 0) {
        OPL_INFO_F("pacer", "wall-clock dual-pacer: sim=%.6f ms (%.6f Hz) render=%.6f ms (render-fps=%d, --tick-scale=%.3f)",
                   (double)sim_period_ns / 1e6, op_period_hz(sim_period),
                   (double)op_period_ns(s_render_period) / 1e6,
                   s_render_fps, tick_scale);
    } else {
        OPL_INFO_F("pacer", "legacy pacer: sim=%.6f ms NTSC (%.6f Hz VBL, --render-fps=0, --tick-scale=%.3f)",
                   (double)sim_period_ns / 1e6, op_period_hz(sim_period), tick_scale);
    }

    /* Bring up the interpolation snapshot ring. Capture is enabled only
     * when the render rate exceeds the sim rate: at parity there is nothing
     * to interpolate between, and the per-tick memcpy is pure overhead.
     * Safe to run unconditionally: capture is a read-only PSX-RAM copy on
     * the host fiber while the game fiber is yielded inside VSync(0). */
    if (openpete_interp_init() == 0) {
        const op_time_t render_period_ns = op_period_ns(s_render_period);
        /* The cadence counters judge a display hole against this. */
        openpete_pace_set_slot_ns((int64_t)render_period_ns);
        const op_time_t sim_hz_x1000 = (OP_NS_PER_SEC * 1000) / sim_period_ns;
        const op_time_t render_hz_x1000 = (render_period_ns > 0)
            ? (OP_NS_PER_SEC * 1000) / render_period_ns
            : 0;
        if (s_render_fps > 0 && render_hz_x1000 > sim_hz_x1000) {
            openpete_interp_set_enabled(1);
            OPL_INFO_F("interp",
                       "capture enabled (render-fps=%d > sim ~30 Hz)",
                       s_render_fps);
        }
        /* Offline capture at 2 presents per tick puts the render period exactly
         * ON the VBL period, which the ">" above reads as parity. It isn't: the
         * game draws every OTHER VBL, so one of those two presents is a sub-tick
         * frame and interpolation has real work to do. */
        if (capture_mult >= 2) {
            openpete_interp_set_enabled(1);
            OPL_INFO_F("interp", "capture enabled (offline capture, %d present(s) per tick)",
                       capture_mult);
        }
        /* Presentation-golden intermediate corpus: capture the ring headless so
         * --screenshot-intermediate-on-frame can re-extract a deterministic
         * alpha=0.5 sub-tick frame. DETERMINISTIC stamps (fixed grid, one tick
         * apart) make the forced present target land alpha exactly at 0.5 with no
         * host-pacing dependency. The live sub-tick present stays headless-blocked
         * and the canonical lag leg is gated !headless (below), so enabling capture
         * here does NOT alter the canonical dump - it only feeds the explicit
         * intermediate capture. */
        if (opts->screenshot_intermediate_count > 0) {
            openpete_interp_set_enabled(1);
            openpete_interp_set_deterministic(1, g_openpete_tick_period_ns);
            OPL_INFO_F("interp",
                       "deterministic capture enabled (%d intermediate screenshot target(s), tick=%lld ns)",
                       opts->screenshot_intermediate_count, (long long)g_openpete_tick_period_ns);
        }
    }

    /* --rewind: allocate the keyframe ring. Driver lives
     * at the quiescent point below (Backspace / --rewind-hold). */
    openpete_rewind_configure(opts->rewind_window);

    /* --runahead: seeded into the live-depth request; the in-loop apply
     * block arms it on the first iteration - the identical path a live
     * dev-overlay change takes (capability gate + snapshot alloc there). */
    int runahead_n = 0;
    openpete_state_mem_t* ra_snap = NULL;
    s_runahead_req   = (opts->runahead > 0) ? opts->runahead : 0;
    SDL_SetAtomicInt(&s_runahead_live, 0);
    s_runahead_dirty = (s_runahead_req > 0);

    /* Wall-clock pacing is now driven by openpete_ticker.h: each iteration
     * blocks on the ticker's condvar until the atomic vbl counter advances,
     * naturally pacing the host loop at ~60 Hz. The previous fractional
     * SDL_Delay accumulator is gone - pacing is implicit in "wait for
     * tick", which means a single VSync(0) call (loading-screen flyby)
     * resumes after one tick = 60 fps and a wait-loop body (gameplay)
     * resumes after two ticks = 30 fps without per-game predicates. */

    /* --position-log: open the CSV. Header is one line so multiple runs
     * can be diff'd without sed magic. fflush after each frame so a
     * killed run still leaves its progress on disk. */
    FILE* position_log_fp = NULL;
    if (opts->position_log_path && *opts->position_log_path) {
        position_log_fp = fopen(opts->position_log_path, "wb");
        if (!position_log_fp) {
            OPL_ERROR_F("position-log", "fopen %s failed: %s",
                        opts->position_log_path, strerror(errno));
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        fputs("frame,vbl,x,y,z\n", position_log_fp);
        OPL_INFO_F("position-log", "writing g_Spyro.m_Position each yield to %s",
                   opts->position_log_path);
    }

    /* Primary determinism gate. See gameplay_hash_log_emit. */
    FILE* gameplay_hash_log_fp = NULL;
    if (opts->gameplay_hash_log_path && *opts->gameplay_hash_log_path) {
        /* Zero registered regions hashes nothing: every frame would log the
         * FNV offset basis and two runs would compare equal on no bytes. */
        if (openpete_state_region_count == 0) {
            OPL_ERROR_F("gameplay-hash-log",
                        "this game registers no gameplay-state regions "
                        "(no games/<game>/manifest/gameplay_state_regions.toml); "
                        "a hash over zero regions is vacuous");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        gameplay_hash_log_fp = fopen(opts->gameplay_hash_log_path, "wb");
        if (!gameplay_hash_log_fp) {
            OPL_ERROR_F("gameplay-hash-log", "fopen %s failed: %s",
                        opts->gameplay_hash_log_path, strerror(errno));
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        gameplay_hash_log_emit_header(gameplay_hash_log_fp);
        OPL_INFO_F("gameplay-hash-log",
                   "FNV1a-64 over %zu state regions to %s",
                   openpete_state_region_count,
                   opts->gameplay_hash_log_path);
    }

    /* Forward-declare the frame-time-log state - opened below after bk2
     * setup so its early-returns don't leak the FILE*. */
    openpete_frame_time_log_t frame_time_log = {0};
    int64_t frame_time_log_target_period_ns = 0;
    int64_t frame_time_log_start_ns = 0;

    /* --bk2-record: prepare the in-memory frame buffer. The file isn't
     * touched until exit, so a SIGTERM/Ctrl-C against the run still
     * produces a usable movie. */
    bool bk2_recording = (opts->bk2_record_path && *opts->bk2_record_path);
    if (bk2_recording) {
        if (bk2_record_begin(opts->bk2_record_path, opts->bk2_record_args) != 0) {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    }

    /* Level-load duration, constant for the run: the option, else the game's
     * own default. Resolved before the movie load so a bk2 recorded under
     * another value is refused up front. */
    openpete_cd_load_delay = (opts->load_delay >= 0)
                               ? opts->load_delay
                               : (int)game_get_load_delay_default();
    OPL_INFO_F("cd", "load delay: %d VBL(s) per level load%s",
               openpete_cd_load_delay,
               opts->load_delay >= 0 ? " (--load-delay)" : " (game default)");

    /* --bk2: load BizHawk movie once, picked up per-frame in the loop. */
    bk2_movie_t bk2 = {0};
    int  bk2_offset = 0;
    bool bk2_active = (opts->bk2_path && *opts->bk2_path);
    openpete_bk2_set_active(bk2_active || bk2_recording);
    if (bk2_active &&
        bk2_replay_load(opts, &bk2, &bk2_offset) != 0)
        return OPENPETE_EXIT_SETUP_ERROR;
    bk2_feed_open(bk2_active ? &bk2 : NULL, bk2_offset);

    /* --frame-time-log: per-present binary recorder for the pacing
     * harness. The "deadline" we record against is a pure-ideal
     * cadence:
     *     deadline = start_op_ns + frame * target_period_ns
     * This is platform-agnostic and pacer-agnostic - it tells us how
     * the *actual* present time drifts from a perfectly-paced clock,
     * regardless of whether the legacy SDL_AddTimer pacer or a future
     * high-res pacer is driving the loop. The target period is the sim
     * cadence. */
    if (opts->frame_time_log_path && *opts->frame_time_log_path) {
        /* Default target: the PSX tick cadence, one present per tick
         * (= 1/2 of the 59.8261054 Hz 240p VBL). When the render-fps target
         * is set (explicit --render-fps N, or the unset windowed default =
         * display refresh) the dual-pacer drives the loop at N Hz instead,
         * so use that. Whole ns: the log's ideal-cadence baseline is a
         * measurement reference, not a deadline the pacer steps. */
        if (render_fps > 0) {
            frame_time_log_target_period_ns =
                (int64_t)((double)OP_NS_PER_SEC / (double)render_fps / tick_scale);
        } else {
            frame_time_log_target_period_ns = (int64_t)g_openpete_tick_period_ns;
        }
        if (openpete_frame_time_log_open(&frame_time_log,
                                         opts->frame_time_log_path,
                                         frame_time_log_target_period_ns) != 0) {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        frame_time_log_start_ns = op_now();
    }

    /* --pace-log: the unified pacing event log. Superset of the recorder
     * above - every VBL as well as every present, on one timebase, so a late
     * tick can be attributed to the present that blocked it. Opened here, with
     * the renderer already up, because the environment block must record the
     * device and the present mode actually GRANTED. */
    if (opts->pace_log_path && *opts->pace_log_path) {
        char env[2048];
        openpete_pace_env_build(env, sizeof(env),
                                openpete_native_render_gpu_name(),
                                openpete_native_render_driver_version(),
                                refresh_mhz,
                                openpete_native_render_present_mode(),
                                opts->fullscreen ? 1 : 0,
                                s_render_fps,
                                g_openpete_interp_enabled ? 1 : 0,
                                s_runahead_req,
                                tick_scale,
                                openpete_split_applies(opts) ? 1 : 0,
                                openpete_host_input_legacy(),
                                opts->headless ? 1 : 0);
        if (openpete_pace_log_open(opts->pace_log_path, env) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
    }

    /* Performance trace (--profile-out): streams CPU scope spans + GPU zones +
     * census, keyed per present / canonical tick. Inert unless the flag is set. */
    if (opts->profile_out_path && *opts->profile_out_path) {
        if (op_prof_open(opts->profile_out_path) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
    }

    /* Input pump rate: every host iteration. g_PsyX_Pad_InputOverride is
     * set per host iteration and the input update is what propagates it into
     * the pad buffer the game polls, so skipping pumps (for instance while
     * Tab is held) would leave the game reading a stale override and break
     * bk2 replay determinism. */

    /* Host hotkeys all come from the keymap (openpete_keymap.h), which
     * owns the bindings AND the press edges: openpete_keymap_poll() once
     * per host iteration advances one latch bit per action, so a held key
     * fires exactly once. That replaces the ~25 prev_*_key bools this
     * function used to carry, and it is what makes every hotkey below
     * rebindable from [keys.host] in openpete.toml.
     *
     * The savestate keys are number-row 1/2/3 by default rather than the
     * F2/F4 they started as, because F4 collides with PsyCross's own
     * debug binding - it cycles g_activeKeyboardControllers, which
     * silently kicks the keyboard off pad 1 and looks exactly like a
     * save-state regression. */
    unsigned active_slot = 0;

    /* Frame-step state. Pause toggles; frame-step advances exactly one
     * game tick, running VBLs until the game draws (hold to repeat). While paused the host
     * loop pumps SDL events, sleeps the deadline, and skips the co_switch
     * into the game fiber - so g_ram / CPUState stay frozen and PsyCross
     * keeps presenting the last completed frame. SPU is muted on enter to
     * avoid the held-voice drone (PsyCross's SPU output plays through
     * OpenAL, whose mixer runs on its own thread; without new SPU register
     * writes voices loop their current sample). */
    bool g_paused = false;
    bool g_step_one = false;
    bool prev_alt_enter = false;

    /* Menu / overlay latches that are NOT key edges: these track the
     * open/closed state itself so the pad override is cleared exactly
     * once on the closing transition. */
    bool prev_menu_close = false;   /* Start/Circle closes on RELEASE */
    bool menu_close_stale = false;  /* menu_close held from before the open */
    int  prev_mouse_owner = OPENPETE_MOUSE_OWNED_NONE;
    bool prev_overlay_hold = false;   /* an overlay held the pad neutral */

    /* Seed the in-game settings menu from the launch flags, and build its FPS
     * ladder from the monitor refresh queried above (60 if unknown/headless).
     * Applies nothing - it only makes the menu reflect what the game launched
     * with (the FPS row reads the live pacer target, which already holds the
     * resolved render-fps default). */
    openpete_menu_init(opts, refresh_hz);

    /* Eager production (on unless OPENPETE_EAGER=0): windowed native display
     * only. */
    if (openpete_native_scene_enabled() && !opts->headless && openpete_present_thread_start()) {
        eager_init();
        s_eager_ftl.log = &frame_time_log;
        s_eager_ftl.start_ns = &frame_time_log_start_ns;
        s_eager_ftl.period_ns = &frame_time_log_target_period_ns;
        openpete_present_thread_set_callback(eager_present_cb, NULL);
        openpete_on_guest_discontinuity(eager_on_discontinuity);
        openpete_on_guest_discontinuity(openpete_native_scene_ring_flush);
    }

    while (!quit) {
        SDL_AddAtomicInt(&g_sim_heartbeat, 1);
        /* Top-of-loop unlock peek used *only* to gate sim_due under
         * --render-fps>0. The authoritative `unlock_fps` write
         * (openpete_unlock_fps + g_skipSwapInterval) still happens at the
         * bottom of the loop, AFTER PsyX_UpdateInput has refreshed SDL's
         * keyboard cache and AFTER the present has consumed the previous
         * iter's swap-interval flag - preserving the original semantics
         * the GL present path was tested under. */
        /* The same cached gate the bottom of the loop reads, so the
         * sim-due predicate follows the pacer when measurement mode is on. */
        bool force_pacing = pace_force_pacing() != 0;
        /* The turbo read here is the PREVIOUS iteration's keymap sample -
         * this peek runs before the poll below. That is what a peek is for;
         * the authoritative unlock write at the bottom of the loop re-reads
         * after the poll, so a press costs at most one iteration of lag. */
        bool unlock_now = (opts->headless && !force_pacing) ||
                          openpete_skip_present_until_level ||
                          openpete_key_down(OPK_TURBO) ||
                          (opts->unlock_ranges_count > 0 &&
                           unlock_frame_in_ranges(opts, openpete_feed_key_now()));

        /* sim_due gates the once-per-yield block that does bk2 override
         * stamping, the co_switch into the game fiber, post-yield
         * accounting (position log, bk2 record, present, ram dump, exit
         * checks). With --render-fps=0 the gate is always true and the loop
         * reduces to the historical "one yield per outer iter" shape; the
         * condvar pacer below preserves byte-identical behavior. With
         * --render-fps>0 the wall-clock pacer drives the outer loop at
         * render cadence, and sim_due fires when the sim deadline has
         * elapsed - or unconditionally under unlock_now so fast-forward
         * actually fast-forwards. */
        /* Live render-fps change (openpete_set_render_fps, e.g. the in-game
         * menu): recompute the period with THIS loop's tick_scale and re-anchor
         * the deadlines to now so a lower->higher switch doesn't fire a burst of
         * catch-up presents. Also keep interp capture in sync (enabled only when
         * the render rate exceeds the sim rate), matching the startup gate. */
        /* Live tick-scale change: re-derive every scaled period, then fall
         * through the render-fps dirty path below for the render period +
         * deadline re-anchor + interp-gate re-sync. */
        live_knob_drain();
        if (s_tick_scale_dirty) {
            s_tick_scale_dirty = 0;
            tick_scale = s_tick_scale;
            sim_period    = op_scaled_vbl_period(tick_scale);
            sim_period_ns = op_period_ns(sim_period);
            tick_period   = op_period_mul(sim_period, OP_TICK_VBLS);
            g_openpete_tick_period_ns = op_period_ns(tick_period);
            slip_threshold_ns = 4 * sim_period_ns;
            s_render_fps_dirty = 1;
            PsyX_SPUFeed_SetNominalRate(tick_scale);
            OPL_INFO_F("pacer", "tick-scale -> %.3f (sim %.6f ms, %.6f Hz)",
                       tick_scale, (double)sim_period_ns / 1e6,
                       op_period_hz(sim_period));
        }
        /* A modset toggle changes what the timeline means: rewind history
         * recorded under the old set must not replay into the new one, and
         * a held runahead snapshot may predate a freshly enabled mod that
         * declares no runahead capability. Drop both; the dirty path below
         * re-arms runahead through its capability gate against the NEW
         * enabled set. */
        {
            static unsigned s_modset_epoch = 0;
            unsigned ep = openpete_mods_toggle_epoch();
            if (ep != s_modset_epoch) {
                s_modset_epoch = ep;
                if (openpete_rewind_enabled()) {
                    openpete_rewind_clear();
                    OPL_INFO_F("rewind", "modset changed - history cleared");
                }
                if (ra_snap) {
                    openpete_state_mem_free(ra_snap);
                    ra_snap = NULL;
                    OPL_INFO_F("runahead",
                               "modset changed - re-checking capability gate");
                }
                s_runahead_dirty = 1;
            }
        }
        /* Live runahead re-arm. Arm only when every enabled mod declares a
         * runahead posture in its manifest (runahead = pure | host-fx) -
         * speculative re-execution of tick hooks with undeclared host side
         * effects would double-fire them. Arms once and holds ra_snap until
         * the modset epoch above disarms it. */
        if (s_runahead_dirty) {
            s_runahead_dirty = 0;
            int ra_depth = s_runahead_req;
            if (ra_depth > 0 && !ra_snap) {
                const char* ra_offender = NULL;
                if (!openpete_mods_runahead_capable(&ra_offender)) {
                    OPL_ERROR_F("runahead",
                                "disabled: mod \"%s\" declares no runahead "
                                "capability (add runahead = \"pure\" or "
                                "\"host-fx\" to its mod.toml)",
                                ra_offender ? ra_offender : "?");
                    ra_depth = s_runahead_req = 0;
                } else {
                    ra_snap = openpete_state_mem_new();
                    if (!ra_snap) ra_depth = s_runahead_req = 0;
                    else if (ra_pc_on()) {
                        (void)openpete_on_guest_discontinuity(ra_pc_invalidate_all);
                        OPL_INFO_F("runahead",
                                   "prediction-exactness check armed");
                    }
                }
            }
            if (ra_depth != runahead_n) {
                runahead_n = ra_depth;
                /* The interp ring must never pair a led-timeline state with
                 * a real-timeline one across the depth switch: reset it so
                 * the next presents snap instead of lerping backward. */
                openpete_interp_reset();
                openpete_interp_objs_reset();
                if (runahead_n > 0)
                    OPL_INFO_F("runahead", "armed: leading presentation by "
                               "%d tick(s)", runahead_n);
                else
                    OPL_INFO_F("runahead", "disarmed");
            }
            SDL_SetAtomicInt(&s_runahead_live, runahead_n);
        }
        if (s_render_fps_dirty) {
            s_render_fps_dirty = 0;
            s_render_period = (s_render_fps > 0)
                ? op_period_from_ns((op_time_t)((double)(OP_NS_PER_SEC / (op_time_t)s_render_fps)
                                                / tick_scale))
                : sim_period;
            const op_time_t render_period_ns = op_period_ns(s_render_period);
            openpete_pace_set_slot_ns((int64_t)render_period_ns);
            op_time_t anchor = op_now();
            op_deadline_anchor(&next_sim, anchor);
            op_deadline_anchor(&next_render, anchor);
            pace_regrid_pending = 1;
            const op_time_t sim_hz_x1000  = (OP_NS_PER_SEC * 1000) / sim_period_ns;
            const op_time_t rend_hz_x1000 = (render_period_ns > 0)
                ? (OP_NS_PER_SEC * 1000) / render_period_ns : 0;
            openpete_interp_set_enabled(s_render_fps > 0 && rend_hz_x1000 > sim_hz_x1000 ? 1 : 0);
            OPL_INFO_F("pacer", "render-fps -> %d (%.4f ms/present)",
                       s_render_fps, (double)render_period_ns / 1e6);
        }
        op_time_t iter_now_ns = op_now();
        bool   sim_due = (s_render_fps == 0) || unlock_now ||
                         (iter_now_ns >= next_sim.at);

        /* Override-channel arbitration for this iteration. The channel has
         * several writers with different priorities; live decode is the
         * lowest and fills it whenever nothing else claims it. Anything that
         * needs the pad to NOT be live input sets this. */
        bool ovr_claimed = false;

        /* --bk2: stamp this frame's bitfield into PsyCross's pad-override
         * slot. PsyX_Pad_InternalPadUpdates uses it instead of polling SDL.
         * Out-of-range frames (before the offset, or past the movie's end)
         * fall back to 0xFFFF - i.e. no override - so live keyboard /
         * gamepad input takes over once the movie ends. */
        unsigned bk2_stamped_frame = openpete_frame;  /* see restamp below */
        if (sim_due && bk2_active) {
            int bk2_frame = bk2_feed_row();
            if (bk2_frame >= bk2.frame_count && opts->bk2_stop_on_end) {
                quit = 1;
                exit_reason = "bk2-end";
                break;
            }
            const int ovr_active = bk2_stamp_row(&bk2, bk2_frame);
            const unsigned short bits = (unsigned short)g_PsyX_Pad_InputOverride;
            {
                /* Mods' read-only bk2 status (api->bk2_status) - also
                 * published pre-offset / past-end (overriding = 0). */
                openpete_mods_bk2_status_t st = {
                    1, bk2_frame, (int)bk2.frame_count, ovr_active };
                openpete_mods_bk2_publish(&st);
            }
            /* Log every input transition so the user can audit the
             * playback timeline. Mirrors the press/release format used
             * on the record path. */
            static unsigned short last_bits = 0xFFFF;
            if (bits != last_bits) {
                char p_buf[17], r_buf[17];
                bk2_format_button_diff(last_bits, bits, p_buf, r_buf);
                OPL_INFO_F("bk2",
                           "openpete_frame=%u bk2_frame=%d press=[%s] release=[%s] bits=0x%04X",
                           openpete_frame, bk2_frame, p_buf, r_buf, bits);
                last_bits = bits;
            }
            /* Pre-offset / past-end frames leave ovr_active 0 on purpose:
             * that is the documented hand-back to live input once a movie
             * ends, which now means the live-decode latch below. */
            if (ovr_active) ovr_claimed = true;
        }

        /* A bk2 frame in range for the movie's row counter owns the override
         * channel; the overlays' neutral stamp yields to it. Evaluated on
         * every iteration, not only sim-due ones, because the overlays stamp
         * on every iteration and the movie restamps only on a tick. */
        if (overlay_pad_stamp(bk2_active,
                              bk2_frame_in_range(bk2_active, bk2_feed_row(),
                                                 bk2.frame_count),
                              &prev_overlay_hold))
            ovr_claimed = true;
        overlay_input_suspend();

        /* Under the thread split the cursor is main's business (see
         * openpete_cursor_update): SetCursor only reaches a window from the
         * thread that owns it. Unsplit, this loop IS that thread. */
        if (!openpete_split_applies(opts)) {
            openpete_cursor_update(opts->headless);
            openpete_host_mouse_apply_capture();
        }

        /* Sample every binding once per iteration; openpete_key_pressed()
         * below reads the edge this computed. Reads SDL's keyboard/gamepad
         * state caches, which the previous iteration's pump refreshed - so
         * this must run before the first hotkey test, and the turbo peek at
         * the top of the loop deliberately sees the previous sample.
         * openpete_handle_event consumes QUIT/CLOSE/ESC without forwarding
         * them, so PsyCross's exit-on-quit path never fires. The binding
         * page's requests apply first, so a capture's Escape cancel and its
         * commit land between two polls. */
        openpete_rebind_drain();
        openpete_keymap_poll();
        if (openpete_host_mouse_probe()) {
            /* Across an ownership change the poll's markers show the snap:
             * they sit at the totals, so nothing made under the overlay is
             * delivered when it closes. */
            static int probe_prev_owner = -1;
            const int owner = openpete_host_mouse_owned();
            if (owner != probe_prev_owner) {
                long long w, mx, my;
                openpete_keymap_mouse_markers(&w, &mx, &my);
                openpete_host_mouse_probe_log("keymap",
                    "owner %d -> %d; markers wheel=%lld mx=%lld my=%lld "
                    "(totals %lld %lld %lld)", probe_prev_owner, owner, w, mx, my,
                    (long long)openpete_host_mouse_wheel_total(),
                    (long long)openpete_host_mouse_motion_total(0),
                    (long long)openpete_host_mouse_motion_total(1));
                probe_prev_owner = owner;
            }
        }

        /* Dev config overlay. Mutually exclusive with the settings menu -
         * two input-owning overlays with different capture models is
         * complexity with no payoff. */
        openpete_devui_host_poll();

        /* The in-game settings menu. While open it captures the nav keys and
         * the game pad is neutralised above, so the other hotkeys and
         * gameplay input stay inert. Esc-closes is handled in the event
         * pump. */
        {
            if (openpete_key_pressed(OPK_MENU)) {
                openpete_menu_toggle();
                if (openpete_menu_is_open()) openpete_devui_close();
            }
            /* A target state requested from another thread (the pump's
             * Escape dismiss, the `menu` inject verb). Consumed here so the
             * toggle lands between two reads of the override clear latch,
             * and mirrors the OPK_MENU block: opening closes the overlay. */
            {
                int want_open = 0;
                if (openpete_host_mouse_take_menu_request(&want_open) &&
                    (want_open != 0) != (openpete_menu_is_open() != 0)) {
                    openpete_menu_toggle();
                    if (openpete_menu_is_open()) openpete_devui_close();
                    openpete_host_mouse_probe_log("menu", "request %s -> open=%d",
                                                  want_open ? "open" : "close",
                                                  openpete_menu_is_open());
                }
            }

            if (openpete_menu_is_open()) {
                /* Keyboard and pad both drive nav through one binding per
                 * direction, so holding a direction - key, d-pad or stick -
                 * steps once until released, the same feel either way. The
                 * pad's own view of these buttons is held neutral while the
                 * menu is open, so this cannot double up with gameplay. */
                bool close_k = openpete_key_down(OPK_MENU_CLOSE);
                /* A mouse-bound menu_close held while the pointer drifts
                 * onto the fill-tag inspector reads a release as the
                 * arbitrated view goes neutral; a release edge on a poll
                 * where the owner changed is not a release. */
                const int mouse_owner = openpete_host_mouse_owned();
                const bool owner_changed = mouse_owner != prev_mouse_owner;
                prev_mouse_owner = mouse_owner;
                if (menu_close_released(close_k, prev_menu_close, owner_changed,
                                        &menu_close_stale)) {
                    /* Close on the RELEASE edge of Start/Circle. The game pad
                     * is held neutral only while the menu is open, so closing
                     * on the press would let the still-held button bleed into
                     * the game next frame (Start -> the game's pause menu); by
                     * release it's already up. */
                    openpete_menu_toggle();   /* closes it */
                } else {
                    if (openpete_key_pressed(OPK_MENU_UP))    openpete_menu_nav(-1);
                    if (openpete_key_pressed(OPK_MENU_DOWN))  openpete_menu_nav(+1);
                    if (openpete_key_pressed(OPK_MENU_LEFT))  openpete_menu_adjust(-1);
                    if (openpete_key_pressed(OPK_MENU_RIGHT)) openpete_menu_adjust(+1);
                    if (openpete_key_pressed(OPK_MENU_SELECT)) openpete_menu_activate();
                    /* Pointer: hover, click, wheel, the X glyph. After the
                     * keys so a key and a click in one iteration apply in
                     * a fixed order. */
                    openpete_menu_mouse_poll();
                }
                prev_menu_close = close_k;

                /* Freecam axes are level-held and consumed every game tick,
                 * but the latch below is skipped while the menu owns the
                 * keyboard - hold neutral here so a movement key held at
                 * menu-open doesn't keep flying the camera behind the panel. */
                if (openpete_freecam_active() || openpete_camlock_active()) {
                    openpete_freecam_axes ax = {0};
                    openpete_freecam_set_axes(&ax);
                }
            } else {
                prev_menu_close = false;
                menu_close_stale = openpete_key_down(OPK_MENU_CLOSE) != 0;
            }
        }
        /* The settings writer: menu changes and the dev overlay's Save
         * settings reach openpete.toml from this thread only. */
        openpete_settings_save_poll();

        bool rewound_this_iter = false;   /* rewind driver stepped: skip
                                           * this iteration's forward tick */
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
        if (!openpete_menu_is_open()) {
            state_ops_collect(opts, active_slot);
            if (openpete_key_pressed(OPK_STATE_CYCLE)) {
                active_slot = (active_slot + 1u) % OPENPETE_STATE_NUM_SLOTS;
                OPL_INFO_F("savestate", "active slot -> %u%s",
                           active_slot,
                           openpete_state_slot_present(active_slot)
                               ? " (occupied)" : " (empty)");
            }
            /* Process handoff: resume on the first pass of a
             * --resume-handoff process; checkpoint+exec when armed
             * (OPENPETE_HANDOFF_AT / openpete_handoff_request). */
            if (state_ops_drain()) {
                state_stress_step(opts);
                rewound_this_iter = rewind_step(opts, g_paused, bk2_active);
                openpete_mods_arena_page_census_tick();
                openpete_handoff_poll(&g_game.cpu);
            }

            /* Any op above may have RE-ANCHORED openpete_frame (savestate
             * load, stress rollback, handoff resume). The bk2 override was
             * stamped for the PRE-jump frame - re-derive it for the new
             * one or the very next tick runs with the future frame's
             * input (measured: rollback+replay self-mismatch on movies
             * with an input edge inside the window). Silent twin of the
             * stamp block above; logging/status stay main-site-only. */
            if (bk2_active && openpete_frame != bk2_stamped_frame)
                (void)bk2_stamp_row(&bk2, bk2_feed_row());

            host_pause_step(&g_paused, &g_step_one);

            /* Player screenshot. The key edge only raises the request: the
             * capture is a blocking readback of the native renderer's
             * offscreen, and the device belongs to the present thread while
             * eager is engaged, so it runs beside the --screenshot-on-frame
             * dumps further down this iteration, under the same handoff. */
            if (openpete_key_pressed(OPK_SCREENSHOT))
                s_screenshot_request = 1;

            /* Live on/off A/B for interpolation itself. Off = intermediate
             * renders present the lagged frame unchanged (a clean "is
             * frame-gen doing anything perceptible?" test). A development
             * instrument: unbound unless [keys.host] asks for it. */
            if (openpete_key_pressed(OPK_INTERP_TOGGLE)) {
                g_openpete_interp_enabled = !g_openpete_interp_enabled;
                OPL_INFO_F("render", "interpolation -> %s",
                           g_openpete_interp_enabled ? "ON" : "OFF");
                openpete_osd_notify("host", "hotkey.interp", 0, "INTERPOLATION: %s",
                                    g_openpete_interp_enabled ? "ON" : "OFF");
            }

            /* Live toggle of sector LOD original <-> extended (the settings
             * menu's LOD row / --lod flag). Goes through openpete_no_lod_set
             * so openpete_fog_scale follows the mode - a raw openpete_no_lod
             * poke would leave the terrain fog latched. The setter logs the
             * new state. Native-renderer display policy; inert under
             * PsyCross. The menu re-syncs its LOD row from openpete_no_lod
             * when next opened. */
            if (openpete_key_pressed(OPK_LOD_TOGGLE)) {
                openpete_no_lod_set(!openpete_no_lod, -1.0f);
                /* Same key the O menu's LOD row uses, so a player who toggles
                 * both ways sees one toast repaint rather than two stack. */
                openpete_osd_notify("host", "menu.lod", 0, "LOD: %s",
                                    openpete_no_lod ? "EXTENDED" : "ORIGINAL");
            }

            /* The LP fill-tag inspector. Engaged, it picks the terrain face
             * under the crosshair each present and tints the fill-tag set, so
             * the frozen list in tests/freecam/fill-tags.verdicts can be
             * audited against the live scene. Preview draws the untagged
             * candidates too - an under-fill site's covering face is
             * invisible until it is tagged. FILL / NOFILL / "no session
             * verdict" record against the picked face and re-render with it
             * immediately; undo steps back. The verdict keys are gated on
             * engaged so their bindings stay free while it is off. */
            if (openpete_key_pressed(OPK_FILLPICK)) openpete_fillpick_toggle();
            if (openpete_fillpick_active()) {
                if (openpete_key_pressed(OPK_FILLPICK_PREVIEW))
                    openpete_fillpick_set_preview(!openpete_fillpick_preview());
                if (openpete_key_pressed(OPK_FILLPICK_FILL))
                    openpete_fillpick_mark(OPENPETE_FILLPICK_FILL);
                if (openpete_key_pressed(OPK_FILLPICK_NOFILL))
                    openpete_fillpick_mark(OPENPETE_FILLPICK_NOFILL);
                if (openpete_key_pressed(OPK_FILLPICK_NONE))
                    openpete_fillpick_mark(OPENPETE_FILLPICK_NONE);
                if (openpete_key_pressed(OPK_FILLPICK_UNDO))
                    openpete_fillpick_undo();
            }

            /* The glitch mark. The edge only raises a flag; the file is
             * written below, on this loop, because it blocks on IO and the
             * ring's other writer is whichever thread owns the display. */
            if (openpete_key_pressed(OPK_GLITCH_MARK))
                openpete_glitch_mark_request("key");

            /* Debug free camera. While engaged: the fly keys / left stick
             * move, the look keys / right stick aim, rise+descend raise and
             * lower, the speed keys / triggers step the speed, and teleport
             * drops Spyro here and hands the camera back. We own only the
             * toggle + input latch here; the camera write and the
             * camera-follow / player-update freeze are game-side
             * (games/common/freecam_game.c). Latched as level-held
             * axes so movement continues while held, consumed once per game
             * tick in game_on_frame_pre. Keyboard and pad sum (then clamp), so
             * either or both drive it. */
            if (openpete_key_pressed(OPK_FREECAM)) {
                openpete_freecam_toggle();
                openpete_osd_notify("host", "menu.freecam", 0, "FREECAM: %s",
                                    openpete_freecam_active() ? "ON" : "OFF");
            }

            /* Render camera lock (openpete_camlock.h) - holds/flies the
             * PRESENTED view while the sim (cutscenes included) continues
             * untouched. Shares the freecam's fly axes and speed step, so the
             * two are mutually exclusive: engaging one disengages the other. */
            if (openpete_key_pressed(OPK_CAMLOCK)) {
                if (!openpete_camlock_active() && openpete_freecam_active())
                    openpete_freecam_toggle();
                openpete_camlock_toggle();
            }
            if (openpete_freecam_active() && openpete_camlock_active())
                openpete_camlock_toggle();   /* freecam engaged while locked wins */
            /* Either camera engaged asks for relative mouse mode; the request
             * is arbitrated against the overlays and applied on the
             * window-owning thread. Dropped on disengage. */
            openpete_host_mouse_request_capture(OPENPETE_MOUSE_CAP_CAMERA,
                openpete_freecam_active() || openpete_camlock_active());

            /* Drop Spyro at the camera and hand the camera back to the game.
             * Latched as a one-shot request - the move needs the game fiber, so
             * freecam_game applies it (and disengages) on the next tick; the
             * request is dropped if the camera isn't engaged. Edge-tracked out
             * here rather than under the engaged branch so a teleport key
             * already held when the camera engages doesn't count as a press. */
            if (openpete_key_pressed(OPK_TELEPORT))
                openpete_freecam_request_teleport();

            if (openpete_freecam_active() || openpete_camlock_active()) {
                /* Digital fly/look keys and the analog stick axes are separate
                 * bindings that SUM (then clamp) - so a key and a stick both
                 * drive the camera, and either can be rebound or unbound
                 * without disturbing the other. */
                float k_fwd = (float)(openpete_key_down(OPK_FLY_FORWARD)
                                    - openpete_key_down(OPK_FLY_BACK));
                float k_str = (float)(openpete_key_down(OPK_FLY_RIGHT)
                                    - openpete_key_down(OPK_FLY_LEFT));
                float k_up  = (float)(openpete_key_down(OPK_FLY_UP)
                                    - openpete_key_down(OPK_FLY_DOWN));
                float k_yaw = (float)(openpete_key_down(OPK_LOOK_RIGHT)
                                    - openpete_key_down(OPK_LOOK_LEFT));
                float k_pit = (float)(openpete_key_down(OPK_LOOK_UP)
                                    - openpete_key_down(OPK_LOOK_DOWN));
                /* The stick axes carry their own sign convention in the
                 * binding: fly_axis_y and look_axis_y default to the INVERTED
                 * axis ("pad:-lefty"), because SDL reports stick-up as
                 * negative and the camera wants stick-up = forward / look up.
                 * Anyone who prefers the other convention flips it in
                 * openpete.toml instead of in this file. */
                float p_fwd = openpete_key_axis(OPK_FLY_AXIS_Y);
                float p_str = openpete_key_axis(OPK_FLY_AXIS_X);
                float p_yaw = openpete_key_axis(OPK_LOOK_AXIS_X);
                float p_pit = openpete_key_axis(OPK_LOOK_AXIS_Y);

                openpete_freecam_axes ax;
                #define FC_CLAMP(x)  ( (x) > 1.0f ? 1.0f : ((x) < -1.0f ? -1.0f : (x)) )
                ax.move_fwd    = FC_CLAMP(k_fwd + p_fwd);
                ax.move_strafe = FC_CLAMP(k_str + p_str);
                ax.move_up     = FC_CLAMP(k_up);
                ax.look_yaw    = FC_CLAMP(k_yaw + p_yaw);
                ax.look_pitch  = FC_CLAMP(k_pit + p_pit);
                #undef FC_CLAMP
                openpete_freecam_set_axes(&ax);
                /* Mouse look: the look axes' mouse:x / mouse:y sources are
                 * displacements, accumulated into running count totals the
                 * camera drives consume against their own markers. Host
                 * state; no guest store here. */
                openpete_freecam_add_look_counts(
                    openpete_key_delta(OPK_LOOK_AXIS_X),
                    openpete_key_delta(OPK_LOOK_AXIS_Y));

                /* Speed step: one step per key or button edge plus one per
                 * wheel notch counted into the action this poll, so several
                 * notches in one iteration step several times. */
                for (int n = openpete_key_edge(OPK_SPEED_DOWN)
                           + openpete_key_pulses(OPK_SPEED_DOWN); n > 0; n--)
                    openpete_freecam_adjust_speed(-1);
                for (int n = openpete_key_edge(OPK_SPEED_UP)
                           + openpete_key_pulses(OPK_SPEED_UP); n > 0; n--)
                    openpete_freecam_adjust_speed(+1);
            }

            /* Alt+Enter (either Alt), the `fullscreen` binding, or a window
             * double-click toggles the native window between windowed and
             * borderless fullscreen. Moved here from PsyCross, which only ever
             * drove its now-hidden GL window; openpete_native_toggle_fullscreen
             * is a no-op in headless / PsyCross-only runs. The PsyCross
             * g_altKeyState gate still stops the Enter from registering as
             * Start while Alt is held.
             *
             * Alt+Enter is hardcoded rather than a binding because the keymap
             * has no chord vocabulary; `fullscreen` is the rebindable single
             * key for it and ships unbound so nothing changes by default. */
            prev_alt_enter = fullscreen_poll(split, prev_alt_enter);
        }

        /* Input, as four explicit steps instead of PsyX_UpdateInput's two.
         * pump -> decode -> arbitrate+stamp -> inject. openpete owns decode
         * and the latch point; PsyCross still owns the byte-exact write into
         * the PSX pad buffer, through the same override channel bk2 replay
         * has always used.
         *
         * Ordering is load-bearing: the decode must read SDL state from THIS
         * iteration's pump, or live input lags a full iteration behind.
         *
         * The Alt gate that PsyX_UpdateInput applied to its injection half
         * moves onto the sample, so Alt+Enter still cannot register as Start. */
        /* Publish the SDL-state snapshot on both paths: hotkeys, mod key
         * queries and the Alt gate all read it, so the legacy A/B would
         * otherwise run with every binding reading a never-updated buffer.
         * Legacy pumps inside PsyX_UpdateInput below, so it publishes the
         * previous pump's state - the same one-iteration lag the polled
         * bindings have always had. */
        if (!split) openpete_host_input_publish_sdl();

        /* Publish instant of the snapshot THIS tick consumes, captured at the
         * latch rather than read back later. Under the thread split main
         * republishes on its own ~1 ms pump loop, so by the end of the
         * iteration several newer samples have landed and a late read would
         * name one the tick never saw - against a staleness of the same
         * order as the error it would introduce. */
        op_time_t pace_latch_ns = 0;
        /* Which published sample this VBL consumed - identity, not a
         * timestamp. OP_PACE_LATCH_NONE on the legacy path (it never latches,
         * so its input phase is unmeasured rather than zero). */
        unsigned  pace_latch_seq = OP_PACE_LATCH_NONE;
        /* Pad bits of that sample: whether a press reached the game is a
         * question about state, not about which sample carried it. */
        uint16_t  pace_latch_bits = 0xFFFFu;

        if (openpete_host_input_legacy()) {
            /* Pre-ownership path, kept as a diagnostic A/B: PsyCross both
             * pumps and decodes, and the override channel carries only what
             * bk2/menu/devui stamped. The two paths must produce identical
             * gameplay for any input the host is not driving. */
            pad_restamp_if_due(0);
            PsyX_UpdateInput();
        } else {
            /* Under the thread split main owns the pump, the SDL-state
             * publish and the pad sample; this thread only consumes what was
             * published. That asymmetry is the whole point of the seam - the
             * consumer never touches SDL. */
            if (!split) {
                openpete_host_mouse_pump_iteration();
                openpete_pump_events();
                /* Re-publish now the pump has refreshed SDL, so the pad sample
                 * and this iteration's consumers see current state rather than
                 * the pre-pump snapshot taken above. */
                openpete_host_input_publish_sdl();
                pad_restamp_if_due(0);
                openpete_host_input_sample(
                    (openpete_host_input_modstate() & SDL_KMOD_ALT) != 0);
            }
            openpete_host_input_latch(ovr_claimed);
            pace_latch_ns  = openpete_host_input_latched_sample_ns();
            pace_latch_seq = ovr_claimed ? OP_PACE_LATCH_CLAIMED
                                         : openpete_host_input_latched_seq();
            pace_latch_bits = openpete_host_input_latched_bits();
            PsyX_Pad_InternalPadUpdates();
            openpete_host_input_parity_check(ovr_claimed);
        }

        /* Stage this tick's input record. Every override writer - bk2
         * replay, the settings menu, devui, the live host latch - has
         * stamped the channel by now, so slot 0's staged bytes are the ones
         * the tick is about to consume, published early enough for a
         * consumer running INSIDE the tick to read them. Slots 1-3 stay
         * empty until host sampling fills them.
         *
         * An inactive override means PsyCross decodes live inside the tick
         * and there is nothing to stage yet; the post-yield readback below
         * fills slot 0 in that case. */
        input_log_stage_reset(openpete_frame);
        /* Before the game calls PadInitDirect there is no pad buffer, so the
         * post-yield readback reports PsyCross's no-buffer default rather
         * than anything the host stamped. Staging that same default keeps
         * the staged record equal to what the log will carry, instead of
         * publishing a host value the guest has nowhere to receive. */
        bool p1_ready  = PsyX_Pad_P1Ready() != 0;
        bool p1_staged = p1_ready && g_PsyX_Pad_OverrideActive != 0;
        if (!p1_ready) {
            const unsigned char no_buffer[4] = { 128, 128, 128, 128 };
            input_log_stage_port(0, 0xFFFF, no_buffer);
        }
        /* Slots 1-3 from this iteration's latch. Absent when no device drives
         * the slot, and absent whenever a higher-priority writer claimed the
         * channel: under bk2 replay or the settings menu the extra players
         * are driven by the movie, not by whatever is plugged in. */
        for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++) {
            openpete_pad_snapshot_t sn;
            if (!openpete_host_input_latched_slot(sl, &sn)) continue;
            /* Snapshot axes are PSX pad->analog order; the log stores bk2. */
            const unsigned char slot_axes[4] = {
                sn.axes[2], sn.axes[3], sn.axes[0], sn.axes[1] };
            input_log_stage_port(sl, sn.bits, slot_axes);
        }
        /* Under bk2 replay the extra player comes from the movie, not from
         * whatever is plugged in: the latch above was cleared when the movie
         * claimed the channel. Movie axes are already in the log's bk2
         * order, so they need no reordering. A movie declaring one port
         * leaves the slot absent, which is what it recorded. */
        if (bk2_active && bk2.players >= 2 && bk2.p2_frames) {
            int bf2 = bk2_feed_row();
            if (bf2 >= 0 && bf2 < bk2.frame_count) {
                const bk2_frame_t* f2 = &bk2.p2_frames[bf2];
                input_log_stage_port(1, f2->buttons, f2->axes);
            }
        }

        /* The harness overrides live devices so the N-port carry stays
         * testable on a machine with one pad, or none. */
        if (synth_pads_enabled() == 1) {
            for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++) {
                uint16_t b; unsigned char ax[4];
                synth_pad_for(openpete_frame, sl, &b, ax);
                input_log_stage_port(sl, b, ax);
            }
        }
        if (p1_staged) {
            /* PSX pad->analog order is RX,RY,LX,LY; the log stores bk2
             * order, LX,LY,RX,RY. */
            const unsigned char staged_axes[4] = {
                (unsigned char)g_PsyX_Pad_AnalogOverride[2],
                (unsigned char)g_PsyX_Pad_AnalogOverride[3],
                (unsigned char)g_PsyX_Pad_AnalogOverride[0],
                (unsigned char)g_PsyX_Pad_AnalogOverride[1],
            };
            input_log_stage_port(0, (uint16_t)g_PsyX_Pad_InputOverride,
                                 staged_axes);
        }

        /* Hand control to the game until the next VSync(0) yield. Gated so
         * --render-fps>0 outer iterations that don't have a sim tick due
         * skip the yield entirely (input is still pumped above; pacer below
         * sleeps to the next deadline). Frame-step adds a second gate:
         * while g_paused the yield only fires while g_step_one is raised
         * (step key or overlay), which stays raised until the game draws. */
        unsigned frame_before_yield = openpete_frame;
        { unsigned e_, l_, j_; (void)bk2_feed_iter_take(&e_, &l_, &j_); }   /* writes outside this yield */
        bool ticked_this_iter = false;
        bool yield_now = sim_due && (!g_paused || g_step_one)
                                 && !rewound_this_iter;
        /* Pacing log, per VBL. The deadline in force for this iteration is
         * next_sim.at as it stands BEFORE the tail advances it - the grid
         * point the pacer slept to at the end of the previous iteration - so
         * exec_start minus that is the tick's start-phase error.
         *
         * `openpete_unlock_fps` carries the previous iteration's verdict,
         * which is the one that governed the sleep this iteration started
         * from: the right question is whether a deadline was in force, not
         * what the next one will be. */
        const int pace_paced = !pace_unlock_prev;
        op_time_t pace_deadline_ns = pace_paced ? next_sim.at : 0;
        if (pace_regrid_pending) { pace_deadline_ns = 0; pace_regrid_pending = 0; }
        op_time_t pace_exec_start_ns = 0, pace_exec_end_ns = 0;
        uint8_t   pace_flags = pace_paced ? 0u : OP_PACE_F_UNLOCK;
        if (pace_slip_pending) { pace_flags |= OP_PACE_F_SLIPPED; pace_slip_pending = 0; }
        if (yield_now) {
            const bool stepping = step_begin_vbl(g_paused, &g_step_one);
            game_on_frame_pre(&g_game.cpu, openpete_frame);
            /* Calibration stall (OPENPETE_PACE_INJECT_US), off unless asked
             * for: a known lateness the log must then report, so the
             * instrument is checked against truth rather than trusted. */
            openpete_pace_inject(pace_paced);
            /* Start-alignment invariant: an iteration that runs the game
             * fiber must have made its latch decision THIS iteration, before
             * this point. The input window that external injection measures
             * - one VBL period, grid-aligned - is a property of that
             * ordering, so a refactor that moved the latch after the switch,
             * or skipped it on some path, would silently change the window
             * while every phase statistic kept reporting normally. Checked
             * rather than commented; warned rather than fatal, because a
             * player's game must not die for a diagnostic. */
            if (!openpete_host_input_legacy() &&
                pace_latch_seq == OP_PACE_LATCH_NONE) {
                static int warned;
                if (!warned++)
                    OPL_ERROR_F("input",
                                "ORDERING: VBL %u runs the fiber with no latch "
                                "decision - input phase is no longer defined "
                                "by the pump->decode->latch->switch order",
                                openpete_frame);
            }

            pace_exec_start_ns = op_now();
            /* Bracket the VBL BEFORE the switch: the guest's pad read fires
             * inside it, and a record stamped against the previous VBL's
             * start would report a phase off by a whole period. */
            openpete_pace_vbl_begin(openpete_frame, pace_exec_start_ns,
                                     pace_latch_ns);
            {
                uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
                /* The guest touches neither the device nor device-visible
                 * memory while it runs (core/rhi/rhi_gpu_audit.h measures
                 * this); under eager production the present thread keeps
                 * presenting through this switch. */
                op_gpu_audit_bind_thread();
                g_openpete_guest_running = 1;
                co_switch((cothread_t)openpete_game_fiber);
                g_openpete_guest_running = 0;
                if (pt0) openpete_phase_add(OPPT_SIM_TICK, openpete_phase_now_us() - pt0);
            }
            pace_exec_end_ns = op_now();
            ticked_this_iter = (openpete_frame != frame_before_yield);
            /* A VBL on which the screen changes without a DrawOTag (the
             * display swaps to a buffer holding a scene no present has
             * shown, or the game draws its fed scene into the buffer on
             * screen) owes that scene a present. */
            openpete_present_pending = openpete_native_scene_yield_owes_present(openpete_present_pending);
            step_after_vbl(stepping, &g_step_one);

            /* Sample the actuator buffer the tick just wrote. Here, rather
             * than beside the pad latch above, because the guest fills that
             * buffer inside its own PadVSync: reading before the switch would
             * ship the previous tick's bytes and put a whole VBL of avoidable
             * latency on every rumble. Before the runahead speculation below,
             * so only the committed timeline can reach a motor. */
            if (ticked_this_iter)
                openpete_rumble_sample();
            {
                /* What this VBL cost on this thread. A VBL that queued no
                 * draw is the update VBL, which the eager planner subtracts
                 * from the span budget; one that did is the tick, whose
                 * execution tail sets the eager lag. */
                const op_time_t ex = pace_exec_end_ns - pace_exec_start_ns;
                if (!openpete_present_pending)
                    s_eager.vbl_ema = s_eager.vbl_ema ? (s_eager.vbl_ema * 7 + ex) / 8 : ex;
                else if (ex < (op_time_t)g_openpete_tick_period_ns / 2) {
                    /* Longer than half a tick is a load or a stall, not a
                     * tick: it must not set the lag, which would plan a
                     * span of a hundred targets and overflow the ring. */
                    if (s_eager.tick_ema) {
                        const op_time_t d = ex > s_eager.tick_ema ? ex - s_eager.tick_ema : s_eager.tick_ema - ex;
                        s_eager.tick_dev = (s_eager.tick_dev * 7 + d) / 8;
                    }
                    s_eager.tick_ema = s_eager.tick_ema ? (s_eager.tick_ema * 7 + ex) / 8 : ex;
                    if (ex > s_eager.tick_peak) s_eager.tick_peak = ex;
                }
            }

            bool ra_speculated = false;
            bool ra_skip_capture = false;
            if (ticked_this_iter) {
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
                int ra_want = (runahead_n > 0 && ra_snap &&
                               game_is_gameplay_live(&g_game.cpu));
                int ra_left = 0;      /* the speculation left gameplay */
                int ra_adopted = 0;   /* the snapshot's park site did not recur: no rollback */
                /* The snapshot is taken at a final yield only: a tick whose
                 * draw is first seen at an intermediate yield of its wait
                 * owes its speculation to the wait's final yield. */
                static int ra_owed = 0;
                if (!ra_want) openpete_ra_input_edge = 0;
                const int ra_due = ra_want && (openpete_present_pending || ra_owed);
                const int ra_now = ra_due && openpete_yield_final;
                ra_owed = ra_note_owed(ra_due && !ra_now, ra_owed);
                if (ra_now &&
                    ra_snapshot_save(ra_snap, &g_game.cpu,
                                     openpete_frame) == 0) {
                    /* Input-edge detection: the led branch changes exactly
                     * when the input we speculate with differs from last
                     * tick's. Flag the tick so presentation consumers can
                     * rebase their interp at the last SHOWN state instead
                     * of snapping to the corrected branch (measured: R2
                     * release at N=2 showed a -88u/present camera
                     * reversal; N ticks of mispredicted pan un-shown). */
                    {
                        static uint16_t s_ra_pbits;
                        static unsigned char s_ra_pax[4];
                        static int s_ra_have = 0;
                        uint16_t rb = PsyX_Pad_GetLastP1Bits();
                        unsigned char rax[4];
                        PsyX_Pad_GetLastP1Analog(rax);
                        openpete_ra_input_edge =
                            (s_ra_have && (rb != s_ra_pbits ||
                             memcmp(rax, s_ra_pax, 4) != 0)) ? 1 : 0;
                        s_ra_pbits = rb;
                        memcpy(s_ra_pax, rax, 4);
                        s_ra_have = 1;
                    }
                    /* Prediction-exactness oracle (env-gated, see ra_pc_*
                     * above): an input edge invalidates every pending
                     * prediction (the led branch legitimately changed);
                     * then any prediction targeting THIS committed tick
                     * must match the committed bytes exactly. RAM here is
                     * the completed committed tick at its yield - the same
                     * phase the led hash was taken at. */
                    if (ra_pc_on()) {
                        if (openpete_ra_input_edge) ra_pc_invalidate_all();
                        for (int i = 0; i < RA_PC_PENDING; i++) {
                            if (!s_ra_pc[i].valid) continue;
                            if (s_ra_pc[i].target < openpete_frame) {
                                s_ra_pc[i].valid = 0;   /* missed (mode transition) */
                                continue;
                            }
                            if (s_ra_pc[i].target != openpete_frame) continue;
                            s_ra_pc[i].valid = 0;
                            s_ra_pc_checked++;
                            if (s_ra_pc[i].hash != ra_pc_hash()) {
                                s_ra_pc_mismatches++;
                                fprintf(stderr, "[ra-predict] MISMATCH f=%u: led "
                                        "prediction != committed state (a read of "
                                        "host state outside the rollback "
                                        "boundary - neither guest RAM nor the "
                                        "mod arena - during speculation?)\n",
                                        openpete_frame);
                                if (ra_pc_on() >= 2 && s_ra_pc[i].led) {
                                    /* Dump differing ranges as PSX vaddrs. */
                                    const uint8_t* a = s_ra_pc[i].led;
                                    const uint8_t* b = openpete_psx_get_ram();
                                    unsigned ranges = 0;
                                    for (uint32_t off = 0; off < OPENPETE_PSX_RAM_SIZE && ranges < 16;) {
                                        if (a[off] == b[off]) { off++; continue; }
                                        uint32_t end = off;
                                        while (end < OPENPETE_PSX_RAM_SIZE && end - off < 4096 &&
                                               (a[end] != b[end] || (end + 16 < OPENPETE_PSX_RAM_SIZE &&
                                                memcmp(a + end, b + end, 16) != 0)))
                                            end++;
                                        fprintf(stderr, "[ra-predict]   diff 0x%08X..0x%08X "
                                                "led=%02X%02X%02X%02X comm=%02X%02X%02X%02X\n",
                                                OPENPETE_PSX_KSEG0_BASE + off,
                                                OPENPETE_PSX_KSEG0_BASE + end,
                                                a[off], a[off+1], a[off+2], a[off+3],
                                                b[off], b[off+1], b[off+2], b[off+3]);
                                        ranges++;
                                        off = end + 1;
                                    }
                                    const uint8_t* sa = s_ra_pc[i].led + OPENPETE_PSX_RAM_SIZE;
                                    const uint8_t* sb = openpete_psx_get_scratch();
                                    if (memcmp(sa, sb, OPENPETE_PSX_SCRATCH_SIZE) != 0)
                                        fprintf(stderr, "[ra-predict]   diff in SCRATCHPAD\n");
                                }
                            }
                        }
                    }
                    /* Host flags the guest's draw thunks mutate are part
                     * of the rolled-back state: a speculative DrawOTag
                     * leaking present_pending fired EXTRA canonical
                     * presents (measured: canon_tick cadence doubled
                     * under runahead - every tick-keyed instrument
                     * sampled the wrong sim moments). */
                    int      ra_pp    = openpete_present_pending;
                    unsigned ra_prims = openpete_primitives_drawn;
                    PsyX_SPUAL_Speculate(1, 0);
                    openpete_speculative = 1;
                    /* Speculated ticks run inside this VBL's bracket but are
                     * not this VBL: their pad reads must not be measured
                     * against its exec_start. */
                    openpete_pace_vbl_suspend(1);
                    /* runahead_n game ticks: the loop anchor's entries
                     * since the snapshot, ending at the next final yield
                     * (where the rollback below may load). A game without
                     * an anchor speculates 2N yields. The bound caps a
                     * speculation that never reaches one. */
                    const uint32_t ra_t0 = openpete_loop_anchor_ticks();
                    const int ra_ticks = openpete_game_runtime()->loop_anchor_pc != 0u;
                    for (int v = 0; v < (ra_ticks ? 8 * runahead_n + 16 : 2 * runahead_n); v++) {
                        if (ra_ticks && v > 0 && openpete_yield_final &&
                            openpete_loop_anchor_ticks() - ra_t0 >= (uint32_t)runahead_n)
                            break;
                        unsigned bf = openpete_frame;
                        game_on_frame_pre(&g_game.cpu, openpete_frame);
                        g_openpete_guest_running = 1;
                        co_switch((cothread_t)openpete_game_fiber);
                        g_openpete_guest_running = 0;
                        if (openpete_frame == bf) break;   /* safety */
                        /* Speculation left gameplay (a load, a cutscene):
                         * stop counting ticks. The rollback below still
                         * loads when the transition waits at the
                         * snapshot's psyq_VSync site. */
                        if (!game_is_gameplay_live(&g_game.cpu)) {
                            ra_left = 1;
                            break;
                        }
                    }
                    /* The rollback below loads only at a final yield. */
                    ra_adopted = ra_speculate_to_final(openpete_state_mem_site(ra_snap),
                                                       8 * runahead_n + 16, &ra_left);
                    openpete_speculative = 0;
                    openpete_pace_vbl_suspend(0);
                    /* An adopted speculation's pad writes and consumer
                     * entries reach the recorder; a rolled-back one's are
                     * run again by the committed ticks. */
                    bk2_feed_speculation_end(ra_adopted);
                    /* An adopted speculation is the timeline: its frames
                     * are fed. A rolled-back one is rendered again by the
                     * committed ticks. */
                    PsyX_SPUAL_Speculate(0, ra_adopted);
                    if (!ra_adopted) {
                        openpete_present_pending  = ra_pp;
                        openpete_primitives_drawn = ra_prims;
                    }
                    /* Record the led prediction while RAM is still the
                     * speculation head (the rollback happens below, after
                     * the interp captures); openpete_frame is the led
                     * frame here. */
                    if (!ra_left && !ra_adopted) ra_pc_record_led();
                    ra_speculated = true;
                    pace_flags |= OP_PACE_F_SPECULATIVE;
                }
                if (ra_left && !ra_adopted) {
                    /* A speculation that left gameplay leads into a
                     * transition the committed ticks reach N ticks later:
                     * roll back now, so the capture below takes the
                     * committed state as on a tick without speculation. */
                    unsigned ra_lf = openpete_frame;
                    if (openpete_state_mem_load(ra_snap, &g_game.cpu, &ra_lf, 1) == 0)
                        openpete_frame = ra_lf;
                }
                ra_skip_capture = (ra_want && !ra_speculated && !ra_left) || ra_adopted;
                if (ra_left || ra_adopted) {
                    /* An adopted speculation's VBLs are the real timeline;
                     * a rolled-back one was restored above. Either way the
                     * rollback below is skipped, predictions pending
                     * against the led branch die, and the presentation
                     * drops its pairs and snaps (a fade is starting), so
                     * no lerp pairs a led state with the committed ticks
                     * that follow. */
                    ra_speculated = false;   /* skip the rollback below */
                    ra_pc_invalidate_all();
                    openpete_ra_input_edge = 0;
                    /* Presentation-only reset (NOT the guest-discontinuity
                     * broadcast - the timeline is continuous forward, and
                     * the broadcast would finalize an in-progress bk2
                     * recording). */
                    openpete_interp_reset();
                    openpete_interp_objs_reset();
                    ra_end_log(ra_left, ra_adopted);
                }

                /* Snapshot the transform-bearing regions into the
                 * interp ring while the game fiber is still yielded
                 * (consistent RAM). No-op unless --render-fps > sim rate.
                 *
                 * Stamp with a PLL-SMOOTHED VBL clock, not raw op_now(): the
                 * raw completion time of a tick jitters by milliseconds with
                 * sim/render load, and every snapshot ROLL resets the sub-tick
                 * alpha mapping against its stamp - jittery stamps make shown
                 * interp time advance 0.05..1.4 ticks between evenly-generated
                 * presents (measured on the titlescreen flyby: fast movers
                 * judder ±half a tick at ANY --render-fps while the slow
                 * background looks fine). Locking stamps to the ideal VBL grid
                 * (proportional 1/32 nudge tracks real cadence; full resync on
                 * a stall/jump, e.g. CD loads) keeps the present-time -> shown-
                 * time mapping continuous across rolls. */
                if (!ra_skip_capture) {
                    static op_time_t s_vclk = 0;
                    static unsigned  s_vclk_f = 0;
                    /* Ideal VBL step in WALL time - scaled so --tick-scale keeps
                     * the stamps on the (stretched) grid; an unscaled step makes
                     * the pred err exceed the resync bound every tick and stamps
                     * degrade to raw jittery completion times. */
                    const op_time_t vT = sim_period_ns;
                    op_time_t raw = op_now();
                    unsigned df = openpete_frame - s_vclk_f;
                    s_vclk_f = openpete_frame;
                    if (s_vclk == 0 || df == 0 || df > 8) s_vclk = raw;
                    else {
                        op_time_t pred = s_vclk + (op_time_t)df * vT;
                        int64_t err = (int64_t)raw - (int64_t)pred;
                        if (err > (int64_t)vT || err < -(int64_t)vT) s_vclk = raw;  /* stall/jump resync */
                        else s_vclk = pred + (op_time_t)(err / 32);
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
                    {
                        static op_time_t s_phase = -1;
                        if (s_phase < 0) {
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
                            s_phase = (op_time_t)(ms * 1e6);
                        }
                        op_time_t stamp = s_vclk - s_phase;
                        openpete_interp_capture(openpete_frame, stamp);
                        extern op_time_t g_openpete_tick_stamp;   /* objs capture shares the stamp */
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
                }

                /* Per-object (indirect) snapshot - walks the game's
                 * object pools (g_LevelMobys) into the keyed prev/curr
                 * table for sub-tick lerp. Same read-only, same
                 * --render-fps>sim gate. No-op if no game callback. */
                if (!ra_skip_capture) openpete_interp_objs_run_capture();

                /* Runahead: captures done at the led head - roll back to
                 * the committed tick before anything else observes RAM. */
                if (ra_speculated) {
                    unsigned ra_lf = openpete_frame;
                    if (ra_snapshot_rollback(ra_snap, &g_game.cpu,
                                             &ra_lf) == 0) {
                        openpete_frame = ra_lf;
                        /* Committed-timeline regions (g_Camera): replace
                         * their led bytes in the ring's newest slot with
                         * the just-restored committed state, so those
                         * regions pair a branch-free timeline and can
                         * never show (or snap back from) a misprediction. */
                        openpete_interp_recapture_committed();
                    }
                }
            }
        }

        /* Position log fires after each yield (post-fast-boot - address
         * holds garbage before BSS init). Frame counter is openpete_frame,
         * vbl is the synchronous yield-driven counter; both are
         * deterministic across runs by construction. */
        if (position_log_fp && ticked_this_iter && !openpete_fast_boot_active) {
            position_log_emit(position_log_fp, openpete_frame,
                              SDL_GetAtomicInt(&openpete_vbl));
        }
        if (gameplay_hash_log_fp && ticked_this_iter && !openpete_fast_boot_active) {
            game_on_hash_emit(openpete_frame);
            gameplay_hash_log_emit(gameplay_hash_log_fp, openpete_frame,
                                   SDL_GetAtomicInt(&openpete_vbl));
        }

        /* --bk2-record: one bk2 frame per unit of the feed key so replay
         * timing matches recording 1:1. Sampled AFTER the yield because PsyX folds
         * keyboard/gamepad/--bk2 override into pad->buttons inside the
         * game's PadVSync handler - pre-yield the value would still be
         * stale from the previous frame. Also skip iterations where the
         * frame counter didn't advance (gated event pumps, no-tick yields
         * from dispatch stubs), which would otherwise inflate the movie
         * length. */
        if (ticked_this_iter) {
            /* Per-VBL input log - ALWAYS on: one entry
             * per VBL the fiber consumed this iteration, all carrying the
             * tick-latched bits (the same expansion the bk2 recorder used
             * before the lift-out, so recorded movies stay byte-identical).
             * Consumers: bk2_record (window [begin,finalize)), the rewind
             * replay and the runahead inject drivers. */
            unsigned ticks = openpete_frame - frame_before_yield;
            uint16_t bits = PsyX_Pad_GetLastP1Bits();
            unsigned char ax[4];                 /* PSX pad->analog order: RX,RY,LX,LY */
            PsyX_Pad_GetLastP1Analog(ax);
            unsigned char bk2axes[4] = { ax[2], ax[3], ax[0], ax[1] };  /* -> LX,LY,RX,RY */
            /* The guest readback stays the truth the log stores for slot 0.
             * When the override was stamped pre-yield the staged bytes are
             * that same value published early, and the counter proves it:
             * a nonzero mismatch means an in-tick slot-0 read saw bytes the
             * log never recorded, which would desync replay. */
            if (p1_staged) {
                const openpete_input_port_t* st = &input_log_staged()->port[0];
                s_p1_stage_checks++;
                if (st->bits != bits || memcmp(st->axes, bk2axes, 4) != 0) {
                    if (s_p1_stage_mismatches == 0) {
                        s_p1_stage_first_frame = openpete_frame;
                        s_p1_stage_first_staged_bits = st->bits;
                        s_p1_stage_first_read_bits = bits;
                        memcpy(s_p1_stage_first_staged_axes, st->axes, 4);
                        memcpy(s_p1_stage_first_read_axes, bk2axes, 4);
                    }
                    s_p1_stage_mismatches++;
                }
            }
            if (synth_pads_enabled()) {
                s_synth_stage_checks++;
                if (!synth_pads_match(input_log_staged(), frame_before_yield))
                    s_synth_stage_mismatches++;
            }
            input_log_stage_p1_readback(bits, bk2axes);
            for (int sl = 0; sl < OPENPETE_PAD_SLOTS; sl++)
                if (input_log_staged()->port[sl].present)
                    s_slot_present_vbls[sl] += ticks;
            for (int a = 0; a < OPENPETE_PAD_SLOTS; a++) {
                const openpete_input_port_t* pa = &input_log_staged()->port[a];
                if (!pa->present) continue;
                for (int b = a + 1; b < OPENPETE_PAD_SLOTS; b++) {
                    const openpete_input_port_t* pb = &input_log_staged()->port[b];
                    if (!pb->present) continue;
                    if (pa->bits != pb->bits ||
                        memcmp(pa->axes, pb->axes, 4) != 0)
                        s_slot_differed[a][b] += ticks;
                }
            }
            input_log_commit_yields(ticks, bk2_recording, bits);
        }

        if (sim_due && opts->debug_triangle) {
            /* Re-issue the static triangle so it's drawn over whatever the
             * game queued - guarantees a non-black on-screen pixel even
             * when the title-screen overlay is a stub. Gated on sim_due so
             * render-only iterations under --render-fps>0 don't pollute the
             * next tick's OT. */
            openpete_debug_triangle_emit();
        }

        /* Game yielded. Only present if the iter actually drew - see the
         * note above the pre-loop block. Screenshot fires BEFORE SwapWindow
         * so it captures the just-rendered backbuffer (post-swap, GL_BACK
         * holds rotated-in stale content that doesn't match the window). */
        /* True once a REAL game frame has been presented this iter.
         * The intermediate block must key off THIS, not
         * openpete_present_pending, which the block below clears to 0
         * before the intermediate gate runs - keying off the cleared
         * flag double-presented (game frame + a spurious alpha≈0
         * interpolated frame) on every draw iter (the "+30 fps"). */
        bool did_present_this_iter = false;
        /* World-liveness stamp: virtual-clock time of the most recent CANONICAL
         * present. During a CD load / level transition the game fiber stops
         * rendering (busy in CdRead/VSync waits) while the virtual clock keeps
         * ticking - the sub-tick spine must FREEZE then (present the last
         * composed scene) instead of re-deriving from RAM that the incoming
         * level is actively overwriting (the "mobys across the whole level
         * visible during the load" bug: half-loaded moby pools re-extracted
         * against a stale camera, with no terrain feed to occlude them). The
         * legacy PsyCross intermediate path never had this bug because it
         * re-presents a FROZEN saved OT; this stamp gives the spine the same
         * frozen-while-stuck semantics. 0 until the first canonical present
         * (spine stays off until there is something real to interpolate). */
        static op_time_t s_last_canon_present_ns = 0;
        if (openpete_present_pending && !openpete_skip_present_until_level) {
            /* Canonical draw-tick counter: the Nth increment is the same sim
             * moment in every run (tick-deterministic sim), unlike
             * openpete_frame whose VBL<->tick mapping drifts with host timing.
             * Keys --screenshot-on-tick (here and the native-variant dump
             * below). */
            openpete_canon_tick++;
            bk2_feed_map_present();
            int shot_key = opts->screenshot_ticks_keyed ? (int)openpete_canon_tick
                                                        : (int)openpete_feed_key_now();
            const bool capture_canon_present = openpete_capture_canon_reference(opts, shot_key);
            /* --record: capture this presented game frame before EndScene
             * stamps the FPS overlay. PTS is the wall-clock capture instant
             * in microseconds; the first frame anchors t=0. Reads the present
             * FBO at internal resolution (bars-free). No-op when disabled.
             * Keyed on the SCENE predicate, not the display one. When a native
             * scene is being drawn, the async readback ring inside
             * openpete_native_render_frame below captures the on-screen frame,
             * so a vk recording holds the native SSAA/widescreen frame. In
             * PsyCross display mode the offscreen holds a blit that update_layout
             * has already aspect-fitted into the window - letterboxed, and
             * downscaled wherever the window is not the reference extent - so
             * recording it would be strictly worse than reading the present FBO
             * here at internal resolution, bars-free. */
            if (!openpete_native_scene_enabled())
                openpete_record_capture_frame((uint64_t)(op_now() / 1000));
            PsyX_EndScene();
            openpete_present_pending = 0;
            did_present_this_iter = true;
            s_last_canon_present_ns = op_now();   /* world-liveness stamp (see the
                                                   * native_intermediate gate) */

            /* Bracket the canonical present the same way the intermediate one
             * below is bracketed. Only the intermediate path was instrumented,
             * which is survivable while a present is cheap and useless the
             * moment it is not: on a GPU-limited machine the shed predicate
             * declines nearly every intermediate (measured: 16 admitted against
             * 3704 shed), so what remains on the sim thread is the canonical
             * present - the one cost shedding can never avoid, and the one the
             * log could not name. Worse, the intermediate figure is a SELECTED
             * sample: the predicate only admits a present it expects to fit, so
             * quoting it as "present cost" understates by construction. */
            const op_time_t pace_canon_t0 = op_now();
            op_time_t pace_canon_extract_ns = 0;
            openpete_pace_inject_canonical();  /* fault injection, off by default */

            /* Per-canonical-frame commit: the game latches which render paths this
             * frame drew (the "canon drawn" mask the sub-tick spine gates each
             * reextract feeder on) and clears its per-frame interval table for the
             * next frame. Driven here - once per canonical present, renderer-
             * agnostic - via a registered callback rather than psyq_DrawOTag, so it
             * does not depend on the PsyCross OT submission. The game
             * fiber's DrawOTag (which ran before this host-loop iteration) already
             * consumed the intervals via the OT-walk filter; the feeder hooks that
             * recorded them keyed off the render functions, not DrawOTag, so the
             * mask stays valid without it. Unconditional (not inside the spine gate
             * below) so the bookkeeping runs every frame even with the spine off. */
            {
                openpete_native_commit_fn commit = openpete_native_get_commit();
                if (commit) commit();
            }

            /* Sub-tick spine: lag the CANONICAL native present too. The game's
             * render just built s_scene at the LATEST pose (tick N), but the
             * intermediates that follow present the 1-tick-lagged interpolation
             * (prev->curr). Showing the un-lagged canonical frame between lagged
             * intermediates makes every object jump forward one tick at the
             * canonical present and snap back on the next intermediate -> a 30 Hz
             * strobe / ghosting (terrain AND mobys) whenever the camera moves.
             * Re-extract the canonical present at the same lagged alpha so EVERY
             * present sits on one continuous, 1-tick-lagged timeline. Same gates
             * as the intermediate block below (minus the !did_present check);
             * mutually exclusive with it (that block is gated on
             * !did_present_this_iter, true only on non-draw iters). */
            {
                extern int g_openpete_interp_enabled;
                openpete_native_reextract_fn lag_re = openpete_native_get_reextract();
                /* Commit the per-tick fade/freeze liveness on EVERY canonical present,
                 * renderer-agnostic (outside the spine gate below). commit_fade rolls
                 * the fade prev/curr AND clears the s_game_fade_seen / s_game_freeze_seen
                 * one-tick flags. Those flags are set by set_fade/set_freeze_backdrop
                 * every canonical tick, so if the only thing that clears them sat inside
                 * the spine gate, toggling interp/unlock/pause mid-fade would latch a
                 * flag stale and flash the fade/freeze for one tick when the spine
                 * re-engages. The raw (spine-off) present path doesn't read what this
                 * writes, so an unconditional commit is harmless there. */
                openpete_native_scene_commit_fade();
                run_canonical_capture();
                /* !headless: headless has no live present stream, so the canonical
                 * dump must stay the PURE canonical frame. The presentation-golden
                 * intermediate corpus enables interp capture headless (for the
                 * ring), which would otherwise arm this lag leg and lag the
                 * canonical -vk; the explicit alpha=0.5 capture below owns the
                 * sub-tick leg instead. */
                const int spine_on = lag_re && !opts->headless &&
                    openpete_native_scene_enabled() && !openpete_unlock_fps &&
                    g_openpete_interp_enabled && openpete_interp_enabled() &&
                    !(g_paused && !g_step_one);
                /* Eager production takes the whole present path from here:
                 * the canonical scene is the first of the span's plan, and
                 * the present thread shows it. Never with a recording or a
                 * screenshot target -- those read the inline present. */
                /* OPENPETE_EAGER_SHOT=1 (diagnostic): keep eager engaged with a
                 * --screenshot-on-frame target; the dump then drains the
                 * present thread, renders from the scene the producer holds,
                 * and hands the display back. */
                static int eager_shot = -1;
                if (eager_shot < 0) { const char *e = getenv("OPENPETE_EAGER_SHOT"); eager_shot = (e && *e && *e != '0'); }
                const int eager_now = spine_on && openpete_present_thread_enabled() &&
                    !opts->record_path && (opts->screenshot_frame_count == 0 || eager_shot) &&
                    opts->screenshot_intermediate_count == 0 &&
                    opts->screenshot_presents_start < 0;
                if (eager_now != s_eager.active) {
                    openpete_present_thread_set_active(eager_now);   /* 0: drains */
                    s_eager.active = eager_now;
                    OPL_INFO_F("eager", "%s", eager_now ? "engaged" : "disengaged (inline presents)");
                }
                if (eager_now) {
                    const op_time_t period = op_period_ns(s_render_period);
                    const op_time_t curr = openpete_interp_curr_tick_ns();
                    const op_time_t now = op_now();
                    op_time_t first = next_render.at;
                    while (first < now) first += period;    /* the next slot not yet gone */
                    /* The span is anchored on the newest capture's stamp - unless
                     * that stamp is stale. Captures dedupe when the interpolated
                     * regions do not change (title screen, a cutscene holding
                     * the camera), and a span anchored on a stamp a tick or
                     * more old collapses to one or two slots while the inline
                     * path keeps presenting the live scene at full rate. Anchor
                     * on this tick's landing instead. */
                    const op_time_t tp = (op_time_t)g_openpete_tick_period_ns;
                    const op_time_t anchor = (curr && now - curr < 2 * tp) ? curr : now;
                    s_eager.anchor_age = now - anchor;
                    const op_time_t span_end = anchor + tp;
                    eager_plan(now, first, period, span_end,
                               next_sim.at + 2 * sim_period_ns);
                    /* The canonical scene, then as much of the span as fits
                     * before the update VBL is due - the latter only while
                     * the world is live (the gate below). */
                    s_eager.world_live = s_last_canon_present_ns != 0 &&
                        (now - s_last_canon_present_ns) <= 6 * sim_period_ns;
                    eager_produce(s_eager.world_live ? next_sim.at + sim_period_ns - 500000
                                                     : now, period);
                    pace_canon_extract_ns = 0;
                } else
                if (spine_on) {
                    uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
                    extern int g_openpete_canon_lag_leg;
                    g_openpete_canon_lag_leg = 1;
                    vp_set_present_target();     /* even display-slot time for this present */
                    const op_time_t rex_t0 = op_now();
                    lag_re(&g_game.cpu);
                    openpete_native_scene_sig_report(1);
                    pace_canon_extract_ns = op_now() - rex_t0;
                    g_openpete_canon_lag_leg = 0;
                    if (pt0) openpete_phase_add(OPPT_REX_TOTAL, openpete_phase_now_us() - pt0);
                }
            }

            /* Capture state of this present, ahead of its device services. */
            openpete_capture_present_begin(opts);

            /* Native renderer: present one frame to the Vulkan window now that
             * PsyCross's present FBO is freshly rendered (read back here as the
             * reference). No-op unless --renderer vk. */
            /* Mod draw service: commit the finished tick's draw commands,
             * run present hooks, push the overlay (near-zero cost modless).
             * See core/mods/mod_draw.c. */
            if (!s_eager.active) {
                openpete_mods_present_flush(&g_game.cpu, 1);

                /* The PTS arg tags this present for --record: the frame is queued
                 * into the async readback ring inside (no GPU stall), so a vk
                 * recording captures the on-screen frame at real speed. No-op unless
                 * native is the display AND recording is active. */
                const int audit_arm = gpu_audit_selftest_arm();
                if (audit_arm) g_openpete_guest_running = 1;
                openpete_native_render_frame((uint64_t)(op_now() / 1000));
                if (audit_arm) g_openpete_guest_running = 0;
            }
            /* Terrain-cache device service on the headless path, once per
             * VBL. Self-no-ops with a window, where the present entry runs
             * it on the thread that owns the device for that present. */
            openpete_native_tercache_service_headless();
            /* Occlusion grid on the headless path, at this present step
             * after the walk: the windowed live present builds it otherwise. */
            openpete_native_occl_service_headless();

            /* Target captures read from the native renderer: the owed canonical
             * present of the present sequence, the capture freeze backdrop and the
             * -vk dumps. The present's device services above ran first. */
            openpete_capture_canon_native(opts, shot_key, capture_canon_present);

            /* Player screenshot (the `screenshot` binding, F12 by default).
             * One unsuffixed PNG of the frame the window just showed: a
             * readback of the offscreen the present blitted, so it carries the
             * sub-tick scene, mods, postfx and overlays as presented, rather
             * than the reference-aligned recompose the --screenshot-on-frame
             * variants above need. Native display only - PsyCross is the
             * reference rasterizer, not a display this port ships. The capture
             * logs the file it wrote, so nothing is reported twice here. */
            {   /* A pending glitch mark, from the key or from
                 * OPENPETE_GLITCH_MARK_AT. Reads both rings and writes the
                 * file; no display state is touched. */
                char gpath[1200];
                openpete_glitch_mark_service(gpath, sizeof gpath);
            }

            if (s_screenshot_request || openpete_host_mouse_take_screenshot_request()) {
                s_screenshot_request = 0;
                char path[1024];
                if (!openpete_native_render_enabled()) {
                    OPL_WARN_F("screenshot",
                               "no native renderer: nothing to capture");
                } else if (openpete_screenshot_user_path(path, sizeof path) == 0) {
                    /* The device is the present thread's while eager is
                     * engaged: take it back for the capture, then return it. */
                    const int pt_was = openpete_present_thread_active();
                    if (pt_was) openpete_present_thread_set_active(0);
                    openpete_native_render_screenshot(path, "player");
                    if (pt_was) openpete_present_thread_set_active(1);
                }
            }

            /* Every-tick identity audit (OPENPETE_IDENT_AUDIT=<start VBL>):
             * compose this present's canonical scene and the alpha-1 re-extract
             * of the same tick, and report their pixel difference. Placed
             * between the two dump blocks so both still see the untouched
             * canonical scene - the audit restores it before returning. */
            if (openpete_ident_audit_armed()) {
                const int pt_was = openpete_present_thread_active();
                if (pt_was) openpete_present_thread_set_active(0);
                openpete_ident_audit_present(&g_game.cpu, openpete_frame,
                                             openpete_canon_tick,
                                             opts->screenshot_output);
                if (pt_was) openpete_present_thread_set_active(1);
            }

            /* Presentation-golden intermediate leg: the sub-tick re-extract of
             * each --screenshot-intermediate-on-frame target reached. */
            openpete_capture_canon_intermediate(opts, &g_game.cpu);

            /* Cache rotate moved to "right before next yield's GTE"
             * (see the yield_now block above) so intermediate renders
             * between sim ticks see the correct cache.s_prev for the
             * saved OT they're re-walking. */

            /* Frame-time recorder: one 32-byte record per
             * actual present. The deadline is computed against a
             * per-PRESENT counter (not openpete_frame, which is the
             * yield counter at PSX VBlank cadence - typically 2x
             * present rate for a 30 fps game), so the drift math in
             * scripts/measure-pacing.py compares like with like. The
             * record's `frame` field still carries openpete_frame as
             * metadata for cross-referencing with position-log /
             * gameplay-hash-log. */
            if (!s_eager.active) {
            ftl_record_present(&frame_time_log, frame_time_log_start_ns,
                               frame_time_log_target_period_ns, openpete_frame);
            pace_record_present(OP_PACE_CANONICAL);
            openpete_pace_present_span(OP_PACE_CANONICAL, pace_canon_t0,
                                       op_now(), pace_canon_extract_ns);

            /* Perf trace: close out this canonical present. Every CPU span
             * (SIM_TICK, the lag re-extract, native prepass/compose/gpu-sync)
             * and census emitted since the previous present binds here. */
            if (op_prof_on())
                op_prof_present((uint32_t)openpete_canon_tick, OP_PROF_CANONICAL);
            }
        }

        /* Native sub-tick intermediate render between sim ticks
         * (--render-fps > 30). Each intermediate iter rebuilds the
         * native scene against interpolated gamestate and presents it.
         * (There is no PsyCross screen-space re-emit fallback.)
         *
         * Conditions:
         *  - Not headless / unlock mode (no display target).
         *  - openpete_interp_enabled(): the render rate genuinely exceeds the
         *    sim rate. This is the SAME gate that arms capture (render_hz >
         *    VBL 59.826, kept in sync at startup + on every live render-fps
         *    change), NOT the weaker s_render_fps > 0. At/below sim parity -
         *    e.g. render-fps 30 vs the ~29.913 Hz sim - sub-tick interpolation
         *    is degenerate: the near-1:1 ratio duplicates/freezes frames
         *    (mobys "stick") while capture is off and the ring is stale. So
         *    30 fps presents canonical frames only, exactly like 'I'-toggle
         *    off (g_openpete_interp_enabled == 0).
         *  - No REAL frame presented this iter (did_present_this_iter).
         *    NOT !sim_due (sim_due fires every PSX VBL ~60 Hz but Spyro
         *    draws every other VBL, so ~30 sim-due iters/sec draw
         *    nothing - !sim_due wrongly made those present NOTHING:
         *    the "render-fps minus 30" deficit). And NOT
         *    !openpete_present_pending either - the present block clears
         *    that to 0 before this gate, so on draw iters it would read
         *    false and we'd double-present a spurious alpha≈0
         *    interpolated frame right after the game frame (the
         *    "render-fps plus 30"). did_present_this_iter is the exact
         *    predicate: present an interpolated frame iff a real one
         *    wasn't already shown this iteration.
         *  - Not paused without step pending. */
        const bool headless_block = opts->headless;

        /* Native sub-tick re-feed (high-FPS spine): rebuild the native
         * scene against interpolated gamestate and present that. */
        openpete_native_reextract_fn native_re = openpete_native_get_reextract();
        /* Eager disengages the moment its gate closes on ANY iteration (a
         * pause pressed mid-span), so the inline path never presents while
         * the present thread still holds the display. Re-engaging waits for
         * the next canonical tick, which owns the plan. */
        if (s_eager.active && (openpete_unlock_fps || !g_openpete_interp_enabled ||
                               !openpete_interp_enabled() || (g_paused && !g_step_one))) {
            openpete_present_thread_set_active(0);
            s_eager.active = 0;
            OPL_INFO_F("eager", "%s", "disengaged (inline presents)");
        }
        /* openpete_unlock_fps is computed at the loop tail, so on the first
         * iteration it does not yet reflect a skip fast-forward armed at
         * boot; read the fast-forward flag directly as well. */
        const bool native_intermediate =
            !headless_block && !openpete_unlock_fps &&
            !openpete_skip_present_until_level &&
            g_openpete_interp_enabled &&
            openpete_interp_enabled() &&
            !did_present_this_iter &&
            openpete_native_scene_enabled() &&
            native_re != NULL &&
            !(g_paused && !g_step_one);
        /* Predictive shed: an intermediate present is optional, and a tick is
         * not. If this present would not finish before the sim's next
         * deadline, skip it - the pacing contract is that presents are shed
         * before ticks slip, and a display frame nobody was promised is the
         * cheapest thing in the system to give up.
         *
         * The cost estimate is an EMA of what intermediate presents have
         * actually taken on this machine in this scene, measured below, so it
         * adapts to the hardware instead of encoding a guess about it. A
         * fresh EMA admits the first presents unconditionally: refusing to
         * present until a cost is known would never learn one. */
        /* The deadline this present must beat.
         *
         * `next_sim.at` is advanced at the LOOP TAIL, so on a sim_due
         * iteration it still names the deadline this iteration has already
         * satisfied. Comparing against it here reads a NEGATIVE budget at the
         * moment a full VBL period actually remains, and the predicate then
         * refuses the one intermediate present a non-drawing VBL is entitled
         * to (a drawing VBL is already excluded by did_present_this_iter).
         *
         * That cost exactly one present slot per tick, invariant to the
         * present rate - measured 0.84 doubled display gaps per tick at BOTH
         * --render-fps 480 and 240, which is the signature of a fixed
         * per-tick loss rather than of a rate or capacity limit.
         *
         * After a slip the tail advances by more than one period, so this
         * runs one period optimistic for a single iteration; the next one
         * reads the corrected grid. */
        const op_time_t pace_sim_deadline =
            sim_due ? next_sim.at + sim_period_ns : next_sim.at;

        int pace_shed_now = 0;
        if (s_eager.active) {
            /* A non-drawing iteration under eager production: this thread is
             * idle, so produce whatever of the plan still fits before the
             * next VBL. Nothing is presented from here. The VBL cost this
             * budgets against is measured on the drawing iterations' update
             * VBL (see pace_exec below). */
            /* The same world-liveness gate the inline intermediate path
             * applies: while canonical presents are not flowing (CD load,
             * level transition, a long producer stall) the guest is
             * rewriting RAM and a re-extract would read it mid-overwrite.
             * The display holds the last image; no scene is produced. */
            s_eager.world_live = s_last_canon_present_ns != 0 &&
                (op_now() - s_last_canon_present_ns) <= 6 * sim_period_ns;
            if (!did_present_this_iter && native_re && s_eager.world_live)
                eager_produce(pace_sim_deadline - 500000, op_period_ns(s_render_period));
            pace_shed_now = 1;   /* the inline path below stays quiet */
        } else
        if (native_intermediate) {
            /* Decay first, then take the max: without the decay this is a
             * high-water mark that one long budget pins forever, and the
             * probe would then wait for a moment that no longer comes. */
            const op_time_t here = pace_sim_deadline - op_now();
            s_present_budget_peak -= s_present_budget_peak / 64;
            if (here > s_present_budget_peak) s_present_budget_peak = here;
        }
        if (native_intermediate && s_present_cost_n >= PACE_COST_WARMUP
                && !openpete_unlock_fps) {
            /* Cover the tail, not the centre. k=3 takes the miss rate from
             * ~19 % to ~5 % on the recorded cost series; the cost of the wider
             * bound is shedding a little earlier, which is the trade this
             * predicate exists to make. */
            const int kk = pace_shed_margin_k();
            const op_time_t margin = kk
                ? s_present_cost_ema + (op_time_t)kk * s_present_cost_dev
                : s_present_cost_ema + s_present_cost_ema / 4;
            /* Lateness budget: how far a present may push the tick's start.
             *
             * The frame refused right before a tick is NOT a duplicate of the
             * canonical that follows - measured, it sits 0.083 of a tick
             * behind it against a 0.069 normal step - so the strict rule
             * discards real motion at the worst moment, just before the
             * display holds one image for the length of the tick.
             *
             * Spending the budget is gated on the TAIL bound rather than the
             * centre one, because the cost distribution is right-skewed (p50
             * 436 us, p90 707 us): admitting on a centre estimate is how a
             * 100 us trade becomes a multi-millisecond late tick. Zero (the
             * default) keeps the strict rule. */
            const op_time_t late_budget = pace_late_budget_ns();
            const op_time_t tail_margin =
                s_present_cost_ema + 3 * s_present_cost_dev;
            const int fits_strict = op_now() + margin <= pace_sim_deadline;
            const int fits_late   = late_budget > 0 &&
                op_now() + tail_margin <= pace_sim_deadline + late_budget;
            if (!fits_strict && !fits_late) {
                /* A shed produces no measurement, so a predictor that only
                 * learns from presents that RAN stops learning the moment it
                 * starts shedding: the estimate latches at whatever the last
                 * admitted present cost - a cold start, a shader compile, one
                 * slow scene - and sheds forever after, with nothing able to
                 * contradict it. Observed doing exactly that: 16 presents
                 * admitted, then every subsequent one shed for the rest of
                 * the run.
                 *
                 * So let one through periodically. The probe costs one late
                 * tick at worst and is the only way the estimate can come
                 * back down. */
                /* Probe only where a probe can do least harm: with at least
                 * half the tick period still available. A counter-based probe
                 * admits an expensive present at an arbitrary moment, which
                 * on slow hardware means one deliberately late tick every N -
                 * measured as a 11.7 ms tick p99 on a software rasterizer,
                 * caused entirely by the mechanism meant to protect it.
                 * Choosing the moment with the most budget bounds the damage
                 * to the case where a present genuinely cannot fit in half a
                 * tick, where a rare late tick is the price of ever
                 * discovering the cost came back down. */
                const op_time_t budget = pace_sim_deadline - op_now();
                /* Relative to what this machine actually offers, not to the
                 * tick period: an absolute threshold is a threshold some
                 * machines never reach, and there the probe silently stops
                 * existing. Three quarters of the observed peak keeps the
                 * "least harm" intent - the probe still waits for a
                 * near-best moment - while guaranteeing such a moment recurs.
                 * Capped at half a tick so a machine with plenty of headroom
                 * behaves exactly as before. */
                op_time_t probe_at = s_present_budget_peak
                                     - s_present_budget_peak / 4;
                if (probe_at > (op_time_t)g_openpete_tick_period_ns / 2)
                    probe_at = (op_time_t)g_openpete_tick_period_ns / 2;
                if (++s_present_shed_run >= PACE_SHED_PROBE_EVERY
                        && budget >= probe_at) {
                    s_present_shed_run = 0;
                } else {
                    pace_shed_now = 1;
                    openpete_pace_shed(OP_PACE_INTERMEDIATE, PACE_SHED_NO_BUDGET,
                                       (int64_t)s_present_cost_ema,
                                       (int64_t)(pace_sim_deadline - op_now()));
                }
            } else {
                s_present_shed_run = 0;
            }
        }

        if (native_intermediate && !pace_shed_now) {
            const op_time_t pace_span_t0 = op_now();
            op_time_t pace_extract_ns = 0;
            openpete_pace_inject_present();   /* fault injection, off by default */
            /* World-liveness gate: only re-derive the scene while canonical
             * presents are FLOWING (the last one within ~3 sim ticks on the
             * virtual clock). When the world is stuck - CD load, level
             * transition, any long producer stall - re-extracting reads RAM
             * mid-overwrite (see the stamp's comment); hold the last composed
             * scene instead and keep presenting it (a frozen frame, exactly
             * what the legacy saved-OT re-present shows there). Legit "camera
             * static but world animating" states (idle-homeworld dragons,
             * cutscenes) keep canonical presents flowing every tick, so the
             * per-object animation path is untouched by this gate. */
            const op_time_t vbl_ns = sim_period_ns;   /* scaled VBL: the gate is
                                                       * "~3 sim ticks" in WALL
                                                       * time under --tick-scale */
            const bool world_live = s_last_canon_present_ns != 0 &&
                (op_now() - s_last_canon_present_ns) <= 6 * vbl_ns; /* ~3 sim ticks */
            if (world_live) {
                uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
                vp_set_present_target();      /* even display-slot time for this present */
                const op_time_t ex0 = op_now();
                native_re(&g_game.cpu);       /* rebuild s_scene @ interpolated gamestate */
                s_inline_rex++;
                openpete_native_scene_sig_report(0);
                pace_extract_ns = op_now() - ex0;
                if (pt0) openpete_phase_add(OPPT_REX_TOTAL, openpete_phase_now_us() - pt0);
            }
            /* Mod draw service: present hooks at sub-tick alpha (no commit). */
            openpete_mods_present_flush(&g_game.cpu, 0);

            /* Present the sub-tick frame, tagged with its own PTS so a --render-fps
             * N vk recording is genuinely ~N fps VFR (each present's frame is
             * queued into the async readback ring with its capture instant). */
            openpete_native_render_frame((uint64_t)(op_now() / 1000));

            /* Log this sub-tick present too, so --frame-time-log sees the real
             * present cadence at --render-fps>30 (not just the ~30 Hz canon). */
            ftl_record_present(&frame_time_log, frame_time_log_start_ns,
                               frame_time_log_target_period_ns, openpete_frame);
            pace_record_present(OP_PACE_INTERMEDIATE);
            {
                /* Feed the predictor from measured cost, and log the span so
                 * attribution can name present CPU as a cause of a late tick
                 * rather than reporting it as unattributed. */
                const op_time_t span = op_now() - pace_span_t0;
                openpete_pace_present_span(OP_PACE_INTERMEDIATE, pace_span_t0,
                                           pace_span_t0 + span, pace_extract_ns);
                /* Deviation first: it is measured against the PREVIOUS
                 * centre, so updating the centre first would shrink every
                 * deviation toward zero and collapse the bound back onto the
                 * mean. */
                if (s_present_cost_n) {
                    const op_time_t d = span > s_present_cost_ema
                        ? span - s_present_cost_ema : s_present_cost_ema - span;
                    s_present_cost_dev = (s_present_cost_dev * 7 + d) / 8;
                }
                s_present_cost_ema = s_present_cost_n
                    ? (s_present_cost_ema * 7 + span) / 8
                    : span;
                if (s_present_cost_n < PACE_COST_WARMUP) s_present_cost_n++;
            }

            /* Perf trace: close out this intermediate present (re-extract +
             * native render spans since the canonical present bind here). */
            if (op_prof_on())
                op_prof_present((uint32_t)openpete_canon_tick, OP_PROF_INTERMEDIATE);

            /* Multi-present capture of native SUB-TICK frames (smoothness check):
             * --screenshot-presents-start/count captures consecutive presents
             * (the canonical p0 + these intermediates p1+), so viewing p0..pN
             * shows objects at interpolated sub-tick positions. */
            openpete_capture_subtick_present(opts);
        }

        /* Paused repaint: no tick runs, so no other present fires. */
        if (g_paused && !g_step_one && !did_present_this_iter)
            host_paused_repaint(opts);

        /* RAM dump runs outside the present_pending gate so a request for
         * the very first rendered frame (ram-dump-on-frame=0) still fires
         * even on iters where DrawOTag hasn't queued anything yet. Use >=
         * with a one-shot guard so the trigger is forgiving of skipped
         * frame numbers. */
        static int ram_dumped = 0;
        if (!ram_dumped && opts->ram_dump_on_frame >= 0 &&
            (int)openpete_feed_key_now() >= opts->ram_dump_on_frame) {
            ram_dumped = 1;
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
            /* Optional CPU state dump alongside the RAM dump. Read
             * g_game.cpu directly: the game fiber is yielded inside
             * VSync(0), the host fiber is here, both share this static
             * struct. Format matches the halt-at-pc path. */
            if (opts->halt_cpu_dump_output) {
                FILE* cfp = fopen(opts->halt_cpu_dump_output, "wb");
                if (cfp) {
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
                               opts->halt_cpu_dump_output, c->sp, c->gp);
                }
            }
        }


        /* PCSX-Redux trace advance fires from the dispatch hook (per
         * game-function entry, not per host-fiber iteration), so all the
         * host loop has to do is observe its verdict + quit-on-condition.
         * The hook sets terminal_status to one of:
         *   0 = still running, 1 = eof, 2 = divergence, 3 = io-error. */
        if (psxtrace) {
            switch (psxtrace->terminal_status) {
            case 1:
                OPL_INFO_F("psxtrace",
                           "trace exhausted (consumed %u records, "
                           "%llu compared, %llu hash mismatches)",
                           psxtrace->records_consumed,
                           (unsigned long long)psxtrace->frames_compared,
                           (unsigned long long)psxtrace->hashes_diverged);
                quit = 1; exit_reason = "psxtrace-eof"; break;
            case 2:
                if (!opts->psxtrace_keep_going) {
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
                    quit = 1; exit_reason = "psxtrace-divergence";
                }
                break;
            case 3:
                quit = 1; exit_reason = "psxtrace-error"; break;
            default: break;
            }
        }

        if (g_shutdown_requested) {
            quit = 1;
            exit_reason = "user-quit";
        }
        if (!quit && g_game.done) {
            quit = 1;
            exit_reason = "game-returned";
        }
        if (!quit && opts->frame_limit > 0 &&
            (int)openpete_feed_key_now() >= opts->frame_limit) {
            quit = 1;
            exit_reason = "frame-limit";
        }
        /* --record-frames: the capture window has closed. Stop here rather
         * than at the next frame/tick boundary so the clip ends on exactly the
         * requested frame count and every rate covers the same game time. */
        if (!quit && openpete_record_finished()) {
            quit = 1;
            exit_reason = "record-frames";
        }
        if (!quit && opts->max_runtime_seconds > 0 &&
            (SDL_GetTicks() - start)
                > (Uint64)opts->max_runtime_seconds * 1000) {
            quit = 1;
            exit_reason = "max-runtime-seconds";
        }

        /* Sample log so the user can see the loop is alive. Gated on
         * sim_due so a render-only iter under --render-fps>0 doesn't
         * re-log the same frame number. Same gate clears
         * openpete_primitives_drawn after a tick consumed/presented its
         * draws, not on render-only iterations that didn't add any. */
        if (sim_due) {
            if ((openpete_frame & 0x3F) == 1u) {
                OPL_INFO_F("loop",
                           "frame=%u prims_last=%u elapsed_ms=%llu",
                           openpete_frame, openpete_primitives_drawn,
                           (unsigned long long)(SDL_GetTicks() - start));
            }
            openpete_primitives_drawn = 0u;
        }

        /* No PsyX_BeginScene here - DrawOTag starts the scene on demand. */

        /* The turbo binding drives the host loop's own pacing + the GL
         * swap-interval bypass. vbl is yield-driven so this only affects
         * wall-clock speed, not game state - fast-forward is invisible to
         * BK2 replay determinism. Scheduled --unlock-frames windows (test
         * harness) OR into the same flag. Read from this iteration's
         * keymap poll, so it is a tick newer than the peek at the top of
         * the loop; this one is authoritative. */
        /* OPENPETE_FORCE_PACING=1 (read once, then cached) keeps the
         * legacy / dual-pacer cadence active even under --headless. Used
         * by scripts/measure-pacing.py to capture pacing numbers without
         * needing a real display. */
        /* One VBL record per iteration that ran the game fiber. Iterations
         * that only presented (--render-fps > tick rate) are PRESENT records;
         * conflating the two would make the tick cadence look like the
         * present cadence, which is the confusion this log exists to end. */
        if (yield_now) {
            if (g_paused)           pace_flags |= OP_PACE_F_PAUSED | OP_PACE_F_STEPPED;
            if (rewound_this_iter)  pace_flags |= OP_PACE_F_REWIND;
            if (s_last_canon_present_ns == 0 ||
                (op_now() - s_last_canon_present_ns) > 6 * sim_period_ns)
                pace_flags |= OP_PACE_F_NOT_LIVE;
            openpete_pace_vbl(openpete_frame, pace_flags,
                              openpete_pace_pad_reads_this_vbl(),
                              pace_deadline_ns, pace_latch_ns,
                              pace_exec_start_ns, pace_exec_end_ns,
                              pace_latch_seq, pace_latch_bits);
            openpete_boot_tick();
            openpete_live_switch_poll(openpete_frame);
        }

        const int s_force_pacing = pace_force_pacing();
        bool tab_unlock = (opts->headless && !s_force_pacing) ||
                          openpete_key_down(OPK_TURBO);
        bool sched_unlock = opts->unlock_ranges_count > 0 &&
                            unlock_frame_in_ranges(opts, openpete_feed_key_now());
        bool unlock_fps = tab_unlock || sched_unlock ||
                          openpete_skip_present_until_level;
        skip_mute_sync();
        if ((unlock_fps ? 1 : 0) != pace_unlock_prev)
            openpete_pace_window(OP_PACE_W_UNLOCK, unlock_fps ? 1 : 0);
        pace_unlock_prev = unlock_fps ? 1 : 0;
        openpete_unlock_fps = unlock_fps ? 1 : 0;
        g_skipSwapInterval = unlock_fps ? 1 : 0;
        audio_feed_sync(unlock_fps, opts->headless, g_paused);

        /* Paused without a step pending: sleep so we don't spin the CPU.
         * The condvar pacer below only waits when ticked_this_iter, and
         * the wall-clock pacer's render deadline does sleep but only at
         * its own cadence - explicit sleep here keeps the paused idle
         * cost near-zero regardless of render_fps. */
        if (g_paused && !ticked_this_iter && !unlock_fps) {
            SDL_Delay(16);
        }

        /* Unified pacer. Both branches step phase-locked integer-ns
         * deadlines and sleep via op_sleep_until. No condvar drain race, no
         * SDL_Delay integer-ms rounding.
         *
         * Unlock path (Tab / --headless / scheduled fast-forward):
         * re-anchor both deadlines to now so we don't stall on Tab
         * release. Mirrors the legacy `next_sim_ms = next_render_ms
         * = now` reset.
         *
         * Skip the sleep entirely once shutdown has been requested
         * (ESC / window close / SIGINT). The pacer's worst-case wait
         * is normally one period (~17 ms), but a Win32 waitable timer
         * that misbehaves could leave us stuck on a multi-second
         * WaitForSingleObject - the legacy condvar pacer always
         * checked g_shutdown_requested in its wait loop; mirror that
         * here so ESC closes the window promptly. */
        if (unlock_fps || g_shutdown_requested) {
            op_time_t anchor = op_now();
            op_deadline_anchor(&next_sim, anchor);
            op_deadline_anchor(&next_render, anchor);
            pace_regrid_pending = 1;
        } else if (s_render_fps == 0) {
            /* Legacy "match sim cadence" pacer. One present per VBL
             * cadence, advanced only when the game fiber actually
             * yielded with a frame bump (ticked_this_iter). dispatch.c
             * no-tick stub yields skip the pace step so a frame full
             * of stubs doesn't park us for 16.72 ms per stub. */
            if (ticked_this_iter) {
                op_deadline_advance(&next_sim, sim_period, 1);
                op_time_t now2 = op_now();
                if (now2 - next_sim.at > slip_threshold_ns) {
                    op_time_t behind = now2 - next_sim.at;
                    op_time_t skip = behind / sim_period_ns;
                    op_deadline_advance(&next_sim, sim_period, skip);
                    s_sim_slip_events++;
                    s_sim_slip_periods += (uint64_t)skip;
                    openpete_pace_slip(OP_PACE_SLIP_SIM, (int64_t)skip, (int64_t)behind);
                    pace_slip_pending = 1;
                    OPL_WARN_F("pacer",
                               "sim slip: host stalled %.1f ms (dropped %lld VBL period(s) "
                               "of wall time)", (double)behind / 1e6, (long long)skip);
                }
                pace_sleep_until(next_sim.at, next_sim.at);
            }
        } else {
            /* Wall-clock dual-pacer. sim deadline advances on sim-due
             * iters that actually ticked; render deadline advances every
             * outer iter; we sleep to the sooner of the two so an
             * under-paced render doesn't starve the sim or vice-versa.
             *
             * ticked_this_iter gates the sim step for the same reason the
             * legacy pacer above gates it: a dispatch.c no-tick stub yield
             * returns without bumping openpete_frame, and charging it a VBL
             * period of wall time slows the guest in proportion to how many
             * such yields a frame takes. A game with one per frame runs at
             * two thirds speed. */
            op_time_t now2 = op_now();
            if (sim_due && ticked_this_iter) {
                op_deadline_advance(&next_sim, sim_period, 1);
                if (now2 - next_sim.at > slip_threshold_ns) {
                    op_time_t behind = now2 - next_sim.at;
                    op_time_t skip = behind / sim_period_ns;
                    op_deadline_advance(&next_sim, sim_period, skip);
                    s_sim_slip_events++;
                    s_sim_slip_periods += (uint64_t)skip;
                    openpete_pace_slip(OP_PACE_SLIP_SIM, (int64_t)skip, (int64_t)behind);
                    pace_slip_pending = 1;
                    OPL_WARN_F("pacer",
                               "sim slip: host stalled %.1f ms (dropped %lld VBL period(s) "
                               "of wall time)", (double)behind / 1e6, (long long)skip);
                }
            }
            if (s_eager.active) {
                /* Under eager production the render grid is the present
                 * thread's business: this loop only borrows its PHASE when it
                 * plans a span. Place it on the grid's first instant after
                 * now (no slip accounting - nothing was missed) and sleep to
                 * the sim deadline alone, instead of waking once per slot to
                 * do nothing, or spinning flat out at an uncapped
                 * --render-fps. It never steps while already ahead of now, so
                 * back-to-back catch-up iterations cannot push it more than
                 * one period past now. */
                const op_time_t rp = op_period_ns(s_render_period);
                if (rp > 0 && now2 >= next_render.at)
                    op_deadline_advance(&next_render, s_render_period,
                                        (now2 - next_render.at) / rp + 1);
                pace_sleep_until(next_sim.at, next_sim.at);
            } else {
                op_deadline_advance(&next_render, s_render_period, 1);
            {
                const op_time_t rp = op_period_ns(s_render_period);
                if (rp > 0 && now2 - next_render.at > slip_threshold_ns) {
                    /* Captured before the advance: afterwards next_render.at
                     * is the NEW grid point and the lateness is gone. */
                    const op_time_t behind_r = now2 - next_render.at;
                    op_time_t skip = behind_r / rp;
                    op_deadline_advance(&next_render, s_render_period, skip);
                    s_render_slip_events++;
                    s_render_slip_periods += (uint64_t)skip;
                    openpete_pace_slip(OP_PACE_SLIP_RENDER, (int64_t)skip,
                                       (int64_t)behind_r);
                }
            }
            op_time_t next = (next_sim.at < next_render.at)
                             ? next_sim.at : next_render.at;
            pace_sleep_until(next, next_sim.at);
            }
        }

#if defined(__EMSCRIPTEN__)
        /* Guarantee one browser event-loop turn per host iteration. The
         * paced pacer branches already yielded inside op_sleep_until's
         * Asyncify arm, but the unlock / fast-forward branch (and
         * render-fps=0 no-tick iters) don't call it - without this the tab
         * would freeze. emscripten_sleep(0) is a single Asyncify
         * unwind/rewind; harmless after an already-yielded paced iter. */
        emscripten_sleep(0);
#endif
    }

    /* The present thread is joined before any teardown: it presents through
     * the device the record flush below drains, writes the frame-time log
     * and the pace log, and waits on this thread's clock, all of which the
     * teardown closes. Stopping it hands the display back to this thread. */
    SDL_SetAtomicInt(&g_sim_teardown, 1);
    sim_teardown_progress();
    openpete_present_thread_stop();
    sim_teardown_progress();
    sim_teardown_injected_stall();
    openpete_settings_save_flush("shutdown");   /* changes still inside the debounce */
    openpete_rebind_log_summary();

    bk2_feed_report();
    if (bk2_active) {
        bk2_movie_free(&bk2);
        g_PsyX_Pad_InputOverride = 0xFFFF;
        g_PsyX_Pad_OverrideActive = 0;
        g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
        g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
    }
    if (bk2_recording) {
        bk2_record_finalize();
        sim_teardown_progress();
    }
    /* --record: drain the native async readback ring's queued tail frames to the
     * encoder (while the Vulkan device is still alive), THEN flush the encoder +
     * write the container trailer. Both safe unconditionally (no-op if recording
     * was never armed / native isn't the display). Reaching here on SIGTERM/Ctrl-C
     * still produces a playable file. */
    openpete_native_render_record_flush();
    sim_teardown_progress();
    openpete_record_shutdown();
    sim_teardown_progress();
    /* --screenshot-presents-start/count engagement, reported unconditionally
     * when the flag was set because that fact is invisible in the images. */
    openpete_capture_report_presents(opts);
    if (position_log_fp) {
        fclose(position_log_fp);
        position_log_fp = NULL;
    }
    if (gameplay_hash_log_fp) {
        fclose(gameplay_hash_log_fp);
        gameplay_hash_log_fp = NULL;
    }
    if (psxtrace) {
        OPL_INFO_F("psxtrace",
                   "summary: records=%u frames_compared=%llu "
                   "hash_mismatches=%llu first_div_frame=%u "
                   "first_non_neutral_div=%s frame=%u",
                   psxtrace->records_consumed,
                   (unsigned long long)psxtrace->frames_compared,
                   (unsigned long long)psxtrace->hashes_diverged,
                   psxtrace->diverged ? psxtrace->first_divergent_frame : 0u,
                   psxtrace->strict_diverged
                       ? psxtrace->region_names[psxtrace->first_strict_divergent_region]
                       : "none",
                   psxtrace->strict_diverged ? psxtrace->first_strict_divergent_frame : 0u);
        /* Per-region divergence frequency: how many of frames_compared
         * each region diverged on. Stable region == real persistent
         * state difference; flickering region == aliasing or genuinely
         * intermittent leak. Logged unconditionally so the gate is
         * actionable on first read. */
        if (psxtrace->frames_compared > 0 && psxtrace->hashes_diverged > 0) {
            for (uint32_t r = 0; r < psxtrace->region_count; r++) {
                uint64_t n = psxtrace->per_region_diverge_count[r];
                if (n > 0) {
                    OPL_INFO_F("psxtrace",
                               "  region[%u] %s: %llu/%llu frames diverged",
                               r, psxtrace->region_names[r],
                               (unsigned long long)n,
                               (unsigned long long)psxtrace->frames_compared);
                }
            }
        }
        /* Engagement (per region): a row whose oracle hash never changed
         * across the run was compared on a constant. */
        if (psxtrace->frames_compared > 1) {
            char just[1024], unjust[1024];
            size_t nj = 0, nu = 0;
            for (uint32_t r = 0; r < psxtrace->region_count; r++) {
                if (psxtrace->per_region_change_count[r] != 0) continue;
                const int ok = (r < (uint32_t)openpete_state_region_count)
                             ? openpete_state_regions[r].constant_ok : 0;
                if (ok) nj += (size_t)snprintf(just + nj, sizeof(just) - nj, " %s",
                                               psxtrace->region_names[r]);
                else    nu += (size_t)snprintf(unjust + nu, sizeof(unjust) - nu, " %s",
                                               psxtrace->region_names[r]);
            }
            OPL_INFO_F("psxtrace", "zero-change regions justified (constant_ok):%s",
                       nj ? just : " none");
            OPL_INFO_F("psxtrace", "zero-change regions unjustified:%s", nu ? unjust : " none");
        }
        if (psxtrace->has_vbl_stamps) {
            char ht[256], ho[256];
            size_t nt = 0, no = 0;
            for (int b = 0; b < 17; b++) {
                if (psxtrace->vbl_hist_trace[b])
                    nt += (size_t)snprintf(ht + nt, sizeof(ht) - nt, " %s%d:%llu",
                                           b == 16 ? ">=" : "", b,
                                           (unsigned long long)psxtrace->vbl_hist_trace[b]);
                if (psxtrace->vbl_hist_op[b])
                    no += (size_t)snprintf(ho + no, sizeof(ho) - no, " %s%d:%llu",
                                           b == 16 ? ">=" : "", b,
                                           (unsigned long long)psxtrace->vbl_hist_op[b]);
            }
            OPL_INFO_F("psxtrace",
                       "vbl deltas: policy=%s mismatches=%llu; reference%s; openpete%s",
                       psxtrace->vbl_policy_exact ? "exact" : "log",
                       (unsigned long long)psxtrace->vbl_delta_mismatches, ht, ho);
            if (psxtrace->vbl_delta_mismatches)
                OPL_INFO_F("psxtrace", "  first vbl-delta mismatch: record %u reference %d openpete %d",
                           psxtrace->first_vbl_mismatch_frame,
                           psxtrace->first_vbl_mismatch_trace,
                           psxtrace->first_vbl_mismatch_op);
            if (openpete_sched_active())
                OPL_INFO_F("psxtrace", "  schedule: %u long frames charged at the next wait, final deficit %lld; "
                           "%u records inside load windows not compared, %u reference records skipped at window ends",
                           psxtrace->vbl_long_frames, (long long)psxtrace->vbl_deficit,
                           psxtrace->records_uncompared, psxtrace->records_skipped);
        }
        /* Promote any EOF-pending CD-load transient to a hard failure and
         * log the PASS/FAIL verdict + absorbed-transient tally. */
        openpete_psxtrace_finalize(psxtrace);
        if (psxtrace->hard_failed && exit_code == OPENPETE_EXIT_OK) {
            exit_code = OPENPETE_EXIT_DIVERGENCE;
        }
        openpete_psxtrace_close(psxtrace);
        psxtrace = NULL;
        s_psxtrace_active = NULL;
        extern void (*openpete_psxtrace_dispatch_hook)(uint32_t);
        openpete_psxtrace_dispatch_hook = NULL;
    }

    Uint64 elapsed = SDL_GetTicks() - start;
    OPL_INFO_F("exit",
               "reason=%s frames=%u key=%u key_kind=%s elapsed_ms=%llu dispatch_ok=%d",
               exit_reason, openpete_frame, openpete_feed_key_now(), feed_key_kind_name(),
               (unsigned long long)elapsed, g_game.dispatch_ok);

    {
        /* Pacing census: printed on every run, no flag needed, the way the
         * determinism gates are ambient. A run that was never paced says so
         * instead of quoting a phase error against a grid it did not follow. */
        char pace_summary[768];
        openpete_pace_census_format(pace_summary, sizeof(pace_summary));
        OPL_INFO_F("pace", "%s", pace_summary);
    }
    /* The swapchain's side of the story, which the census cannot see. Printed
     * unconditionally: a run where the acquire budget expired is a run whose
     * display rate is not its present count, and that has to be legible
     * without asking for a log file. */
    if (openpete_native_render_enabled()) {
        const uint64_t acq_sheds = openpete_native_render_acquire_sheds();
        const uint64_t acq_to    = openpete_native_render_acquire_timeouts();
        OPL_INFO_F("pace",
                   "swapchain: %u image(s) granted; acquire refused %llu "
                   "present(s) (%llu budget expiry, the rest suppressed by "
                   "the back-off it opened)",
                   openpete_native_render_swapchain_images(),
                   (unsigned long long)acq_sheds,
                   (unsigned long long)acq_to);
    }
    openpete_pace_log_close();

    if (s_sim_slip_events || s_render_slip_events) {
        OPL_INFO_F("pacer",
                   "slip totals: sim %llu event(s) / %llu period(s) discarded; "
                   "render %llu event(s) / %llu period(s)",
                   (unsigned long long)s_sim_slip_events,
                   (unsigned long long)s_sim_slip_periods,
                   (unsigned long long)s_render_slip_events,
                   (unsigned long long)s_render_slip_periods);
    }

    /* The clock is shut down by the caller once every thread that reads it
     * has stopped (openpete_ticker_stop in run_recomp_fiber and
     * run_split_threaded): the split's pump keeps calling op_now until the
     * worker is joined. */
    openpete_frame_time_log_close(&frame_time_log);
    op_functrace_dump_aggregate();   /* emit per-function aggregate into the trace */
    if (openpete_interp_enabled()) {
        OPL_INFO_F("interp",
                   "captured=%llu, dedupe-skipped=%llu",
                   (unsigned long long)openpete_interp_total_captures(),
                   (unsigned long long)openpete_interp_total_dedupe_skips());
    }
    if (ra_pc_on()) {
        /* stderr, not the log: the roundtrip gate greps this summary.
         * Emitted whenever the check is armed, counters or not: a window
         * that retired no prediction then reads as checked=0 instead of
         * an absent line, which a harness cannot tell from a binary that
         * never armed the check. */
        fprintf(stderr, "[ra-predict] checked=%u mismatch=%u invalidated=%u\n",
                s_ra_pc_checked, s_ra_pc_mismatches, s_ra_pc_invalidated);
    }
    {
        /* Vertex-stream field census (stderr: harnesses grep it). A
         * non-zero `unresolved` in a run with no handle-writing mod means
         * a stock frame word lives below the handle bound. */
        openpete_vstream_census_t vc;
        openpete_vstream_census(&vc);
        if (vc.stock || vc.handle || vc.unresolved)
            fprintf(stderr, "[vstream] stock=%llu handle=%llu unresolved=%llu"
                    " first=%#x\n", (unsigned long long)vc.stock,
                    (unsigned long long)vc.handle,
                    (unsigned long long)vc.unresolved,
                    vc.first_unresolved);
    }
    {
        /* sfx pack census (stderr: harnesses grep it). */
        sfx_pack_census_t sc;
        sfx_pack_census(&sc);
        if (sc.keyed || sc.active)
            fprintf(stderr, "[sfxpack] files=%d keyed=%llu resolved=%llu "
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
    openpete_interp_shutdown();

    /* Best-effort fiber teardown. Our game fiber is parked in its forever-
     * yield loop, so co_delete is safe. */
    if (openpete_game_fiber) {
        openpete_fiber_delete_guarded(openpete_game_fiber);
        openpete_game_fiber = NULL;
    }

    openpete_present_thread_report();
    /* The present thread was joined after the loop: it raises trace records
     * while it renders, and closing the stream under a live producer would
     * flush its chunk while it is still appending to it. */
    op_prof_close();
    openpete_native_present_census_report();
    eager_report();
    if (s_eager.produced_inter || s_inline_rex)
        OPL_INFO_F("sub-tick", "%u re-extract(s) presented: %u eager, %u inline",
                   s_eager.produced_inter + s_inline_rex,
                   s_eager.produced_inter, s_inline_rex);
    openpete_capture_report_intermediate();

    disc_cache_exit_census();
    /* Report before PsyX_Shutdown: the instrument reads PsyCross pad state. */
    openpete_host_input_parity_report();

    /* Staged-vs-readback equality for slot 0. Zero checks means the override
     * channel was never stamped before a tick (no host-owned input at all),
     * which makes the mismatch count uninformative rather than clean. */
    OPL_INFO_F("input-log",
               "slot-0 staging: %llu check(s), %llu mismatch(es)",
               s_p1_stage_checks, s_p1_stage_mismatches);
    OPL_INFO_F("input-log",
               "pad slots present (VBLs): p1=%llu p2=%llu p3=%llu p4=%llu",
               s_slot_present_vbls[0], s_slot_present_vbls[1],
               s_slot_present_vbls[2], s_slot_present_vbls[3]);
    OPL_INFO_F("input-log",
               "pad slot independence (VBLs differing): "
               "p1/p2=%llu p1/p3=%llu p1/p4=%llu p2/p3=%llu p2/p4=%llu p3/p4=%llu",
               s_slot_differed[0][1], s_slot_differed[0][2], s_slot_differed[0][3],
               s_slot_differed[1][2], s_slot_differed[1][3], s_slot_differed[2][3]);
    if (synth_pads_enabled())
        OPL_INFO_F("input-log",
                   "synthetic slots 1-3: %llu stage check(s)/%llu mismatch(es), "
                   "%llu rewind check(s)/%llu mismatch(es)",
                   s_synth_stage_checks, s_synth_stage_mismatches,
                   s_synth_rewind_checks, s_synth_rewind_mismatches);
    if (s_p1_stage_mismatches)
        OPL_ERROR_F("input-log",
                    "slot-0 staging first mismatch at frame %u: "
                    "staged bits=0x%04X axes=%u,%u,%u,%u; "
                    "readback bits=0x%04X axes=%u,%u,%u,%u",
                    s_p1_stage_first_frame,
                    s_p1_stage_first_staged_bits,
                    s_p1_stage_first_staged_axes[0], s_p1_stage_first_staged_axes[1],
                    s_p1_stage_first_staged_axes[2], s_p1_stage_first_staged_axes[3],
                    s_p1_stage_first_read_bits,
                    s_p1_stage_first_read_axes[0], s_p1_stage_first_read_axes[1],
                    s_p1_stage_first_read_axes[2], s_p1_stage_first_read_axes[3]);

    rumble_census_report();
    openpete_rewind_report();
    OPL_INFO_F("savestate", "deferred to a final yield: %llu snapshot request(s), %llu load request(s); "
               "periodic state work sat out %llu intermediate yield(s); "
               "runahead: %llu snapshot(s) owed, %llu rollback(s) deferred, "
               "%llu adopted at another park site; "
               "left gameplay: %llu rolled back, %llu adopted; "
               "park site: loads=%llu (%llu final yield(s) waited) rewind=%llu stress=%llu "
               "runahead=%llu refused=%llu stress-abandoned=%llu",
               s_state_ops_deferred[0], s_state_ops_deferred[1], s_state_yields_skipped,
               s_ra_deferred[0], s_ra_deferred[1], s_ra_adopted,
               s_ra_left[0], s_ra_left[1],
               s_site_deferred[SITE_DEF_LOAD], s_site_wait_yields, s_site_deferred[SITE_DEF_REWIND],
               s_site_deferred[SITE_DEF_STRESS], s_site_deferred[SITE_DEF_RUNAHEAD],
               s_site_refused, s_site_abandoned);
    {
        /* The VBL delivery census (games/common/psyq/psyq_thunks.cpp). */
        extern void psyq_vbl_delivery_report(void) __attribute__((weak));
        if (psyq_vbl_delivery_report) psyq_vbl_delivery_report();
    }

    /* OSD engagement. A capture-based gate reading zero here never composited
     * a toast, whatever its pixels looked like - reported unconditionally so
     * that cannot pass unnoticed. */
    if (openpete_osd_enabled())
        OPL_INFO_F("osd", "census: %u toast(s) composited",
                   openpete_native_osd_composited());
    /* Version-line engagement, on the same footing: a capture that never drew
     * it proves nothing about the line, whatever its pixels looked like. */
    OPL_INFO_F("version", "census: %s (%s), %u line(s) drawn",
               openpete_build_version(), openpete_native_version_reason(),
               openpete_native_version_drawn());

    {
        /* Whether the guest's own execution ever reached the Vulkan device. A
         * present thread can only run concurrently with a tick if this is
         * zero; any name listed here is a call that would have to be moved,
         * locked, or deferred first. */
        unsigned gpu_hits = op_gpu_audit_total();
        unsigned mw = op_gpu_audit_map_writes();
        /* Mapped-memory writes are reported even when the call count is zero:
         * they are host memcpys into device-visible buffers (the terrain
         * sector arenas, filled from hooks inside the guest's render walk), so
         * no Vulkan call names them, and a present thread reading those arenas
         * races them regardless of how clean the call audit looks. */
        OPL_INFO_F("gpu-audit",
                   "mapped-buffer writes during guest execution: %u (%llu bytes)",
                   mw, (unsigned long long)op_gpu_audit_map_bytes());
        if (gpu_hits == 0) {
            OPL_INFO_F("gpu-audit", "%s",
                       "no Vulkan calls during guest execution");
        } else {
            const char *names[32];
            int n = op_gpu_audit_names(names, 32);
            char buf[1024];
            int at = 0;
            for (int i = 0; i < n && at < (int)sizeof(buf) - 1; i++)
                at += snprintf(buf + at, sizeof(buf) - (size_t)at, "%s%s",
                               i ? " " : "", names[i]);
            if (!n) buf[0] = 0;
            OPL_WARN_F("gpu-audit",
                       "%u Vulkan calls during guest execution (%d distinct): %s",
                       gpu_hits, n, buf);
        }
    }

    /* Renderer + PsyCross teardown destroy the window and call SDL_Quit, which
     * belong to the thread that created them. Under the split that is main, so
     * the worker leaves them alone and main runs them after the join -
     * tearing the window down from here would hang the process on exit. */
    if (!split) {
        openpete_native_render_shutdown();
        PsyX_Shutdown();
    }

    fprintf(stdout,
            "openpete-exit: reason=%s frames=%u key=%u key_kind=%s ms=%llu code=%d\n",
            exit_reason, openpete_frame, openpete_feed_key_now(), feed_key_kind_name(),
            (unsigned long long)elapsed, exit_code);
    /* Host-input end state, printed when the lock or the injection script is
     * in play: what the lock discarded and what the host-side consumers a
     * stray key or pointer reaches ended up as. */
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
    }
    /* The player's end state, in --player-pose's own grammar, so the next
     * run starts where this one stopped: `--skip-to-level <level>
     * --player-pose "pos=(x, y, z) yaw=N"`. Printed only while the pose is
     * placeable (a title, demo, load or fly-in exit names the reason). */
    {
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
    }
    return exit_code;
}
#endif /* OPENPETE_HAS_PSYCROSS */

/** @brief Log the last openpete_frame value at process exit (atexit handler). */
static void log_frame_on_exit(void) {
    OPL_INFO_F("exit", "last openpete_frame=%u", openpete_frame);
}

/** @brief Run host init and the host loop on the calling thread.
 *
 * The path when the thread split does not apply, and the fallback when
 * run_split_threaded cannot create the worker thread.
 * @param opts The parsed options.
 * @return The init error code, or the host loop's exit code. */
static int run_recomp_fiber(const openpete_options_t* opts) {
    int rc = run_host_init(opts);
    if (rc != 0) return rc;
    rc = run_host_loop(opts);
    openpete_ticker_stop();   /* the loop's threads are joined; nothing reads the clock now */
    return rc;
}

/* Worker entry: the entire host loop and the game fiber. libco fibers are
 * thread-affine, so the fiber must be created and switched on this one thread
 * for its whole life - which is why the loop moves wholesale rather than
 * having main drive parts of it. */
static int  s_worker_rc;
static const openpete_options_t* s_worker_opts;

/* PsyCross's GL context and the hidden window it was created on. A GL
 * context is current on one thread at a time: bring-up creates it current on
 * main, the worker takes it for the life of the host loop (psycross mode
 * rasterizes the OT and reads the present FBO back there), and main takes it
 * back for PsyX_Shutdown. Both NULL under null-GPU vk, which has no context. */
static SDL_GLContext s_psyx_gl_ctx;
static SDL_Window*   s_psyx_gl_win;

/** @brief Body of the sim thread under the thread split: take the GL context and run the game loop until it exits. */
static int SDLCALL openpete_sim_thread(void* unused)
{
    (void)unused;
    openpete_crash_thread_attach();
    if (s_psyx_gl_ctx && !SDL_GL_MakeCurrent(s_psyx_gl_win, s_psyx_gl_ctx))
        OPL_ERROR_F("boot", "GL context handoff to the sim thread failed: %s",
                    SDL_GetError());
    s_worker_rc = run_host_loop(s_worker_opts);
    if (s_psyx_gl_ctx)
        SDL_GL_MakeCurrent(s_psyx_gl_win, NULL);
    SDL_SetAtomicInt(&g_worker_done, 1);
    return 0;
}

/* Pump-side sim liveness. A guest-side hang (a corrupted model walk, an OT
 * loop) leaves the worker spinning forever with the window still pumping, so
 * the process would outlive every quit request. Silence past
 * SIM_STALL_WARN_MS is logged with the last heartbeat. After a quit request
 * the process is ended from the pump when the worker makes no progress for
 * SIM_QUIT_GRACE_MS, counted from the later of the request and the last
 * heartbeat; once the worker reports teardown the bound between heartbeats
 * is SIM_TEARDOWN_GRACE_MS, since one recorder flush may take seconds. */
enum { SIM_STALL_WARN_MS = 5000, SIM_QUIT_GRACE_MS = 3000, SIM_TEARDOWN_GRACE_MS = 20000 };
typedef struct {
    int    hb_last;          /* heartbeat at the last observed change */
    Uint64 hb_last_ms;       /* when it changed */
    Uint64 quit_req_ms;      /* when the quit request was first seen; 0 = none */
    int    stall_logged;
} sim_liveness_t;

/** @brief Seed the pump's liveness state from the current heartbeat. */
static void sim_liveness_init(sim_liveness_t* L)
{
    L->hb_last = SDL_GetAtomicInt(&g_sim_heartbeat);
    L->hb_last_ms = SDL_GetTicks();
    L->quit_req_ms = 0;
    L->stall_logged = 0;
}

/** @brief One pump iteration of the sim liveness check: log a stall, and force exit on a quit the worker has stopped progressing on.
 *
 * @param L The pump's liveness state. */
static void sim_liveness_check(sim_liveness_t* L)
{
    const int    hb  = SDL_GetAtomicInt(&g_sim_heartbeat);
    const Uint64 now = SDL_GetTicks();
    if (hb != L->hb_last) {
        if (L->stall_logged)
            OPL_INFO_F("host", "sim thread resumed after %llu ms",
                       (unsigned long long)(now - L->hb_last_ms));
        L->hb_last = hb; L->hb_last_ms = now; L->stall_logged = 0;
    } else if (hb > 0 && !L->stall_logged && now - L->hb_last_ms >= SIM_STALL_WARN_MS) {
        L->stall_logged = 1;
        OPL_WARN_F("host", "sim thread has made no progress for %llu ms "
                   "(heartbeat %d): the game is hung; closing the "
                   "window exits", (unsigned long long)(now - L->hb_last_ms), hb);
    }
    if (!g_shutdown_requested) return;
    if (!L->quit_req_ms) L->quit_req_ms = now ? now : 1;
    const Uint64 since = L->hb_last_ms > L->quit_req_ms ? L->hb_last_ms : L->quit_req_ms;
    const int grace = SDL_GetAtomicInt(&g_sim_teardown) ? SIM_TEARDOWN_GRACE_MS : SIM_QUIT_GRACE_MS;
    if (now - since < (Uint64)grace) return;
    OPL_ERROR_F("host", "sim thread made no progress for %d ms after the quit request "
                "(heartbeat %d, request %llu ms ago%s): forcing exit", grace, hb,
                (unsigned long long)(now - L->quit_req_ms),
                SDL_GetAtomicInt(&g_sim_teardown) ? ", in teardown" : "");
    openpete_log_file_sync();
    _exit(OPENPETE_EXIT_PANIC);
}

/** @brief Run the host with main owning SDL video, the window and the event pump, and a worker thread running the host loop.
 *
 * Main runs run_host_init, seeds the input seam, releases PsyCross's GL
 * context, and starts the worker (openpete_sim_thread, 64 MiB stack). Main
 * then pumps events, publishes input, updates the cursor and mouse capture,
 * and performs fullscreen toggles the worker requests, until the worker
 * finishes. The sim liveness check (sim_liveness_check) logs a heartbeat
 * silent for 5 s and forces exit when the worker makes no progress for
 * 3 s after a quit request (20 s between teardown heartbeats). After the join it retakes the GL
 * context and shuts down the native renderer, the disc cache and PsyCross.
 * openpete_thread_split_enabled gives the reason for the split.
 * @param opts The parsed options.
 * @return The init error code, the worker's exit code, or
 *         run_recomp_fiber's when the thread cannot be created. */
static int run_split_threaded(const openpete_options_t* opts)
{
    /* Bring SDL video up here, on the thread that will pump; the
     * SDL_Init(SDL_INIT_VIDEO) inside PsyX_Initialise (run_host_init, also on
     * this thread) then only takes another reference. */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        OPL_ERROR_F("boot", "SDL_Init(video) failed: %s", SDL_GetError());
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    /* All host, PsyCross and SDL-subsystem init runs here, on main, before
     * the worker exists. The window must belong to the pumping thread (Win32
     * delivers a window's messages only to its creator), and SDL brings its
     * video, gamepad and haptic subsystems up on one thread and pumps them on
     * that thread. Initialising before main starts pumping serialises the
     * two without a barrier. */
    int init_rc = run_host_init(opts);
    if (init_rc != 0) return init_rc;

    /* Seed the input seam before the worker exists: its first latch must find
     * a valid snapshot, or pad injection falls back to reading SDL on the
     * worker. Also publish an initial SDL-state snapshot for the same reason
     * - bindings evaluated on the worker must never reach SDL themselves. */
    openpete_host_input_seed_neutral();
    openpete_host_input_publish_sdl();

    /* Release PsyCross's GL context from this thread so the worker can take
     * it; main makes no GL call between here and the join. */
    s_psyx_gl_ctx = SDL_GL_GetCurrentContext();
    s_psyx_gl_win = SDL_GL_GetCurrentWindow();
    if (s_psyx_gl_ctx)
        SDL_GL_MakeCurrent(s_psyx_gl_win, NULL);

    s_worker_opts = opts;
    SDL_SetAtomicInt(&g_worker_done, 0);
    /* Explicit, large stack. The host loop and everything it calls used to
     * run on the process's main stack, which the OS grows generously; a
     * default-sized thread stack is a fraction of that, and this call tree is
     * deep (recompiled dispatch, feeders, extraction, the RHI). */
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           (void*)openpete_sim_thread);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "openpete-sim");
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER,
                          64 * 1024 * 1024);
    SDL_Thread* th = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!th) {
        OPL_ERROR_F("boot", "SDL_CreateThread(openpete-sim) failed: %s - "
                            "running single-threaded", SDL_GetError());
        if (s_psyx_gl_ctx)
            SDL_GL_MakeCurrent(s_psyx_gl_win, s_psyx_gl_ctx);
        return run_recomp_fiber(opts);
    }
    OPL_INFO_F("boot", "%s", "thread split: main owns window+pump, worker owns the sim");

    /* Pump loop. Deliberately fast relative to the sim: the worker latches
     * whatever was last published, so this period bounds input staleness.
     * SDL_WaitEventTimeout parks the thread instead of spinning, and still
     * wakes immediately when an event arrives: a window or input message
     * wakes the platform wait, and while a gamepad is open SDL polls it
     * every 1 ms inside the wait and returns on the event a poll produces.
     *
     * The park is about 1 ms on every platform. SDL's Windows wait
     * (WIN_WaitEventTimeout) converts the time left of the request to whole
     * milliseconds by truncation, after the time already spent inside the
     * call is subtracted; a 1 ms request becomes a 0 ms
     * MsgWaitForMultipleObjects that returns at once, and the loop spins.
     * Asking Windows for 2 ms leaves a remainder that truncates to 1 ms. X11
     * and Wayland wait in nanoseconds and take 1 ms as asked. */
#ifdef _WIN32
    enum { PUMP_PARK_REQUEST_MS = 2 };
#else
    enum { PUMP_PARK_REQUEST_MS = 1 };
#endif
    sim_liveness_t live;
    sim_liveness_init(&live);
    while (!SDL_GetAtomicInt(&g_worker_done)) {
        sim_liveness_check(&live);
        /* Four instants per iteration, so a stall on this thread can be
         * attributed rather than merely noticed. A Win32 modal resize or move
         * runs the OS's own loop inside the event pump, which is where the
         * split's display defects have historically hidden, and it is
         * indistinguishable from a quiet loop unless the pump is timed apart
         * from the park that precedes it. */
        const int64_t hp_t0 = (int64_t)op_now();
        SDL_WaitEventTimeout(NULL, PUMP_PARK_REQUEST_MS);
        const int64_t hp_waited = (int64_t)op_now();
        openpete_host_mouse_pump_iteration();
        openpete_pump_events();
        const int64_t hp_pumped = (int64_t)op_now();
        openpete_host_input_publish_sdl();
        pad_restamp_if_due(1);
        openpete_host_input_sample(
            (openpete_host_input_modstate() & SDL_KMOD_ALT) != 0);
        const int64_t hp_input = (int64_t)op_now();

        /* Idle-hide of the OS cursor. Another window op that belongs to this
         * thread, and cheap: one atomic read plus three flag reads. */
        openpete_cursor_update(opts->headless);
        /* Relative mouse mode is a window op too: applied here from the
         * capture request bits and the overlay ownership state. */
        openpete_host_mouse_apply_capture();

        /* Window ops belong to this thread. The worker raises the flag (from
         * a binding or Alt+Enter) and main performs the SDL call. */
        int hp_window = 0;
        if (SDL_SetAtomicInt(&g_native_fs_toggle_request, 0)) {
            hp_window = 1;
            OPL_INFO_F("native", "%s", "fullscreen toggle: main performing");
            openpete_native_toggle_fullscreen();
            OPL_INFO_F("native", "%s", "fullscreen toggle: main done");
        }
        openpete_pace_host_pump(hp_t0, hp_waited, hp_pumped, hp_input,
                                (int64_t)op_now(), hp_window);
    }

    SDL_WaitThread(th, NULL);

    /* Worker is joined, so nothing else touches the device or the window.
     * These run here because they own main-thread resources (see the matching
     * comment in run_host_loop). PsyX_Shutdown destroys the GL context, which
     * must be current on the destroying thread. */
    if (s_psyx_gl_ctx)
        SDL_GL_MakeCurrent(s_psyx_gl_win, s_psyx_gl_ctx);
    openpete_native_render_shutdown();
    openpete_disc_cache_shutdown();
    PsyX_Shutdown();
    openpete_ticker_stop();   /* after the join: the pump read the clock until now */
    return s_worker_rc;
}

/** @brief The disc trace's vbl column: openpete_vbl.
 *
 * @return The current VBL count. */
static int disc_trace_vbl(void) { return SDL_GetAtomicInt(&openpete_vbl_device); }

/** @brief Keep a headless run's SDL init from autolaunching a D-Bus session bus.
 *
 * Every SDL_Init/SDL_InitSubSystem calls SDL_DBus_Init, which opens the
 * session bus through libdbus. With DBUS_SESSION_BUS_ADDRESS unset and no
 * socket at $XDG_RUNTIME_DIR/bus (ssh, cron, a WSL shell without a user
 * manager), libdbus autolaunches one through `dbus-launch --autolaunch`, and
 * that dbus-daemon outlives the process. A headless run uses no session-bus
 * service (screensaver inhibit, IME, portals), so it sets the address to
 * the unknown transport `disabled:`: the connect fails and SDL runs without
 * D-Bus. A set address and a windowed run are left unchanged.
 * @param headless The run is headless. */
static void headless_disable_session_bus_autolaunch(bool headless) {
    if (headless && !SDL_getenv_unsafe("DBUS_SESSION_BUS_ADDRESS"))
        SDL_setenv_unsafe("DBUS_SESSION_BUS_ADDRESS", "disabled:", 0);
}

int openpete_run(const openpete_options_t* opts) {
    openpete_log_init(opts->log_format, opts->log_level_spec);
    openpete_disc_trace_set_vbl_source(disc_trace_vbl);
    /* The stderr mirror is normally already up (main() opens it before
     * attaching the recomp module). Harmless no-op if so; this covers
     * embedders that call openpete_run() directly. Before
     * atexit(log_frame_on_exit) because atexit runs LIFO - registering the
     * sink first makes it close LAST, so the exit lines still land in it. */
    openpete_log_file_open(opts->log_file);
    atexit(log_frame_on_exit);
    /* The glitch-mark rings, allocated before any other thread exists so the
     * two note paths never have to test for initialization. */
    openpete_glitch_mark_init();
    OPL_INFO_F("boot", "starting %s (frame_limit=%d max_seconds=%d headless=%d)",
               opts->display_name, opts->frame_limit, opts->max_runtime_seconds,
               (int)opts->headless);
    headless_disable_session_bus_autolaunch(opts->headless);

    /* Extraction worker pool size (--extract-threads), before any consumer
     * can resolve it. Threads themselves spawn lazily on first parallel run. */
    openpete_workpool_configure(opts->extract_threads);

    /* libco: capture the host fiber. Skipped under the split, where main
     * never switches and the worker captures its own. libco is built with
     * LIBCO_MP so co_active_ is thread-local; without it main's conversion
     * would make the worker skip its own (see core/CMakeLists.txt). */
    if (!openpete_split_applies(opts)) {
        cothread_t host_fiber = co_active();
        OPL_INFO_F("boot", "libco host fiber=%p", host_fiber);
    }

    openpete_psx_install_crash_handler();

    /* Smoke test: dispatch one recompiled function and exit before the SDL
     * loop runs. */
    if (opts->test_recomp_call_addr != 0 || opts->test_recomp_segv) {
        return run_recomp_smoke_test(opts);
    }

#ifdef OPENPETE_HAS_PSYCROSS
    /* Dispatch the entry (the EXE header's, or OPENPETE_RUN_RECOMP's) on a fiber
     * and drive a frame loop with VSync yielding back here for present and
     * scene swap. */
    if (openpete_split_applies(opts))
        return run_split_threaded(opts);
    return run_recomp_fiber(opts);
#endif

    if (opts->headless) {
        SDL_setenv_unsafe("SDL_VIDEODRIVER", "offscreen", 0);
        OPL_INFO_F("boot", "headless mode requested (preferred driver: %s)",
                   SDL_getenv("SDL_VIDEODRIVER"));
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        if (opts->headless) {
            OPL_WARN_F("boot", "SDL_Init under '%s' failed (%s); retrying with dummy",
                       SDL_getenv("SDL_VIDEODRIVER"), SDL_GetError());
            SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
            if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
                OPL_ERROR_F("boot", "SDL_Init dummy fallback also failed: %s", SDL_GetError());
                return OPENPETE_EXIT_PANIC;
            }
        } else {
            OPL_ERROR_F("boot", "SDL_Init: %s", SDL_GetError());
            return OPENPETE_EXIT_PANIC;
        }
    }
    OPL_INFO_F("boot", "SDL video driver: %s", SDL_GetCurrentVideoDriver());

    /* SDL3: windows are shown by default; SDL_WINDOW_SHOWN is gone. */
    SDL_WindowFlags window_flags = opts->headless ? SDL_WINDOW_HIDDEN : 0;
    SDL_Window* window = SDL_CreateWindow(
        opts->display_name, game_default_window_w(), game_default_window_h(),
        window_flags);
    if (!window) {
        OPL_ERROR_F("boot", "SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return OPENPETE_EXIT_PANIC;
    }

    /* SDL3: SDL_CreateRenderer takes a driver name, not an index+flags.
     * NULL = let SDL pick (accelerated); SDL_SOFTWARE_RENDERER forces the
     * software rasteriser used for the headless screenshot path. */
    SDL_Renderer* renderer = opts->headless
        ? SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER)
        : SDL_CreateRenderer(window, NULL);
    if (!renderer && !opts->headless) {
        OPL_WARN_F("boot", "accelerated renderer unavailable, falling back: %s",
                   SDL_GetError());
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    }
    if (!renderer) {
        OPL_ERROR_F("boot", "SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return OPENPETE_EXIT_PANIC;
    }

    Uint64 start = SDL_GetTicks();  /* SDL3: SDL_GetTicks is 64-bit */
    int frame = 0;
    int exit_code = OPENPETE_EXIT_OK;
    const char* exit_reason = "window-closed";
    bool quit = false;
    int  screenshot_status = 0;

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                quit = true;
                exit_reason = "sdl-quit";
            }
        }

        SDL_SetRenderDrawColor(renderer, CLEAR_R, CLEAR_G, CLEAR_B, 0xFF);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);

        for (int si = 0; si < opts->screenshot_frame_count; ++si) {
            if (frame != opts->screenshot_frames[si]) continue;
            char path[1024];
            openpete_screenshot_format_path(opts->screenshot_output,
                                            opts->screenshot_frames[si],
                                            /*present=*/0,
                                            path, sizeof(path));
            int rc = openpete_screenshot_save_png(renderer, path);
            if (rc != 0) {
                screenshot_status = 1;
                exit_code = OPENPETE_EXIT_PANIC;
                exit_reason = "screenshot-failed";
                quit = true;
                break;
            }
        }

        frame++;

        if (opts->frame_limit > 0 && frame >= opts->frame_limit) {
            quit = true;
            exit_reason = "frame-limit";
        }
        if (opts->max_runtime_seconds > 0 &&
            (SDL_GetTicks() - start) > (Uint64)opts->max_runtime_seconds * 1000) {
            quit = true;
            exit_reason = "max-runtime-seconds";
        }
    }

    Uint64 elapsed_ms = SDL_GetTicks() - start;
    OPL_INFO_F("exit", "reason=%s frames=%d elapsed_ms=%llu screenshot_status=%d",
               exit_reason, frame, (unsigned long long)elapsed_ms,
               screenshot_status);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    fprintf(stdout, "openpete-exit: reason=%s frames=%d ms=%llu code=%d\n",
            exit_reason, frame, (unsigned long long)elapsed_ms, exit_code);
    return exit_code;
}
