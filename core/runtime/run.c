// core/runtime/run.c — entry point: boot PSX EXE, optionally smoke-test a
// recompiled function, then drop into the SDL run loop.
//
// Phase 1b adds the boot path. The actual main loop is still a clear-color
// stub — Phase 1c will replace it with the recompiled `main` driving the
// gamestate dispatcher.

#include "openpete.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "screenshot.h"
#include "game_extras.h"

#include <SDL.h>
#include <libco.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* _exit */

#define WIN_W 640
#define WIN_H 480

#define CLEAR_R 0x10
#define CLEAR_G 0x14
#define CLEAR_B 0x20

/* Cooperative shutdown flag. Set from SDL event filter (window close / Alt-F4),
 * an Escape keypress, or our SIGINT handler. The host-fiber loop checks this
 * once per iteration and drains cleanly, instead of PsyCross's PsyX_Exit()
 * calling exit(0) from inside PsyX_BeginScene (which can hang in WSL/GL with
 * the game fiber and vblank thread still mid-flight). */
static volatile sig_atomic_t g_shutdown_requested = 0;

static int openpete_sdl_event_filter(void* userdata, SDL_Event* ev) {
    (void)userdata;
    if (ev->type == SDL_QUIT) {
        g_shutdown_requested = 1;
        return 0;  /* drop so PsyX_Sys_DoPollEvent never sees it */
    }
    if (ev->type == SDL_WINDOWEVENT &&
        ev->window.event == SDL_WINDOWEVENT_CLOSE) {
        g_shutdown_requested = 1;
        return 0;
    }
    if (ev->type == SDL_KEYDOWN &&
        ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        g_shutdown_requested = 1;
        return 0;
    }
    return 1;  /* keep everything else in queue for PsyCross */
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

/* Implemented in core/runtime/exe_loader.cpp. */
extern uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                                const char* exe_filename,
                                                size_t* out_size);
extern uint8_t* openpete_psx_read_file(const char* path, size_t* out_size);

extern void openpete_psx_set_diag_cpu(CPUState* cpu);

#ifdef OPENPETE_HAS_PSYCROSS
/* PsyCross window/GL initialiser. Must be called before any libgpu/libgte
 * call (ResetGraph, SetGeomScreen, etc.) — those touch internal SDL/GL
 * state set up here. */
extern void PsyX_Initialise(char* windowName, int screenWidth,
                            int screenHeight, int fullscreen);
extern void PsyX_Shutdown(void);
extern void PsyX_CDFS_Init(const char* imageFileName, int track, int sectorSize);
extern char PsyX_BeginScene(void);
extern void PsyX_EndScene(void);
extern void PsyX_UpdateInput(void);

/* Phase 1e infra-triangle (core/runtime/debug_render.cpp). */
extern void openpete_debug_triangle_init(void);
extern void openpete_debug_triangle_emit(void);
extern void openpete_psyx_take_screenshot(void);  /* writes ./SCREENSHOT.BMP */
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
    PsyX_Initialise((char*)opts->display_name, WIN_W, WIN_H, /*fullscreen=*/0);

    /* Wire PsyCross's CDFS to the disc .bin so CdRead has real data.
     * --disc PATH.cue derives the sibling .bin; otherwise we skip and
     * CdRead returns failure (acceptable for the leaf smoke test). */
    if (opts->disc_path) {
        char* bin = derive_bin_path(opts->disc_path);
        if (bin) {
            OPL_INFO_F("boot", "PsyX_CDFS_Init: %s", bin);
            PsyX_CDFS_Init(bin, /*track=*/0, /*sectorSize=*/0);
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
    int matched = psx_dispatch_compiled(&cpu, entry);
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

static void game_fiber_entry(void) {
    if (g_game.gp != 0u) {
        OPL_INFO_F("boot",
                   "game fiber: overriding cpu.gp 0x%08X -> 0x%08X "
                   "(skipping PSYQ _96_init)",
                   g_game.cpu.gp, g_game.gp);
        g_game.cpu.gp = g_game.gp;
    }
    OPL_INFO_F("boot", "game fiber: dispatching 0x%08X", g_game.entry);
    g_game.dispatch_ok = psx_dispatch_compiled(&g_game.cpu, g_game.entry);
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

static int run_recomp_fiber(const openpete_options_t* opts) {
    memset(&g_game, 0, sizeof(g_game));
    openpete_psx_runtime_init(&g_game.cpu);
    openpete_psx_set_diag_cpu(&g_game.cpu);

    uint8_t* bytes = NULL;
    size_t   len   = 0;
    if (try_load_exe(opts, &bytes, &len) != 0) {
        OPL_ERROR_F("boot", "could not load PSX EXE (need --exe-path or --disc)");
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    int rc = openpete_psx_boot(&g_game.cpu, bytes, len);
    free(bytes);
    if (rc != 0) return OPENPETE_EXIT_PANIC;

    OPL_INFO_F("boot", "PsyX_Initialise: opening %dx%d window (headless=%d)",
               WIN_W, WIN_H, (int)opts->headless);
    /* Headless: tell SDL to use offscreen video before PsyX touches it. */
    if (opts->headless) SDL_setenv("SDL_VIDEODRIVER", "offscreen", 0);
    PsyX_Initialise((char*)opts->display_name, WIN_W, WIN_H, /*fullscreen=*/0);

    /* Intercept window-close / Alt-F4 / Escape before PsyCross sees them —
     * PsyX_Sys_DoPollEvent reacts to SDL_QUIT / WINDOWEVENT_CLOSE by calling
     * PsyX_Exit() → exit(0), which in WSL+GL+libco tends to hang on static
     * teardown. Our filter sets g_shutdown_requested; the host-fiber loop
     * picks it up and we shut down through the normal path. */
    SDL_SetEventFilter(openpete_sdl_event_filter, NULL);
    signal(SIGINT,  openpete_on_sigint);
#ifdef SIGTERM
    signal(SIGTERM, openpete_on_sigint);
#endif

    if (opts->disc_path) {
        char* bin = derive_bin_path(opts->disc_path);
        if (bin) {
            OPL_INFO_F("boot", "PsyX_CDFS_Init: %s", bin);
            PsyX_CDFS_Init(bin, /*track=*/0, /*sectorSize=*/0);
            free(bin);
        } else {
            OPL_WARN_F("boot", "could not derive .bin path from --disc=%s",
                       opts->disc_path);
        }
    }

    g_game.entry = opts->run_recomp_addr;
    g_game.gp    = opts->test_recomp_gp;

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

    Uint32 start = SDL_GetTicks();
    int    exit_code = OPENPETE_EXIT_OK;
    const char* exit_reason = "frame-limit";
    int    quit = 0;

    /* Wall-clock pacing: PSX vblank is 60 Hz NTSC. Each VSync(0) yield must
     * advance >=1/60s of real time, otherwise the game fiber races ahead and
     * the cutscene plays at >2x speed. PsyCross's swap-interval path only
     * gates the GL present, and we present every other yield (Spyro is 30fps,
     * see the DrawOTag note above), so vsync alone wouldn't pace the empty
     * iterations. We pace every iteration here with a fractional accumulator
     * to avoid the integer-ms drift you'd get from SDL_Delay(16). */
    const double frame_period_ms = 1000.0 / 60.0;  /* 16.6667 */
    double next_yield_ms = (double)start;

    while (!quit) {
        /* Drain SDL events + refresh pad state before the game runs. PsyCross's
         * libpad reads keyboard via SDL_GetKeyboardState (pumped here) and
         * writes the bitfield into the buffer registered by PadInitDirect, so
         * the recompiled PadGetState sees live input. Our SDL_SetEventFilter
         * already swallows QUIT/CLOSE/ESC before this poll, so PsyCross's own
         * exit-on-quit path inside PsyX_Sys_DoPollEvent never fires. */
        PsyX_UpdateInput();

        /* Hand control to the game until the next VSync(0) yield. */
        co_switch((cothread_t)openpete_game_fiber);

        if (opts->debug_triangle) {
            /* Re-issue the static triangle so it's drawn over whatever the
             * game queued — guarantees a non-black on-screen pixel for the
             * Phase 1e milestone even when the title-screen overlay is a
             * stub. */
            openpete_debug_triangle_emit();
        }

        /* Game yielded. Only present if the iter actually drew — see the
         * note above the pre-loop block. */
        if (openpete_present_pending) {
            PsyX_EndScene();
            openpete_present_pending = 0;
        }

        if (opts->screenshot_on_frame >= 0 &&
            (int)openpete_frame == opts->screenshot_on_frame) {
            /* PsyCross's built-in glReadPixels → SCREENSHOT.BMP path.
             * Phase 1e+: replace with a PNG path that honours --output. */
            openpete_psyx_take_screenshot();
            OPL_INFO_F("screenshot",
                       "wrote ./SCREENSHOT.BMP on frame %u (--output=%s ignored for now)",
                       openpete_frame, opts->screenshot_output);
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
        if (!quit && opts->max_runtime_seconds > 0 &&
            (SDL_GetTicks() - start)
                > (Uint32)opts->max_runtime_seconds * 1000) {
            quit = 1;
            exit_reason = "max-runtime-seconds";
        }

        /* Sample log so the user can see the loop is alive. */
        if ((openpete_frame & 0x3F) == 1u) {
            OPL_INFO_F("loop",
                       "frame=%u prims_last=%u elapsed_ms=%u",
                       openpete_frame, openpete_primitives_drawn,
                       (unsigned)(SDL_GetTicks() - start));
        }
        openpete_primitives_drawn = 0u;

        /* No PsyX_BeginScene here — DrawOTag starts the scene on demand. */

        /* Pace this vblank to ~16.667 ms wall time (see frame_period_ms note
         * above the loop). If we're already past the deadline (slow GL frame,
         * heavy game work, debugger pause), don't try to catch up by skipping
         * the wait — but if we fell behind by >100 ms (e.g. window drag), drop
         * the accumulator forward so we don't busy-burn the next 6 frames. */
        next_yield_ms += frame_period_ms;
        Uint32 now_ms = SDL_GetTicks();
        if ((double)now_ms < next_yield_ms) {
            SDL_Delay((Uint32)(next_yield_ms - (double)now_ms));
        } else if ((double)now_ms > next_yield_ms + 100.0) {
            next_yield_ms = (double)now_ms;
        }
    }

    Uint32 elapsed = SDL_GetTicks() - start;
    OPL_INFO_F("exit",
               "reason=%s frames=%u elapsed_ms=%u dispatch_ok=%d",
               exit_reason, openpete_frame, elapsed, g_game.dispatch_ok);

    /* Best-effort fiber teardown. Our game fiber is parked in its forever-
     * yield loop, so co_delete is safe. */
    if (openpete_game_fiber) {
        co_delete((cothread_t)openpete_game_fiber);
        openpete_game_fiber = NULL;
    }

    PsyX_Shutdown();

    fprintf(stdout,
            "openpete-exit: reason=%s frames=%u ms=%u code=%d\n",
            exit_reason, openpete_frame, elapsed, exit_code);
    return exit_code;
}
#endif /* OPENPETE_HAS_PSYCROSS */

int openpete_run(const openpete_options_t* opts) {
    openpete_log_init(opts->log_format, opts->log_level_spec);
    OPL_INFO_F("boot", "starting %s (frame_limit=%d max_seconds=%d headless=%d)",
               opts->display_name, opts->frame_limit, opts->max_runtime_seconds,
               (int)opts->headless);

    /* libco: capture the host fiber. Phase 1b uses only co_active(); Phase 1c
     * creates the gameplay fiber so PSYQ calls can yield (VSync etc.). */
    cothread_t host_fiber = co_active();
    OPL_INFO_F("boot", "libco host fiber=%p", host_fiber);

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
        return run_recomp_fiber(opts);
    }
#endif

    if (opts->headless) {
        SDL_setenv("SDL_VIDEODRIVER", "offscreen", 0);
        OPL_INFO_F("boot", "headless mode requested (preferred driver: %s)",
                   SDL_getenv("SDL_VIDEODRIVER"));
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        if (opts->headless) {
            OPL_WARN_F("boot", "SDL_Init under '%s' failed (%s); retrying with dummy",
                       SDL_getenv("SDL_VIDEODRIVER"), SDL_GetError());
            SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
                OPL_ERROR_F("boot", "SDL_Init dummy fallback also failed: %s", SDL_GetError());
                return OPENPETE_EXIT_PANIC;
            }
        } else {
            OPL_ERROR_F("boot", "SDL_Init: %s", SDL_GetError());
            return OPENPETE_EXIT_PANIC;
        }
    }
    OPL_INFO_F("boot", "SDL video driver: %s", SDL_GetCurrentVideoDriver());

    Uint32 window_flags = SDL_WINDOW_SHOWN;
    if (opts->headless) {
        window_flags = SDL_WINDOW_HIDDEN;
    }
    SDL_Window* window = SDL_CreateWindow(
        opts->display_name,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, window_flags);
    if (!window) {
        OPL_ERROR_F("boot", "SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return OPENPETE_EXIT_PANIC;
    }

    Uint32 renderer_flags = opts->headless ? SDL_RENDERER_SOFTWARE
                                           : SDL_RENDERER_ACCELERATED;
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    if (!renderer && !opts->headless) {
        OPL_WARN_F("boot", "accelerated renderer unavailable, falling back: %s",
                   SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        OPL_ERROR_F("boot", "SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return OPENPETE_EXIT_PANIC;
    }

    Uint32 start = SDL_GetTicks();
    int frame = 0;
    int exit_code = OPENPETE_EXIT_OK;
    const char* exit_reason = "window-closed";
    bool quit = false;
    int  screenshot_status = 0;

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                quit = true;
                exit_reason = "sdl-quit";
            }
        }

        SDL_SetRenderDrawColor(renderer, CLEAR_R, CLEAR_G, CLEAR_B, 0xFF);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);

        if (opts->screenshot_on_frame >= 0 && frame == opts->screenshot_on_frame) {
            int rc = openpete_screenshot_save_png(renderer, opts->screenshot_output);
            if (rc != 0) {
                screenshot_status = 1;
                exit_code = OPENPETE_EXIT_PANIC;
                exit_reason = "screenshot-failed";
                quit = true;
            }
        }

        frame++;

        if (opts->frame_limit > 0 && frame >= opts->frame_limit) {
            quit = true;
            exit_reason = "frame-limit";
        }
        if (opts->max_runtime_seconds > 0 &&
            (SDL_GetTicks() - start) > (Uint32)opts->max_runtime_seconds * 1000) {
            quit = true;
            exit_reason = "max-runtime-seconds";
        }
    }

    Uint32 elapsed_ms = SDL_GetTicks() - start;
    OPL_INFO_F("exit", "reason=%s frames=%d elapsed_ms=%u screenshot_status=%d",
               exit_reason, frame, elapsed_ms, screenshot_status);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    fprintf(stdout, "openpete-exit: reason=%s frames=%d ms=%u code=%d\n",
            exit_reason, frame, elapsed_ms, exit_code);
    return exit_code;
}
