// core/runtime/run.c — entry point: boot PSX EXE, optionally smoke-test a
// recompiled function, then drop into the SDL run loop.
//
// Phase 1b adds the boot path. The actual main loop is still a clear-color
// stub — Phase 1c will replace it with the recompiled `main` driving the
// gamestate dispatcher.

#include "openpete.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_host_input.h"
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
#include "rhi.h"
#include "../rhi/rhi_gpu_audit.h"
#include "openpete_profile.h"
#include "openpete_func_trace.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "screenshot.h"
#include "game_extras.h"
#include "bk2_movie.h"
#include "bk2_record.h"
#include "openpete_input_log.h"
#include "openpete_rewind.h"
#include "openpete_record.h"
#include "openpete_native_render.h"
#include "openpete_menu.h"
#include "openpete_devui.h"
#include "openpete_camlock.h"
#include "openpete_freecam.h"
#include "openpete_fillpick.h"
#include "openpete_keymap.h"  /* [keys] — every host hotkey below */
#include "openpete_osd.h"     /* toast the changes the player makes */
#include "feature_registry.h"
#include "openpete_mods.h"
#include "features/no_lod.h"
#include "features/widescreen.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* _exit */
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>  /* emscripten_sleep (Asyncify) */
#endif

#define WIN_W 640
/* Window default matches Spyro's visible 224-line viewport (DRAWENV.clip.h)
 * rather than the full 240-line DISPENV (which has 8px overscan bands top
 * and bottom that the game never draws into). PsyCross renders to an
 * oversized FBO and crops those bands at present time so the window
 * displays the visible region 1:1. */
#define WIN_H 448

#define CLEAR_R 0x10
#define CLEAR_G 0x14
#define CLEAR_B 0x20

/* 1 while the canonical present's lagged re-extract (lag_re) is running —
 * lets feeder-side diagnostics tag which present leg produced a sample. */
int g_openpete_canon_lag_leg = 0;

/* Virtual present clock (see openpete_interp.h): PLL over the present stream
 * so each present's CONTENT is timed for the evenly-spaced display slot FIFO
 * will scan it out at, not the uneven wall instant it was generated at (the
 * canonical present fires off the render pacer's grid — measured +8ms/+25ms
 * alternation per tick at --render-fps 60, i.e. 3:1 display judder if phase
 * comes from generation time). Steps at the EMA of the measured present
 * cadence (robust to any achieved rate vs the nominal --render-fps), with a
 * bounded 1/16 pull toward reality and a resync on stalls/jumps.
 * OPENPETE_PRESENT_TARGET_CLOCK=0 disables (phase falls back to op_now()). */
static void vp_set_present_target(void) {
    static int s_on = -1;
    if (s_on < 0) {
        /* OFF by default: the clock spaces animation time evenly per PRESENT,
         * and display time is not evenly spaced. The tick executes on this
         * thread, so one image is held for the length of it — and an evenly
         * spaced alpha gives that frame the same slice of motion as a frame
         * shown for one refresh. Measured at --render-fps 480, three runs
         * each: the frame held through the tick carried 0.083 of a tick of
         * motion for an 8.0 ms hold, an animation error of -5.24 / -5.10 /
         * -5.15 ms repeating 30 times a second, with a +0.3 ms mid-tick bias
         * as its mirror image. Letting the phase follow wall time instead put
         * the boundary error at +0.05 / +0.12 / +0.15 ms and the mid-tick
         * error at +0.01 ms.
         *
         * What the clock was for is real but smaller: it removes submit-time
         * jitter, and without it the error tail widens (p99 0.6-1.7 ms ->
         * ~2.2 ms, roughly one refresh at 480 Hz). A periodic deficit thirty
         * times a second is more visible than random error of that size on a
         * percent of frames, so the systematic term is the one to kill.
         *
         * The clamping that sank an earlier attempt at this is no longer the
         * problem it was — 0.0 % of presents in two of three runs — because
         * the consumers now allow slightly negative alpha rather than
         * snapping shown time forward at every interval roll. */
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
        /* HARD drift bound (±6 ms): the capture stamps live on their own
         * PLL-smoothed grid anchored to wall time; this clock smooths the
         * present cadence — two independent oscillators. When the present
         * cadence shifts under load (runahead's speculation inside tick
         * iterations was the trigger; measured drift to ~10 ms), the 1/16
         * pull is too weak and the standing offset shows up directly as an
         * interp-alpha bias: presents clamp at the interval's start and the
         * tail is never shown — motion arrives as 30 Hz roll-jumps ("whole
         * world jittery"). Smoothing within ±6 ms keeps the jitter
         * rejection; beyond that, reality wins. */
        const op_time_t cap = 6 * OP_NS_PER_MS;
        if (s_clk < now - cap) s_clk = now - cap;
        else if (s_clk > now + cap) s_clk = now + cap;
    }
    openpete_interp_set_present_target(s_clk);
}

/* PCSX-Redux trace state — file-static so the dispatch hook (which has
 * to be a plain function pointer with no closure) can find it. There's
 * only ever one open trace per run. */
static openpete_psxtrace_t* s_psxtrace_active = NULL;
/* Set during host init, consumed by the host loop — which run on different
 * threads under the split, so this cannot stay a local. Written once before
 * the worker exists and read-only thereafter. */
static openpete_psxtrace_t* psxtrace = NULL;

static void openpete_psxtrace_on_anchor(uint32_t addr) {
    (void)addr;
    if (!s_psxtrace_active) return;
    /* EOF (1) and error (3) are terminal — stop advancing. Divergence (2)
     * is sticky for the host loop's exit-decision but the verifier keeps
     * comparing every subsequent frame so --psxtrace-keep-going can
     * surface ALL leaks across the trace, not just the first. Only a HARD
     * failure (non-transient mismatch, or a CD-load transient that didn't
     * heal in the window) trips status 2 — a healing CD-load transient
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
 * does NOT walk/submit the OT — psyq_DrawOTag skips the GL rasterization (keeping
 * only the sim-tick timing pin) and PsyX_EndScene auto-no-ops via begin_scene_flag,
 * so there's no redundant GL draw and no second vsync swap on the hidden window.
 * It STAYS true (PsyCross renders) whenever the native renderer isn't the display
 * or its output is still needed: headless / psycross|ab|diff modes / native
 * bring-up failure / --record (taps the PsyCross present FBO) / --screenshot
 * (the -og + native -ab/-diff reference variants). Read by psyq_DrawOTag. */
int openpete_psycross_renders = 1;

/* Master interpolation/frame-gen toggle ('I' key). Gates the native
 * sub-tick intermediate presents; 1 = smooth (default), 0 = canonical
 * ~29.913 fps only. (Moved here from the deleted screen-space render
 * matcher, whose substitute path it also used to gate.) */
int g_openpete_interp_enabled = 1;

/* Cooperative shutdown flag. Set from SDL event filter (window close / Alt-F4),
 * an Escape keypress, or our SIGINT handler. The host-fiber loop checks this
 * once per iteration and drains cleanly, instead of PsyCross's PsyX_Exit()
 * calling exit(0) from inside PsyX_BeginScene (which can hang in WSL/GL with
 * the game fiber and vblank thread still mid-flight). */
static volatile sig_atomic_t g_shutdown_requested = 0;

/* Public request for a clean shutdown (the host-fiber loop polls
 * g_shutdown_requested and tears down through the normal path). Used by the RHI
 * on an unrecoverable VK_ERROR_DEVICE_LOST instead of spinning failed frames. */
void openpete_request_shutdown(void) { g_shutdown_requested = 1; }

/* Set by the SDL event filter on a window double-click; consumed once per host
 * loop iteration to toggle native-window fullscreen. Kept as a flag so the
 * heavy SDL_SetWindowFullscreen call runs in the loop, not the filter. */
static volatile sig_atomic_t g_native_fs_toggle_request = 0;

/* Ask for a windowed <-> fullscreen toggle from any thread.
 * SDL_SetWindowFullscreen belongs to the thread that owns the window (on
 * Win32 a cross-thread call SendMessages into a pump that may be busy, or
 * silently misdelivers the resize), so callers that are not that thread —
 * the settings menu on the sim worker, the dev overlay's knob row on the
 * presenting thread — raise this request and the host loop routes it: under
 * the thread split main performs the SDL call, unsplit the host loop does. */
void openpete_native_request_fullscreen_toggle(void)
{
    g_native_fs_toggle_request = 1;
}

/* The process's single event pump.
 *
 * openpete owns the SDL_PollEvent loop; every dequeued event is routed on to
 * PsyCross's handlers (controller hotplug, resize, its debug keys, text input,
 * Alt tracking) so those keep working without PsyCross draining the queue
 * itself. SDL_PollEvent drains one shared queue, so a second drain does not
 * split the work — it steals events from whichever loop polls second.
 *
 * Each event goes to openpete_handle_event first, which consumes the ones
 * openpete owns outright — quit, window close, Escape's overlay dismiss, the
 * quit binding — and forwards the rest. Consuming quit/close here is what
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

/* OPENPETE_GPU_AUDIT_SELFTEST=1 arms the guest flag around exactly one real
 * present. "Zero Vulkan calls during guest execution" is only worth reading
 * from a build that can be shown catching the calls it says never happen; a
 * mis-wired macro set reports the same zero as a genuinely clean tick. */
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

/* Host thread split (on by default; OPENPETE_THREAD_SPLIT=0 disables).
 *
 * Win32 runs a modal loop inside DefWindowProc for a title-bar drag or a
 * resize, which blocks whichever thread owns the window. Today that thread
 * also runs the game, so dragging the window freezes the sim. The fix is to
 * keep the window and the pump on main and move the host loop to a worker:
 * a drag then blocks main while the worker keeps ticking.
 *
 * The knob remains as the A/B for diagnosing anything the split is suspected
 * of causing: OPENPETE_THREAD_SPLIT=0 puts the loop back on main. */
static int openpete_thread_split_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_THREAD_SPLIT");
        v = (e && *e) ? (*e != '0') : 1;   /* default ON */
    }
    return v;
}

/* Only the default windowed pure-vk configuration splits. Everything else —
 * headless, --renderer psycross|ab|diff, screenshot capture — keeps PsyCross
 * rasterizing, which means a thread-affine GL context created on whichever
 * thread runs bring-up, and those are also the modes every gate runs in. They
 * stay single-threaded until this path has proven itself. */
static bool openpete_split_applies(const openpete_options_t* opts)
{
    return openpete_thread_split_enabled()
        && !opts->headless
        && opts->renderer == OPENPETE_RENDERER_VK
        && opts->screenshot_frame_count <= 0
        && opts->screenshot_presents_start < 0
        && !openpete_host_input_legacy();
}

/* Set once the worker has finished, so main can leave its pump loop. */
static SDL_AtomicInt g_worker_done;

static bool openpete_handle_event(SDL_Event* ev);

/* Idle-hide timer for the OS cursor: last mouse activity, in SDL ticks.
 * Written by the event pump (main thread) and read by the host loop (the
 * worker, once the thread split applies), hence the atomic. Unsigned 32-bit
 * differences stay correct across the ~49-day wrap. Seeded to 0 rather than
 * "now" would hide the cursor for the first two seconds of a session; the
 * host loop seeds it from the current tick on its first pass instead. */
static SDL_AtomicU32 g_mouse_last_activity_ms;

/* Cursor follows who owns the mouse. PsyCross used to hide it outright
 * whenever it owned a window, but it no longer creates one in the default
 * path, so the OS cursor sat over the game — including borderless fullscreen.
 * Hiding it unconditionally is wrong the other way: the dev overlay and the
 * settings menu are mouse-driven and need it back, and a player reaching for
 * the window controls needs to see where the pointer is. So over the game it
 * hides once the mouse has been still for CURSOR_IDLE_HIDE_MS and comes back
 * on the next mouse event.
 *
 * MUST run on the thread that owns the window. SDL's Win32 backend paints a
 * visibility change with SetCursor(), which only affects the calling thread's
 * windows; called from the sim worker it silently does nothing, and the
 * hidden state then waits for a WM_SETCURSOR — which only arrives on mouse
 * movement, the very thing that un-hides it again. Net effect off-thread: the
 * cursor never disappears. (The old unconditional hide survived that bug
 * because every later WM_SETCURSOR kept re-applying the same NULL cursor.)
 *
 * Edge-triggered, so this is one SDL call per transition rather than one per
 * iteration. */
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

static void openpete_pump_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (openpete_handle_event(&ev))
            PsyX_Sys_HandleEvent(&ev);
    }
    /* Actuators are driven from here because this thread owns the SDL gamepad
     * handles: PsyCross opens and closes them in the controller-added /
     * -removed handlers just above, so applying on any other thread would race
     * a close. It also puts the focus gate where focus is knowable. */
    openpete_rumble_apply();
}

/* Handle one dequeued event. Returns true to pass it on to PsyCross, false to
 * consume it here.
 *
 * This used to be an SDL event *filter*, which ran at push time and dropped
 * events before they were ever queued — necessary back when PsyCross owned the
 * pump and the only way to keep an event away from it was to stop it existing.
 * Now that openpete owns the only pump, consuming an event is just declining
 * to forward it, and everything runs in one place in a defined order. */
static bool openpete_handle_event(SDL_Event* ev) {
    /* Dev overlay sees every event (imgui io bookkeeping; frames only run
     * while it is open), including ones consumed below. */
    openpete_devui_process_event(ev);
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
    /* Double-click toggles native-window fullscreen (see the host loop) —
     * unless the click landed on the dev overlay. */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT && ev->button.clicks == 2 &&
        !openpete_devui_wants_mouse()) {
        g_native_fs_toggle_request = 1;
    }
    if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        g_shutdown_requested = 1;
        return false;
    }
    /* Quit-by-key, and Escape's overlay dismiss.
     *
     * Handled here rather than through the polled hotkey block because the
     * filter sees one key event at a time: polling would have to re-derive
     * "was this press already consumed by closing the menu?" from held
     * state, and get it wrong the first time a user presses Escape to
     * close the menu and quits the game instead.
     *
     * (We used to gate quitting on !SDL_HasGamepad() so a Steam-Input
     * phantom keystroke couldn't kill the game, but that also broke
     * Escape-to-quit for everyone playing on a normal controller — see
     * km_sdl_key_down in keymap_sdl.c for the trade-off we re-accepted.
     * `quit = "none"` is now the supported cure.) The window-close path
     * above quits regardless of any binding. */
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        const bool esc = (ev->key.scancode == SDL_SCANCODE_ESCAPE);

        /* Escape dismisses an open overlay whatever `quit` is bound to —
         * a UI convention rather than a binding, and it keeps unbinding
         * quit from stranding a panel open. (The filter runs during the
         * main-thread event pump, same thread as the host loop, so
         * reading the open flags here is race-free.)
         *
         * Consuming the key is what stops the dismiss fighting a polled
         * binding on the same key: with `menu` bound to Escape the poll
         * would otherwise still see it held and immediately reopen what
         * this just closed. Consumed until release, so pressing Escape
         * again reopens — the dismiss and the toggle compose. */
        if (esc && openpete_devui_is_open()) {
            openpete_devui_close();
            openpete_keymap_consume_key(ev->key.scancode);
            return false;
        }
        if (esc && openpete_menu_is_open()) {
            openpete_menu_toggle();
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

static void openpete_on_sigint(int sig) {
    (void)sig;
    if (g_shutdown_requested) {
        /* Second signal: something is hung in graceful shutdown — escape
         * hatch so the user isn't stuck. _exit is async-signal-safe. */
        _exit(130);  /* 128 + SIGINT */
    }
    g_shutdown_requested = 1;
}

static int unlock_frame_in_ranges(const openpete_options_t* opts, unsigned frame) {
    int f = (int)frame;
    for (int i = 0; i < opts->unlock_ranges_count; i++) {
        if (f >= opts->unlock_ranges[i].lo && f <= opts->unlock_ranges[i].hi) {
            return 1;
        }
    }
    return 0;
}

static void position_log_emit(FILE* fp, unsigned frame, int vbl) {
    int32_t x = 0, y = 0, z = 0;
    if (game_get_player_position(&x, &y, &z) != 0) return;
    fprintf(fp, "%u,%d,%d,%d,%d\n", frame, vbl, x, y, z);
}

/* Default-empty fallback for the gameplay-state regions table. Per-game
 * TUs call openpete_state_regions_register() from game main (explicit
 * registration, NOT weak-symbol override: MinGW's PE/COFF linker
 * mis-resolves weak overrides in this configuration — count resolved
 * strong while the pointer resolved weak-NULL, a NULL deref the moment
 * --psxtrace-verify computed the first hash on Windows). Without
 * registration the hash log is the FNV1a-64 offset basis on every
 * frame — useless but link-safe. */
const OpenpeteStateRegion* openpete_state_regions = NULL;
size_t openpete_state_region_count = 0;

void openpete_state_regions_register(const OpenpeteStateRegion* regions,
                                     size_t count) {
    openpete_state_regions = regions;
    openpete_state_region_count = count;
}

/* FNV1a-64 hash over every gameplay-readable region the per-game TU
 * registered. The harness consumes this log as the primary
 * determinism gate — anything that perturbs gameplay state shows up
 * as a hash divergence at the first frame the perturbation reaches
 * one of the registered regions. See
 * notes/plans/openpete-determinism-enforcement.md Phase 1. */
/* Compute the rolling total hash across all regions and, optionally,
 * a parallel per-region hash array. Per-region hashes let the harness
 * report which named region first diverged (Phase 5) without RAM
 * dumps. `out_per_region` (if non-NULL) must hold at least
 * `openpete_state_region_count` slots; sentinel 0 means the region
 * was skipped (NULL indirect pointer / out-of-RAM). */
uint64_t gameplay_hash_compute_ex(uint64_t* out_per_region);
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
                              || reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_COUNT);
        uint32_t base_vaddr = 0;
        /* Gameplay-state regions are canonical-2 MB by contract (regions
         * registered via psx_runtime_register_region are presentation-
         * only and explicitly excluded from the determinism gate).
         * psx_runtime_canonicalize is `addr & 0x1FFFFF` in release; in
         * debug it asserts the input doesn't fall in a registered
         * region's band — catching a future state-region manifest
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

/* --- OPENPETE_RA_PREDICT_CHECK: prediction-exactness oracle --------------
 * Runahead's correctness argument has two halves. The committed-timeline
 * half is gated (state-roundtrip determinism). The PREDICTION half — "with
 * unchanged input the led state IS the future committed state, bit for
 * bit" — was only an argument: a thunk reading host state that behaves
 * differently under speculation than in the real future (a stealth
 * rand()-alike — wall clock, an unsnapshotted host static) corrupts
 * predictions while leaving every determinism gate green, surfacing only
 * as unexplained presentation snaps. This check closes the gap: after each
 * speculation, hash the led guest RAM+scratch keyed by the led frame; when
 * the committed timeline reaches that frame the bytes must match.
 * Predictions crossing an input edge are invalidated (the branch
 * legitimately changed), as are all pending ones on a guest discontinuity
 * (rewind/state-load replays run under LIVE input, not the predicted
 * branch). Zero cost unless the env is set. */
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
static void ra_pc_invalidate_all(void) {
    for (int i = 0; i < RA_PC_PENDING; i++)
        if (s_ra_pc[i].valid) { s_ra_pc[i].valid = 0; s_ra_pc_invalidated++; }
}
static uint64_t ra_pc_hash(void) {
    uint64_t h = openpete_fnv1a64_init();
    h = openpete_fnv1a64_update(h, openpete_psx_get_ram(), OPENPETE_PSX_RAM_SIZE);
    return openpete_fnv1a64_update(h, openpete_psx_get_scratch(), OPENPETE_PSX_SCRATCH_SIZE);
}

static void gameplay_hash_log_emit_header(FILE* fp) {
    fputs("frame,vbl,fnv1a64", fp);
    /* One column per registered region, headed by its name. The
     * harness uses these to pinpoint which region first diverged.
     * Capped at OPENPETE_HASH_LOG_MAX_REGIONS to keep the per-frame
     * line bounded; over the cap we still emit the total hash and
     * skip extra columns (loud assert on first hit). */
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

/* Implemented in core/runtime/exe_loader.cpp. */
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
 * call (ResetGraph, SetGeomScreen, etc.) — those touch internal SDL/GL
 * state set up here. */
extern void PsyX_Initialise(char* windowName, int screenWidth,
                            int screenHeight, int fullscreen, int startHidden);
extern void PsyX_Shutdown(void);
#include "openpete_disc_cache.h"
extern void PsyX_CDFS_Init(const char* imageFileName, int track, int sectorSize);
extern char PsyX_BeginScene(void);
extern void PsyX_EndScene(void);
extern void PsyX_UpdateInput(void);
/* Declared in PsyCross's private src/audio/PsyX_SPUAL.h, which is not on
 * any public include path. Runahead mutes the OpenAL layer around a
 * speculative tick so the replayed audio is emitted once, not twice. */
extern int PsyX_SPUAL_SetMute(int on_off);

/* Phase 1e infra-triangle (core/runtime/debug_render.cpp). */
extern void openpete_debug_triangle_init(void);
extern void openpete_debug_triangle_emit(void);
extern void openpete_psyx_take_screenshot(const char* path);  /* BMP via SDL_SaveBMP */

/* Top-left FPS overlay (core/runtime/fps_overlay.cpp). */
extern void openpete_fps_overlay_emit(void);

/* PsyCross internal: set non-zero to bypass SDL_GL_SetSwapInterval at the
 * start of each scene, which uncaps the GL present and lets the host loop
 * actually run as fast as the pipeline allows. Without this Tab still
 * skips our wall-clock pacer, but GL vsync re-locks us to monitor refresh. */
extern int g_skipSwapInterval;

/* PsyCross post-render overlay hook (PsyX_public.h). PsyX_EndScene calls this
 * between GR_StoreFrameBuffer and GR_SwapWindow; we point it at the FPS
 * overlay so it stamps onto the presented frame without polluting the VRAM
 * mirror that the game reads back. */
typedef void (*PsyXPostRenderOverlayFunc)(void);
extern PsyXPostRenderOverlayFunc g_psyx_postRenderOverlay;

/* PsyCross's GL window — created by PsyX_Initialise. We need it so --headless
 * can hide the window post-init (we keep a real GL driver for the context;
 * SDL's offscreen driver gives back a context with NULL function pointers
 * and we segfault on the first gl* call from GR_InitialisePSX). */
extern SDL_Window* g_window;

/* Push the resolved [keys.pad] bindings into PsyCross's pad maps.
 *
 * PsyCross reads g_cfg_keyboardMapping / g_cfg_controllerMapping on every
 * internal pad update, but only ever fills them from its own hardcoded
 * table in PsyX_Sys_InitialiseInput. They are exported C globals, so
 * stamping them right after PsyX_Initialise makes the PSX pad
 * configurable without touching PsyCross at all.
 *
 * Structural limit: each field holds exactly ONE keyboard scancode or ONE
 * gamepad button/axis, so an action bound to several of either keeps the
 * first and warns about the rest. Lifting that means openpete synthesizing
 * the pad bits itself through the g_PsyX_Pad_InputOverride channel — a
 * change to the replay-critical path, deliberately not folded in here.
 *
 * `[keys.pad2]` is accepted by the config but inert: PsyCross uses the one
 * g_cfg_controllerMapping for both slots (see the "TODO: separate mapping
 * for Pad 2" in PsyX_Pad_UpdateGameControllerInput), so a P2 map has
 * nowhere to land. Say so rather than pretend it took. */
static void stamp_psx_pad_bindings(void) {
    /* Every field of both structs, paired with the action that feeds it.
     * A table rather than 20 assignments so a new pad action is one row. */
    static const struct {
        openpete_action_t action;
        size_t kbd_off;    /* offset into PsyXKeyboardMapping */
        size_t pad_off;    /* offset into PsyXControllerMapping */
        int    kbd_only;   /* pad_off is shared with another row: don't write */
        const char* name;
    } map[] = {
#define KM_PAD_FIELD(act, kfield, gfield, nm) \
        { act, offsetof(PsyXKeyboardMapping, kfield), \
               offsetof(PsyXControllerMapping, gfield), 0, nm }
#define KM_PAD_KEYONLY(act, kfield, gfield, nm) \
        { act, offsetof(PsyXKeyboardMapping, kfield), \
               offsetof(PsyXControllerMapping, gfield), 1, nm }
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
        /* The WASD-as-left-stick synthesis. Keyboard-only: all four share
         * the two gc_axis_left_* fields with each other and with the
         * left_x / left_y actions below, so a pad source here would fight
         * its opposite number. Bind the stick itself with pad.left_x. */
        KM_PAD_KEYONLY(OPK_PAD_STICK_UP,    kc_axis_left_up,    gc_axis_left_y, "stick_up"),
        KM_PAD_KEYONLY(OPK_PAD_STICK_DOWN,  kc_axis_left_down,  gc_axis_left_y, "stick_down"),
        KM_PAD_KEYONLY(OPK_PAD_STICK_LEFT,  kc_axis_left_left,  gc_axis_left_x, "stick_left"),
        KM_PAD_KEYONLY(OPK_PAD_STICK_RIGHT, kc_axis_left_right, gc_axis_left_x, "stick_right"),
#undef KM_PAD_KEYONLY
#undef KM_PAD_FIELD
    };

    unsigned char* kbd = (unsigned char*)&g_cfg_keyboardMapping;
    unsigned char* pad = (unsigned char*)&g_cfg_controllerMapping;

    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        km_source_t src[KM_MAX_SOURCES];
        int n = openpete_keymap_sources(map[i].action, src, KM_MAX_SOURCES);
        int have_kbd = 0, have_pad = 0;

        for (int s = 0; s < n; s++) {
            if (src[s].kind == KM_SRC_KEY) {
                if (have_kbd++) continue;
                *(int*)(kbd + map[i].kbd_off) = src[s].code;
            } else {
                if (map[i].kbd_only) {
                    OPL_WARN_F("keys", "pad.%s takes a keyboard key only "
                               "(bind the stick itself with pad.left_x / "
                               "pad.left_y)", map[i].name);
                    continue;
                }
                if (have_pad++) continue;
                int code = src[s].code;
                if (src[s].kind == KM_SRC_PADAXIS) {
                    code |= CONTROLLER_MAP_FLAG_AXIS;
                    if (src[s].invert < 0) code |= CONTROLLER_MAP_FLAG_INVERSE;
                }
                *(int*)(pad + map[i].pad_off) = code;
            }
        }
        /* An unbound half must be cleared, not left at PsyCross's default —
         * otherwise `cross = "pad:south"` would silently keep C bound too.
         * Both sentinels are PsyCross's own "NONE" values, the ones
         * PsyX_LookupKeyboardMapping / PsyX_LookupGameControllerMapping
         * hand back, so its readers already treat them as unbound: scancode
         * 0 indexes a key that is never down, and button -1 matches no
         * gamepad binding and reads 0. */
        if (!have_kbd) *(int*)(kbd + map[i].kbd_off) = SDL_SCANCODE_UNKNOWN;
        if (!have_pad && !map[i].kbd_only)
            *(int*)(pad + map[i].pad_off) = SDL_GAMEPAD_BUTTON_INVALID;

        if (have_kbd > 1 || have_pad > 1)
            OPL_WARN_F("keys", "pad.%s: PsyCross holds one keyboard and one "
                       "gamepad binding per button; the extra source%s "
                       "%s ignored", map[i].name,
                       (have_kbd + have_pad > 3) ? "s" : "",
                       (have_kbd + have_pad > 3) ? "are" : "is");
    }

    /* The four analog-stick axes are their own actions, and unlike the
     * buttons they only accept a gamepad axis. */
    static const struct { openpete_action_t action; size_t off; const char* name; }
    axes[] = {
        { OPK_PAD_LEFT_X,  offsetof(PsyXControllerMapping, gc_axis_left_x),  "left_x"  },
        { OPK_PAD_LEFT_Y,  offsetof(PsyXControllerMapping, gc_axis_left_y),  "left_y"  },
        { OPK_PAD_RIGHT_X, offsetof(PsyXControllerMapping, gc_axis_right_x), "right_x" },
        { OPK_PAD_RIGHT_Y, offsetof(PsyXControllerMapping, gc_axis_right_y), "right_y" },
    };
    for (size_t i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        km_source_t src[KM_MAX_SOURCES];
        int n = openpete_keymap_sources(axes[i].action, src, KM_MAX_SOURCES);
        int done = 0;
        for (int s = 0; s < n && !done; s++) {
            if (src[s].kind != KM_SRC_PADAXIS) {
                OPL_WARN_F("keys", "pad.%s needs a gamepad axis "
                           "(e.g. \"pad:leftx\"); ignoring", axes[i].name);
                continue;
            }
            int code = src[s].code | CONTROLLER_MAP_FLAG_AXIS;
            if (src[s].invert < 0) code |= CONTROLLER_MAP_FLAG_INVERSE;
            *(int*)(pad + axes[i].off) = code;
            done = 1;
        }
    }

    /* P2 is a valid namespace but has nowhere to land; only say so if the
     * user actually bound something there. */
    for (int a = OPK_PAD2_CROSS; a < OPK_ACTION_COUNT; a++) {
        km_source_t src[KM_MAX_SOURCES];
        if (openpete_keymap_sources((openpete_action_t)a, src, KM_MAX_SOURCES)) {
            OPL_WARN_F("keys", "[keys.pad2] is inert: PsyCross shares one "
                       "controller map across both pad slots");
            break;
        }
    }
}
#endif

/* Given a .cue path, build the sibling .bin path. Caller frees.
 * Returns NULL on alloc failure or unrecognised extension. */
static char* derive_bin_path(const char* cue_path) {
    if (!cue_path) return NULL;
    size_t n = strlen(cue_path);
    if (n < 4) return NULL;
    char* p = (char*)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, cue_path, n + 1);
    /* Replace ".cue"/".CUE" suffix with ".bin"/".BIN". */
    if (strcmp(p + n - 4, ".cue") == 0)      memcpy(p + n - 4, ".bin", 4);
    else if (strcmp(p + n - 4, ".CUE") == 0) memcpy(p + n - 4, ".BIN", 4);
    else { free(p); return NULL; }
    return p;
}

/* Phase 1b: try to load a PSX EXE; return 0 on success, non-zero on
 * failure (caller decides whether to continue without it). On success
 * sets *out_bytes (caller frees) and *out_size. */
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
        OPL_INFO_F("boot", "loading EXE from --disc: %s (%s)",
                   opts->disc_path, game_get_exe_filename());
        *out_bytes = openpete_psx_load_exe_from_disc(
            opts->disc_path, game_get_exe_filename(), out_size);
        return *out_bytes ? 0 : 1;
    }
    return 1;
}

/* Phase 1b smoke test: boot, dispatch one function, exit. */
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
    OPL_INFO_F("boot", "PsyX_Initialise: opening %dx%d window", WIN_W, WIN_H);
    PsyX_Initialise((char*)opts->display_name, WIN_W, WIN_H, /*fullscreen=*/0,
                    /*startHidden=*/opts->headless ? 1 : 0);
    /* PsyX_Sys_InitialiseInput just wrote its hardcoded pad maps; replace
     * them with the resolved [keys.pad] bindings. */
    stamp_psx_pad_bindings();

    /* Wire PsyCross's CDFS to the disc .bin so CdRead has real data.
     * --disc PATH.cue derives the sibling .bin; otherwise we skip and
     * CdRead returns failure (acceptable for the leaf smoke test). */
    if (opts->disc_path) {
        char* bin = derive_bin_path(opts->disc_path);
        if (bin) {
            if (!(opts->disc_cache && openpete_disc_cache_attach(bin))) {
                OPL_INFO_F("boot", "PsyX_CDFS_Init: %s", bin);
                PsyX_CDFS_Init(bin, /*track=*/0, /*sectorSize=*/0);
            }
            free(bin);
        } else {
            OPL_WARN_F("boot", "could not derive .bin path from --disc=%s",
                       opts->disc_path);
        }
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
/* Phase 1d main-loop driver: dispatch the recompiled function at
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
 * OPENPETE_EAGER=1 enables (off by default). OPENPETE_EAGER_POLICY=greedy
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
    op_time_t last_target;            /* the previous span's final display instant */
    int       world_live;             /* canonical presents are flowing (see the inline gate) */
    unsigned  spans, produced, skipped_late, cut, refused, level_up, level_down;
    int       span_cut;               /* this span ended with targets unproduced */
} s_eager = { .level = 1 };

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

/* Next lattice instant at or after `t`: v(n) = base + n * period. */
static op_time_t eager_lattice_ceil(op_time_t t, int64_t base, double period)
{
    const double n = ceil((double)(t - base) / period - 1e-6);
    return base + (op_time_t)(n * period + 0.5);
}

static op_time_t eager_tail_cost(void)
{
    return s_eager.cost_n >= EAGER_COST_WARMUP
        ? s_eager.cost_ema + 3 * s_eager.cost_dev : 0;
}

/* Plan the span that starts now: grid instants from `first` (the next slot
 * on the render grid) to `span_end` (the moment the next tick lands, where
 * alpha reaches 1). */
/* Room for the canonical scene between its plan and its vblank: production
 * (~0.25 ms), compose (0.3 ms on a light scene, 1.2 ms on a 48k-prim vista)
 * and the driver's own lead to the flip (a submit 0.33 ms before a vblank
 * still made it; one 0.13 ms before did not). */
#define EAGER_PUBLISH_LEAD_NS 2000000
static void eager_plan(op_time_t now, op_time_t first, op_time_t period, op_time_t span_end)
{
    /* Close the previous span's books first. */
    if (s_eager.n && s_eager.i < s_eager.n) { s_eager.cut++; s_eager.span_cut = 1; }
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
    int64_t lat_base = 0; double lat_period = 0.0;
    const int on_lattice = openpete_native_display_vsync() &&
                           openpete_present_thread_lattice(&lat_base, &lat_period);
    {
        const float hz = openpete_native_display_refresh_hz();
        if (on_lattice) {
            /* The present thread has fitted the display's vblank lattice
             * from observed flips: targets go ON it, a whole number of
             * refreshes apart, so each target is a vblank and the
             * animation instant (target - lag) is exact. The nominal mode
             * rate is not good enough for this -- it was 2e-5 off the
             * measured one, a full slot of phase every ~50 s. */
            const op_time_t lp = (op_time_t)(lat_period + 0.5);
            op_time_t k = (period + lp / 2) / lp;             /* refreshes per render slot */
            if (k < 1) k = 1;
            period = (op_time_t)(lat_period * (double)k + 0.5);
            /* The canonical scene is produced right after this plan and
             * shown at once; its vblank must leave room for that. */
            op_time_t f = now + EAGER_PUBLISH_LEAD_NS;
            if (f < first) f = first;
            first = eager_lattice_ceil(f, lat_base, lat_period);
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
        op_time_t floor_ns = eager_tail_cost() ? eager_tail_cost() : 250000;
        const op_time_t pc = openpete_present_thread_cost_ns();
        if (pc > floor_ns) floor_ns = pc;
        if (period < floor_ns) period = floor_ns;
    }
    /* Display instants whose ANIMATION instant (t - lag) is inside the span.
     * The previous span already covers the instants up to its last target
     * (that is what the lag is for: its late scenes show through this tick's
     * execution), so this span begins at the slot after it -- a scene for an
     * instant the old span covers would sit at NEGATIVE alpha of the new
     * bracket, a backward extrapolation the old bracket renders exactly. */
    span_end += s_eager.lag;
    if (s_eager.last_target && s_eager.last_target + period > first) {
        op_time_t f = s_eager.last_target + period;
        /* the old span's grid, unless it drifted a whole slot from ours */
        if (f - first > period) f = first + ((f - first) / period) * period;
        /* re-snapped: the lattice fit moves by microseconds between spans */
        if (on_lattice) f = eager_lattice_ceil(f - period / 2, lat_base, lat_period);
        first = f;
    }
    op_time_t slots[EAGER_MAX_TARGETS]; int ns = 0;
    for (op_time_t t = first; t <= span_end && ns < EAGER_MAX_TARGETS; t += period) slots[ns++] = t;
    if (ns == 0) { slots[ns++] = first; }

    /* Budget on the tail of the per-scene cost, against what this thread
     * has left of the span once the update VBL has run. */
    const op_time_t tail = eager_tail_cost();
    op_time_t budget = span_end - now - s_eager.vbl_ema - 1000000;   /* 1 ms safety */
    if (budget < 0) budget = 0;
    int afford = tail > 0 ? (int)(budget / tail) : ns;
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
    static int dbg = -1;
    if (dbg < 0) { const char *e = getenv("OPENPETE_EAGER_DBG"); dbg = (e && *e && *e != '0'); }
    if (dbg && (s_eager.span_cut || s_eager.level != s_eager.dbg_level)) {
        fprintf(stderr, "[eager f=%u] plan: slots=%d budget=%.2fms tail=%.3fms afford=%d level=%d cut=%d lag=%.2fms vbl=%.2fms\n",
                openpete_frame, ns, (double)budget / 1e6, (double)tail / 1e6, afford,
                s_eager.level, s_eager.span_cut, (double)s_eager.lag / 1e6, (double)s_eager.vbl_ema / 1e6);
        s_eager.dbg_level = s_eager.level;
    }
    s_eager.span_cut = 0;

    s_eager.n = 0; s_eager.i = 0;
    if (s_eager.policy == 1) {
        int k = afford < ns ? afford : ns;
        const op_time_t len = span_end - first;
        for (int q = 0; q < k; q++) s_eager.target[s_eager.n++] = first + (len * q) / k;
    } else {
        for (int q = 0; q < ns; q += s_eager.level) s_eager.target[s_eager.n++] = slots[q];
    }
    s_eager.last_target = s_eager.n ? s_eager.target[s_eager.n - 1] : 0;
    s_eager.spans++;
}

/* Produce one scene for instant `t`. The canonical scene (first of the
 * span) also carries the tick boundary for mods and the canon-lag leg. */
static void eager_produce_one(op_time_t t, int canonical)
{
    extern int g_openpete_canon_lag_leg;
    openpete_native_reextract_fn re = openpete_native_get_reextract();
    const op_time_t t0 = op_now();
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
    if (slot >= 0) { openpete_present_thread_kick(); s_eager.produced++; }
    else s_eager.refused++;
    if (pt0) openpete_phase_add(OPPT_REX_TOTAL, openpete_phase_now_us() - pt0);
    if (op_prof_on())
        op_prof_present((uint32_t)openpete_canon_tick,
                        canonical ? OP_PROF_CANONICAL : OP_PROF_INTERMEDIATE);
    const op_time_t span = op_now() - t0;
    openpete_pace_present_span((uint8_t)kind, t0, t0 + span, rex_ns);
    if (s_eager.cost_n) {
        const op_time_t d = span > s_eager.cost_ema ? span - s_eager.cost_ema : s_eager.cost_ema - span;
        s_eager.cost_dev = (s_eager.cost_dev * 7 + d) / 8;
    }
    s_eager.cost_ema = s_eager.cost_n ? (s_eager.cost_ema * 7 + span) / 8 : span;
    if (s_eager.cost_n < EAGER_COST_WARMUP) s_eager.cost_n++;
}

/* Produce planned scenes while they fit before `deadline`. Scene-atomic:
 * the check is between scenes. Returns the number produced. */
static int eager_produce(op_time_t deadline, op_time_t period)
{
    int made = 0;
    const op_time_t tail = eager_tail_cost();
    while (s_eager.i < s_eager.n) {
        const op_time_t now = op_now();
        const op_time_t t = s_eager.target[s_eager.i];
        const int canonical = s_eager.i == 0;
        if (!canonical && t < now - period / 2) {   /* its instant is gone */
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

static void eager_report(void)
{
    if (!s_eager.spans) return;
    /* Under the greedy policy the divisor is bookkeeping only (the plan
     * comes from `afford`), so it is reported as such. */
    OPL_INFO_F("eager", "%u span(s): %u scene(s) produced, %u skipped (instant passed), "
               "%u refused (ring full), %u cut short; level now %s%d (%u down, %u up); "
               "scene cost %.3f ms ema, %.3f ms dev; lag %.3f ms (tick %.3f ms ema, %.3f dev); policy %s",
               s_eager.spans, s_eager.produced, s_eager.skipped_late, s_eager.refused,
               s_eager.cut, s_eager.policy ? "n/a " : "", s_eager.level, s_eager.level_down, s_eager.level_up,
               (double)s_eager.cost_ema / 1e6, (double)s_eager.cost_dev / 1e6,
               (double)s_eager.lag / 1e6, (double)s_eager.tick_ema / 1e6, (double)s_eager.tick_dev / 1e6,
               s_eager.policy ? "greedy" : "divisor");
}

/* Drop the plan on a guest discontinuity (savestate / rewind / level load);
 * the ring flushes its own pending scenes. */
static void eager_on_discontinuity(void) { s_eager.n = s_eager.i = 0; s_eager.last_target = 0; s_eager.tick_peak = s_eager.tick_ema; }

/* Frame-time-log hook for presents made on the present thread. */
static struct { openpete_frame_time_log_t *log; int64_t *start_ns, *period_ns; } s_eager_ftl;
static void ftl_record_present(openpete_frame_time_log_t* log, int64_t start_ns,
                               int64_t period_ns, uint32_t frame);
static void eager_present_cb(void *ctx, int kind, double alpha, int paired)
{
    (void)ctx; (void)kind; (void)alpha; (void)paired;
    if (s_eager_ftl.log)
        ftl_record_present(s_eager_ftl.log, *s_eager_ftl.start_ns, *s_eager_ftl.period_ns,
                           openpete_frame);
}

static void game_fiber_entry(void) {
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
    /* Permanent yield — never resume. */
    while (1) co_switch((cothread_t)openpete_host_fiber);
}

/* Frame-time-log: write one 32-byte record per ACTUAL present — both the
 * canonical sim-tick present and each high-FPS sub-tick (intermediate) present.
 * The deadline advances by one target period per present (records_written), so
 * with --render-fps N the log captures the true ~N Hz present cadence; before,
 * only canonical presents were logged, so a 480 fps run read as ~30 Hz. The
 * `frame` field carries openpete_frame as cross-reference metadata (it repeats
 * across a tick's sub-ticks; measure-pacing.py keys off op_now_ns deltas). */
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

/* OPENPETE_FORCE_PACING: run the pacer even headless, which is otherwise
 * fast-forwarded. Resolved once, and readable before the loop starts —
 * the loop's unlock verdict has to exist before the FIRST iteration, or the
 * boot VBL is recorded as paced against a grid nothing slept to. */
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

/* Sleep to a deadline and record where the pacer aimed against where it
 * actually woke. Splitting a late tick into "the OS returned late" and
 * "everything after waking was slow" needs both instants; with only the
 * second, a residual fraction of late ticks has no attributable cause. */
/* Spend the idle wait on a display-timing measurement, when one is asked for.
 *
 * OPENPETE_PACE_FLIP_LOG=1 turns the sleep before the next deadline into a
 * bounded vkWaitForPresentKHR on the present just queued. The wait returns at
 * the FLIP, which is the one timestamp the engine cannot otherwise observe —
 * and it costs nothing the loop was not already going to spend, because the
 * timeout never exceeds the deadline we were sleeping to. The remainder of the
 * sleep still runs, so the pacing contract is unchanged.
 *
 * It must run on the presenting thread: the swapchain is externally
 * synchronized and a wait from anywhere else races the present. That is also
 * why this is measurement-only rather than always on — a driver whose wait
 * misbehaves would stall the sim clock, and the point of the instrument is to
 * measure the pacer, not to become part of it. */
static int pace_flip_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("OPENPETE_PACE_FLIP_LOG");
        on = (e && *e && *e != '0');
    }
    return on;
}

static void pace_sleep_until(op_time_t deadline_ns, op_time_t sim_deadline_ns)
{
    const op_time_t req = op_now();
    /* Under eager production the presenting thread is the present thread,
     * which does its own flip logging; a wait from here would race it. */
    if (pace_flip_log_on() && !s_eager.active) {
        /* Never wait into the pacer's slack ahead of a SIM deadline. The sleep
         * is not idle right up to it: op_sleep_until parks until the deadline
         * minus slack and the loop uses what remains to be ready on time.
         * Waiting into that window made the instrument the defect — every
         * flip-logged run measured 760-783 late ticks per 1000 against 12.5
         * for the same leg without it.
         *
         * Bounding by the RENDER target instead would disable the instrument
         * outright wherever the slack exceeds a present slot, which at
         * --render-fps 480 (2.08 ms slots, 2.5 ms slack) is always. Only the
         * tick's own margin is protected. */
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
 * cost, and an EMA alone is a centre estimate — on a right-skewed cost
 * distribution it is exceeded constantly. Measured over 49k recorded presents,
 * the shipped `ema + ema/4` bound was exceeded by **18-19 % of presents**, in
 * the attract demo and in a level alike. */
static op_time_t s_present_cost_dev;
static int       s_present_cost_n;
static int       s_present_shed_run;
/* Largest remaining-budget seen at an intermediate opportunity, decayed so it
 * tracks the machine instead of latching on one lucky moment. The probe below
 * needs "the moment in the cycle with the most budget"; expressed as a FIXED
 * fraction of a tick that moment does not exist on every machine — measured on
 * an Arc 140V laptop, the budget at 11 119 consecutive sheds never once
 * exceeded 9.6 ms against a 16.7 ms requirement, so the probe could not fire
 * and interpolation stayed off for the whole run. */
static op_time_t s_present_budget_peak;

/* Multiplier on the deviation. 0 selects the legacy centre-only bound, which
 * is how the two are A/B'd against each other on one machine. */
/* OPENPETE_PACE_LATE_BUDGET_US: microseconds of tick lateness an intermediate
 * present may spend to reach the display. 0 = the strict rule (never start a
 * present that could overrun the tick's start). Capped well under the slip
 * threshold: this may cost phase, never the grid. */
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

static int pace_shed_margin_k(void)
{
    static int k = -1;
    if (k < 0) {
        const char* e = getenv("OPENPETE_PACE_SHED_K");
        /* Default 0 = the shipped centre-only bound. k=3 is provably the
         * better PREDICTOR (miss rate 19 % -> 5 % over 49k recorded presents)
         * but no end-to-end difference is measurable on this hardware, and
         * k=4 collapses interpolation outright — 16 intermediate presents in a
         * run instead of ~16000, because a bound that wide almost never fits
         * the budget. A parameter with a cliff that near, tested on one fast
         * machine, does not become a default on a prediction-quality argument
         * alone. */
        k = (e && *e) ? atoi(e) : 0;
        if (k < 0) k = 0;
    }
    return k;
}

static void pace_record_present(uint8_t kind)
{
    openpete_pace_record_present(kind, openpete_pace_last_alpha(),
                                 openpete_pace_last_alpha_paired());
}

/* Slip ledger for the pacer's deadline clamps below. A clamp fires when the
 * loop has fallen more than slip_threshold_ns behind and snaps the deadline
 * forward by whole periods instead of firing catch-up iterations back to
 * back. That is the correct behaviour — a stall must never be repaid as a
 * burst — but it DISCARDS real time, so it must never be silent.
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

void openpete_set_render_fps(int fps) {
    if (fps < 0) fps = 0;
    if (fps == s_render_fps) return;
    s_render_fps = fps;
    s_render_fps_dirty = 1;   /* the host loop recomputes the period + re-anchors */
}

/* Live tick-scale (dev overlay). Same dirty-flag shape as render-fps: the
 * host loop re-derives every scaled period (sim, render, tick, slip) at its
 * top-of-iteration quiescent point and re-anchors the deadlines. Pacing
 * only — the sim executes identical ticks, just on a stretched wall clock
 * (headless/unlock ignores the deadlines entirely). */
static double s_tick_scale       = 1.0;
static int    s_tick_scale_dirty = 0;

void openpete_set_tick_scale(double scale) {
    if (scale < 0.05) scale = 0.05;
    if (scale > 8.0)  scale = 8.0;
    if (scale == s_tick_scale) return;
    s_tick_scale = scale;
    s_tick_scale_dirty = 1;
}

double openpete_get_tick_scale(void) { return s_tick_scale; }

/* Live runahead depth (dev overlay). Same dirty-flag shape: the host loop
 * re-arms at its top-of-iteration quiescent point — speculation is fully
 * contained inside one ticked iteration (save → speculate → restore), so
 * there is never in-flight state to tear. s_runahead_live is the applied
 * depth after the mod-capability gate (may be forced 0). */
static int s_runahead_req   = 0;
static int s_runahead_dirty = 0;
static int s_runahead_live  = 0;

void openpete_runahead_set(int n) {
    if (n < 0) n = 0;
    if (n > 3) n = 3;
    if (n == s_runahead_req) return;
    s_runahead_req = n;
    s_runahead_dirty = 1;
}

int openpete_runahead_get(void) { return s_runahead_live; }

/* Live engine-level mute (dev overlay). Same three gain clamps the boot
 * apply uses; s_mute replaces opts->mute as the baseline the F7 pause
 * exit restores, so a live toggle survives a pause/resume cycle. Host
 * audio only — never gameplay state. (Toggling OFF while paused unmutes
 * the pause silence until resume; harmless dev-tool edge.) */
static int s_mute = 0;

void openpete_set_mute(int on) {
    extern int  PsyX_SPUAL_SetMute(int on_off);
    extern void PsyX_XA_ForceGain(float linear_gain);
    s_mute = on ? 1 : 0;
    PsyX_SPUAL_SetMute(s_mute);
    PsyX_XA_ForceGain(s_mute ? 0.0f : -1.0f);
    music_override_set_forced_gain(s_mute ? 0.0f : -1.0f);
    /* Mod one-shots are AL sources outside the SPU mixer, so the SPU mute
     * flag above does not cover them. */
    openpete_mod_sound_set_mute(s_mute);
}

int openpete_get_mute(void) { return s_mute; }

/* --sfx / --music: the requested in-game slider levels, 0..10, or -1 for
 * "leave the game's own slider alone". These are a host-side REQUEST, not a
 * write: the per-game audio-volume feature reads them from its tick hook and
 * stamps the game's volume globals there. Keeping the guest write on the sim's
 * schedule is what lets the dev overlay edit them safely (north-star: nothing
 * outside the tick touches gameplay-readable RAM). */
static int s_sfx_volume   = -1;
static int s_music_volume = -1;

static int clamp_volume(int v) {
    if (v < 0)  return -1;
    if (v > 10) return 10;
    return v;
}

void openpete_set_sfx_volume(int v)   { s_sfx_volume   = clamp_volume(v); }
int  openpete_get_sfx_volume(void)    { return s_sfx_volume; }
void openpete_set_music_volume(int v) { s_music_volume = clamp_volume(v); }
int  openpete_get_music_volume(void)  { return s_music_volume; }

/* --vibration. Unlike the sliders this one has two halves: the guest enable
 * flag (stamped by the per-game feature on its tick hook) and the host motor
 * amplitude (applied on the pump thread). Both read the same request, so a
 * dev-overlay edit moves them together. */
void openpete_set_vibration(int v) { openpete_rumble_set_strength(clamp_volume(v)); }
int  openpete_get_vibration(void)  { return openpete_rumble_get_strength(); }

int openpete_get_render_fps(void) {
    return s_render_fps;
}

static int run_host_init(const openpete_options_t* opts) {
    /* True when main owns the window, the pump and the SDL-state publish, and
     * this function is running on the worker. Resolved once: it decides which
     * half of the input seam this thread is responsible for. */
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
     * context gates. Headless is a refusal rather than a default — a run with
     * no window has no player at the desk, and every gate battery, capture and
     * scripted launch is one.
     *
     * Installing the actuator-registration hook is what moves the per-vsync
     * half of libpad's PadSetAct contract onto the host; left uninstalled
     * (the OPENPETE_INPUT_LEGACY A/B) PsyCross keeps its historical one-shot
     * behaviour, which is the "before" side of the vibration A/B. */
    openpete_set_vibration(opts->vibration);
    openpete_rumble_set_headless(opts->headless ? 1 : 0);
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
                                 "(%s) — PadSetAct keeps its one-shot behaviour",
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
     * the --test-recomp-gp override below — if the user asked us to seed
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

    /* Feature modules apply their CLI-derived knobs. Phase 1 of
     * notes/plans/openpete-feature-modules.md: registry exists but
     * no features are migrated yet (openpete_feature_count == 0), so
     * this is a no-op. Phase 2 moves the widescreen --aspect block
     * below into core/features/widescreen.c where it gets called via
     * this single line. */
    feature_registry_apply_options(opts);

    /* Modding framework: discover/build/load mods (P1 vertical slice,
     * notes/plans/openpete-modding-framework.md). No mods directory ⇒
     * zero work and a byte-identical modless run (inertness gate); a
     * broken mod is disabled with a log, never a failed launch. */
    openpete_mods_init(opts);

    int win_w = (opts->window_w > 0) ? opts->window_w : WIN_W;
    int win_h = (opts->window_h > 0) ? opts->window_h : WIN_H;
    /* If --aspect is set but the user didn't pin --window, scale the
     * default window so the *cropped* (post-overscan) src exactly fills
     * it — otherwise the present blit letterboxes the un-rendered
     * disp/clip difference. Spyro's clip is 224 of 240 disp rows, so the
     * cropped aspect = requested_aspect × 240/224. User-provided --window
     * always wins. */
    if (opts->aspect_w_over_h > 0.0f &&
        opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        /* The PsyCross-mode Hor+ substitution stack was retired 2026-07-06
         * (Phase 6, openpete-native-renderer.md): the reference renderer
         * draws canonical 4:3 only, so a widened window would just
         * stretch it. Native modes keep --aspect (window-driven Hor+). */
        OPL_WARN_F("aspect", "--aspect is native-renderer-only (the PsyCross "
                   "Hor+ substitution stack was retired); the PsyCross "
                   "reference renders canonical 4:3 — flag ignored");
    } else if (opts->aspect_w_over_h > 0.0f &&
        opts->window_w <= 0 && opts->window_h <= 0) {
        const float crop_factor = 240.0f / 224.0f;
        win_w = (int)((float)WIN_H * opts->aspect_w_over_h * crop_factor + 0.5f);
        win_h = WIN_H;
        OPL_INFO_F("aspect", "auto window size %dx%d (--aspect=%.4f × crop %.4f)",
                   win_w, win_h, (double)opts->aspect_w_over_h, (double)crop_factor);
    }

    /* --fov derivation (GTE H override) lives in core/features/
     * widescreen.c — PsyCross reference mode only (the native renderer
     * projects canonically; its FOV policy is Phase-7 work).
     *
     * The PsyCross-mode --aspect substitution stack — projection widening
     * (GTE OFX + GL ortho), the cull-widening knobs, the render bracket,
     * env all-walk arm and cyclorama multi-pass — was RETIRED 2026-07-06
     * (Phase 6, openpete-native-renderer.md). Every renderer mode now runs
     * the game's verdict producers pure canonical 4:3 by construction;
     * widescreen is a native-renderer display policy driven by the window
     * aspect (--aspect only sizes the window above). */
    if (opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        widescreen_apply_fov(opts);
        if (opts->fullscreen)
            OPL_WARN_F("display", "%s", "--fullscreen is native-renderer-only "
                       "(the PsyCross GL window has no fullscreen path); ignored");
    } else {
        if (opts->fov_deg > 0.0f) {
            OPL_WARN_F("fov", "--fov is PsyCross-reference-mode only for now "
                       "(native FOV policy is Phase-7 work); ignored");
        }
        if (opts->aspect_w_over_h > 0.0f) {
            OPL_INFO_F("aspect", "--aspect %.4f sizes the window; the native "
                       "renderer widens Hor+ at the display layer from "
                       "canonical 4:3 producers",
                       (double)opts->aspect_w_over_h);
        }
    }

    /* --no-lod: native-renderer display policy (sets openpete_no_lod +
     * resolves the fog scale; warns + ignores in PsyCross reference
     * mode — the game-side producer stack was deleted 2026-07-06). See
     * notes/plans/openpete-disable-lods.md. */
    if (no_lod_apply_options(opts) != 0) {
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    /* Arm audio capture before any SPU init: when recording (and audio not
     * disabled), PsyX_SPUAL_InitSound opens an OpenAL loopback device so the
     * mixed PCM can be pulled into the recording. Set before PsyX_Initialise
     * so it's in place regardless of when SpuInit fires. Default 0 leaves
     * normal playback untouched. */
    extern int g_psyx_audioCaptureMode;
    g_psyx_audioCaptureMode =
        (opts->record_path && !opts->record_no_audio) ? 1 : 0;

    /* --psycross-native-raster: size the reference renderer's present FBO to
     * the console display grid instead of the window. Read per scene begin, so
     * setting it before init is enough. */
    extern int g_psyx_present_native_raster;
    g_psyx_present_native_raster = opts->psycross_native_raster ? 1 : 0;

    /* Phase 6 decouple: pure vk runs PsyCross as a LIBRARY only (null-GPU
     * mode — SDL + vblank/intr core + input + SPU + CDFS, but no GL window,
     * no GL context, no PSX GPU state; every GL-touching GR entry point
     * early-returns). Kills the hidden-window GL context AND the per-frame
     * GL tax the libgpu thunks were still paying on it (DrawSync/LoadImage2
     * texture uploads + framebuffer PBO readbacks). ab/diff/psycross modes
     * need PsyCross's real renderer as the reference and init as before.
     * The CPU-side vram array stays live in null-GPU mode, so StoreImage
     * readbacks the game does are byte-identical to the prior hidden-window
     * behaviour minus the (never-rendered) framebuffer PBO path. */
    /* Null-GPU applies exactly when PsyCross would never rasterize anything:
     * windowed pure-vk with no screenshot requests (the play path). Everything
     * that needs the PsyCross REFERENCE — ab/diff, the -og/-ab/-diff
     * screenshot variants, present-sequence capture — and headless (native
     * render disabled there; PsyCross still rasterizes for the oracle) keeps
     * the full GL init. Mirrors the openpete_psycross_renders computation
     * below: if that would be 1, PsyCross needs its GPU. */
    extern int g_psyx_null_gpu;
    g_psyx_null_gpu = (opts->renderer == OPENPETE_RENDERER_VK
                       && !opts->headless
                       && opts->screenshot_frame_count == 0
                       && opts->screenshot_presents_start < 0) ? 1 : 0;
    OPL_INFO_F("boot", "PsyX_Initialise: %s (headless=%d)",
               g_psyx_null_gpu ? "null-GPU (library-only: no window/GL)"
                               : "opening window",
               (int)opts->headless);
    /* Headless still uses the real GL driver — SDL's "offscreen" video driver
     * hands back a context with NULL gl* function pointers, so the first call
     * from GR_InitialisePSX segfaults. A GPU/display backend (X/Wayland/WSLg/
     * WGL) must therefore be present; pure-console CI needs Xvfb. The window
     * is created unmapped via startHidden so it never flashes or steals
     * focus — GL/FBO rendering works fine without it ever being shown.
     * (Both apply only when PsyCross's renderer actually initialises —
     * psycross/ab/diff; null-GPU vk creates no window or context at all.) */
    /* Hide PsyCross's GL window when a native renderer mode owns the display
     * (--renderer ab|diff): PsyCross renders to its FBO only and we read it
     * back into the Vulkan window, which becomes the single visible one.
     * Keyboard focus moves to the Vulkan window; gamepad input is
     * focus-independent. */
    bool native_renderer = (opts->renderer != OPENPETE_RENDERER_PSYCROSS);
    PsyX_Initialise((char*)opts->display_name, win_w, win_h, /*fullscreen=*/0,
                    /*startHidden=*/(opts->headless || native_renderer) ? 1 : 0);
    /* PsyX_Sys_InitialiseInput just wrote its hardcoded pad maps; replace
     * them with the resolved [keys.pad] bindings. */
    stamp_psx_pad_bindings();

    /* App icon decoded at build time from the PSX save-file header
     * (the red gem from the BIOS memory-card screen). The Win32 .rc
     * resource handles taskbar / .exe shortcut / alt-tab on Windows;
     * SDL_SetWindowIcon covers the live window on every platform.
     * Weakly referenced so a build that opts out of the icon
     * generator step still links. */
    if (g_window) {
        extern const uint8_t openpete_spyro1_icon_rgba[]
            __attribute__((weak));
        extern const int openpete_spyro1_icon_w __attribute__((weak));
        extern const int openpete_spyro1_icon_h __attribute__((weak));
        if (&openpete_spyro1_icon_w && openpete_spyro1_icon_w > 0) {
            SDL_Surface* icon = SDL_CreateSurfaceFrom(
                openpete_spyro1_icon_w, openpete_spyro1_icon_h,
                SDL_PIXELFORMAT_RGBA32,
                (void*)openpete_spyro1_icon_rgba,
                openpete_spyro1_icon_w * 4);
            if (icon) {
                SDL_SetWindowIcon(g_window, icon);
                SDL_DestroySurface(icon);
            }
        }
    }
    /* Same icon for the native Vulkan window, which is the visible one in the
     * default path (PsyCross runs null-GPU and creates no window at all).
     * Handed over rather than applied: that window does not exist yet. */
    {
        extern const uint8_t openpete_spyro1_icon_rgba[] __attribute__((weak));
        extern const int openpete_spyro1_icon_w __attribute__((weak));
        extern const int openpete_spyro1_icon_h __attribute__((weak));
        if (&openpete_spyro1_icon_w && openpete_spyro1_icon_w > 0)
            openpete_native_set_icon(openpete_spyro1_icon_rgba,
                                     openpete_spyro1_icon_w,
                                     openpete_spyro1_icon_h);
    }

    if (opts->headless && g_window) {
        /* Window was created unmapped (PsyX_Initialise startHidden=1) — no
         * hide needed here; just record the driver SDL picked. */
        OPL_INFO_F("boot", "headless: PsyCross window created hidden (driver=%s)",
                   SDL_GetCurrentVideoDriver());
    }

    /* Wire up the post-render overlay so PsyX_EndScene stamps an FPS counter
     * onto every presented frame. Has to be after PsyX_Initialise (the GL
     * context must exist before the overlay's first emit, but registration
     * itself doesn't touch GL — it just stores the callback). */
    g_psyx_postRenderOverlay = openpete_fps_overlay_emit;

    /* --record: arm gameplay video capture. The encoder is built lazily on
     * the first captured frame (once the present FBO's native dimensions are
     * known); this only stashes the parameters. The FPS overlay is excluded
     * from the recording — capture fires before PsyX_EndScene (which is where
     * g_psyx_postRenderOverlay stamps), reading the present FBO directly. */
    if (openpete_record_init(opts) != 0)
        return OPENPETE_EXIT_SETUP_ERROR;

    /* --renderer vk|ab|diff: bring up the native Vulkan renderer's window. SDL
     * is already initialized by PsyX_Initialise. Pass the same resolved window
     * size handed to PsyCross so the native window matches the game's. A
     * bring-up failure is non-fatal — we log and run PsyCross-only. The
     * native path is a pure display consumer; it never touches PSX RAM, so
     * the oracle gate / determinism are unaffected. */
    /* Create the window before bring-up so it belongs to this thread — the
     * one that pumps. Bring-up adopts it exactly as it adopts the splash's. */
    openpete_native_precreate_window(opts, win_w, win_h);
    (void)openpete_native_render_init(opts, win_w, win_h);
    openpete_native_set_perf_stats(opts->perf_stats);   /* stats overlay level (O / M menus move it live) */
    openpete_native_set_perf_anchor(opts->perf_position);  /* which corner it anchors to (--perf-position) */
    openpete_osd_set_enabled(opts->osd);
    openpete_osd_set_anchor(opts->osd_position);
    openpete_osd_set_default_timeout_ms(opts->osd_timeout_ms);
    /* OPENPETE_OSD_DEMO=N seeds N sample toasts at startup. The OSD is
     * otherwise driven entirely by live user action — a menu row, a hotkey, a
     * mod — which a headless or scripted capture cannot perform, so a gate
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
             * burst of 4 — an eight-toast gate that silently captured four. */
            char src[16];
            snprintf(src, sizeof src, "demo%d", i);
            openpete_osd_notify(src, NULL, OPENPETE_OSD_TIMEOUT_MAX_MS,
                                "%s", demo[i]);
        }
    }
    openpete_native_set_fps_in_record(opts->record_fps_overlay);  /* --record-fps-overlay */

    /* Decide whether PsyCross still rasterizes the OT (see the global's comment).
     * Only a pure, successfully-brought-up native vk display with no comparison/
     * capture consumer lets us skip the redundant GL submission. Everything else
     * (headless, ab/diff, native bring-up failure, --screenshot) keeps PsyCross
     * rendering. NOTE: --record does NOT force it on for the native display — the
     * native --record path reads the native offscreen directly (rhi_read_offscreen,
     * self-synchronizing), so forcing PsyCross to ALSO render every frame just
     * doubled the GPU work and throttled the capture to ~8 fps. (Headless --record
     * still uses PsyCross — native is disabled there, so the first term is true.) */
    openpete_psycross_renders =
        (!openpete_native_render_enabled()                 /* headless / psycross / bring-up failed */
         || opts->renderer != OPENPETE_RENDERER_VK         /* ab/diff need the PsyCross reference */
         /* --screenshot-native-only discards the PsyCross reference, so don't pay
          * the GL OT walk to build it — the native -vk dump reads gamestate. */
         || (opts->screenshot_frame_count > 0 && !opts->screenshot_native_only) /* -og + native -ab/-diff */
         || opts->screenshot_presents_start >= 0)          /* present-sequence capture (-og leg) */
        /* null-GPU hard override: no GL context exists, so PsyCross MUST NOT
         * rasterize regardless of the terms above (the reachable case is a
         * native bring-up failure in a windowed pure-vk run — display is lost
         * either way, but a black window beats a NULL-gl-pointer segfault). */
        && !g_psyx_null_gpu;
    if (g_psyx_null_gpu && !openpete_native_render_enabled())
        OPL_WARN_F("boot", "%s", "native renderer unavailable but PsyCross is "
                   "in null-GPU mode — no display path this run");
    OPL_INFO_F("boot", "PsyCross OT rasterization: %s (renderer=%d native=%d)",
               openpete_psycross_renders ? "ON" : "OFF (native vk owns display)",
               (int)opts->renderer, (int)openpete_native_render_enabled());

    signal(SIGINT,  openpete_on_sigint);
#ifdef SIGTERM
    signal(SIGTERM, openpete_on_sigint);
#endif

    if (opts->disc_path) {
        char* bin = derive_bin_path(opts->disc_path);
        if (bin) {
            if (!(opts->disc_cache && openpete_disc_cache_attach(bin))) {
                OPL_INFO_F("boot", "PsyX_CDFS_Init: %s", bin);
                PsyX_CDFS_Init(bin, /*track=*/0, /*sectorSize=*/0);
            }
            free(bin);
        } else {
            OPL_WARN_F("boot", "could not derive .bin path from --disc=%s",
                       opts->disc_path);
        }
    }

    g_game.entry = opts->run_recomp_addr;
    /* When --seed-cpu is supplied, suppress the --test-recomp-gp override
     * so it can't clobber the freshly-seeded gp inside game_fiber_entry. */
    g_game.gp    = opts->seed_cpu_path ? 0u : opts->test_recomp_gp;

    /* Pad-read probe: arm from the per-game address before the first
     * dispatch, so tick zero's read is seen like every other. */
    {
        extern uint32_t openpete_pad_read_pc;
        /* OPENPETE_PAD_READ_PC overrides the per-game address. It is how the
         * probe is falsified — pointed at an address the game never calls,
         * the census must report NEVER — and how a second game's read site is
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
    }

    /* Engine-level mute: clamp the SPU AL sources and the XA stream's AL
     * gain. Bypasses the game's volume sliders entirely. The music WAV
     * override mirrors the XA forced gain so --mute covers it too.
     * Routed through the live setter (dev overlay toggles it too). */
    music_override_init();  /* logs the assets/music/ search path */
    openpete_set_mute(opts->mute ? 1 : 0);

    /* Host-backed memory card: apply CLI/env config before the game's
     * MemCardInit thunk fires (during the game's Initialize()). */
    openpete_memcard_set_dir_override(opts->card_dir);
    openpete_memcard_set_paths(opts->card1_path, opts->card2_path);
    openpete_memcard_set_presence(0, opts->card_a_present);
    openpete_memcard_set_presence(1, opts->card_b_present);
    openpete_memcard_set_unformatted(opts->card_unformatted);
    openpete_memcard_set_adopt(opts->adopt_saves);
    openpete_memcard_startup_check();
    openpete_handoff_init(opts->resume_handoff, opts->handoff_argc,
                          opts->handoff_argv);

    /* Skip fast-forward is windowed-only — headless is already unlocked and
     * has nothing to show. Published before the setter below so the boot
     * CLI skip (and every later runtime warp) arms correctly. */
    openpete_skip_ffwd_windowed = opts->headless ? 0 : 1;

    /* Forward CLI overrides to the per-game impl. game_on_frame applies
     * them when the relevant in-game globals are ready. The setter itself
     * arms the present fast-forward (openpete_skip_present_until_level). */
    game_set_skip_to_level(opts->skip_to_level);

    /* Fast-boot gate is on by default (see fiber.c). psyq_DrawOTag clears it
     * the first time real draw commands hit, so host yields engage exactly
     * when the game starts rendering — no CLI knob needed. */
    OPL_INFO_F("boot", "fast-boot: skipping host yields until first DrawOTag");

    /* Start the free-running 60 Hz vblank ticker. Skipped under --headless
     * to keep that path deterministic — psyq_VSync advances `openpete_vbl`
     * synchronously when no ticker is running. See
     * plans/openpete-free-running-vblank-counter.md. */
    {
        /* Ticker is normally suppressed under --headless (vbl is then
         * advanced synchronously inside psyq_VSync — see
         * plans/openpete-free-running-vblank-counter.md). The
         * frame-pacing measurement harness needs the ticker active even
         * under --headless to capture a representative paced baseline;
         * OPENPETE_FORCE_PACING=1 opts in. */
        const char* fp = getenv("OPENPETE_FORCE_PACING");
        bool force_pacing = (fp && *fp && *fp != '0');
        if (!opts->headless || force_pacing) {
            openpete_ticker_start();
        }
    }

    return 0;
}

/* The worker half: the game fiber and the host loop.
 *
 * Split from run_host_init above because SDL brings its video, gamepad and
 * haptic subsystems up on one thread and pumps them on that same thread. With
 * init on the worker, main's pump raced it — a GAMEPAD_ADDED delivered on main
 * reaches PsyX_Pad_Event_ControllerAdded, which tears down and re-creates the
 * haptic subsystem while the worker is still inside PsyX_Pad_InitSystem. Init
 * on main serialises the two for free: main does not start pumping until init
 * has returned, so no barrier is needed (and a barrier could not work anyway —
 * the worker's SDL init needs main to be pumping, so main waiting on it
 * deadlocks).
 *
 * libco fibers are created and switched here, on the one thread that owns them
 * for their whole life. */
static int run_host_loop(const openpete_options_t* opts) {
    /* Which half of the input seam this thread owns (see run_host_init). */
    const bool split = openpete_split_applies(opts);

    /* The device was created during init — under the thread split that was
     * main — but inline presents come from THIS thread; the display's seam
     * fence needs the ownership stated, not inferred from who created what.
     * Eager's engage/disengage re-claims at every later handoff. */
    openpete_native_display_claim();

    openpete_host_fiber = co_active();
    /* 8 MB fiber stack — Spyro's deepest reasonable call chain is well under
     * 1 MB but PSYQ + recompiler shadow stacks add up; budget generously. */
    openpete_game_fiber = co_create(8u * 1024u * 1024u, game_fiber_entry);
    if (!openpete_game_fiber) {
        OPL_ERROR_F("boot", "co_create failed (out of memory?)");
        return OPENPETE_EXIT_PANIC;
    }
    OPL_INFO_F("boot", "fibers ready: host=%p game=%p",
               openpete_host_fiber, openpete_game_fiber);

    if (opts->debug_triangle) {
        openpete_debug_triangle_init();
        OPL_INFO_F("boot",
                   "debug-triangle enabled — emitting hardcoded POLY_F3 each frame");
    }

    /* Don't open a scene here — Spyro is a 30fps game (two PSX vblanks per
     * logical frame), so the host loop yields twice per DrawOTag. We let the
     * DrawOTag thunk start the scene on demand via its own PsyX_BeginScene()
     * call, and only EndScene + SwapWindow on host iterations where DrawOTag
     * actually fired. Empty iterations leave the front buffer untouched, so
     * the previous game frame stays visible until the next DrawOTag.
     *
     * Net effect: GL output is naturally 30fps and there's no yellow flash
     * between game frames. (Phase 1f rendering fix.) */

    Uint64 start = SDL_GetTicks();  /* SDL3: SDL_GetTicks is 64-bit */
    int    exit_code = OPENPETE_EXIT_OK;
    const char* exit_reason = "frame-limit";
    int    quit = 0;

    /* Monitor refresh rate (primary display — the window spawns there; 60
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

    /* Resolve the launch --render-fps target (Phase 2f default-on decision,
     * openpete-fps-unlock.md). -1 = unset: windowed runs default to the
     * display's refresh rate — the wall-clock dual-pacer + transform-interp
     * path — while headless keeps the legacy match-sim pacer (0): those runs
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

    /* Phase 2 pacer state — single time source (op_now / op_sleep_until)
     * for both the legacy --render-fps=0 path and the --render-fps>0
     * wall-clock pacer. Both branches step phase-locked deadlines in
     * integer ns and slip-clamp identically, eliminating the
     * condvar-drain race that left Phase 1a ~3 % rate-hot.
     *
     * Period choices:
     *   sim_period    = the PSX 240p VBL, exactly 37609/2250000 s
     *                   (59.8261054… Hz — see OP_VBL_* in openpete_time.h)
     *   render_period = 1/render_fps when --render-fps>0, else
     *                   sim_period (one present per sim tick).
     *
     * Both are op_period_t, so the fractional nanosecond each period carries
     * (a VBL is 16715111 + 1/9 ns) accrues on the deadline instead of being
     * truncated away every step. Deadlines land on floor(n * period) for all
     * n: under a nanosecond off the true grid, and never drifting further.
     *
     * Invariant (locked in by openpete-fps-unlock.md "vbl-counter
     * loops"): the inner sim block yields the game fiber at most once
     * per outer iteration. Multi-vbl catch-up would race-complete the
     * `pre - post < 2` GPU-pacing loops in draw.c and the 210-vbl Sony /
     * Universal logo waits in initialization.c. The slip-clamp resets
     * the deadline forward without firing extra yields. */
    /* --tick-scale F slows/speeds the WHOLE engine by dividing both the sim and
     * the render period by F: sim_rate = F*29.913 Hz, present_rate = F*render_fps,
     * so the count of sub-tick presents PER tick (present_rate/sim_rate) is
     * invariant — a slow-motion view of the SAME interpolation. Pacing only: the
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
     * is exact no matter how long a frame takes to render — a software
     * rasterizer and a fast GPU emit the identical frame sequence. That is what
     * makes two captures at different rates cover the same span of game time.
     *
     * The render period is derived from the tick period rather than from an
     * integer --render-fps: 60 Hz against a 59.826 Hz VBL is a 2.0058 ratio,
     * which hands some ticks two intermediates and others three. Dividing the
     * tick period gives exactly `capture_mult` evenly spaced presents per tick.
     * op_period_div keeps that division exact — a tick split three ways is
     * 11143407 + 11/27 ns, which no whole-ns period can express — so the
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

    /* Was a deadline in force for the iteration that just ended? The loop's
     * own unlock verdict is not written until the tail, so iteration 1 has
     * nothing to read — and an unlocked headless run would record its first
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
     * grid — the slip belongs to the tick that inherited it. */
    int           pace_slip_pending = 0;
    /* Set when the deadline chain is deliberately re-anchored (a live
     * --render-fps / --tick-scale change, an unlock exit). The next VBL's
     * sleep targeted the OLD grid, so subtracting the NEW anchor would
     * manufacture a near-zero phase error — flattering, and untrue. That VBL
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

    /* fps-unlock Phase 2 (scaffolding): bring up the interpolation
     * snapshot ring. Capture is enabled only when the render rate
     * exceeds sim rate — at parity there's nothing to interpolate
     * between, and the per-tick memcpy is pure overhead. The lerped
     * re-emit path is task #58; for now the ring captures + dedupes
     * and that's all. Safe to run unconditionally: capture is a
     * read-only PSX-RAM memcpy on the host fiber while the game
     * fiber is yielded inside VSync(0). */
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
         * here does NOT alter the canonical dump — it only feeds the explicit
         * intermediate capture. */
        if (opts->screenshot_intermediate_count > 0) {
            openpete_interp_set_enabled(1);
            openpete_interp_set_deterministic(1, g_openpete_tick_period_ns);
            OPL_INFO_F("interp",
                       "deterministic capture enabled (%d intermediate screenshot target(s), tick=%lld ns)",
                       opts->screenshot_intermediate_count, (long long)g_openpete_tick_period_ns);
        }
    }

    /* --rewind: allocate the keyframe ring (savestate P4). Driver lives
     * at the quiescent point below (Backspace / --rewind-hold). */
    openpete_rewind_configure(opts->rewind_window);

    /* --runahead: seeded into the live-depth request; the in-loop apply
     * block arms it on the first iteration — the identical path a live
     * dev-overlay change takes (capability gate + snapshot alloc there). */
    int runahead_n = 0;
    openpete_state_mem_t* ra_snap = NULL;
    s_runahead_req   = (opts->runahead > 0) ? opts->runahead : 0;
    s_runahead_live  = 0;
    s_runahead_dirty = (s_runahead_req > 0);

    /* Wall-clock pacing is now driven by openpete_ticker.h: each iteration
     * blocks on the ticker's condvar until the atomic vbl counter advances,
     * naturally pacing the host loop at ~60 Hz. The previous fractional
     * SDL_Delay accumulator is gone — pacing is implicit in "wait for
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

    /* Primary determinism gate. See gameplay_hash_log_emit + the
     * Phase 1 doc. */
    FILE* gameplay_hash_log_fp = NULL;
    if (opts->gameplay_hash_log_path && *opts->gameplay_hash_log_path) {
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

    /* Forward-declare the frame-time-log state — opened below after bk2
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

    /* --bk2: load BizHawk movie once, picked up per-frame in the loop. */
    bk2_movie_t bk2 = {0};
    int  bk2_offset = 0;
    bool bk2_active = (opts->bk2_path && *opts->bk2_path);
    if (bk2_active) {
        if (bk2_movie_load(opts->bk2_path, &bk2) != 0) {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        /* Default offset is 0: openpete_frame is anchored at the first
         * DrawOTag (post fast-boot) on both record and replay, so the
         * frame numbers line up 1:1 and replay sees the same game state
         * the player saw when they pressed the button. Auto-skipping to
         * first_input_frame would collapse the player's idle wait and
         * fire input before the game is ready. Use --bk2-frame-offset
         * to opt into a manual skip. */
        bk2_offset = opts->bk2_frame_offset_set ? opts->bk2_frame_offset : 0;
        OPL_INFO_F("bk2",
                   "loaded %d frames from %s, first input at bk2-frame=%d, "
                   "openpete_frame=%d → bk2-frame=openpete_frame-(%d)%s",
                   bk2.frame_count, opts->bk2_path, bk2.first_input_frame,
                   bk2_offset, bk2_offset,
                   opts->bk2_frame_offset_set ? " (explicit)" : "");
    }

    /* --frame-time-log: per-present binary recorder for the pacing
     * harness. The "deadline" we record against is a pure-ideal
     * cadence:
     *     deadline = start_op_ns + frame * target_period_ns
     * This is platform-agnostic and pacer-agnostic — it tells us how
     * the *actual* present time drifts from a perfectly-paced clock,
     * regardless of whether the legacy SDL_AddTimer pacer or a future
     * high-res pacer is driving the loop. The target period is the sim
     * cadence. See notes/plans/openpete-frame-pacing.md. */
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
     * above — every VBL as well as every present, on one timebase, so a late
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

    /* Input pump rate. We used to throttle to 120 Hz when Tab was held
     * to keep per-iter cost down at 1000+ Hz, but that broke BK2 replay
     * determinism: g_PsyX_Pad_InputOverride is set per host iter, and
     * PsyX_UpdateInput is what propagates it into the pad buffer the
     * game polls — skipping pumps means the game reads a stale override.
     * Pump every iter; the cost is tolerable. */

    /* Host hotkeys all come from the keymap (openpete_keymap.h), which
     * owns the bindings AND the press edges: openpete_keymap_poll() once
     * per host iteration advances one latch bit per action, so a held key
     * fires exactly once. That replaces the ~25 prev_*_key bools this
     * function used to carry, and it is what makes every hotkey below
     * rebindable from [keys.host] in openpete.toml.
     *
     * The savestate keys are number-row 1/2/3 by default rather than the
     * F2/F4 they started as, because F4 collides with PsyCross's own
     * debug binding — it cycles g_activeKeyboardControllers, which
     * silently kicks the keyboard off pad 1 and looks exactly like a
     * save-state regression. */
    unsigned active_slot = 0;

    /* Frame-step state. Pause toggles; frame-step advances exactly one
     * game tick (hold to repeat at host-loop rate). While paused the host
     * loop pumps SDL events, sleeps the deadline, and skips the co_switch
     * into the game fiber — so g_ram / CPUState stay frozen and PsyCross
     * keeps presenting the last completed frame. SPU is muted on enter to
     * avoid the held-voice drone (Cubeb runs on its own callback thread;
     * without new SPU register writes voices loop their current sample). */
    bool g_paused = false;
    bool g_step_one = false;
    bool prev_alt_enter = false;

    /* Menu / overlay latches that are NOT key edges: these track the
     * open/closed state itself so the pad override is cleared exactly
     * once on the closing transition. */
    bool prev_menu_close = false;   /* Start/Circle closes on RELEASE */
    bool prev_menu_open = false;
    bool prev_devui_kb = false;

    /* Seed the in-game settings menu from the launch flags, and build its FPS
     * ladder from the monitor refresh queried above (60 if unknown/headless).
     * Applies nothing — it only makes the menu reflect what the game launched
     * with (the FPS row reads the live pacer target, which already holds the
     * resolved render-fps default). */
    openpete_menu_init(opts, refresh_hz);

    /* Eager production (OPENPETE_EAGER=1): windowed native display only. */
    if (openpete_native_render_enabled() && !opts->headless && openpete_present_thread_start()) {
        eager_init();
        s_eager_ftl.log = &frame_time_log;
        s_eager_ftl.start_ns = &frame_time_log_start_ns;
        s_eager_ftl.period_ns = &frame_time_log_target_period_ns;
        openpete_present_thread_set_callback(eager_present_cb, NULL);
        openpete_on_guest_discontinuity(eager_on_discontinuity);
        openpete_on_guest_discontinuity(openpete_native_scene_ring_flush);
    }

    while (!quit) {
        /* Top-of-loop unlock peek used *only* to gate sim_due under
         * --render-fps>0. The authoritative `unlock_fps` write
         * (openpete_unlock_fps + g_skipSwapInterval) still happens at the
         * bottom of the loop, AFTER PsyX_UpdateInput has refreshed SDL's
         * keyboard cache and AFTER the present has consumed the previous
         * iter's swap-interval flag — preserving the original semantics
         * the GL present path was tested under. */
        /* See OPENPETE_FORCE_PACING note below. The bottom of the loop
         * caches it; mirror the same gate here so the sim-due predicate
         * follows the pacer when measurement mode is on. Re-read each
         * iter — cheap, and getenv on Linux is a libc env-pointer lookup. */
        const char* fp_env = getenv("OPENPETE_FORCE_PACING");
        bool force_pacing = (fp_env && *fp_env && *fp_env != '0');
        /* The turbo read here is the PREVIOUS iteration's keymap sample —
         * this peek runs before the poll below. That is what a peek is for;
         * the authoritative unlock write at the bottom of the loop re-reads
         * after the poll, so a press costs at most one iteration of lag. */
        bool unlock_now = (opts->headless && !force_pacing) ||
                          openpete_skip_present_until_level ||
                          openpete_key_down(OPK_TURBO) ||
                          (opts->unlock_ranges_count > 0 &&
                           unlock_frame_in_ranges(opts, openpete_frame));

        /* sim_due gates the once-per-yield block that does bk2 override
         * stamping, the co_switch into the game fiber, post-yield
         * accounting (position log, bk2 record, present, ram dump, exit
         * checks). With --render-fps=0 the gate is always true and the loop
         * reduces to the historical "one yield per outer iter" shape; the
         * condvar pacer below preserves byte-identical behavior. With
         * --render-fps>0 the wall-clock pacer drives the outer loop at
         * render cadence, and sim_due fires when the sim deadline has
         * elapsed — or unconditionally under unlock_now so fast-forward
         * actually fast-forwards. */
        /* Live render-fps change (openpete_set_render_fps, e.g. the in-game
         * menu): recompute the period with THIS loop's tick_scale and re-anchor
         * the deadlines to now so a lower→higher switch doesn't fire a burst of
         * catch-up presents. Also keep interp capture in sync (enabled only when
         * the render rate exceeds the sim rate), matching the startup gate. */
        /* Live tick-scale change: re-derive every scaled period, then fall
         * through the render-fps dirty path below for the render period +
         * deadline re-anchor + interp-gate re-sync. */
        if (s_tick_scale_dirty) {
            s_tick_scale_dirty = 0;
            tick_scale = s_tick_scale;
            sim_period    = op_scaled_vbl_period(tick_scale);
            sim_period_ns = op_period_ns(sim_period);
            tick_period   = op_period_mul(sim_period, OP_TICK_VBLS);
            g_openpete_tick_period_ns = op_period_ns(tick_period);
            slip_threshold_ns = 4 * sim_period_ns;
            s_render_fps_dirty = 1;
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
                    OPL_INFO_F("rewind", "modset changed — history cleared");
                }
                if (ra_snap) {
                    openpete_state_mem_free(ra_snap);
                    ra_snap = NULL;
                    OPL_INFO_F("runahead",
                               "modset changed — re-checking capability gate");
                }
                s_runahead_dirty = 1;
            }
        }
        /* Live runahead re-arm. Arm only when every enabled mod declares a
         * runahead posture in its manifest (runahead = pure | host-fx) —
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
            s_runahead_live = runahead_n;
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
         * fall back to 0xFFFF — i.e. no override — so live keyboard /
         * gamepad input takes over once the movie ends. */
        unsigned bk2_stamped_frame = openpete_frame;  /* see restamp below */
        if (sim_due && bk2_active) {
            int bk2_frame = (int)openpete_frame - bk2_offset;
            unsigned short bits = 0xFFFF;
            int ovr_active = 0;
            if (bk2_frame >= 0 && bk2_frame < bk2.frame_count) {
                const bk2_frame_t* fr = &bk2.frames[bk2_frame];
                bits = fr->buttons;
                /* bk2 axis order is LX,LY,RX,RY; PSX pad->analog is RX,RY,LX,LY. */
                g_PsyX_Pad_AnalogOverride[0] = fr->axes[2]; /* RStick X */
                g_PsyX_Pad_AnalogOverride[1] = fr->axes[3]; /* RStick Y */
                g_PsyX_Pad_AnalogOverride[2] = fr->axes[0]; /* LStick X */
                g_PsyX_Pad_AnalogOverride[3] = fr->axes[1]; /* LStick Y */
                ovr_active = 1;
            } else if (bk2_frame >= bk2.frame_count && opts->bk2_stop_on_end) {
                quit = 1;
                exit_reason = "bk2-end";
                break;
            }
            {
                /* Mods' read-only bk2 status (api->bk2_status) — also
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
            g_PsyX_Pad_InputOverride = bits;
            g_PsyX_Pad_OverrideActive = ovr_active;
            /* Pre-offset / past-end frames leave ovr_active 0 on purpose:
             * that is the documented hand-back to live input once a movie
             * ends, which now means the live-decode latch below. */
            if (ovr_active) ovr_claimed = true;
        }

        /* Settings menu open: neutralise the game pad so the arrow keys drive
         * the menu (not Spyro). Reuses the bk2 override channel (all buttons
         * released, sticks centred). On the frame the menu closes, clear it —
         * unless a bk2 movie is driving the override, in which case bk2 re-stamps
         * on the next sim-due iter. The sim keeps running (input just goes
         * nowhere), so aspect/ssaa/draw-distance changes preview live. */
        if (openpete_menu_is_open()) {
            ovr_claimed = true;
            g_PsyX_Pad_InputOverride  = 0xFFFF;
            g_PsyX_Pad_OverrideActive = 1;
            g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
            g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
        } else if (prev_menu_open && !bk2_active) {
            g_PsyX_Pad_InputOverride  = 0xFFFF;
            g_PsyX_Pad_OverrideActive = 0;
            g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
            g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
        }
        prev_menu_open = openpete_menu_is_open();

        /* Dev overlay typing: neutralise the pad only while imgui owns the
         * keyboard (a text field / nav focus), so typed characters can't reach
         * the PSX pad through PsyCross's keyboard->pad map. Mouse-only
         * interaction leaves the game fully playable behind the panel — this
         * is a dev tool; watching the live scene while flipping knobs is the
         * point. Same bk2 override channel + clear-once latch as the menu. */
        {
            bool devui_kb = openpete_devui_wants_keyboard() != 0;
            if (devui_kb && !openpete_menu_is_open()) {
                ovr_claimed = true;
                g_PsyX_Pad_InputOverride  = 0xFFFF;
                g_PsyX_Pad_OverrideActive = 1;
                g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
                g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
            } else if (prev_devui_kb && !devui_kb && !bk2_active &&
                       !openpete_menu_is_open()) {
                g_PsyX_Pad_InputOverride  = 0xFFFF;
                g_PsyX_Pad_OverrideActive = 0;
                g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
                g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
            }
            prev_devui_kb = devui_kb;
        }

        /* While imgui owns the keyboard, every KEYBOARD-sourced hotkey goes
         * inert in one stroke. Pad sources stay live on purpose: mouse-only
         * interaction with the dev panel is meant to leave the game
         * playable, and the same override block above only neutralises the
         * PSX pad while imgui actually wants typing. Esc-close and the
         * overlay toggle still work: Esc via the event filter, the toggle
         * once the text field loses focus (or via the panel's close
         * button). */
        openpete_keymap_suspend_keyboard(openpete_devui_wants_keyboard() ? 1 : 0);

        /* Under the thread split the cursor is main's business (see
         * openpete_cursor_update): SetCursor only reaches a window from the
         * thread that owns it. Unsplit, this loop IS that thread. */
        if (!openpete_split_applies(opts))
            openpete_cursor_update(opts->headless);

        /* Sample every binding once per iteration; openpete_key_pressed()
         * below reads the edge this computed. Reads SDL's keyboard/gamepad
         * state caches, which the previous iteration's pump refreshed — so
         * this must run before the first hotkey test, and the turbo peek at
         * the top of the loop deliberately sees the previous sample.
         * openpete_handle_event consumes QUIT/CLOSE/ESC without forwarding
         * them, so PsyCross's exit-on-quit path never fires. */
        openpete_keymap_poll();

        /* Dev config overlay. Mutually exclusive with the settings menu —
         * two input-owning overlays with different capture models is
         * complexity with no payoff. */
        if (openpete_key_pressed(OPK_DEVUI)) {
            if (openpete_devui_toggle() && openpete_menu_is_open())
                openpete_menu_toggle();   /* close the settings menu underneath */
        }

        /* The in-game settings menu. While open it captures the nav keys and
         * the game pad is neutralised above, so the other hotkeys and
         * gameplay input stay inert. Esc-closes is handled in the SDL event
         * filter. */
        {
            if (openpete_key_pressed(OPK_MENU)) {
                openpete_menu_toggle();
                if (openpete_menu_is_open()) openpete_devui_close();
            }

            if (openpete_menu_is_open()) {
                /* Keyboard and pad both drive nav through one binding per
                 * direction, so holding a direction — key, d-pad or stick —
                 * steps once until released, the same feel either way. The
                 * pad's own view of these buttons is held neutral while the
                 * menu is open, so this cannot double up with gameplay. */
                bool close_k = openpete_key_down(OPK_MENU_CLOSE);
                if (!close_k && prev_menu_close) {
                    /* Close on the RELEASE edge of Start/Circle. The game pad
                     * is held neutral only while the menu is open, so closing
                     * on the press would let the still-held button bleed into
                     * the game next frame (Start → the game's pause menu); by
                     * release it's already up. */
                    openpete_menu_toggle();   /* closes it */
                } else {
                    if (openpete_key_pressed(OPK_MENU_UP))    openpete_menu_nav(-1);
                    if (openpete_key_pressed(OPK_MENU_DOWN))  openpete_menu_nav(+1);
                    if (openpete_key_pressed(OPK_MENU_LEFT))  openpete_menu_adjust(-1);
                    if (openpete_key_pressed(OPK_MENU_RIGHT)) openpete_menu_adjust(+1);
                    if (openpete_key_pressed(OPK_MENU_SELECT)) openpete_menu_activate();
                }
                prev_menu_close = close_k;

                /* Freecam axes are level-held and consumed every game tick,
                 * but the latch below is skipped while the menu owns the
                 * keyboard — hold neutral here so a movement key held at
                 * menu-open doesn't keep flying the camera behind the panel. */
                if (openpete_freecam_active() || openpete_camlock_active()) {
                    openpete_freecam_axes ax = {0};
                    openpete_freecam_set_axes(&ax);
                }
            } else {
                prev_menu_close = false;
            }
        }

        bool rewound_this_iter = false;   /* rewind driver stepped: skip
                                           * this iteration's forward tick */
        /* Save-state hotkeys (default 1 quick-save, 2 quick-load, 3 cycle
         * slot). Edge-triggered, one action per press. The game fiber is
         * yielded right now, so g_game.cpu and g_ram are quiescent — we can
         * snapshot or restore without racing the recompiled code.
         *
         * Run BEFORE PsyX_UpdateInput so a load that overwrites the pad
         * buffer (which lives in restored g_ram) is immediately followed by
         * a fresh write of current input — otherwise the game fiber reads
         * stale buttons from the saved RAM and feels like input has stopped
         * registering until the next iter.
         *
         * Skipped while the settings menu is open — it owns input. The
         * keymap latch keeps advancing underneath, so a key held across the
         * close is already latched and cannot misfire on the first iteration
         * after it, and a press made while the menu was up is swallowed
         * rather than replayed into the game. */
        if (!openpete_menu_is_open()) {
            if (openpete_key_pressed(OPK_STATE_SAVE)) {
                (void)openpete_state_save(active_slot, &g_game.cpu,
                                          openpete_frame);
            }
            if (openpete_key_pressed(OPK_STATE_LOAD)) {
                unsigned loaded_frame = openpete_frame;
                if (openpete_state_load(active_slot, &g_game.cpu,
                                        &loaded_frame) == 0) {
                    /* Re-anchor the host frame counter so one-shot
                     * triggers (screenshot-on-frame, ram-dump-on-frame,
                     * frame-limit) reason about wall-time-from-load
                     * the same way they would on a fresh run. */
                    openpete_frame = loaded_frame;
                }
            }
            if (openpete_key_pressed(OPK_STATE_CYCLE)) {
                active_slot = (active_slot + 1u) % OPENPETE_STATE_NUM_SLOTS;
                OPL_INFO_F("savestate", "active slot -> %u%s",
                           active_slot,
                           openpete_state_slot_present(active_slot)
                               ? " (occupied)" : " (empty)");
            }

            /* Mod-requested savestate ops (api->savestate_request)
             * execute here — the same quiescent point as the hotkeys
             * above, with the same post-load frame re-anchor. Runs
             * headless too (mods can rewind in headless tests). */
            unsigned mod_slot = 0;
            int mod_op = openpete_mods_pending_state_op(&mod_slot);
            if (mod_op == 1) {
                (void)openpete_state_save(mod_slot, &g_game.cpu,
                                          openpete_frame);
            } else if (mod_op == 2) {
                unsigned loaded_frame = openpete_frame;
                if (openpete_state_load(mod_slot, &g_game.cpu,
                                        &loaded_frame) == 0) {
                    openpete_frame = loaded_frame;
                }
            }

            /* Savestate stress harnesses (P3 restore-at-yield gates).
             * --state-stress N: every N ticked iterations, in-memory save
             * + immediate quiet restore — a stressed run must be hash-
             * identical to a clean one. --state-stress-rewind K: save,
             * run K iterations, restore (the runahead rollback shape) —
             * the repeated window must self-match. */
            if (opts->state_stress > 0 || opts->state_stress_rewind > 0) {
                static openpete_state_mem_t* sm = NULL;
                static unsigned it = 0, ops = 0;
                static int armed = 0;   /* rewind mode: snapshot taken */
                if (!sm) sm = openpete_state_mem_new();
                it++;
                if (sm && opts->state_stress > 0 &&
                    it % (unsigned)opts->state_stress == 0) {
                    if (openpete_state_mem_save(sm, &g_game.cpu,
                                                openpete_frame) == 0 &&
                        openpete_state_mem_load(sm, &g_game.cpu,
                                                NULL, 1) == 0) {
                        ops++;
                        if ((ops & 0x3FFu) == 1u)
                            OPL_INFO_F("savestate",
                                       "stress: %u roundtrips (f=%u)",
                                       ops, openpete_frame);
                    }
                }
                if (sm && opts->state_stress_rewind > 0) {
                    /* 3-phase cycle so the timeline still advances:
                     * armed=0 ARM (save@F) -> armed=1 run K -> ROLLBACK
                     * to F -> armed=2 REPLAY K (must reproduce pass 1
                     * bit-for-bit; the hash-log self-match check) ->
                     * re-arm at F+K. Armed only in steady-state gameplay:
                     * the mem snapshot excludes boot/load-window sections
                     * (VRAM, SPU sample RAM) BY CONTRACT — rolling back
                     * across boot is undefined (measured: segfault). */
                    if (armed == 1 && !game_is_gameplay_live(&g_game.cpu)) {
                        /* The window crossed into a transition/load: the
                         * mem snapshot excludes the sections mutating
                         * there — ABANDON the pending rollback (exactly
                         * how runahead hard-gates OFF around loads). */
                        armed = 0;
                        it = 0;
                    }
                    if (armed == 0 && !game_is_gameplay_live(&g_game.cpu)) {
                        it = 0;
                    } else if (armed == 0) {
                        if (openpete_state_mem_save(sm, &g_game.cpu,
                                                    openpete_frame) == 0)
                            armed = 1;
                        it = 0;
                    } else if (armed == 1 &&
                               it >= (unsigned)opts->state_stress_rewind) {
                        unsigned lf = openpete_frame;
                        if (openpete_state_mem_load(sm, &g_game.cpu,
                                                    &lf, 1) == 0) {
                            openpete_frame = lf;
                            ops++;
                            if ((ops & 0xFFu) == 1u)
                                OPL_INFO_F("savestate",
                                           "stress-rewind: %u rollbacks "
                                           "(back to f=%u)", ops, lf);
                        }
                        armed = 2;
                        it = 0;
                    } else if (armed == 2 &&
                               it >= (unsigned)opts->state_stress_rewind) {
                        armed = 0;   /* replay done — re-arm here */
                    }
                }
            }

            /* Rewind (savestate P4): keyframe cadence + the Backspace
             * driver. Each held iteration steps the timeline back one
             * tick: quiet-load the covering keyframe, replay forward to
             * target with the recorded per-VBL inputs, truncate the
             * future (input log + newer keyframes). Interp stores are
             * reset manually (cheap memsets) so presents SNAP backward
             * instead of lerping — the full discontinuity broadcast
             * (texture-pack rescan = 1 MiB copy) is deliberately NOT
             * fired per held iteration. */
            if (!openpete_rewind_enabled()) {
                /* Backspace with no --rewind window: say so once instead
                 * of silently doing nothing. */
                static bool hinted = false;
                if (!hinted && openpete_key_down(OPK_REWIND)) {
                    hinted = true;
                    OPL_WARN_F("rewind", "the rewind key was pressed but "
                               "rewind is disabled — launch with --rewind 30 "
                               "to enable a 30 s window");
                }
            }
            if (openpete_rewind_enabled()) {
                int rw_live = game_is_gameplay_live(&g_game.cpu);
                openpete_rewind_on_iter(&g_game.cpu, openpete_frame, rw_live);
                bool rw_k = openpete_key_down(OPK_REWIND);
                /* Timeline jumps need every enabled code mod to declare
                 * its host-state posture (mod.toml state=) — an
                 * undeclared mod's host state desyncs silently when the
                 * world moves under it. */
                if (rw_k) {
                    const char* st_offender = NULL;
                    if (!openpete_mods_state_safe(&st_offender)) {
                        static bool st_hinted = false;
                        if (!st_hinted) {
                            st_hinted = true;
                            OPL_WARN_F("rewind", "refused: mod \"%s\" "
                                       "declares no state posture (add "
                                       "state = \"none\" or "
                                       "\"rebuildable\" to its mod.toml)",
                                       st_offender ? st_offender : "?");
                        }
                        rw_k = false;
                    }
                }
                /* --rewind-hold A:N — headless driver harness: simulate
                 * holding Backspace for N iterations once frame A is
                 * reached (the P4 determinism gate). */
                static int rw_held = 0, rw_armed_hold = 0;
                if (opts->rewind_hold_frame >= 0 && !rw_armed_hold &&
                    (int)openpete_frame >= opts->rewind_hold_frame)
                    rw_armed_hold = 1;   /* latch: a real held key doesn't
                                          * release when the frame drops
                                          * below the trigger */
                if (rw_armed_hold && rw_held < opts->rewind_hold_iters) {
                    rw_k = true;
                    rw_held++;
                    OPL_INFO_F("rewind", "hold %d/%d at f=%u (live=%d paused=%d)",
                               rw_held, opts->rewind_hold_iters,
                               openpete_frame, rw_live, (int)g_paused);
                }
                if (rw_k && rw_live && !g_paused) {
                    unsigned step = 2;   /* one tick back per held iter */
                    unsigned target = openpete_frame > step
                                    ? openpete_frame - step : 0;
                    const openpete_rewind_kf_t* kf =
                        openpete_rewind_pick(target);
                    if (!kf)
                        OPL_WARN_F("rewind", "no keyframe covers f=%u "
                                   "(window exhausted or none captured)",
                                   target);
                    unsigned lf = openpete_frame;
                    if (kf && openpete_state_mem_load(kf->mem, &g_game.cpu,
                                                      &lf, 1) == 0) {
                        openpete_frame = lf;
                        openpete_interp_reset();
                        openpete_interp_objs_reset();
                        /* Everything below re-executes ticks that already
                         * happened. Presentation-side announcements and
                         * per-tick animation phases must sit out the burst —
                         * a held Backspace replays up to a keyframe of ticks
                         * with no present between them. */
                        openpete_replaying = 1;
                        while (openpete_frame < target) {
                            int idx = kf->log_idx +
                                      (int)(openpete_frame - kf->frame);
                            const openpete_input_rec_t* r = input_log_get(idx);
                            if (r) {
                                g_PsyX_Pad_InputOverride = r->bits;
                                g_PsyX_Pad_AnalogOverride[0] = r->axes[2];
                                g_PsyX_Pad_AnalogOverride[1] = r->axes[3];
                                g_PsyX_Pad_AnalogOverride[2] = r->axes[0];
                                g_PsyX_Pad_AnalogOverride[3] = r->axes[1];
                                g_PsyX_Pad_OverrideActive = 1;
                            }
                            unsigned before = openpete_frame;
                            /* Same pre-yield hook as the main loop —
                             * per-frame guest mutations (demo-mode clamp,
                             * quirk pokes) are part of the timeline;
                             * skipping them made replay diverge
                             * (measured: psyq_rand_seed at the first
                             * replayed tick). */
                            game_on_frame_pre(&g_game.cpu, openpete_frame);
                            /* Replayed ticks are past VBLs re-executed to
                             * reach the target; their pad reads belong to
                             * those, not to whatever bracket is open. */
                            openpete_pace_vbl_suspend(1);
                            g_openpete_guest_running = 1;
                            co_switch((cothread_t)openpete_game_fiber);
                            g_openpete_guest_running = 0;
                            openpete_pace_vbl_suspend(0);
                            if (openpete_frame == before) break; /* safety */
                        }
                        openpete_replaying = 0;
                        input_log_truncate(kf->log_idx +
                                           (int)(target - kf->frame));
                        openpete_rewind_drop_after(openpete_frame);
                        if (!bk2_active) g_PsyX_Pad_OverrideActive = 0;
                        rewound_this_iter = true;
                    }
                }
            }

            /* Scheduled savestate ops (--state-save-on-frame /
             * --state-load-on-frame): the autonomous test vehicle for the
             * savestate plan's restore gates. One-shot each, same quiescent
             * point + post-load frame re-anchor as the hotkeys. Save is
             * checked BEFORE load so save@N + load@N in one run captures
             * then immediately restores (a same-frame roundtrip probe). */
            static bool sched_saved = false, sched_loaded = false;
            if (!sched_saved && opts->state_save_on_frame >= 0 &&
                (int)openpete_frame >= opts->state_save_on_frame) {
                sched_saved = true;
                (void)openpete_state_save_file(opts->state_save_output,
                                               &g_game.cpu, openpete_frame);
            }
            if (!sched_loaded && opts->state_load_on_frame >= 0 &&
                (int)openpete_frame >= opts->state_load_on_frame) {
                sched_loaded = true;
                if (!opts->state_load_input) {
                    OPL_ERROR_F("savestate",
                                "--state-load-on-frame armed but no --state-load-input");
                } else {
                    unsigned loaded_frame = openpete_frame;
                    if (openpete_state_load_file(opts->state_load_input,
                                                 &g_game.cpu,
                                                 &loaded_frame) == 0) {
                        openpete_frame = loaded_frame;
                    }
                }
            }

            /* Process handoff (modding P2): resume on the first pass of
             * a --resume-handoff process; checkpoint+exec when armed
             * (OPENPETE_HANDOFF_AT / openpete_handoff_request). Same
             * quiescence guarantees as the savestate ops above. */
            openpete_handoff_poll(&g_game.cpu);

            /* Any op above may have RE-ANCHORED openpete_frame (savestate
             * load, stress rollback, handoff resume). The bk2 override was
             * stamped for the PRE-jump frame — re-derive it for the new
             * one or the very next tick runs with the future frame's
             * input (measured: rollback+replay self-mismatch on movies
             * with an input edge inside the window). Silent twin of the
             * stamp block above; logging/status stay main-site-only. */
            if (bk2_active && openpete_frame != bk2_stamped_frame) {
                int bf = (int)openpete_frame - bk2_offset;
                unsigned short bits = 0xFFFF;
                int ovr = 0;
                if (bf >= 0 && bf < bk2.frame_count) {
                    const bk2_frame_t* fr = &bk2.frames[bf];
                    bits = fr->buttons;
                    g_PsyX_Pad_AnalogOverride[0] = fr->axes[2];
                    g_PsyX_Pad_AnalogOverride[1] = fr->axes[3];
                    g_PsyX_Pad_AnalogOverride[2] = fr->axes[0];
                    g_PsyX_Pad_AnalogOverride[3] = fr->axes[1];
                    ovr = 1;
                }
                g_PsyX_Pad_InputOverride = bits;
                g_PsyX_Pad_OverrideActive = ovr;
            }

            /* Frame-step hotkeys. Pause is an edge-triggered toggle; step
             * sets g_step_one whenever HELD while paused, so the natural
             * key-repeat behavior is "step every host iter while held"
             * without needing an OS-level repeat-rate dependency. */
            bool step_k = openpete_key_down(OPK_FRAME_STEP);
            if (openpete_key_pressed(OPK_PAUSE)) {
                g_paused = !g_paused;
                /* Enter: mute. Exit: restore the live mute baseline (boot
                 * --mute, possibly toggled since via the dev overlay). */
                {
                    extern int PsyX_SPUAL_SetMute(int on_off);
                    PsyX_SPUAL_SetMute(g_paused ? 1 : openpete_get_mute());
                }
                /* A paused game is not producing actuator bytes, but the last
                 * pair it published would keep being re-issued; silence the
                 * motors for the same reason the audio mutes. */
                openpete_rumble_set_paused(g_paused ? 1 : 0);
                OPL_INFO_F("frame-step", "%s at openpete_frame=%u",
                           g_paused ? "paused" : "resumed", openpete_frame);
            }
            if (g_paused && step_k) {
                g_step_one = true;
            }

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
             * so openpete_fog_scale follows the mode — a raw openpete_no_lod
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
             * candidates too — an under-fill site's covering face is
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

            /* Debug free camera. While engaged: the fly keys / left stick
             * move, the look keys / right stick aim, rise+descend raise and
             * lower, the speed keys / triggers step the speed, and teleport
             * drops Spyro here and hands the camera back. We own only the
             * toggle + input latch here; the camera write and the
             * CameraFollowSpyro / UpdateSpyro freeze are game-side
             * (games/spyro-1/features/freecam_native.c). Latched as level-held
             * axes so movement continues while held, consumed once per game
             * tick in game_on_frame_pre. Keyboard and pad sum (then clamp), so
             * either or both drive it. */
            if (openpete_key_pressed(OPK_FREECAM)) {
                openpete_freecam_toggle();
                openpete_osd_notify("host", "menu.freecam", 0, "FREECAM: %s",
                                    openpete_freecam_active() ? "ON" : "OFF");
            }

            /* Render camera lock (openpete_camlock.h) — holds/flies the
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

            /* Drop Spyro at the camera and hand the camera back to the game.
             * Latched as a one-shot request — the move needs the game fiber, so
             * freecam_native applies it (and disengages) on the next tick; the
             * request is dropped if the camera isn't engaged. Edge-tracked out
             * here rather than under the engaged branch so a teleport key
             * already held when the camera engages doesn't count as a press. */
            if (openpete_key_pressed(OPK_TELEPORT))
                openpete_freecam_request_teleport();

            if (openpete_freecam_active() || openpete_camlock_active()) {
                /* Digital fly/look keys and the analog stick axes are separate
                 * bindings that SUM (then clamp) — so a key and a stick both
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

                /* Speed step, edge-triggered. The shoulder buttons that used
                 * to be read separately are now just extra sources on the
                 * same two actions. */
                if (openpete_key_pressed(OPK_SPEED_DOWN))
                    openpete_freecam_adjust_speed(-1);
                if (openpete_key_pressed(OPK_SPEED_UP))
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
            bool alt_enter = openpete_keymap_scancode_down(SDL_SCANCODE_RETURN) &&
                             (openpete_host_input_modstate() & SDL_KMOD_ALT) != 0;
            /* OPENPETE_FS_TOGGLE_AT_VBL=n: toggle fullscreen once when the VBL
             * counter reaches n — the fullscreen transition, reproducible
             * from a harness. Diagnostic only. */
            {
                static long s_fs_at = -2;
                if (s_fs_at == -2) { const char *e = getenv("OPENPETE_FS_TOGGLE_AT_VBL"); s_fs_at = (e && *e) ? atol(e) : -1; }
                if (s_fs_at >= 0 && (long)openpete_frame >= s_fs_at) {
                    g_native_fs_toggle_request = 1; s_fs_at = -1;
                    OPL_INFO_F("native", "fullscreen toggle requested at VBL %u", openpete_frame);
                }
            }
            if ((alt_enter && !prev_alt_enter) ||
                openpete_key_pressed(OPK_FULLSCREEN) ||
                g_native_fs_toggle_request) {
                if (split) {
                    /* SDL_SetWindowFullscreen belongs to the thread that owns
                     * the window. Raise the request; main performs it. */
                    g_native_fs_toggle_request = 1;
                } else {
                    g_native_fs_toggle_request = 0;
                    openpete_native_toggle_fullscreen();
                }
            }
            prev_alt_enter = alt_enter;
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
         * previous pump's state — the same one-iteration lag the polled
         * bindings have always had. */
        if (!split) openpete_host_input_publish_sdl();

        /* Publish instant of the snapshot THIS tick consumes, captured at the
         * latch rather than read back later. Under the thread split main
         * republishes on its own ~1 ms pump loop, so by the end of the
         * iteration several newer samples have landed and a late read would
         * name one the tick never saw — against a staleness of the same
         * order as the error it would introduce. */
        op_time_t pace_latch_ns = 0;
        /* Which published sample this VBL consumed — identity, not a
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
            PsyX_UpdateInput();
        } else {
            /* Under the thread split main owns the pump, the SDL-state
             * publish and the pad sample; this thread only consumes what was
             * published. That asymmetry is the whole point of the seam — the
             * consumer never touches SDL. */
            if (!split) {
                openpete_pump_events();
                /* Re-publish now the pump has refreshed SDL, so the pad sample
                 * and this iteration's consumers see current state rather than
                 * the pre-pump snapshot taken above. */
                openpete_host_input_publish_sdl();
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

        /* Hand control to the game until the next VSync(0) yield. Gated so
         * --render-fps>0 outer iterations that don't have a sim tick due
         * skip the yield entirely (input is still pumped above; pacer below
         * sleeps to the next deadline). Frame-step adds a second gate:
         * while g_paused the yield only fires when g_step_one was set by
         * F8 — one tick per held-iter. */
        unsigned frame_before_yield = openpete_frame;
        bool ticked_this_iter = false;
        bool yield_now = sim_due && (!g_paused || g_step_one)
                                 && !rewound_this_iter;
        /* Pacing log, per VBL. The deadline in force for this iteration is
         * next_sim.at as it stands BEFORE the tail advances it — the grid
         * point the pacer slept to at the end of the previous iteration — so
         * exec_start minus that is the tick's start-phase error (S1).
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
            g_step_one = false;
            game_on_frame_pre(&g_game.cpu, openpete_frame);
            /* Calibration stall (OPENPETE_PACE_INJECT_US), off unless asked
             * for: a known lateness the log must then report, so the
             * instrument is checked against truth rather than trusted. */
            openpete_pace_inject(pace_paced);
            /* Start-alignment invariant: an iteration that runs the game
             * fiber must have made its latch decision THIS iteration, before
             * this point. The input window Tier B measured — one VBL period,
             * grid-aligned — is a property of that ordering, so a refactor
             * that moved the latch after the switch, or skipped it on some
             * path, would silently change the window while every phase
             * statistic kept reporting normally. Checked rather than
             * commented; warned rather than fatal, because a player's game
             * must not die for a diagnostic. */
            if (!openpete_host_input_legacy() &&
                pace_latch_seq == OP_PACE_LATCH_NONE) {
                static int warned;
                if (!warned++)
                    OPL_ERROR_F("input",
                                "ORDERING: VBL %u runs the fiber with no latch "
                                "decision — input phase is no longer defined "
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

                /* Runahead (savestate P5): the tick above is the
                 * COMMITTED timeline. Save it, speculate 2N more VBLs
                 * with held input (audible SPU muted; external side
                 * effects gated by openpete_speculative), let the interp
                 * captures below snapshot the LED state, then restore
                 * the real tick after them — everything downstream
                 * (hash/position logs, input log, recorders, pacer,
                 * canonical state) sees the real timeline while every
                 * present interpolates one led by ~N ticks. Measured
                 * L_submit = 2 ticks; N=2 collapses it.
                 *
                 * ONCE PER TICK, on the tick's DRAW VBL only (the real
                 * yield produced a present): the led state only advances
                 * once per game tick, so speculating on the update VBL
                 * too costs a full extra speculation for a capture that
                 * dedupes away — and that cost lands INSIDE the present-
                 * critical iteration, dragging the virtual present clock
                 * behind the capture stamps (measured: alpha drifting to
                 * -0.27, motion delivered as 30 Hz steps = the "whole
                 * world jittery" report). Non-speculated ticked
                 * iterations skip the interp captures entirely — the
                 * ring must never see a real-timeline (led-minus-N)
                 * state or it would pair backward. */
                int ra_want = (runahead_n > 0 && ra_snap &&
                               game_is_gameplay_live(&g_game.cpu));
                int ra_live_lost = 0;
                if (!ra_want) openpete_ra_input_edge = 0;
                if (ra_want && openpete_present_pending &&
                    openpete_state_mem_save(ra_snap, &g_game.cpu,
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
                     * the completed committed tick at its yield — the same
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
                                        "prediction != committed state (stealth "
                                        "host-state read during speculation?)\n",
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
                     * under runahead — every tick-keyed instrument
                     * sampled the wrong sim moments). */
                    int      ra_pp    = openpete_present_pending;
                    unsigned ra_prims = openpete_primitives_drawn;
                    int prev_mute = PsyX_SPUAL_SetMute(1);
                    openpete_speculative = 1;
                    /* Speculated ticks run inside this VBL's bracket but are
                     * not this VBL: their pad reads must not be measured
                     * against its exec_start. */
                    openpete_pace_vbl_suspend(1);
                    for (int v = 0; v < 2 * runahead_n; v++) {
                        unsigned bf = openpete_frame;
                        game_on_frame_pre(&g_game.cpu, openpete_frame);
                        g_openpete_guest_running = 1;
                        co_switch((cothread_t)openpete_game_fiber);
                        g_openpete_guest_running = 0;
                        if (openpete_frame == bf) break;   /* safety */
                        /* Speculation crossed into a load/cutscene. The
                         * rollback contract is void here: the game fiber
                         * is now parked at a TRANSITION-code yield whose
                         * host call stack differs structurally from the
                         * gameplay yield the snapshot was taken at, and
                         * the fiber stack is not snapshottable (Windows
                         * libco = opaque OS fibers). Restoring guest
                         * state under the mismatched continuation
                         * corrupts the game's own VSync pacing — the
                         * committed timeline then runs a full tick's
                         * work every VBL and the load never completes.
                         * So the speculation is ADOPTED instead: these
                         * VBLs become the real timeline (their held
                         * input is exactly what the input log records
                         * for them — the per-VBL append below covers
                         * every VBL this iteration consumed), and
                         * runahead disengages until gameplay is live
                         * again. Costs at most 2N VBLs of held-input
                         * substitution at the moment a fade begins. */
                        if (!game_is_gameplay_live(&g_game.cpu)) {
                            ra_live_lost = 1;
                            break;
                        }
                    }
                    openpete_speculative = 0;
                    openpete_pace_vbl_suspend(0);
                    (void)PsyX_SPUAL_SetMute(prev_mute);
                    if (!ra_live_lost) {
                        openpete_present_pending  = ra_pp;
                        openpete_primitives_drawn = ra_prims;
                    }
                    /* Record the led prediction while RAM is still the
                     * speculation head (the rollback happens below, after
                     * the interp captures); openpete_frame is the led
                     * frame here. */
                    if (ra_pc_on() && !ra_live_lost) {
                        for (int i = 0; i < RA_PC_PENDING; i++) {
                            if (s_ra_pc[i].valid) continue;
                            s_ra_pc[i].target = openpete_frame;
                            s_ra_pc[i].hash   = ra_pc_hash();
                            s_ra_pc[i].valid  = 1;
                            if (ra_pc_on() >= 2) {
                                if (!s_ra_pc[i].led)
                                    s_ra_pc[i].led = (uint8_t*)malloc(
                                        OPENPETE_PSX_RAM_SIZE + OPENPETE_PSX_SCRATCH_SIZE);
                                if (s_ra_pc[i].led) {
                                    memcpy(s_ra_pc[i].led, openpete_psx_get_ram(),
                                           OPENPETE_PSX_RAM_SIZE);
                                    memcpy(s_ra_pc[i].led + OPENPETE_PSX_RAM_SIZE,
                                           openpete_psx_get_scratch(),
                                           OPENPETE_PSX_SCRATCH_SIZE);
                                }
                            }
                            break;
                        }
                    }
                    ra_speculated = true;
                    pace_flags |= OP_PACE_F_SPECULATIVE;
                }
                ra_skip_capture = (ra_want && !ra_speculated) || ra_live_lost;
                if (ra_live_lost) {
                    /* Adoption (see the spec-loop comment): the led VBLs
                     * are the real timeline now. No rollback; predictions
                     * pending against the abandoned branch die; the
                     * presentation drops its pre-adoption pairs and snaps
                     * (invisible — a fade is starting). */
                    ra_speculated = false;             /* skip the rollback below */
                    ra_pc_invalidate_all();
                    openpete_ra_input_edge = 0;
                    /* Presentation-only reset (NOT the guest-discontinuity
                     * broadcast — the timeline is continuous forward, and
                     * the broadcast would finalize an in-progress bk2
                     * recording): drop the pre-adoption interp pairs so
                     * the next present snaps instead of sweeping a lerp
                     * across the adopted VBLs. */
                    openpete_interp_reset();
                    openpete_interp_objs_reset();
                    OPL_INFO_F("runahead",
                               "speculation crossed into a transition; adopted "
                               "the led timeline through f=%u (runahead idle "
                               "until gameplay resumes)", openpete_frame);
                }

                /* fps-unlock Phase 2: snapshot the transform-bearing
                 * regions into the interp ring while the game fiber
                 * is still yielded (consistent RAM). No-op unless
                 * --render-fps > sim rate.
                 *
                 * Stamp with a PLL-SMOOTHED VBL clock, not raw op_now(): the
                 * raw completion time of a tick jitters by milliseconds with
                 * sim/render load, and every snapshot ROLL resets the sub-tick
                 * alpha mapping against its stamp — jittery stamps make shown
                 * interp time advance 0.05..1.4 ticks between evenly-generated
                 * presents (measured on the titlescreen flyby: fast movers
                 * judder ±half a tick at ANY --render-fps while the slow
                 * background looks fine). Locking stamps to the ideal VBL grid
                 * (proportional 1/32 nudge tracks real cadence; full resync on
                 * a stall/jump, e.g. CD loads) keeps the present-time → shown-
                 * time mapping continuous across rolls. */
                if (!ra_skip_capture) {
                    static op_time_t s_vclk = 0;
                    static unsigned  s_vclk_f = 0;
                    /* Ideal VBL step in WALL time — scaled so --tick-scale keeps
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
                     * — the [0,1] clamp then snaps shown time forward ~0.5 VBL
                     * at every roll and the following intermediates crawl to
                     * compensate (measured: ~24% of all motion delivered as
                     * 30 Hz jumps — the flyby judder). Biasing the stamp back
                     * by a fixed phase keeps every present's alpha inside
                     * (0,1): the mapping now−→shown-time stays linear through
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
                        /* openpete_interp_capture grids the DIRECT ring's stamp under
                         * deterministic capture (fixed 0,P,2P,… grid); mirror that for the
                         * objs ring so per-object sub-tick interps (moby positions/rotation,
                         * particles, sparkles, HUD icons, dragon-stars) pair the pinned
                         * present clock against a GRIDDED object timestamp instead of wall
                         * time — else those tiles flake run-to-run on the intermediate leg. */
                        g_openpete_tick_stamp = openpete_interp_deterministic()
                                              ? openpete_interp_curr_tick_ns() : stamp;
                    }
                }

                /* High-FPS step 1: per-object (indirect) snapshot — walks
                 * the game's object pools (g_LevelMobys) into the keyed
                 * prev/curr table for sub-tick lerp. Same read-only, same
                 * --render-fps>sim gate. No-op if no game callback. */
                if (!ra_skip_capture) openpete_interp_objs_run_capture();

                /* Runahead: captures done at the led head — roll back to
                 * the committed tick before anything else observes RAM. */
                if (ra_speculated) {
                    unsigned ra_lf = openpete_frame;
                    if (openpete_state_mem_load(ra_snap, &g_game.cpu,
                                                &ra_lf, 1) == 0) {
                        openpete_frame = ra_lf;
                        /* Committed-timeline regions (g_Camera): replace
                         * their led bytes in the ring's newest slot with
                         * the just-restored committed state, so those
                         * regions pair a branch-free timeline and can
                         * never show (or snap back from) a misprediction. */
                        openpete_interp_recapture_committed();
                    }
                }

                /* Spine health probe (OPENPETE_INTERP_QUERYTEST): confirm the
                 * interp ring PAIRS consecutive snapshots when gameplay state
                 * differs (paired=1) vs dedupes a static screen (paired=0).
                 * Verifies the one runtime-dependent piece of the sub-tick
                 * spine in the reliable headless level-load path. Read-only. */
                {
                    static int qt = -1;
                    if (qt < 0) { const char* e = getenv("OPENPETE_INTERP_QUERYTEST"); qt = (e && *e && *e != '0'); }
                    if (qt && (openpete_frame % 120u) == 0) {
                        double a = 0.0;
                        const openpete_interp_snapshot_t *pv = NULL, *cv = NULL;
                        int paired = openpete_interp_query(
                            op_now(), g_openpete_tick_period_ns, &a, &pv, &cv);
                        fprintf(stderr, "[interp-query f=%u] paired=%d captures=%llu dedupe=%llu\n",
                                openpete_frame, paired,
                                (unsigned long long)openpete_interp_total_captures(),
                                (unsigned long long)openpete_interp_total_dedupe_skips());
                    }
                }
                /* Numerical LOO gate lives game-side (it needs g_Camera/g_Spyro
                 * addresses; core stays address-free) — see
                 * spyro_interp_regions.c::spyro_interp_capture_objs. */
            }
        }

        /* Position log fires after each yield (post-fast-boot — address
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

        /* --bk2-record: one bk2 frame per openpete_frame so replay timing
         * matches recording 1:1. Sampled AFTER the yield because PsyX folds
         * keyboard/gamepad/--bk2 override into pad->buttons inside the
         * game's PadVSync handler — pre-yield the value would still be
         * stale from the previous frame. Also skip iterations where the
         * frame counter didn't advance (gated event pumps, no-tick yields
         * from dispatch stubs), which would otherwise inflate the movie
         * length. */
        if (ticked_this_iter) {
            /* Per-VBL input log — ALWAYS on (savestate plan P1): one entry
             * per VBL the fiber consumed this iteration, all carrying the
             * tick-latched bits (the same expansion the bk2 recorder used
             * before the lift-out, so recorded movies stay byte-identical).
             * Consumers: bk2_record (window [begin,finalize)), and later
             * the rewind replay / runahead inject drivers. */
            unsigned ticks = openpete_frame - frame_before_yield;
            uint16_t bits = PsyX_Pad_GetLastP1Bits();
            unsigned char ax[4];                 /* PSX pad->analog order: RX,RY,LX,LY */
            PsyX_Pad_GetLastP1Analog(ax);
            unsigned char bk2axes[4] = { ax[2], ax[3], ax[0], ax[1] };  /* -> LX,LY,RX,RY */
            for (unsigned t = 0; t < ticks; t++) {
                int idx = input_log_append(bits, bk2axes);
                if (bk2_recording) bk2_record_note(bits, idx);
            }
        }

        if (sim_due && opts->debug_triangle) {
            /* Re-issue the static triangle so it's drawn over whatever the
             * game queued — guarantees a non-black on-screen pixel for the
             * Phase 1e milestone even when the title-screen overlay is a
             * stub. Gated on sim_due so render-only iterations under
             * --render-fps>0 don't pollute the next tick's OT. */
            openpete_debug_triangle_emit();
        }

        /* Game yielded. Only present if the iter actually drew — see the
         * note above the pre-loop block. Screenshot fires BEFORE SwapWindow
         * so it captures the just-rendered backbuffer (post-swap, GL_BACK
         * holds rotated-in stale content that doesn't match the window).
         *
         * The FPS overlay rides on PsyX's `g_psyx_postRenderOverlay` hook:
         * PsyX_EndScene calls openpete_fps_overlay_emit between
         * StoreFrameBuffer and SwapWindow, so it lands on the presented
         * backbuffer without polluting the VRAM mirror. */
        /* True once a REAL game frame has been presented this iter.
         * The intermediate block must key off THIS, not
         * openpete_present_pending, which the block below clears to 0
         * before the intermediate gate runs — keying off the cleared
         * flag double-presented (game frame + a spurious alpha≈0
         * interpolated frame) on every draw iter (the "+30 fps"). */
        bool did_present_this_iter = false;
        /* World-liveness stamp: virtual-clock time of the most recent CANONICAL
         * present. During a CD load / level transition the game fiber stops
         * rendering (busy in CdRead/VSync waits) while the virtual clock keeps
         * ticking — the sub-tick spine must FREEZE then (present the last
         * composed scene) instead of re-deriving from RAM that the incoming
         * level is actively overwriting (the "mobys across the whole level
         * visible during the load" bug: half-loaded moby pools re-extracted
         * against a stale camera, with no terrain feed to occlude them). The
         * legacy PsyCross intermediate path never had this bug because it
         * re-presents a FROZEN saved OT; this stamp gives the spine the same
         * frozen-while-stuck semantics. 0 until the first canonical present
         * (spine stays off until there is something real to interpolate). */
        static op_time_t s_last_canon_present_ns = 0;
        /* Multi-present capture (--screenshot-presents-start/count):
         * shared between the canonical block immediately below and the
         * intermediate block further down so both contribute to the
         * present sequence. Tracks how many presents have fired since
         * the sim reached the start frame. */
        static int s_presents_captured = 0;
        if (openpete_present_pending && !openpete_skip_present_until_level) {
            /* Canonical draw-tick counter: the Nth increment is the same sim
             * moment in every run (tick-deterministic sim), unlike
             * openpete_frame whose VBL↔tick mapping drifts with host timing.
             * Keys --screenshot-on-tick (here and the native-variant dump
             * below). */
            openpete_canon_tick++;
            int shot_key = opts->screenshot_ticks_keyed ? (int)openpete_canon_tick
                                                        : (int)openpete_frame;
            static bool s_shot_fired[OPENPETE_MAX_SCREENSHOT_FRAMES] = {0};
            /* --screenshot-native-only skips the -og leg: PsyCross isn't rasterizing
             * (openpete_psycross_renders == 0), so the present FBO this reads is
             * stale. The native -vk dump below is the sole output. */
            for (int si = 0; !opts->screenshot_native_only &&
                             si < opts->screenshot_frame_count; ++si) {
                if (s_shot_fired[si]) continue;
                if (shot_key < opts->screenshot_frames[si]) continue;
                s_shot_fired[si] = true;
                char base[1024], path[1100];
                openpete_screenshot_format_path(opts->screenshot_output,
                                                opts->screenshot_frames[si],
                                                /*present=*/0,
                                                base, sizeof(base));
                /* PsyCross reference render -> "<base>-og.png" so it sits
                 * alongside the native -vk/-ab/-diff variants dumped below. */
                const char *odot = strrchr(base, '.');
                if (odot) snprintf(path, sizeof(path), "%.*s-og%s",
                                   (int)(odot - base), base, odot);
                else      snprintf(path, sizeof(path), "%s-og", base);
                openpete_psyx_take_screenshot(path);
                OPL_INFO_F("screenshot",
                           "wrote %s on frame %u tick %u (target=%d, %s-keyed)",
                           path, openpete_frame, openpete_canon_tick,
                           opts->screenshot_frames[si],
                           opts->screenshot_ticks_keyed ? "tick" : "frame");
            }
            if (opts->screenshot_presents_start >= 0
                    && (int)openpete_frame >= opts->screenshot_presents_start
                    && s_presents_captured < opts->screenshot_presents_count) {
                char path[1024];
                openpete_screenshot_format_path(opts->screenshot_output,
                                                (int)openpete_frame,
                                                s_presents_captured,
                                                path, sizeof(path));
                openpete_psyx_take_screenshot(path);
                OPL_INFO_F("screenshot",
                           "wrote %s on frame %u (present=%d, canonical)",
                           path, openpete_frame, s_presents_captured);
                s_presents_captured++;
            }
            /* --record: capture this presented game frame before EndScene
             * stamps the FPS overlay. PTS is the wall-clock capture instant
             * in microseconds; the first frame anchors t=0. Reads the present
             * FBO at internal resolution (bars-free). No-op when disabled.
             * Only the PsyCross path here — when the native renderer owns the
             * display, the on-screen frame is captured by the async readback ring
             * inside openpete_native_render_frame below, so a vk recording holds
             * the native SSAA/widescreen frame, not the PsyCross reference. */
            if (!openpete_native_render_enabled())
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
             * present — the one cost shedding can never avoid, and the one the
             * log could not name. Worse, the intermediate figure is a SELECTED
             * sample: the predicate only admits a present it expects to fit, so
             * quoting it as "present cost" understates by construction. */
            const op_time_t pace_canon_t0 = op_now();
            op_time_t pace_canon_extract_ns = 0;
            openpete_pace_inject_canonical();  /* fault injection, off by default */

            /* Per-canonical-frame commit: the game latches which render paths this
             * frame drew (the "canon drawn" mask the sub-tick spine gates each
             * reextract feeder on) and clears its per-frame interval table for the
             * next frame. Driven here — once per canonical present, renderer-
             * agnostic — via a registered callback rather than psyq_DrawOTag, so it
             * survives the planned removal of the PsyCross OT submission. The game
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
             * (prev→curr). Showing the un-lagged canonical frame between lagged
             * intermediates makes every object jump forward one tick at the
             * canonical present and snap back on the next intermediate → a 30 Hz
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
                /* !headless: headless has no live present stream, so the canonical
                 * dump must stay the PURE canonical frame. The presentation-golden
                 * intermediate corpus enables interp capture headless (for the
                 * ring), which would otherwise arm this lag leg and lag the
                 * canonical -vk; the explicit alpha=0.5 capture below owns the
                 * sub-tick leg instead. */
                const int spine_on = lag_re && !opts->headless &&
                    openpete_native_render_enabled() && !openpete_unlock_fps &&
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
                    opts->screenshot_presents_start < 0 &&
                    !opts->screenshot_freeze_backdrop;
                if (eager_now != s_eager.active) {
                    openpete_present_thread_set_active(eager_now);   /* 0: drains */
                    s_eager.active = eager_now;
                    OPL_INFO_F("eager", "%s", eager_now ? "engaged" : "disengaged (inline presents)");
                }
                if (eager_now) {
                    openpete_native_scene_capture_overlay();
                    const op_time_t period = op_period_ns(s_render_period);
                    const op_time_t curr = openpete_interp_curr_tick_ns();
                    const op_time_t now = op_now();
                    op_time_t first = next_render.at;
                    while (first < now) first += period;    /* the next slot not yet gone */
                    /* The span is anchored on the newest capture's stamp — unless
                     * that stamp is stale. Captures dedupe when the interpolated
                     * regions do not change (title screen, a cutscene holding
                     * the camera), and a span anchored on a stamp a tick or
                     * more old collapses to one or two slots while the inline
                     * path keeps presenting the live scene at full rate. Anchor
                     * on this tick's landing instead. */
                    const op_time_t tp = (op_time_t)g_openpete_tick_period_ns;
                    const op_time_t anchor = (curr && now - curr < 2 * tp) ? curr : now;
                    const op_time_t span_end = anchor + tp;
                    eager_plan(now, first, period, span_end > first ? span_end : first);
                    /* The canonical scene, then as much of the span as fits
                     * before the update VBL is due — the latter only while
                     * the world is live (the gate below). */
                    s_eager.world_live = s_last_canon_present_ns != 0 &&
                        (now - s_last_canon_present_ns) <= 6 * sim_period_ns;
                    eager_produce(s_eager.world_live ? next_sim.at + sim_period_ns - 500000
                                                     : now, period);
                    pace_canon_extract_ns = 0;
                } else
                if (spine_on) {
                    /* Snapshot the 2D-UI overlay (fade/letterbox/menu panel + the
                     * 2D-UI sprites/lines) from THIS canonical render's scene before
                     * the re-extract wipes it. Captured on the CANONICAL present only
                     * — never on sub-ticks, where s_scene holds the already-re-applied
                     * carry set (re-capturing it accumulates unbounded). The re-extract
                     * (here and each sub-tick) only re-applies it. */
                    openpete_native_scene_capture_overlay();
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
                } else {
                    /* Spine off (unlock / paused / interp disabled): the carry is
                     * never applied, so drop this tick's feed-time accumulation —
                     * a long spine-off stretch must not pool stale 2D-UI tris
                     * that would flash when the spine re-engages. */
                    openpete_native_scene_overlay_accum_reset();
                }
            }

            /* Native renderer: present one frame to the Vulkan window now that
             * PsyCross's present FBO is freshly rendered (read back here as the
             * reference). No-op unless --renderer vk|ab|diff. */
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

            /* Maintain the capture freeze backdrop: render this present's world
             * frame into the capture freeze texture so a later headless dump of a
             * freeze frame (pause / flight-results / level-exit) shows the green
             * frozen-world effect. The live path snapshots frozen_tex only with a
             * window, so a headless capture must keep it itself. A no-op on
             * freeze/empty frames, so once the game pauses it holds the last world
             * frame. Off by default (an extra render per present); enabled only for
             * bk2s/poses that reach a freeze state. */
            if (opts->screenshot_freeze_backdrop && openpete_native_render_enabled()
                && (opts->screenshot_frame_count > 0 || opts->screenshot_intermediate_count > 0))
                openpete_native_render_snapshot_frozen(openpete_native_widescreen_factor());

            /* Native screenshots: on the same --screenshot-on-frame targets,
             * dump every native variant — "<out>-vk.png", "<out>-ab.png",
             * "<out>-diff.png" — regardless of the live --renderer mode, so the
             * native views sit beside the PsyCross "<out>-og.png" above. */
            if (openpete_native_render_enabled() && opts->screenshot_frame_count > 0) {
                static bool s_ab_shot[OPENPETE_MAX_SCREENSHOT_FRAMES] = {0};
                for (int si = 0; si < opts->screenshot_frame_count; ++si) {
                    if (s_ab_shot[si]) continue;
                    if (shot_key < opts->screenshot_frames[si]) continue;
                    s_ab_shot[si] = true;
                    char base[1024];
                    openpete_screenshot_format_path(opts->screenshot_output,
                                                    opts->screenshot_frames[si],
                                                    0, base, sizeof(base));
                    /* The device is the present thread's while eager is
                     * engaged: take it back for the dump, then return it. */
                    const int pt_was = openpete_present_thread_active();
                    if (pt_was) openpete_present_thread_set_active(0);
                    openpete_native_render_screenshot_all(base);
                    if (pt_was) openpete_present_thread_set_active(1);
                }
            }

            /* Presentation-golden intermediate leg: deterministic sub-tick
             * (alpha 0.5) native capture. Force the present clock to the tick
             * midpoint (curr_tick_ns + period/2) so the re-extract callback
             * interpolates exactly halfway between the two most-recent DISTINCT
             * ticks, then dump the native -vk. Headless-capable — this drives an
             * explicit re-extract; the live sub-tick present path is headless-
             * blocked. Keyed on openpete_frame (VBL): fires on the first present
             * at/after each target. Deterministic capture (enabled above) pins the
             * ring stamps to a fixed grid so alpha lands at exactly 0.5. */
            if (openpete_native_render_enabled() && opts->screenshot_intermediate_count > 0) {
                openpete_native_reextract_fn ire = openpete_native_get_reextract();
                static bool s_int_shot[OPENPETE_MAX_SCREENSHOT_FRAMES] = {0};
                for (int si = 0; ire && si < opts->screenshot_intermediate_count; ++si) {
                    if (s_int_shot[si]) continue;
                    if ((int)openpete_frame < opts->screenshot_intermediate_frames[si]) continue;
                    s_int_shot[si] = true;
                    int paired = openpete_interp_paired();
                    if (!paired)
                        OPL_WARN_F("screenshot",
                                   "intermediate frame %d: no interp bracket "
                                   "(spawn/load/static) — capturing canonical pose",
                                   opts->screenshot_intermediate_frames[si]);
                    op_time_t curr_ns = openpete_interp_curr_tick_ns();
                    openpete_interp_set_present_target(curr_ns + g_openpete_tick_period_ns / 2);
                    ire(&g_game.cpu);                      /* s_scene @ sub-tick midpoint */
                    openpete_interp_set_present_target(0); /* restore op_now() fallback */
                    char base[1024];
                    openpete_screenshot_format_path(opts->screenshot_output,
                                                    opts->screenshot_intermediate_frames[si],
                                                    0, base, sizeof(base));
                    /* The re-extract built the interpolated scene into the CURRENT
                     * scene (never pushed to frame history), so compose depth 0 —
                     * the normal shot-compose delay reads an empty lagged slot. */
                    openpete_native_render_set_capture_nodelay(1);
                    openpete_native_render_screenshot_all(base);
                    openpete_native_render_set_capture_nodelay(0);
                    OPL_INFO_F("screenshot",
                               "wrote intermediate alpha=0.5 native -vk on frame %u (target=%d, paired=%d)",
                               openpete_frame, opts->screenshot_intermediate_frames[si], paired);
                }
            }

            /* Cache rotate moved to "right before next yield's GTE"
             * (see the yield_now block above) so intermediate renders
             * between sim ticks see the correct cache.s_prev for the
             * saved OT they're re-walking. */

            /* Phase 4.1 frame-time recorder: one 32-byte record per
             * actual present. The deadline is computed against a
             * per-PRESENT counter (not openpete_frame, which is the
             * yield counter at PSX VBlank cadence — typically 2x
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
         * (The legacy PsyCross screen-space re-emit that used to be the
         * fallback here was deleted 2026-07-06 with the render matcher.)
         *
         * Conditions:
         *  - Not headless / unlock mode (no display target).
         *  - openpete_interp_enabled(): the render rate genuinely exceeds the
         *    sim rate. This is the SAME gate that arms capture (render_hz >
         *    VBL 59.826, kept in sync at startup + on every live render-fps
         *    change), NOT the weaker s_render_fps > 0. At/below sim parity —
         *    e.g. render-fps 30 vs the ~29.913 Hz sim — sub-tick interpolation
         *    is degenerate: the near-1:1 ratio duplicates/freezes frames
         *    (mobys "stick") while capture is off and the ring is stale. So
         *    30 fps presents canonical frames only, exactly like 'I'-toggle
         *    off (g_openpete_interp_enabled == 0).
         *  - No REAL frame presented this iter (did_present_this_iter).
         *    NOT !sim_due (sim_due fires every PSX VBL ~60 Hz but Spyro
         *    draws every other VBL, so ~30 sim-due iters/sec draw
         *    nothing — !sim_due wrongly made those present NOTHING:
         *    the "render-fps minus 30" deficit). And NOT
         *    !openpete_present_pending either — the present block clears
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
        const bool native_intermediate =
            !headless_block && !openpete_unlock_fps &&
            g_openpete_interp_enabled &&
            openpete_interp_enabled() &&
            !did_present_this_iter &&
            openpete_native_render_enabled() &&
            native_re != NULL &&
            !(g_paused && !g_step_one);
        /* Predictive shed: an intermediate present is optional, and a tick is
         * not. If this present would not finish before the sim's next
         * deadline, skip it — the pacing contract is that presents are shed
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
         * present rate — measured 0.84 doubled display gaps per tick at BOTH
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
             * canonical that follows — measured, it sits 0.083 of a tick
             * behind it against a 0.069 normal step — so the strict rule
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
                 * admitted present cost — a cold start, a shader compile, one
                 * slow scene — and sheds forever after, with nothing able to
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
                 * on slow hardware means one deliberately late tick every N —
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
                 * "least harm" intent — the probe still waits for a
                 * near-best moment — while guaranteeing such a moment recurs.
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
             * virtual clock). When the world is stuck — CD load, level
             * transition, any long producer stall — re-extracting reads RAM
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
             * (canonical p0 from the block above + these intermediates p1+), so
             * viewing p0..pN shows objects at interpolated sub-tick positions. */
            if (opts->screenshot_presents_start >= 0
                    && (int)openpete_frame >= opts->screenshot_presents_start
                    && s_presents_captured < opts->screenshot_presents_count) {
                char path[1024];
                openpete_screenshot_format_path(opts->screenshot_output,
                                                (int)openpete_frame,
                                                s_presents_captured, path, sizeof(path));
                openpete_native_render_screenshot(path);
                OPL_INFO_F("screenshot",
                           "wrote %s on frame %u (present=%d, native-intermediate)",
                           path, openpete_frame, s_presents_captured);
                s_presents_captured++;
            }
        }

        /* RAM dump runs outside the present_pending gate so a request for
         * the very first rendered frame (ram-dump-on-frame=0) still fires
         * even on iters where DrawOTag hasn't queued anything yet. Use >=
         * with a one-shot guard so the trigger is forgiving of skipped
         * frame numbers. */
        static int ram_dumped = 0;
        if (!ram_dumped && opts->ram_dump_on_frame >= 0 &&
            (int)openpete_frame >= opts->ram_dump_on_frame) {
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
                     * yielded — typically inside VSync's host yield
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
            (int)openpete_frame >= opts->frame_limit) {
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

        /* No PsyX_BeginScene here — DrawOTag starts the scene on demand. */

        /* The turbo binding drives the host loop's own pacing + the GL
         * swap-interval bypass. vbl is yield-driven so this only affects
         * wall-clock speed, not game state — fast-forward is invisible to
         * BK2 replay determinism. Scheduled --unlock-frames windows (test
         * harness) OR into the same flag. Read from this iteration's
         * keymap poll, so it is a tick newer than the peek at the top of
         * the loop; this one is authoritative. */
        /* OPENPETE_FORCE_PACING=1 (read once, then cached) keeps the
         * legacy / dual-pacer cadence active even under --headless. Used
         * by scripts/measure-pacing.py to capture before/after numbers
         * for the frame-pacing plan without needing a real display. */
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
        }

        const int s_force_pacing = pace_force_pacing();
        bool tab_unlock = (opts->headless && !s_force_pacing) ||
                          openpete_key_down(OPK_TURBO);
        bool sched_unlock = opts->unlock_ranges_count > 0 &&
                            unlock_frame_in_ranges(opts, openpete_frame);
        bool unlock_fps = tab_unlock || sched_unlock ||
                          openpete_skip_present_until_level;
        if ((unlock_fps ? 1 : 0) != pace_unlock_prev)
            openpete_pace_window(OP_PACE_W_UNLOCK, unlock_fps ? 1 : 0);
        pace_unlock_prev = unlock_fps ? 1 : 0;
        openpete_unlock_fps = unlock_fps ? 1 : 0;
        g_skipSwapInterval = unlock_fps ? 1 : 0;

        /* Paused without a step pending: sleep so we don't spin the CPU.
         * The condvar pacer below only waits when ticked_this_iter, and
         * the wall-clock pacer's render deadline does sleep but only at
         * its own cadence — explicit sleep here keeps the paused idle
         * cost near-zero regardless of render_fps. */
        if (g_paused && !ticked_this_iter && !unlock_fps) {
            SDL_Delay(16);
        }

        /* Phase 2 unified pacer. Both branches step phase-locked
         * integer-ns deadlines and sleep via op_sleep_until (Phase 0
         * abstraction). No condvar drain race, no SDL_Delay integer-ms
         * rounding.
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
         * WaitForSingleObject — the legacy condvar pacer always
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
             * iters; render deadline advances every outer iter; we
             * sleep to the sooner of the two so an under-paced render
             * doesn't starve the sim or vice-versa. */
            op_time_t now2 = op_now();
            if (sim_due) {
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
            op_deadline_advance(&next_render, s_render_period, 1);
            if (s_eager.active) {
                /* Under eager production the render grid is the present
                 * thread's business: this loop only borrows its PHASE when it
                 * plans a span. Step it silently to the present (no slip
                 * accounting — nothing was missed) and sleep to the sim
                 * deadline alone, instead of waking once per slot to do
                 * nothing, or spinning flat out at an uncapped --render-fps. */
                const op_time_t rp = op_period_ns(s_render_period);
                if (rp > 0 && now2 > next_render.at)
                    op_deadline_advance(&next_render, s_render_period,
                                        (now2 - next_render.at) / rp + 1);
                pace_sleep_until(next_sim.at, next_sim.at);
            } else {
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
         * render-fps=0 no-tick iters) don't call it — without this the tab
         * would freeze. emscripten_sleep(0) is a single Asyncify
         * unwind/rewind; harmless after an already-yielded paced iter. */
        emscripten_sleep(0);
#endif
    }

    if (bk2_active) {
        bk2_movie_free(&bk2);
        g_PsyX_Pad_InputOverride = 0xFFFF;
        g_PsyX_Pad_OverrideActive = 0;
        g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
        g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
    }
    if (bk2_recording) {
        bk2_record_finalize();
    }
    /* --record: drain the native async readback ring's queued tail frames to the
     * encoder (while the Vulkan device is still alive), THEN flush the encoder +
     * write the container trailer. Both safe unconditionally (no-op if recording
     * was never armed / native isn't the display). Reaching here on SIGTERM/Ctrl-C
     * still produces a playable file. */
    openpete_native_render_record_flush();
    openpete_record_shutdown();
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
                   "hash_mismatches=%llu first_div_frame=%u",
                   psxtrace->records_consumed,
                   (unsigned long long)psxtrace->frames_compared,
                   (unsigned long long)psxtrace->hashes_diverged,
                   psxtrace->diverged ? psxtrace->first_divergent_frame : 0u);
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
               "reason=%s frames=%u elapsed_ms=%llu dispatch_ok=%d",
               exit_reason, openpete_frame,
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

    openpete_ticker_stop();
    openpete_frame_time_log_close(&frame_time_log);
    op_functrace_dump_aggregate();   /* emit per-function aggregate into the trace */
    op_prof_close();
    if (openpete_interp_enabled()) {
        OPL_INFO_F("interp",
                   "captured=%llu, dedupe-skipped=%llu",
                   (unsigned long long)openpete_interp_total_captures(),
                   (unsigned long long)openpete_interp_total_dedupe_skips());
    }
    if (ra_pc_on() && (s_ra_pc_checked || s_ra_pc_mismatches || s_ra_pc_invalidated)) {
        /* stderr, not the log: the roundtrip gate greps this summary. */
        fprintf(stderr, "[ra-predict] checked=%u mismatch=%u invalidated=%u\n",
                s_ra_pc_checked, s_ra_pc_mismatches, s_ra_pc_invalidated);
    }
    openpete_interp_shutdown();

    /* Best-effort fiber teardown. Our game fiber is parked in its forever-
     * yield loop, so co_delete is safe. */
    if (openpete_game_fiber) {
        co_delete((cothread_t)openpete_game_fiber);
        openpete_game_fiber = NULL;
    }

    /* Joined before the renderer is torn down: it presents through the same
     * device. */
    openpete_present_thread_stop();
    openpete_present_thread_report();
    eager_report();

    {   uint64_t m; int64_t w, ln; size_t b;
        openpete_disc_cache_census(&m, &w, &b, &ln);
        if (openpete_disc_cache_active())
            OPL_INFO_F("cd", "disc cache: %zu MB, loaded in %.2f s, %llu miss(es), %.2f ms blocked",
                       b >> 20, (double)ln / 1e9, (unsigned long long)m, (double)w / 1e6);
        openpete_disc_cache_shutdown(); }
    /* Report before PsyX_Shutdown: the instrument reads PsyCross pad state. */
    openpete_host_input_parity_report();

    /* Vibration census. Emitted unconditionally because the numbers only mean
     * something together, and every one of them is a zero in some legitimate
     * configuration:
     *
     *   registered/nonzero  the diagnosis. libpad's PadSetAct is a
     *                       registration, so the expected reading is a small
     *                       number of calls all carrying {0,0} — a NON-zero
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
        if (openpete_host_input_vpad_rumble_calls() ||
            openpete_host_input_vpad_rumble_live()) {
            OPL_INFO_F("rumble",
                       "recorder: live=%d, %d callback(s), %d non-zero",
                       openpete_host_input_vpad_rumble_live(),
                       openpete_host_input_vpad_rumble_calls(),
                       openpete_host_input_vpad_rumble_nonzero());
        }
    }

    /* Whether the guest's own execution ever reached the Vulkan device. A
     * present thread can only run concurrently with a tick if this is zero;
     * any name listed here is a call that would have to be moved, locked, or
     * deferred first. */
    /* OSD engagement. A capture-based gate reading zero here never composited
     * a toast, whatever its pixels looked like — reported unconditionally so
     * that cannot pass unnoticed. */
    if (openpete_osd_enabled())
        OPL_INFO_F("osd", "census: %u toast(s) composited",
                   openpete_native_osd_composited());

    {
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
     * the worker leaves them alone and main runs them after the join —
     * tearing the window down from here would hang the process on exit. */
    if (!split) {
        openpete_native_render_shutdown();
        PsyX_Shutdown();
    }

    fprintf(stdout,
            "openpete-exit: reason=%s frames=%u ms=%u code=%d\n",
            exit_reason, openpete_frame, elapsed, exit_code);
    return exit_code;
}
#endif /* OPENPETE_HAS_PSYCROSS */

static void log_frame_on_exit(void) {
    OPL_INFO_F("exit", "last openpete_frame=%u", openpete_frame);
}

/* Single-threaded path: init then loop, on one thread, exactly as before the
 * split existed. */
static int run_recomp_fiber(const openpete_options_t* opts) {
    int rc = run_host_init(opts);
    if (rc != 0) return rc;
    return run_host_loop(opts);
}

/* Worker entry: the entire host loop and the game fiber. libco fibers are
 * thread-affine, so the fiber must be created and switched on this one thread
 * for its whole life — which is why the loop moves wholesale rather than
 * having main drive parts of it. */
static int  s_worker_rc;
static const openpete_options_t* s_worker_opts;

static int SDLCALL openpete_sim_thread(void* unused)
{
    (void)unused;
    s_worker_rc = run_host_loop(s_worker_opts);
    SDL_SetAtomicInt(&g_worker_done, 1);
    return 0;
}

/* main: owns SDL video, the window, and the event pump. See
 * openpete_thread_split_enabled for why. */
static int run_split_threaded(const openpete_options_t* opts)
{
    /* Bring SDL video up HERE, on the thread that will pump. PsyX_Initialise
     * calls SDL_Init(SDL_INIT_VIDEO) too, but from the worker — by then this
     * is an existing-subsystem refcount bump rather than a second init on the
     * wrong thread. */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        OPL_ERROR_F("boot", "SDL_Init(video) failed: %s", SDL_GetError());
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    /* The window must belong to the pumping thread: Win32 delivers a window's
     * messages only to its creator. Bring-up on the worker adopts this. */
    /* All host + PsyCross + SDL-subsystem init runs HERE, on main, before the
     * worker exists. SDL brings its video/gamepad/haptic subsystems up on one
     * thread and pumps them on that thread; with init on the worker, main's
     * pump raced it and faulted inside SDL. Doing init before main starts
     * pumping serialises the two with no barrier — and a barrier could not
     * work anyway, since the worker's SDL init needs main to be pumping. */
    int init_rc = run_host_init(opts);
    if (init_rc != 0) return init_rc;

    /* Seed the input seam before the worker exists: its first latch must find
     * a valid snapshot, or pad injection falls back to reading SDL on the
     * worker. Also publish an initial SDL-state snapshot for the same reason
     * — bindings evaluated on the worker must never reach SDL themselves. */
    openpete_host_input_seed_neutral();
    openpete_host_input_publish_sdl();

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
        OPL_ERROR_F("boot", "SDL_CreateThread(openpete-sim) failed: %s — "
                            "running single-threaded", SDL_GetError());
        return run_recomp_fiber(opts);
    }
    OPL_INFO_F("boot", "%s", "thread split: main owns window+pump, worker owns the sim");

    /* Pump loop. Deliberately fast relative to the sim: the worker latches
     * whatever was last published, so this period bounds input staleness.
     * SDL_WaitEventTimeout parks the thread instead of spinning, and still
     * wakes immediately when an event arrives. */
    while (!SDL_GetAtomicInt(&g_worker_done)) {
        /* Four instants per iteration, so a stall on this thread can be
         * attributed rather than merely noticed. A Win32 modal resize or move
         * runs the OS's own loop inside the event pump, which is where the
         * split's display defects have historically hidden, and it is
         * indistinguishable from a quiet loop unless the pump is timed apart
         * from the park that precedes it. */
        const int64_t hp_t0 = (int64_t)op_now();
        SDL_WaitEventTimeout(NULL, 1);
        const int64_t hp_waited = (int64_t)op_now();
        openpete_pump_events();
        const int64_t hp_pumped = (int64_t)op_now();
        openpete_host_input_publish_sdl();
        openpete_host_input_sample(
            (openpete_host_input_modstate() & SDL_KMOD_ALT) != 0);
        const int64_t hp_input = (int64_t)op_now();

        /* Idle-hide of the OS cursor. Another window op that belongs to this
         * thread, and cheap: one atomic read plus three flag reads. */
        openpete_cursor_update(opts->headless);

        /* Window ops belong to this thread. The worker raises the flag (from
         * a binding or Alt+Enter) and main performs the SDL call. */
        int hp_window = 0;
        if (g_native_fs_toggle_request) {
            g_native_fs_toggle_request = 0;
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
     * comment in run_host_loop). */
    openpete_native_render_shutdown();
    openpete_disc_cache_shutdown();
    PsyX_Shutdown();
    return s_worker_rc;
}

int openpete_run(const openpete_options_t* opts) {
    openpete_log_init(opts->log_format, opts->log_level_spec);
    /* The stderr mirror is normally already up (main() opens it before
     * attaching the recomp module). Harmless no-op if so; this covers
     * embedders that call openpete_run() directly. Before
     * atexit(log_frame_on_exit) because atexit runs LIFO — registering the
     * sink first makes it close LAST, so the exit lines still land in it. */
    openpete_log_file_open(opts->log_file);
    atexit(log_frame_on_exit);
    OPL_INFO_F("boot", "starting %s (frame_limit=%d max_seconds=%d headless=%d)",
               opts->display_name, opts->frame_limit, opts->max_runtime_seconds,
               (int)opts->headless);

    /* Extraction worker pool size (--extract-threads), before any consumer
     * can resolve it. Threads themselves spawn lazily on first parallel run. */
    openpete_workpool_configure(opts->extract_threads);

    /* libco: capture the host fiber. Phase 1b uses only co_active(); Phase 1c
     * creates the gameplay fiber so PSYQ calls can yield (VSync etc.). */
    /* Pointless under the split — main never switches, so it has no business
     * being a fiber. (This call was also FATAL until libco was built with
     * LIBCO_MP: without it `thread_local` is an empty macro, co_active_ is a
     * plain global, and main's conversion made the worker skip its own —
     * see the note in core/CMakeLists.txt. LIBCO_MP is the fix; this guard is
     * just hygiene.) */
    if (!openpete_split_applies(opts)) {
        cothread_t host_fiber = co_active();
        OPL_INFO_F("boot", "libco host fiber=%p", host_fiber);
    }

    openpete_psx_install_crash_handler();

    /* Phase 1b smoke test: dispatch one recompiled function and exit before
     * the SDL loop runs. */
    if (opts->test_recomp_call_addr != 0 || opts->test_recomp_segv) {
        return run_recomp_smoke_test(opts);
    }

#ifdef OPENPETE_HAS_PSYCROSS
    /* Phase 1d: dispatch entry on a fiber and drive a frame loop with VSync
     * yielding back here for present + scene swap. */
    if (opts->run_recomp_addr != 0) {
        if (openpete_split_applies(opts))
            return run_split_threaded(opts);
        return run_recomp_fiber(opts);
    }
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
        opts->display_name, WIN_W, WIN_H, window_flags);
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
