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
#include "openpete_ticker.h"
#include "screenshot.h"
#include "game_extras.h"
#include "bk2_movie.h"
#include "bk2_record.h"
#include "openpete_state.h"

#include "PsyX/PsyX_public.h"  /* g_PsyX_Pad_InputOverride */

#include <SDL.h>
#include <libco.h>
#include <errno.h>
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
                            int screenHeight, int fullscreen);
extern void PsyX_Shutdown(void);
extern void PsyX_CDFS_Init(const char* imageFileName, int track, int sectorSize);
extern char PsyX_BeginScene(void);
extern void PsyX_EndScene(void);
extern void PsyX_UpdateInput(void);

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

    OPL_INFO_F("boot", "PsyX_Initialise: opening %dx%d window (headless=%d)",
               WIN_W, WIN_H, (int)opts->headless);
    /* Headless still uses the real GL driver — SDL's "offscreen" video driver
     * hands back a context with NULL gl* function pointers, so the first call
     * from GR_InitialisePSX segfaults. We hide the window post-init below. A
     * GPU/display backend (X/Wayland/WSLg/WGL) must therefore be present;
     * pure-console CI needs Xvfb. */
    PsyX_Initialise((char*)opts->display_name, WIN_W, WIN_H, /*fullscreen=*/0);
    if (opts->headless && g_window) {
        SDL_HideWindow(g_window);
        OPL_INFO_F("boot", "headless: hid PsyCross window (driver=%s)",
                   SDL_GetCurrentVideoDriver());
    }

    /* Wire up the post-render overlay so PsyX_EndScene stamps an FPS counter
     * onto every presented frame. Has to be after PsyX_Initialise (the GL
     * context must exist before the overlay's first emit, but registration
     * itself doesn't touch GL — it just stores the callback). */
    g_psyx_postRenderOverlay = openpete_fps_overlay_emit;

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
    /* When --seed-cpu is supplied, suppress the --test-recomp-gp override
     * so it can't clobber the freshly-seeded gp inside game_fiber_entry. */
    g_game.gp    = opts->seed_cpu_path ? 0u : opts->test_recomp_gp;

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

    /* Engine-level mute: clamp the SPU AL sources and the XA stream's AL
     * gain. Bypasses the game's volume sliders entirely. */
    extern int  PsyX_SPUAL_SetMute(int on_off);
    extern void PsyX_XA_ForceGain(float linear_gain);
    PsyX_SPUAL_SetMute(opts->mute ? 1 : 0);
    PsyX_XA_ForceGain(opts->mute ? 0.0f : -1.0f);

    /* Forward CLI overrides to the per-game impl. game_on_frame applies
     * them when the relevant in-game globals are ready. */
    game_set_skip_to_level(opts->skip_to_level);

    /* Fast-boot gate is on by default (see fiber.c). psyq_DrawOTag clears it
     * the first time real draw commands hit, so host yields engage exactly
     * when the game starts rendering — no CLI knob needed. */
    OPL_INFO_F("boot", "fast-boot: skipping host yields until first DrawOTag");

    /* Start the free-running 60 Hz vblank ticker. Skipped under --headless
     * to keep that path deterministic — psyq_VSync advances `openpete_vbl`
     * synchronously when no ticker is running. See
     * plans/openpete-free-running-vblank-counter.md. */
    if (!opts->headless) {
        openpete_ticker_start();
    }

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

    /* Wall-clock pacing is now driven by openpete_ticker.h: each iteration
     * blocks on the ticker's condvar until the atomic vbl counter advances,
     * naturally pacing the host loop at ~60 Hz. The previous fractional
     * SDL_Delay accumulator is gone — pacing is implicit in "wait for
     * tick", which means a single VSync(0) call (loading-screen flyby)
     * resumes after one tick = 60 fps and a wait-loop body (gameplay)
     * resumes after two ticks = 30 fps without per-game predicates. */

    /* --bk2-record: prepare the in-memory frame buffer. The file isn't
     * touched until exit, so a SIGTERM/Ctrl-C against the run still
     * produces a usable movie. */
    bool bk2_recording = (opts->bk2_record_path && *opts->bk2_record_path);
    if (bk2_recording) {
        if (bk2_record_begin(opts->bk2_record_path) != 0) {
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

    /* Tab-held detection used for both the wall-clock pacer AND throttling
     * the SDL event pump: at 1000+ Hz, the per-iteration cost of SDL_PollEvent
     * + PsyX_Pad_InternalPadUpdates dominates. Polling at 120 Hz wall-clock
     * is plenty for catching Tab-release. We re-pump on every frame whenever
     * Tab isn't held so input feels normal during play. */
    Uint32 last_input_pump_ms = 0;

    /* Save-state hotkey state. Edge-triggered against the previous host-loop
     * iteration so a held key fires once per press. Number-row 1=save,
     * 2=load, 3=cycle active slot through 0..9. (The plan's original F2/F4
     * choices collide with PsyCross's debug bindings — F4 in particular
     * cycles g_activeKeyboardControllers, which silently kicks the keyboard
     * off pad 1 and looks identical to a save-state regression.) */
    bool prev_save = false, prev_load = false, prev_cycle = false;
    unsigned active_slot = 0;

    while (!quit) {
        /* --bk2: stamp this frame's bitfield into PsyCross's pad-override
         * slot. PsyX_Pad_InternalPadUpdates uses it instead of polling SDL.
         * Out-of-range frames (before the offset, or past the movie's end)
         * fall back to 0xFFFF — i.e. no override — so live keyboard /
         * gamepad input takes over once the movie ends. */
        if (bk2_active) {
            int bk2_frame = (int)openpete_frame - bk2_offset;
            unsigned short bits = 0xFFFF;
            if (bk2_frame >= 0 && bk2_frame < bk2.frame_count) {
                bits = bk2.frames[bk2_frame];
            } else if (bk2_frame >= bk2.frame_count && opts->bk2_stop_on_end) {
                quit = 1;
                exit_reason = "bk2-end";
                break;
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
        }

        /* Drain SDL events + refresh pad state before the game runs. PsyCross's
         * libpad reads keyboard via SDL_GetKeyboardState (pumped here) and
         * writes the bitfield into the buffer registered by PadInitDirect, so
         * the recompiled PadGetState sees live input. Our SDL_SetEventFilter
         * already swallows QUIT/CLOSE/ESC before this poll, so PsyCross's own
         * exit-on-quit path inside PsyX_Sys_DoPollEvent never fires.
         *
         * Tab-held throttle: cap the event pump at ~120 Hz so per-iteration
         * cost drops at unlocked frame rate. We still pump *some* events so
         * the Tab-release transition gets seen and we can drop back to 60Hz. */
        const Uint8* kbd_pre = SDL_GetKeyboardState(NULL);
        bool tab_held = (kbd_pre && kbd_pre[SDL_SCANCODE_TAB]);

        /* Save-state hotkeys: 1 quick-save, 2 quick-load, 3 cycle slot.
         * Edge-triggered (one action per keypress). The game fiber is
         * yielded right now, so g_game.cpu and g_ram are quiescent — we can
         * snapshot or restore without racing the recompiled code.
         *
         * Run BEFORE PsyX_UpdateInput so a load that overwrites the pad
         * buffer (which lives in restored g_ram) is immediately followed by
         * a fresh write of current input — otherwise the game fiber reads
         * stale buttons from the saved RAM and feels like input has stopped
         * registering until the next iter. */
        {
            bool save_k  = kbd_pre && kbd_pre[SDL_SCANCODE_1];
            bool load_k  = kbd_pre && kbd_pre[SDL_SCANCODE_2];
            bool cycle_k = kbd_pre && kbd_pre[SDL_SCANCODE_3];
            if (save_k && !prev_save) {
                (void)openpete_state_save(active_slot, &g_game.cpu,
                                          openpete_frame);
            }
            if (load_k && !prev_load) {
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
            if (cycle_k && !prev_cycle) {
                active_slot = (active_slot + 1u) % OPENPETE_STATE_NUM_SLOTS;
                OPL_INFO_F("savestate", "active slot -> %u%s",
                           active_slot,
                           openpete_state_slot_present(active_slot)
                               ? " (occupied)" : " (empty)");
            }
            prev_save  = save_k;
            prev_load  = load_k;
            prev_cycle = cycle_k;
        }

        Uint32 now_for_input = SDL_GetTicks();
        if (!tab_held || (now_for_input - last_input_pump_ms) >= 8) {
            PsyX_UpdateInput();
            last_input_pump_ms = now_for_input;
        }

        /* Hand control to the game until the next VSync(0) yield. */
        unsigned frame_before_yield = openpete_frame;
        game_on_frame_pre(&g_game.cpu, openpete_frame);
        co_switch((cothread_t)openpete_game_fiber);
        bool ticked_this_iter = (openpete_frame != frame_before_yield);

        if (ticked_this_iter) {
            game_on_frame(&g_game.cpu, openpete_frame);
        }

        /* --bk2-record: one bk2 frame per openpete_frame so replay timing
         * matches recording 1:1. Sampled AFTER the yield because PsyX folds
         * keyboard/gamepad/--bk2 override into pad->buttons inside the
         * game's PadVSync handler — pre-yield the value would still be
         * stale from the previous frame. Also skip iterations where the
         * frame counter didn't advance (gated event pumps, no-tick yields
         * from dispatch stubs), which would otherwise inflate the movie
         * length. */
        if (bk2_recording && ticked_this_iter) {
            unsigned ticks = openpete_frame - frame_before_yield;
            uint16_t bits = PsyX_Pad_GetLastP1Bits();
            for (unsigned t = 0; t < ticks; t++) {
                bk2_record_append(bits);
            }
        }

        if (opts->debug_triangle) {
            /* Re-issue the static triangle so it's drawn over whatever the
             * game queued — guarantees a non-black on-screen pixel for the
             * Phase 1e milestone even when the title-screen overlay is a
             * stub. */
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
        if (openpete_present_pending) {
            static int screenshotted = 0;
            if (!screenshotted && opts->screenshot_on_frame >= 0 &&
                (int)openpete_frame >= opts->screenshot_on_frame) {
                screenshotted = 1;
                const char* path = opts->screenshot_output;
                openpete_psyx_take_screenshot(path);
                OPL_INFO_F("screenshot",
                           "wrote %s on frame %u",
                           path, openpete_frame);
            }
            PsyX_EndScene();
            openpete_present_pending = 0;
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
         * the accumulator forward so we don't busy-burn the next 6 frames.
         *
         * Hold Tab to disable the limiter and run as fast as the GL/CPU
         * pipeline allows. Useful for chewing through cutscenes / loading
         * screens while iterating on a downstream gap. We re-anchor
         * next_yield_ms to "now" while Tab is held so that releasing it
         * resumes 60 Hz pacing without trying to catch up on dropped
         * frames.
         *
         * Tab also flips PsyCross's g_skipSwapInterval — without that the
         * GL present (SDL_GL_SwapWindow with swap interval 1) would still
         * lock us to the monitor refresh, so the host loop wouldn't actually
         * spin faster than ~165 Hz on a 165 Hz display. */
        const Uint8* kbd = SDL_GetKeyboardState(NULL);
        bool unlock_fps = opts->headless || (kbd && kbd[SDL_SCANCODE_TAB]);
        g_skipSwapInterval = unlock_fps ? 1 : 0;

        /* Block until the ticker advances `openpete_vbl`, gating game-fiber
         * resume on a wall-clock vblank. dispatch.c's no-tick stub yields
         * (where `ticked_this_iter` is false) skip the wait so a frame full
         * of overlay stubs doesn't multiply 16.67 ms by N stubs. Tab-held /
         * --headless skip the wait too — they want to spin as fast as the
         * pipeline allows. */
        if (!unlock_fps && ticked_this_iter && openpete_ticker_drives_vbl) {
            int vbl_before = SDL_AtomicGet(&openpete_vbl);
            SDL_LockMutex(openpete_ticker_wake_mutex);
            while (SDL_AtomicGet(&openpete_vbl) == vbl_before &&
                   !g_shutdown_requested) {
                SDL_CondWaitTimeout(openpete_ticker_wake_cond,
                                    openpete_ticker_wake_mutex, 100);
            }
            SDL_UnlockMutex(openpete_ticker_wake_mutex);
        }
    }

    if (bk2_active) {
        bk2_movie_free(&bk2);
        g_PsyX_Pad_InputOverride = 0xFFFF;
    }
    if (bk2_recording) {
        bk2_record_finalize();
    }

    Uint32 elapsed = SDL_GetTicks() - start;
    OPL_INFO_F("exit",
               "reason=%s frames=%u elapsed_ms=%u dispatch_ok=%d",
               exit_reason, openpete_frame, elapsed, g_game.dispatch_ok);

    openpete_ticker_stop();

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

static void log_frame_on_exit(void) {
    OPL_INFO_F("exit", "last openpete_frame=%u", openpete_frame);
}

int openpete_run(const openpete_options_t* opts) {
    openpete_log_init(opts->log_format, opts->log_level_spec);
    atexit(log_frame_on_exit);
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
