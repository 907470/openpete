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
#include "openpete_time.h"
#include "openpete_frame_time_log.h"
#include "openpete_interp.h"
#include "screenshot.h"
#include "game_extras.h"
#include "bk2_movie.h"
#include "bk2_record.h"
#include "feature_registry.h"
#include "features/no_lod.h"
#include "features/widescreen.h"
#include "openpete_state.h"
#include "openpete_fnv1a64.h"
#include "openpete_psxtrace.h"

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

/* PCSX-Redux trace state — file-static so the dispatch hook (which has
 * to be a plain function pointer with no closure) can find it. There's
 * only ever one open trace per run. */
static openpete_psxtrace_t* s_psxtrace_active = NULL;

static void openpete_psxtrace_on_anchor(uint32_t addr) {
    (void)addr;
    if (!s_psxtrace_active) return;
    /* EOF (1) and error (3) are terminal — stop advancing. Divergence (2)
     * is sticky for the host loop's exit-decision but the verifier keeps
     * comparing every subsequent frame so --psxtrace-keep-going can
     * surface ALL leaks across the trace, not just the first. */
    if (s_psxtrace_active->terminal_status == 1 ||
        s_psxtrace_active->terminal_status == 3) return;
    int rc = openpete_psxtrace_advance(s_psxtrace_active);
    if (rc < 0)      s_psxtrace_active->terminal_status = 3;
    else if (rc == 0) s_psxtrace_active->terminal_status = 1;
    else if (s_psxtrace_active->diverged &&
             s_psxtrace_active->terminal_status == 0) {
        s_psxtrace_active->terminal_status = 2;
    }
}

/* Cooperative shutdown flag. Set from SDL event filter (window close / Alt-F4),
 * an Escape keypress, or our SIGINT handler. The host-fiber loop checks this
 * once per iteration and drains cleanly, instead of PsyCross's PsyX_Exit()
 * calling exit(0) from inside PsyX_BeginScene (which can hang in WSL/GL with
 * the game fiber and vblank thread still mid-flight). */
static volatile sig_atomic_t g_shutdown_requested = 0;

static bool openpete_sdl_event_filter(void* userdata, SDL_Event* ev) {
    (void)userdata;
    if (ev->type == SDL_EVENT_QUIT) {
        g_shutdown_requested = 1;
        return false;  /* drop so PsyX_Sys_DoPollEvent never sees it */
    }
    if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        g_shutdown_requested = 1;
        return false;
    }
    /* Escape quits — but not while a gamepad is connected, so a controller's
     * phantom desktop-config keystroke can't kill the game. The window-close
     * path above still quits. */
    if (ev->type == SDL_EVENT_KEY_DOWN &&
        ev->key.scancode == SDL_SCANCODE_ESCAPE &&
        !SDL_HasGamepad()) {
        g_shutdown_requested = 1;
        return false;
    }
    return true;  /* keep everything else in queue for PsyCross */
}

/* Host hotkeys (Tab fast-forward, 1/2/3 savestates, F7-F10, I) read the
 * keyboard directly. A connected controller can inject phantom keystrokes —
 * the Steam Controller's Steam-Input desktop config does exactly this — and
 * SDL surfaces them with which==0, indistinguishable from a real keyboard.
 * So while a gamepad is connected we suppress the keyboard for hotkeys,
 * EXCEPT keys that no controller config emits and which are therefore always
 * a genuine press. Tab (fast-forward) is the one such key today; add more to
 * the allowlist below if they prove phantom-safe. Returns a buffer indexed
 * exactly like SDL_GetKeyboardState's array; all callers already null-check. */
static const bool* openpete_keyboard_state(void) {
    const bool* sdl = SDL_GetKeyboardState(NULL);
    if (!sdl || !SDL_HasGamepad())
        return sdl;
    static bool filtered[SDL_SCANCODE_COUNT];
    memset(filtered, 0, sizeof(filtered));
    filtered[SDL_SCANCODE_TAB] = sdl[SDL_SCANCODE_TAB];  /* phantom-safe */
    return filtered;
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
 * TUs override this by defining their own non-weak symbols (see
 * games/spyro-1/spyro_state_regions.c). Without an override the hash
 * log is the FNV1a-64 offset basis on every frame — useless but
 * link-safe, so a smoke build of core/ alone still works. */
__attribute__((weak)) const OpenpeteStateRegion* openpete_state_regions = NULL;
__attribute__((weak)) size_t openpete_state_region_count = 0;

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
    PsyX_Initialise((char*)opts->display_name, WIN_W, WIN_H, /*fullscreen=*/0,
                    /*startHidden=*/opts->headless ? 1 : 0);

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

    /* Feature modules apply their CLI-derived knobs. Phase 1 of
     * notes/plans/openpete-feature-modules.md: registry exists but
     * no features are migrated yet (openpete_feature_count == 0), so
     * this is a no-op. Phase 2 moves the widescreen --aspect block
     * below into core/features/widescreen.c where it gets called via
     * this single line. */
    feature_registry_apply_options(opts);

    int win_w = (opts->window_w > 0) ? opts->window_w : WIN_W;
    int win_h = (opts->window_h > 0) ? opts->window_h : WIN_H;
    /* If --aspect is set but the user didn't pin --window, scale the
     * default window so the *cropped* (post-overscan) src exactly fills
     * it — otherwise the present blit letterboxes the un-rendered
     * disp/clip difference. Spyro's clip is 224 of 240 disp rows, so the
     * cropped aspect = requested_aspect × 240/224. User-provided --window
     * always wins. */
    if (opts->aspect_w_over_h > 0.0f &&
        opts->window_w <= 0 && opts->window_h <= 0) {
        const float crop_factor = 240.0f / 224.0f;
        win_w = (int)((float)WIN_H * opts->aspect_w_over_h * crop_factor + 0.5f);
        win_h = WIN_H;
        OPL_INFO_F("aspect", "auto window size %dx%d (--aspect=%.4f × crop %.4f)",
                   win_w, win_h, (double)opts->aspect_w_over_h, (double)crop_factor);
    }

    /* --aspect: widen the framebuffer + GTE offset so the GTE projects
     * vertices across a wider screen (Hor+ widescreen). The factor
     * stays as a float global; thunks multiply at every Set*Env / offset
     * call. 0.0f sentinel keeps the multiplier inactive when no
     * override is requested. */
    /* --pgxp 0|1: PGXP cache consumer toggle (PsyCross-side). The
     * GTE-side emit (psxrecomp gte_rtps_internal) always populates
     * the cache when USE_PGXP=1 and render_pass_active; these flags
     * decide whether the consumer (ApplyVertexPGXP) actually reads
     * from it. Default 1 as of Day 1c (2026-05-26) — float-projected
     * SXY in PSX pixel space passes per-level soak across all 6
     * levels (Artisans + 4 homeworlds + Gnasty). See
     * notes/plans/openpete-pgxp.md. */
    {
        int want_pgxp = (opts->pgxp_enable >= 0) ? opts->pgxp_enable : 1;
        g_cfg_pgxpTextureCorrection = want_pgxp;
        g_cfg_pgxpZBuffer = want_pgxp;
        OPL_INFO_F("pgxp", "consumer flags texture_correction=%d zbuffer=%d "
                   "(--pgxp %s)", want_pgxp, want_pgxp,
                   (opts->pgxp_enable >= 0) ? (want_pgxp ? "1" : "0") : "default 1");
    }

    /* --aspect / --fov derivation lives in core/features/widescreen.c.
     * Sets openpete_disp_w_factor + openpete_fov_h_override; must run
     * before PsyX_Initialise. */
    widescreen_apply_disp_w_factor(opts);

    /* --no-lod: feature module owns scratch-pool relocation, queue-
     * offset / moby-LP / DBG_KILL_LP knob arming, and polybuf grow.
     * Module's apply_options ran inside feature_registry_apply_options
     * above and installed the per-game setup hook; the call below
     * actually fires the hook when opts->no_lod is set. See
     * notes/plans/openpete-disable-lods.md. */
    if (no_lod_apply_options(opts) != 0) {
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    OPL_INFO_F("boot", "PsyX_Initialise: opening %dx%d window (headless=%d)",
               win_w, win_h, (int)opts->headless);
    /* Headless still uses the real GL driver — SDL's "offscreen" video driver
     * hands back a context with NULL gl* function pointers, so the first call
     * from GR_InitialisePSX segfaults. A GPU/display backend (X/Wayland/WSLg/
     * WGL) must therefore be present; pure-console CI needs Xvfb. The window
     * is created unmapped via startHidden so it never flashes or steals
     * focus — GL/FBO rendering works fine without it ever being shown. */
    PsyX_Initialise((char*)opts->display_name, win_w, win_h, /*fullscreen=*/0,
                    /*startHidden=*/opts->headless ? 1 : 0);

    /* Hor+ post-PsyX knob population — see core/features/widescreen.c.
     * Sets g_psyx_render_aspect_factor + every psx_runtime_* knob the
     * recompiler's address-keyed substitutions consult. */
    widescreen_apply_runtime_knobs(opts);

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

    /* PCSX-Redux oracle verifier: open the trace before the game loop and
     * register the dispatch hook so advance() ticks at every dispatch of
     * the trace's anchor PC (read from the trace itself unless the user
     * overrides via --psxtrace-anchor-pc), matching the BP
     * scripts/pcsx-redux-trace.lua used to record the trace. */
    static openpete_psxtrace_t s_psxtrace;
    openpete_psxtrace_t* psxtrace = NULL;
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

    /* Phase 2 pacer state — single time source (op_now / op_sleep_until)
     * for both the legacy --render-fps=0 path and the --render-fps>0
     * wall-clock pacer. Both branches step phase-locked deadlines in
     * integer ns and slip-clamp identically, eliminating the
     * condvar-drain race that left Phase 1a ~3 % rate-hot.
     *
     * Period choices:
     *   sim_period_ns    = NTSC PSX VBL (1001/60000 s = 16683333 ns,
     *                      matches core/runtime/ticker.c constant)
     *   render_period_ns = 1/render_fps when --render-fps>0, else
     *                      sim_period_ns (one present per sim tick).
     *
     * Invariant (locked in by openpete-fps-unlock.md "vbl-counter
     * loops"): the inner sim block yields the game fiber at most once
     * per outer iteration. Multi-vbl catch-up would race-complete the
     * `pre - post < 2` GPU-pacing loops in draw.c and the 210-vbl Sony /
     * Universal logo waits in initialization.c. The slip-clamp resets
     * the deadline forward without firing extra yields. */
    const op_time_t sim_period_ns    = (OP_NS_PER_SEC * 1001) / 60000;
    const op_time_t render_period_ns = (opts->render_fps > 0)
        ? (OP_NS_PER_SEC / (op_time_t)opts->render_fps)
        : sim_period_ns;
    const op_time_t slip_threshold_ns = 4 * sim_period_ns; /* ~67 ms */
    op_time_t start_ns      = op_now();
    op_time_t next_sim_ns    = start_ns;
    op_time_t next_render_ns = start_ns;
    if (opts->render_fps > 0) {
        OPL_INFO_F("pacer", "wall-clock dual-pacer: sim=%.4f ms render=%.4f ms (--render-fps=%d)",
                   (double)sim_period_ns / 1e6,
                   (double)render_period_ns / 1e6,
                   opts->render_fps);
    } else {
        OPL_INFO_F("pacer", "legacy pacer: sim=%.4f ms NTSC (--render-fps=0)",
                   (double)sim_period_ns / 1e6);
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
        const op_time_t sim_hz_x1000 = (OP_NS_PER_SEC * 1000) / sim_period_ns;
        const op_time_t render_hz_x1000 = (render_period_ns > 0)
            ? (OP_NS_PER_SEC * 1000) / render_period_ns
            : 0;
        if (opts->render_fps > 0 && render_hz_x1000 > sim_hz_x1000) {
            openpete_interp_set_enabled(1);
            OPL_INFO_F("interp",
                       "capture enabled (--render-fps=%d > sim ~30 Hz)",
                       opts->render_fps);
        }
    }

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

    /* --frame-time-log: per-present binary recorder for the pacing
     * harness. The "deadline" we record against is a pure-ideal
     * cadence:
     *     deadline = start_op_ns + frame * target_period_ns
     * This is platform-agnostic and pacer-agnostic — it tells us how
     * the *actual* present time drifts from a perfectly-paced clock,
     * regardless of whether the legacy SDL_AddTimer pacer or a future
     * high-res pacer is driving the loop. The target period is the
     * sim cadence today (30 fps = 33.333 ms), upgraded to 29.97 once
     * Phase 1 lands the NTSC-correct ticker. See
     * notes/plans/openpete-frame-pacing.md. */
    if (opts->frame_time_log_path && *opts->frame_time_log_path) {
        /* Default target: NTSC present cadence = 1001/30000 sec per
         * present (= 1/2 of 59.94 Hz VBL). When --render-fps N is set
         * the dual-pacer drives the loop at N Hz instead, so use that.
         * See plan §1: "ship --vbl-rate=59.94 as default". */
        if (opts->render_fps > 0) {
            frame_time_log_target_period_ns =
                (int64_t)((double)OP_NS_PER_SEC / (double)opts->render_fps);
        } else {
            frame_time_log_target_period_ns =
                (OP_NS_PER_SEC * (int64_t)1001) / (int64_t)30000;
        }
        if (openpete_frame_time_log_open(&frame_time_log,
                                         opts->frame_time_log_path,
                                         frame_time_log_target_period_ns) != 0) {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        frame_time_log_start_ns = op_now();
    }

    /* Substitute clamp — apply CLI override or env, else compiled default. */
    {
        extern float g_openpete_substitute_clamp_px;
        if (opts->substitute_clamp_px >= 0.0f) {
            g_openpete_substitute_clamp_px = opts->substitute_clamp_px;
        } else {
            const char* e = getenv("OPENPETE_SUBSTITUTE_CLAMP_PX");
            if (e && *e) g_openpete_substitute_clamp_px = (float)atof(e);
        }
        OPL_INFO_F("render", "substitute clamp = %.1f px/render (0 = disabled)",
                   (double)g_openpete_substitute_clamp_px);
    }

    /* Debug-highlight-smoothed — CLI flag wins, else env. A real global
     * (not a getenv at draw time) so it works across the WSL2→Win32
     * boundary where bare env vars never reach the .exe. */
    {
        extern int g_openpete_dbg_highlight_smoothed;
        if (opts->debug_highlight_smoothed >= 0) {
            g_openpete_dbg_highlight_smoothed = opts->debug_highlight_smoothed;
        } else {
            const char* e = getenv("OPENPETE_DEBUG_HIGHLIGHT_SMOOTHED");
            if (e && *e) g_openpete_dbg_highlight_smoothed = (atoi(e) != 0);
        }
        OPL_INFO_F("render",
                   "debug-highlight-smoothed = %d (F9 toggles live)",
                   g_openpete_dbg_highlight_smoothed);
    }
    {
        extern int g_openpete_dbg_black_present;
        if (opts->debug_black_intermediates >= 0) {
            g_openpete_dbg_black_present = opts->debug_black_intermediates;
        } else {
            const char* e = getenv("OPENPETE_DEBUG_BLACK_INTERMEDIATES");
            if (e && *e) g_openpete_dbg_black_present = (atoi(e) != 0);
        }
        OPL_INFO_F("render",
                   "debug-black-intermediates = %d (F10 toggles live)",
                   g_openpete_dbg_black_present);
    }

    /* Vertex trace — see openpete_vertex_trace.h. Off unless
     * --vertex-trace-log PATH given. */
    if (opts->vertex_trace_log_path && *opts->vertex_trace_log_path) {
        extern int openpete_vtrace_open(const char* path);
        if (openpete_vtrace_open(opts->vertex_trace_log_path) != 0) {
            fprintf(stderr, "openpete: failed to open --vertex-trace-log %s\n",
                    opts->vertex_trace_log_path);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    }

    /* Input pump rate. We used to throttle to 120 Hz when Tab was held
     * to keep per-iter cost down at 1000+ Hz, but that broke BK2 replay
     * determinism: g_PsyX_Pad_InputOverride is set per host iter, and
     * PsyX_UpdateInput is what propagates it into the pad buffer the
     * game polls — skipping pumps means the game reads a stale override.
     * Pump every iter; the cost is tolerable. */

    /* Save-state hotkey state. Edge-triggered against the previous host-loop
     * iteration so a held key fires once per press. Number-row 1=save,
     * 2=load, 3=cycle active slot through 0..9. (The plan's original F2/F4
     * choices collide with PsyCross's debug bindings — F4 in particular
     * cycles g_activeKeyboardControllers, which silently kicks the keyboard
     * off pad 1 and looks identical to a save-state regression.) */
    bool prev_save = false, prev_load = false, prev_cycle = false;
    unsigned active_slot = 0;

    /* Frame-step state. F7 toggles pause; F8 advances exactly one game
     * tick (hold to repeat at host-loop rate). While paused the host loop
     * pumps SDL events, sleeps the deadline, and skips the co_switch into
     * the game fiber — so g_ram / CPUState stay frozen and PsyCross keeps
     * presenting the last completed frame. SPU is muted on enter to avoid
     * the held-voice drone (Cubeb runs on its own callback thread; without
     * new SPU register writes voices loop their current sample). */
    bool g_paused = false;
    bool g_step_one = false;
    bool prev_pause_key = false;
    bool prev_dbg_key = false;
    bool prev_blk_key = false;
    bool prev_i_key = false;

    while (!quit) {
        /* Top-of-loop unlock peek used *only* to gate sim_due under
         * --render-fps>0. The authoritative `unlock_fps` write
         * (openpete_unlock_fps + g_skipSwapInterval) still happens at the
         * bottom of the loop, AFTER PsyX_UpdateInput has refreshed SDL's
         * keyboard cache and AFTER the present has consumed the previous
         * iter's swap-interval flag — preserving the original semantics
         * the GL present path was tested under. */
        const bool* kbd_top = openpete_keyboard_state();
        /* See OPENPETE_FORCE_PACING note below. The bottom of the loop
         * caches it; mirror the same gate here so the sim-due predicate
         * follows the pacer when measurement mode is on. Re-read each
         * iter — cheap, and getenv on Linux is a libc env-pointer lookup. */
        const char* fp_env = getenv("OPENPETE_FORCE_PACING");
        bool force_pacing = (fp_env && *fp_env && *fp_env != '0');
        bool unlock_now = (opts->headless && !force_pacing) ||
                          (kbd_top && kbd_top[SDL_SCANCODE_TAB]) ||
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
        op_time_t iter_now_ns = op_now();
        bool   sim_due = (opts->render_fps == 0) || unlock_now ||
                         (iter_now_ns >= next_sim_ns);

        /* --bk2: stamp this frame's bitfield into PsyCross's pad-override
         * slot. PsyX_Pad_InternalPadUpdates uses it instead of polling SDL.
         * Out-of-range frames (before the offset, or past the movie's end)
         * fall back to 0xFFFF — i.e. no override — so live keyboard /
         * gamepad input takes over once the movie ends. */
        if (sim_due && bk2_active) {
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
        const bool* kbd_pre = openpete_keyboard_state();

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

            /* Frame-step hotkeys. F7 edge-triggered toggle; F8 sets
             * g_step_one whenever held while paused, so the natural
             * key-repeat behavior is "step every host iter while held"
             * without needing an OS-level repeat-rate dependency. */
            bool pause_k = kbd_pre && kbd_pre[SDL_SCANCODE_F7];
            bool step_k  = kbd_pre && kbd_pre[SDL_SCANCODE_F8];
            if (pause_k && !prev_pause_key) {
                g_paused = !g_paused;
                /* Enter: mute. Exit: restore the user's --mute baseline
                 * (true by default; --mute off re-enables audio). */
                PsyX_SPUAL_SetMute(g_paused ? 1 : (opts->mute ? 1 : 0));
                OPL_INFO_F("frame-step", "%s at openpete_frame=%u",
                           g_paused ? "paused" : "resumed", openpete_frame);
            }
            if (g_paused && step_k) {
                g_step_one = true;
            }
            prev_pause_key = pause_k;

            /* F9: live-toggle the smoothed-batch red overlay. */
            bool dbg_k = kbd_pre && kbd_pre[SDL_SCANCODE_F9];
            if (dbg_k && !prev_dbg_key) {
                extern int g_openpete_dbg_highlight_smoothed;
                g_openpete_dbg_highlight_smoothed =
                    !g_openpete_dbg_highlight_smoothed;
                OPL_INFO_F("render", "debug-highlight-smoothed -> %d",
                           g_openpete_dbg_highlight_smoothed);
            }
            prev_dbg_key = dbg_k;

            /* F10: live-toggle the black-intermediate presentation test. */
            bool blk_k = kbd_pre && kbd_pre[SDL_SCANCODE_F10];
            if (blk_k && !prev_blk_key) {
                extern int g_openpete_dbg_black_present;
                g_openpete_dbg_black_present =
                    !g_openpete_dbg_black_present;
                OPL_INFO_F("render", "debug-black-intermediates -> %d",
                           g_openpete_dbg_black_present);
            }
            prev_blk_key = blk_k;

            /* I: live on/off A/B for interpolation itself. Off =
             * intermediate renders present the lagged frame unchanged
             * (clean "is frame-gen doing anything perceptible?" test). */
            bool i_k = kbd_pre && kbd_pre[SDL_SCANCODE_I];
            if (i_k && !prev_i_key) {
                extern int g_openpete_interp_enabled;
                g_openpete_interp_enabled = !g_openpete_interp_enabled;
                OPL_INFO_F("render", "interpolation -> %s",
                           g_openpete_interp_enabled ? "ON" : "OFF");
            }
            prev_i_key = i_k;
        }

        PsyX_UpdateInput();

        /* Hand control to the game until the next VSync(0) yield. Gated so
         * --render-fps>0 outer iterations that don't have a sim tick due
         * skip the yield entirely (input is still pumped above; pacer below
         * sleeps to the next deadline). Frame-step adds a second gate:
         * while g_paused the yield only fires when g_step_one was set by
         * F8 — one tick per held-iter. */
        unsigned frame_before_yield = openpete_frame;
        bool ticked_this_iter = false;
        bool yield_now = sim_due && (!g_paused || g_step_one);
        if (yield_now) {
            g_step_one = false;
            game_on_frame_pre(&g_game.cpu, openpete_frame);
            /* Renderer-foundation Phase 2.1 — advance the sim-tick
             * timestamp + period. The exact tick-origin is re-stamped by
             * psyq_DrawOTag→mark_now (Phase 2.1 hotfix) so this call's
             * tick value is overridden; we keep it only to set the
             * sim_period_ns the compute_alpha clamp uses. */
            {
                extern void openpete_render_sim_tick_advance_time(
                    int64_t sim_tick_ns, int64_t sim_period_ns);
                openpete_render_sim_tick_advance_time(op_now(),
                                                      2 * sim_period_ns);
            }
            /* Renderer-foundation: arm the deferred cache rotate. The
             * actual ring advance happens on the FIRST emit_vertex of
             * this yield's GTE pass. PadVSync-only yields produce no
             * emits, so the arm is harmlessly carried over to the next
             * yield that does run GTE — keeps the cache aligned with
             * the 30 Hz GAME FRAME cadence on a 60 Hz VBL fiber.
             *
             * Earlier iterations tried (a) post-EndScene rotate, which
             * left cache.prev = THIS frame's emits during intermediate
             * walks of LAST frame's OT — stretching artifacts; and
             * (b) unconditional pre-co_switch rotate, which destroyed
             * data on PadVSync-only yields — 86% miss rate in soak. */
            {
                extern void openpete_render_cache_arm_rotate(void);
                openpete_render_cache_arm_rotate();
            }
            co_switch((cothread_t)openpete_game_fiber);
            ticked_this_iter = (openpete_frame != frame_before_yield);

            if (ticked_this_iter) {
                game_on_frame(&g_game.cpu, openpete_frame);

                /* fps-unlock Phase 2: snapshot the transform-bearing
                 * regions into the interp ring while the game fiber
                 * is still yielded (consistent RAM). No-op unless
                 * --render-fps > sim rate. */
                openpete_interp_capture(openpete_frame, op_now());
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
        if (bk2_recording && ticked_this_iter) {
            unsigned ticks = openpete_frame - frame_before_yield;
            uint16_t bits = PsyX_Pad_GetLastP1Bits();
            for (unsigned t = 0; t < ticks; t++) {
                bk2_record_append(bits);
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
        if (openpete_present_pending) {
            static bool s_shot_fired[OPENPETE_MAX_SCREENSHOT_FRAMES] = {0};
            for (int si = 0; si < opts->screenshot_frame_count; ++si) {
                if (s_shot_fired[si]) continue;
                if ((int)openpete_frame < opts->screenshot_frames[si]) continue;
                s_shot_fired[si] = true;
                char path[1024];
                openpete_screenshot_format_path(opts->screenshot_output,
                                                opts->screenshot_frames[si],
                                                path, sizeof(path));
                openpete_psyx_take_screenshot(path);
                OPL_INFO_F("screenshot",
                           "wrote %s on frame %u (target=%d)",
                           path, openpete_frame, opts->screenshot_frames[si]);
            }
            PsyX_EndScene();
            openpete_present_pending = 0;
            did_present_this_iter = true;

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
            if (frame_time_log.fp) {
                int64_t now_ns      = op_now();
                int64_t deadline_ns = frame_time_log_start_ns +
                                      (int64_t)frame_time_log.records_written *
                                      frame_time_log_target_period_ns;
                openpete_frame_time_log_record(&frame_time_log,
                                               openpete_frame,
                                               0u,
                                               now_ns,
                                               deadline_ns,
                                               deadline_ns - now_ns);
            }
        }

        /* Renderer-foundation Phase 2.1 — intermediate render between
         * sim ticks. When render-fps > sim and we have a saved OT from
         * the last DrawOTag, re-walk it through PsyCross with a fresh
         * substitute alpha. Each intermediate iter presents a smoothed
         * intermediate frame; alpha advances across the sim period via
         * openpete_render_compute_alpha(op_now()).
         *
         * Conditions:
         *  - Not headless / unlock mode (no display target).
         *  - render-fps > 0 and exceeds sim cadence.
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
         *  - At least one DrawOTag has fired (saved OT available).
         *  - Not paused without step pending. */
        extern uint8_t** openpete_last_ot_slot_host_ptr;
        extern int       openpete_last_ot_slot_count;
        extern void      openpete_re_emit_last_ot(void);
        /* Headless gate is lifted when --vertex-trace-log is on so the
         * harness can characterize the substitute pipeline without a
         * display surface. BeginScene/EndScene are no-ops in headless,
         * but prepare_substitute still runs and writes trace records. */
        extern int openpete_vtrace_enabled(void);
        extern int g_openpete_interp_enabled;
        const bool headless_block = opts->headless && !openpete_vtrace_enabled();
        if (!headless_block && !openpete_unlock_fps &&
            g_openpete_interp_enabled &&   /* 'I' off → no intermediates → ~29.97 fps */
            opts->render_fps > 0 &&
            !did_present_this_iter &&
            openpete_last_ot_slot_host_ptr &&
            openpete_last_ot_slot_count > 0 &&
            !(g_paused && !g_step_one)) {
            if (PsyX_BeginScene()) {
                /* nothing else needed — re_emit does ClearSplits */
            }
            openpete_re_emit_last_ot();
            PsyX_EndScene();
            /* No cache rotate here — sim_tick_advanced fires only at
             * sim-due iters. Intermediate renders reuse the cache state
             * from the most recent sim tick. */
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
                    int32_t r = psxtrace->first_divergent_region;
                    const char* rname = (r >= 0 &&
                        r < (int32_t)psxtrace->region_count)
                        ? psxtrace->region_names[r] : "<total>";
                    OPL_ERROR_F("psxtrace",
                                "DIVERGED at frame=%u region=%d (%s) "
                                "op=0x%016llx oracle=0x%016llx (record %u)",
                                psxtrace->first_divergent_frame,
                                (int)r, rname,
                                (unsigned long long)psxtrace->first_divergent_op,
                                (unsigned long long)psxtrace->first_divergent_oracle,
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

        /* Sample Tab for the host loop's own pacing + GL swap-interval
         * bypass. vbl is yield-driven so this only affects wall-clock
         * speed, not game state — fast-forward is invisible to BK2
         * replay determinism. Scheduled --unlock-frames windows (test
         * harness) OR into the same flag. */
        const bool* kbd = openpete_keyboard_state();
        /* OPENPETE_FORCE_PACING=1 (read once, then cached) keeps the
         * legacy / dual-pacer cadence active even under --headless. Used
         * by scripts/measure-pacing.py to capture before/after numbers
         * for the frame-pacing plan without needing a real display. */
        static int s_force_pacing_init = 0;
        static int s_force_pacing = 0;
        if (!s_force_pacing_init) {
            const char* v = getenv("OPENPETE_FORCE_PACING");
            s_force_pacing = (v && *v && *v != '0') ? 1 : 0;
            s_force_pacing_init = 1;
            OPL_INFO_F("pacer", "OPENPETE_FORCE_PACING=%d (env=%s)",
                       s_force_pacing, v ? v : "<unset>");
        }
        bool tab_unlock = (opts->headless && !s_force_pacing) ||
                          (kbd && kbd[SDL_SCANCODE_TAB]);
        bool sched_unlock = opts->unlock_ranges_count > 0 &&
                            unlock_frame_in_ranges(opts, openpete_frame);
        bool unlock_fps = tab_unlock || sched_unlock;
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
            next_sim_ns    = anchor;
            next_render_ns = anchor;
        } else if (opts->render_fps == 0) {
            /* Legacy "match sim cadence" pacer. One present per VBL
             * cadence, advanced only when the game fiber actually
             * yielded with a frame bump (ticked_this_iter). dispatch.c
             * no-tick stub yields skip the pace step so a frame full
             * of stubs doesn't park us for 16.67 ms per stub. */
            if (ticked_this_iter) {
                next_sim_ns += sim_period_ns;
                op_time_t now2 = op_now();
                if (now2 - next_sim_ns > slip_threshold_ns) {
                    op_time_t skip = (now2 - next_sim_ns) / sim_period_ns;
                    next_sim_ns += skip * sim_period_ns;
                }
                op_sleep_until(next_sim_ns);
            }
        } else {
            /* Wall-clock dual-pacer. sim deadline advances on sim-due
             * iters; render deadline advances every outer iter; we
             * sleep to the sooner of the two so an under-paced render
             * doesn't starve the sim or vice-versa. */
            op_time_t now2 = op_now();
            if (sim_due) {
                next_sim_ns += sim_period_ns;
                if (now2 - next_sim_ns > slip_threshold_ns) {
                    op_time_t skip = (now2 - next_sim_ns) / sim_period_ns;
                    next_sim_ns += skip * sim_period_ns;
                }
            }
            next_render_ns += render_period_ns;
            if (now2 - next_render_ns > slip_threshold_ns) {
                op_time_t skip = (now2 - next_render_ns) / render_period_ns;
                next_render_ns += skip * render_period_ns;
            }
            op_time_t next = (next_sim_ns < next_render_ns)
                             ? next_sim_ns : next_render_ns;
            op_sleep_until(next);
        }
    }

    if (bk2_active) {
        bk2_movie_free(&bk2);
        g_PsyX_Pad_InputOverride = 0xFFFF;
    }
    if (bk2_recording) {
        bk2_record_finalize();
    }
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
        if (psxtrace->diverged && exit_code == OPENPETE_EXIT_OK) {
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

    openpete_ticker_stop();
    openpete_frame_time_log_close(&frame_time_log);
    {
        extern void openpete_vtrace_close(void);
        openpete_vtrace_close();
    }
    if (openpete_interp_enabled()) {
        OPL_INFO_F("interp",
                   "captured=%llu, dedupe-skipped=%llu",
                   (unsigned long long)openpete_interp_total_captures(),
                   (unsigned long long)openpete_interp_total_dedupe_skips());
    }
    openpete_interp_shutdown();

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
