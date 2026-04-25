// core/runtime/run.c — entry point: boot PSX EXE, optionally smoke-test a
// recompiled function, then drop into the SDL run loop.
//
// Phase 1b adds the boot path. The actual main loop is still a clear-color
// stub — Phase 1c will replace it with the recompiled `main` driving the
// gamestate dispatcher.

#include "openpete.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "screenshot.h"
#include "game_extras.h"

#include <SDL.h>
#include <libco.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 640
#define WIN_H 480

#define CLEAR_R 0x10
#define CLEAR_G 0x14
#define CLEAR_B 0x20

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
