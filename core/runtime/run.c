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
#include "openpete_discontinuity.h"
#include "openpete_recomp_module.h"
#include "openpete_ticker.h"
#include "openpete_time.h"
#include "openpete_frame_time_log.h"
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
#include "openpete_freecam.h"
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
        const char* e = getenv("OPENPETE_PRESENT_TARGET_CLOCK");
        s_on = !(e && *e == '0');
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
 * ~29.97 fps only. (Moved here from the deleted screen-space render
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

static bool openpete_sdl_event_filter(void* userdata, SDL_Event* ev) {
    (void)userdata;
    /* Dev overlay sees every event (imgui io bookkeeping; frames only run
     * while it is open). The filter runs during the main-thread event pump —
     * same thread as the host loop — so this is race-free. */
    openpete_devui_process_event(ev);
    if (ev->type == SDL_EVENT_QUIT) {
        g_shutdown_requested = 1;
        return false;  /* drop so PsyX_Sys_DoPollEvent never sees it */
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
    /* Escape quits. (We used to gate this on !SDL_HasGamepad() so a
     * Steam-Input phantom keystroke couldn't kill the game, but that also
     * broke Escape-to-quit for everyone playing on a normal controller — see
     * openpete_keyboard_state below for the trade-off we re-accepted.) The
     * window-close path above quits too. */
    if (ev->type == SDL_EVENT_KEY_DOWN &&
        ev->key.scancode == SDL_SCANCODE_ESCAPE) {
        /* While an overlay is open, Escape closes it instead of quitting.
         * (The filter runs during the main-thread event pump, same thread as the
         * host loop, so reading the open flags here is race-free.) */
        if (openpete_devui_is_open()) { openpete_devui_close(); return false; }
        if (openpete_menu_is_open()) { openpete_menu_toggle(); return false; }
        g_shutdown_requested = 1;
        return false;
    }
    return true;  /* keep everything else in queue for PsyCross */
}

/* Host hotkeys (Tab fast-forward, 1/2/3 savestates, F7-F10, I) read the
 * keyboard directly. This is a pass-through: the real keyboard always drives
 * the hotkeys, including while a gamepad is connected.
 *
 * We previously zeroed the keyboard (allowlisting only Tab) whenever a gamepad
 * was present, so a Steam-Input desktop config couldn't fire a hotkey via a
 * phantom keystroke (SDL surfaces those with which==0, so the *polled*
 * SDL_GetKeyboardState array can't tell them from a real press). But that
 * penalised every controller user — testers reported keyboard hotkeys dead
 * whenever any pad was plugged in — to defend an esoteric, Steam-only
 * misconfiguration. Re-accepted trade-off: a Steam Input config that maps a
 * button to a keyboard key can trip a hotkey; the cure is Steam-side (remap
 * the button / disable Steam Input), as run.c's earlier revert (036381d) noted.
 * Note the PSX *gameplay* pad is unaffected either way — PsyCross already gates
 * keyboard-as-pad to gamepad-free slots, so phantom keys can't reach gameplay.
 *
 * Kept as the seam for the correct fix if this ever bites a real user: build
 * keyboard state from the SDL_EVENT_KEY_* stream and drop events whose `which`
 * is not a physical keyboard, instead of polling the merged array. Returns a
 * buffer indexed like SDL_GetKeyboardState's; callers already null-check. */
static const bool* openpete_keyboard_state(void) {
    return SDL_GetKeyboardState(NULL);
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
static int       s_render_fps       = 0;
static op_time_t s_render_period_ns = 0;
static int       s_render_fps_dirty = 0;

void openpete_set_render_fps(int fps) {
    if (fps < 0) fps = 0;
    if (fps == s_render_fps) return;
    s_render_fps = fps;
    s_render_fps_dirty = 1;   /* the host loop recomputes the period + re-anchors */
}

int openpete_get_render_fps(void) {
    return s_render_fps;
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
    (void)openpete_native_render_init(opts, win_w, win_h);
    openpete_native_set_fps_overlay(opts->fps_overlay);  /* vk FPS overlay (O-menu SHOW FPS row toggles live) */
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
     * gain. Bypasses the game's volume sliders entirely. The music WAV
     * override mirrors the XA forced gain so --mute covers it too. */
    extern int  PsyX_SPUAL_SetMute(int on_off);
    extern void PsyX_XA_ForceGain(float linear_gain);
    PsyX_SPUAL_SetMute(opts->mute ? 1 : 0);
    PsyX_XA_ForceGain(opts->mute ? 0.0f : -1.0f);
    music_override_init();  /* logs the assets/music/ search path */
    music_override_set_forced_gain(opts->mute ? 0.0f : -1.0f);

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

    /* Forward CLI overrides to the per-game impl. game_on_frame applies
     * them when the relevant in-game globals are ready. */
    game_set_skip_to_level(opts->skip_to_level);

    /* --skip-to-level: fast-forward the logos / title / loading / flyby with a
     * black window (skip presents + pacer) until the target level is playable.
     * Windowed-only — headless is already unlocked and has nothing to show; the
     * game side clears it at GS_Playing (see openpete_skip_present_until_level). */
    openpete_skip_present_until_level =
        (opts->skip_to_level > 0 && !opts->headless) ? 1 : 0;
    if (openpete_skip_present_until_level)
        OPL_INFO_F("skip-to-level",
                   "fast-forwarding presents until the level loads");

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

    /* Monitor refresh rate (primary display — the window spawns there; 60
     * when headless / query failure). Feeds the --render-fps default below
     * and the settings menu's FPS ladder before the loop. */
    int refresh_hz = 60;
    if (!opts->headless) {
        const SDL_DisplayMode* dm =
            SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        if (dm && dm->refresh_rate > 0.0f)
            refresh_hz = (int)(dm->refresh_rate + 0.5f);
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
    /* --tick-scale F slows/speeds the WHOLE engine by dividing both the sim and
     * the render period by F: sim_rate = F*29.97 Hz, present_rate = F*render_fps,
     * so the count of sub-tick presents PER tick (present_rate/sim_rate) is
     * invariant — a slow-motion view of the SAME interpolation. Pacing only: the
     * sim still executes identical ticks (the interp alpha is wall-clock-derived
     * from the actual tick timestamps, so it auto-stretches), and the deadlines
     * are reset to now under unlock/headless, so this is inert in the oracle. */
    const double tick_scale = (opts->tick_scale > 0.0) ? opts->tick_scale : 1.0;
    const op_time_t sim_period_ns    =
        (op_time_t)((double)((OP_NS_PER_SEC * 1001) / 60000) / tick_scale);
    const op_time_t render_period_ns = (render_fps > 0)
        ? (op_time_t)((double)(OP_NS_PER_SEC / (op_time_t)render_fps) / tick_scale)
        : sim_period_ns;   /* sim_period_ns is already tick-scaled */
    /* Seed the live pacer state (openpete_set_render_fps mutates it; the loop
     * reads s_render_fps / s_render_period_ns instead of the consts above). */
    s_render_fps       = render_fps;
    s_render_period_ns = render_period_ns;
    s_render_fps_dirty = 0;
    /* One WALL-time tick for every sub-tick alpha denominator (ring +
     * per-object phase lerps). Must stretch with --tick-scale: interp alphas
     * are wall-clock-derived, so an unscaled (NTSC) cap makes every sweep run
     * 1/tick_scale× too fast and pin at the clamp for the rest of the tick. */
    g_openpete_tick_period_ns = 2 * sim_period_ns;   /* tick = 2 VBLs, scaled */
    const op_time_t slip_threshold_ns = 4 * sim_period_ns; /* ~67 ms (scaled) */
    op_time_t start_ns      = op_now();
    op_time_t next_sim_ns    = start_ns;
    op_time_t next_render_ns = start_ns;
    if (render_fps > 0) {
        OPL_INFO_F("pacer", "wall-clock dual-pacer: sim=%.4f ms render=%.4f ms (render-fps=%d, --tick-scale=%.3f)",
                   (double)sim_period_ns / 1e6,
                   (double)render_period_ns / 1e6,
                   render_fps, tick_scale);
    } else {
        OPL_INFO_F("pacer", "legacy pacer: sim=%.4f ms NTSC (--render-fps=0, --tick-scale=%.3f)",
                   (double)sim_period_ns / 1e6, tick_scale);
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
        if (render_fps > 0 && render_hz_x1000 > sim_hz_x1000) {
            openpete_interp_set_enabled(1);
            OPL_INFO_F("interp",
                       "capture enabled (render-fps=%d > sim ~30 Hz)",
                       render_fps);
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

    /* --runahead: arm only when every enabled mod declares a runahead
     * capability (OP_MOD_RA_PURE / OP_MOD_RA_HOST_FX) — speculative
     * re-execution of tick hooks with undeclared host side effects would
     * double-fire them. */
    int runahead_n = opts->runahead;
    openpete_state_mem_t* ra_snap = NULL;
    if (runahead_n > 0) {
        const char* ra_offender = NULL;
        if (!openpete_mods_runahead_capable(&ra_offender)) {
            OPL_ERROR_F("runahead",
                        "disabled: mod \"%s\" declares no runahead "
                        "capability (add OPENPETE_MOD_STATE flags or "
                        "OPENPETE_MOD_RA_CAPS)",
                        ra_offender ? ra_offender : "?");
            runahead_n = 0;
        } else {
            ra_snap = openpete_state_mem_new();
            if (!ra_snap) runahead_n = 0;
            else OPL_INFO_F("runahead", "armed: leading presentation by "
                            "%d tick(s)", runahead_n);
            if (ra_snap && ra_pc_on()) {
                (void)openpete_on_guest_discontinuity(ra_pc_invalidate_all);
                OPL_INFO_F("runahead", "prediction-exactness check armed");
            }
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
     * high-res pacer is driving the loop. The target period is the
     * sim cadence today (30 fps = 33.333 ms), upgraded to 29.97 once
     * Phase 1 lands the NTSC-correct ticker. See
     * notes/plans/openpete-frame-pacing.md. */
    if (opts->frame_time_log_path && *opts->frame_time_log_path) {
        /* Default target: NTSC present cadence = 1001/30000 sec per
         * present (= 1/2 of 59.94 Hz VBL). When the render-fps target is
         * set (explicit --render-fps N, or the unset windowed default =
         * display refresh) the dual-pacer drives the loop at N Hz instead,
         * so use that. See plan §1: "ship --vbl-rate=59.94 as default". */
        if (render_fps > 0) {
            frame_time_log_target_period_ns =
                (int64_t)((double)OP_NS_PER_SEC / (double)render_fps / tick_scale);
        } else {
            frame_time_log_target_period_ns =
                (int64_t)((double)((OP_NS_PER_SEC * (int64_t)1001) / (int64_t)30000) / tick_scale);
        }
        if (openpete_frame_time_log_open(&frame_time_log,
                                         opts->frame_time_log_path,
                                         frame_time_log_target_period_ns) != 0) {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        frame_time_log_start_ns = op_now();
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
    bool prev_i_key = false;
    bool prev_l_key = false;
    bool prev_alt_enter = false;

    /* Debug free camera (F / left-stick click). Edge state for the toggles and
     * the [ / ] / L2 / R2 speed steps; the WASD/arrow/Space/Shift + analog
     * stick movement is level-held (latched each iteration while engaged). */
    bool prev_f_key = false;
    bool prev_freecam_pad_toggle = false;
    bool prev_t_key = false;
    bool prev_freecam_lb = false, prev_freecam_rb = false;
    bool prev_freecam_l2 = false, prev_freecam_r2 = false;

    /* In-game settings menu (O). Edge state for the toggle + arrow-key nav, and
     * a latch so we clear the pad override exactly once when the menu closes. */
    bool prev_o_key   = false;
    bool prev_menu_up = false, prev_menu_dn = false;
    bool prev_menu_lf = false, prev_menu_rt = false;
    bool prev_menu_close = false;   /* controller Start/Circle → close menu */
    bool prev_menu_open = false;

    /* Dev config overlay (M). Edge state for the toggle + a latch to clear the
     * pad override exactly once when imgui releases the keyboard. */
    bool prev_m_key = false;
    bool prev_devui_kb = false;

    /* Seed the in-game settings menu from the launch flags, and build its FPS
     * ladder from the monitor refresh queried above (60 if unknown/headless).
     * Applies nothing — it only makes the menu reflect what the game launched
     * with (the FPS row reads the live pacer target, which already holds the
     * resolved render-fps default). */
    openpete_menu_init(opts, refresh_hz);

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
                          openpete_skip_present_until_level ||
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
        /* Live render-fps change (openpete_set_render_fps, e.g. the in-game
         * menu): recompute the period with THIS loop's tick_scale and re-anchor
         * the deadlines to now so a lower→higher switch doesn't fire a burst of
         * catch-up presents. Also keep interp capture in sync (enabled only when
         * the render rate exceeds the sim rate), matching the startup gate. */
        if (s_render_fps_dirty) {
            s_render_fps_dirty = 0;
            s_render_period_ns = (s_render_fps > 0)
                ? (op_time_t)((double)(OP_NS_PER_SEC / (op_time_t)s_render_fps) / tick_scale)
                : sim_period_ns;
            op_time_t anchor = op_now();
            next_sim_ns    = anchor;
            next_render_ns = anchor;
            const op_time_t sim_hz_x1000  = (OP_NS_PER_SEC * 1000) / sim_period_ns;
            const op_time_t rend_hz_x1000 = (s_render_period_ns > 0)
                ? (OP_NS_PER_SEC * 1000) / s_render_period_ns : 0;
            openpete_interp_set_enabled(s_render_fps > 0 && rend_hz_x1000 > sim_hz_x1000 ? 1 : 0);
            OPL_INFO_F("pacer", "render-fps -> %d (%.4f ms/present)",
                       s_render_fps, (double)s_render_period_ns / 1e6);
        }
        op_time_t iter_now_ns = op_now();
        bool   sim_due = (s_render_fps == 0) || unlock_now ||
                         (iter_now_ns >= next_sim_ns);

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
        }

        /* Settings menu open: neutralise the game pad so the arrow keys drive
         * the menu (not Spyro). Reuses the bk2 override channel (all buttons
         * released, sticks centred). On the frame the menu closes, clear it —
         * unless a bk2 movie is driving the override, in which case bk2 re-stamps
         * on the next sim-due iter. The sim keeps running (input just goes
         * nowhere), so aspect/ssaa/draw-distance changes preview live. */
        if (openpete_menu_is_open()) {
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

        /* While imgui owns the keyboard, every polled host hotkey goes inert
         * in one stroke — they all null-check kbd_pre. Esc-close and the M
         * toggle-off still work: Esc via the event filter, M again once the
         * text field loses focus (or via the panel's close button). */
        if (openpete_devui_wants_keyboard()) kbd_pre = NULL;

        /* M: toggle the dev config overlay. Mutually exclusive with the O
         * menu — two input-owning overlays with different capture models is
         * complexity with no payoff. */
        {
            bool m_k = kbd_pre && kbd_pre[SDL_SCANCODE_M];
            if (m_k && !prev_m_key) {
                if (openpete_devui_toggle() && openpete_menu_is_open())
                    openpete_menu_toggle();   /* close the O menu underneath */
            }
            prev_m_key = m_k;
        }

        /* O: toggle the in-game settings menu. While open it captures the arrow
         * keys (Up/Down move the cursor, Left/Right change the value) and the
         * game pad is neutralised below, so the other hotkeys and gameplay input
         * stay inert. Esc-closes is handled in the SDL event filter. */
        {
            bool o_k = kbd_pre && kbd_pre[SDL_SCANCODE_O];
            if (o_k && !prev_o_key) {
                openpete_menu_toggle();
                if (openpete_menu_is_open()) openpete_devui_close();
            }
            prev_o_key = o_k;

            if (openpete_menu_is_open()) {
                bool up_k = kbd_pre && kbd_pre[SDL_SCANCODE_UP];
                bool dn_k = kbd_pre && kbd_pre[SDL_SCANCODE_DOWN];
                bool lf_k = kbd_pre && kbd_pre[SDL_SCANCODE_LEFT];
                bool rt_k = kbd_pre && kbd_pre[SDL_SCANCODE_RIGHT];
                /* Controller (P1): digital d-pad AND the left analog stick drive
                 * the same nav; X (cross) also acts as Right. Read raw from SDL —
                 * the game's pad view is held neutral while the menu is open, so
                 * this is independent of it. OR-ing into the keyboard bools means
                 * one shared edge-trigger (prev_menu_*): holding a direction (key
                 * or stick) steps once until released, matching the keyboard feel. */
                bool close_k = false;   /* Start / Circle held (closes on release; open stays on O) */
                PsyX_RawPad rp;
                if (PsyX_Pad_GetRawP1(&rp)) {
                    const int DZ = 16000;   /* ~half-throw left-stick deadzone */
                    if (rp.dpad_up    || rp.lstick_y < -DZ)             up_k = true;
                    if (rp.dpad_down  || rp.lstick_y >  DZ)             dn_k = true;
                    if (rp.dpad_left  || rp.lstick_x < -DZ)             lf_k = true;
                    if (rp.dpad_right || rp.lstick_x >  DZ || rp.cross) rt_k = true;
                    close_k = rp.start || rp.circle;
                }
                if (!close_k && prev_menu_close) {
                    /* Close on the RELEASE edge of Start/Circle. The game pad is held
                     * neutral only while the menu is open, so closing on the press
                     * would let the still-held button bleed into the game next frame
                     * (Start → the game's pause menu); by release it's already up.
                     * Temporary binding — a fuller open/close scheme lands later. */
                    openpete_menu_toggle();   /* closes it */
                } else {
                    if (up_k && !prev_menu_up) openpete_menu_nav(-1);
                    if (dn_k && !prev_menu_dn) openpete_menu_nav(+1);
                    if (lf_k && !prev_menu_lf) openpete_menu_adjust(-1);
                    if (rt_k && !prev_menu_rt) openpete_menu_adjust(+1);
                }
                prev_menu_up = up_k; prev_menu_dn = dn_k;
                prev_menu_lf = lf_k; prev_menu_rt = rt_k;
                prev_menu_close = close_k;

                /* Freecam axes are level-held and consumed every game tick,
                 * but the latch below is skipped while the menu owns the
                 * keyboard — hold neutral here so a movement key held at
                 * menu-open doesn't keep flying the camera behind the panel. */
                if (openpete_freecam_active()) {
                    openpete_freecam_axes ax = {0};
                    openpete_freecam_set_axes(&ax);
                }
            } else {
                prev_menu_up = prev_menu_dn = prev_menu_lf = prev_menu_rt = false;
                prev_menu_close = false;
            }
        }

        bool rewound_this_iter = false;   /* rewind driver stepped: skip
                                           * this iteration's forward tick */
        /* Save-state hotkeys: 1 quick-save, 2 quick-load, 3 cycle slot.
         * Edge-triggered (one action per keypress). The game fiber is
         * yielded right now, so g_game.cpu and g_ram are quiescent — we can
         * snapshot or restore without racing the recompiled code.
         *
         * Run BEFORE PsyX_UpdateInput so a load that overwrites the pad
         * buffer (which lives in restored g_ram) is immediately followed by
         * a fresh write of current input — otherwise the game fiber reads
         * stale buttons from the saved RAM and feels like input has stopped
         * registering until the next iter.
         *
         * Skipped while the settings menu is open (it owns the keyboard): the
         * prev_* edge state freezes at its pre-menu value, so no held key
         * misfires when the menu closes. */
        if (!openpete_menu_is_open()) {
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
                if (!hinted && kbd_pre && kbd_pre[SDL_SCANCODE_BACKSPACE]) {
                    hinted = true;
                    OPL_WARN_F("rewind", "Backspace pressed but rewind is "
                               "disabled — launch with --rewind 30 to "
                               "enable a 30 s window");
                }
            }
            if (openpete_rewind_enabled()) {
                int rw_live = game_is_gameplay_live(&g_game.cpu);
                openpete_rewind_on_iter(&g_game.cpu, openpete_frame, rw_live);
                bool rw_k = kbd_pre && kbd_pre[SDL_SCANCODE_BACKSPACE];
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
                            co_switch((cothread_t)openpete_game_fiber);
                            if (openpete_frame == before) break; /* safety */
                        }
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

            /* Frame-step hotkeys. F7 edge-triggered toggle; F8 sets
             * g_step_one whenever held while paused, so the natural
             * key-repeat behavior is "step every host iter while held"
             * without needing an OS-level repeat-rate dependency. */
            bool pause_k = kbd_pre && kbd_pre[SDL_SCANCODE_F7];
            bool step_k  = kbd_pre && kbd_pre[SDL_SCANCODE_F8];
            if (pause_k && !prev_pause_key) {
                g_paused = !g_paused;
                /* Enter: mute. Exit: restore the user's --mute baseline
                 * (false by default = audio on; --mute 1 keeps it silent). */
                PsyX_SPUAL_SetMute(g_paused ? 1 : (opts->mute ? 1 : 0));
                OPL_INFO_F("frame-step", "%s at openpete_frame=%u",
                           g_paused ? "paused" : "resumed", openpete_frame);
            }
            if (g_paused && step_k) {
                g_step_one = true;
            }
            prev_pause_key = pause_k;

            /* I: live on/off A/B for interpolation itself. Off =
             * intermediate renders present the lagged frame unchanged
             * (clean "is frame-gen doing anything perceptible?" test). */
            bool i_k = kbd_pre && kbd_pre[SDL_SCANCODE_I];
            if (i_k && !prev_i_key) {
                g_openpete_interp_enabled = !g_openpete_interp_enabled;
                OPL_INFO_F("render", "interpolation -> %s",
                           g_openpete_interp_enabled ? "ON" : "OFF");
            }
            prev_i_key = i_k;

            /* L: live toggle sector LOD original <-> extended (the O-menu LOD
             * row / --lod flag). Mirrors I. Goes through openpete_no_lod_set so
             * openpete_fog_scale follows the mode — a raw openpete_no_lod poke
             * would leave the terrain fog latched. The setter logs the new
             * state. Native-renderer display policy; inert under PsyCross. The
             * menu re-syncs its LOD row from openpete_no_lod when next opened. */
            bool l_k = kbd_pre && kbd_pre[SDL_SCANCODE_L];
            if (l_k && !prev_l_key) {
                openpete_no_lod_set(!openpete_no_lod, -1.0f);
            }
            prev_l_key = l_k;

            /* Debug free camera. Toggle with F or a left-stick click. While
             * engaged: WASD / left stick move, arrows / right stick look,
             * Space+LShift / R1+L1 raise+lower, [ ] / L2 R2 step the speed,
             * T teleports Spyro here and hands the camera back. We
             * own only the toggle + input latch here; the camera write and the
             * CameraFollowSpyro / UpdateSpyro freeze are game-side
             * (games/spyro-1/features/freecam_native.c). Latched as level-held
             * axes so movement continues while held, consumed once per game
             * tick in game_on_frame_pre. Keyboard and pad sum (then clamp), so
             * either or both drive it. */
            PsyX_RawPad fc_pad;
            bool fc_have_pad = PsyX_Pad_GetRawP1(&fc_pad);

            bool f_k = kbd_pre && kbd_pre[SDL_SCANCODE_F];
            bool pad_tog = fc_have_pad && fc_pad.l3;
            if (f_k && !prev_f_key) openpete_freecam_toggle();
            if (pad_tog && !prev_freecam_pad_toggle) openpete_freecam_toggle();
            prev_f_key = f_k;
            prev_freecam_pad_toggle = pad_tog;

            /* T: drop Spyro at the camera and hand the camera back to the game.
             * Latched as a one-shot request — the move needs the game fiber, so
             * freecam_native applies it (and disengages) on the next tick; the
             * request is dropped if the camera isn't engaged. Edge-tracked out
             * here rather than under the engaged branch so a T already held
             * when the camera engages doesn't count as a press. */
            bool t_k = kbd_pre && kbd_pre[SDL_SCANCODE_T];
            if (t_k && !prev_t_key) openpete_freecam_request_teleport();
            prev_t_key = t_k;

            if (openpete_freecam_active()) {
                /* Normalise a signed SDL stick axis to [-1, 1] past a small
                 * deadzone. */
                #define FC_STICK_DZ  8000.0f
                #define FC_NORM(v)   ( (v) >  FC_STICK_DZ || (v) < -FC_STICK_DZ \
                                        ? (float)(v) / 32767.0f : 0.0f )
                float k_fwd = 0, k_str = 0, k_up = 0, k_yaw = 0, k_pit = 0;
                if (kbd_pre) {
                    k_fwd = (float)((int)kbd_pre[SDL_SCANCODE_W]     - (int)kbd_pre[SDL_SCANCODE_S]);
                    k_str = (float)((int)kbd_pre[SDL_SCANCODE_D]     - (int)kbd_pre[SDL_SCANCODE_A]);
                    k_up  = (float)((int)kbd_pre[SDL_SCANCODE_SPACE] - (int)kbd_pre[SDL_SCANCODE_LSHIFT]);
                    k_yaw = (float)((int)kbd_pre[SDL_SCANCODE_RIGHT] - (int)kbd_pre[SDL_SCANCODE_LEFT]);
                    k_pit = (float)((int)kbd_pre[SDL_SCANCODE_UP]    - (int)kbd_pre[SDL_SCANCODE_DOWN]);
                }
                float p_fwd = 0, p_str = 0, p_up = 0, p_yaw = 0, p_pit = 0;
                if (fc_have_pad) {
                    p_fwd = -FC_NORM(fc_pad.lstick_y);   /* stick up = forward */
                    p_str =  FC_NORM(fc_pad.lstick_x);
                    p_yaw =  FC_NORM(fc_pad.rstick_x);   /* stick right = turn right */
                    p_pit = -FC_NORM(fc_pad.rstick_y);   /* stick up = look up */
                    p_up  = (float)(fc_pad.r1 - fc_pad.l1);
                }
                openpete_freecam_axes ax;
                #define FC_CLAMP(x)  ( (x) > 1.0f ? 1.0f : ((x) < -1.0f ? -1.0f : (x)) )
                ax.move_fwd    = FC_CLAMP(k_fwd + p_fwd);
                ax.move_strafe = FC_CLAMP(k_str + p_str);
                ax.move_up     = FC_CLAMP(k_up  + p_up);
                ax.look_yaw    = FC_CLAMP(k_yaw + p_yaw);
                ax.look_pitch  = FC_CLAMP(k_pit + p_pit);
                #undef FC_CLAMP
                #undef FC_NORM
                #undef FC_STICK_DZ
                openpete_freecam_set_axes(&ax);

                /* Speed step: [ / ] keys or L2 / R2 (edge-triggered). */
                bool lb = kbd_pre && kbd_pre[SDL_SCANCODE_LEFTBRACKET];
                bool rb = kbd_pre && kbd_pre[SDL_SCANCODE_RIGHTBRACKET];
                bool l2 = fc_have_pad && fc_pad.l2;
                bool r2 = fc_have_pad && fc_pad.r2;
                if ((lb && !prev_freecam_lb) || (l2 && !prev_freecam_l2))
                    openpete_freecam_adjust_speed(-1);
                if ((rb && !prev_freecam_rb) || (r2 && !prev_freecam_r2))
                    openpete_freecam_adjust_speed(+1);
                prev_freecam_lb = lb; prev_freecam_rb = rb;
                prev_freecam_l2 = l2; prev_freecam_r2 = r2;
            } else {
                prev_freecam_lb = prev_freecam_rb = false;
                prev_freecam_l2 = prev_freecam_r2 = false;
            }

            /* Alt+Enter (either Alt) or a window double-click toggles the
             * native window between windowed and borderless fullscreen. Moved
             * here from PsyCross, which only ever drove its now-hidden GL
             * window; openpete_native_toggle_fullscreen is a no-op in headless
             * / PsyCross-only runs. The PsyCross g_altKeyState gate still stops
             * the Enter from registering as Start while Alt is held. */
            bool alt_enter = kbd_pre && kbd_pre[SDL_SCANCODE_RETURN] &&
                             (SDL_GetModState() & SDL_KMOD_ALT) != 0;
            if ((alt_enter && !prev_alt_enter) || g_native_fs_toggle_request) {
                g_native_fs_toggle_request = 0;
                openpete_native_toggle_fullscreen();
            }
            prev_alt_enter = alt_enter;
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
        bool yield_now = sim_due && (!g_paused || g_step_one)
                                 && !rewound_this_iter;
        if (yield_now) {
            g_step_one = false;
            game_on_frame_pre(&g_game.cpu, openpete_frame);
            {
                uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
                co_switch((cothread_t)openpete_game_fiber);
                if (pt0) openpete_phase_add(OPPT_SIM_TICK, openpete_phase_now_us() - pt0);
            }
            ticked_this_iter = (openpete_frame != frame_before_yield);

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
                    for (int v = 0; v < 2 * runahead_n; v++) {
                        unsigned bf = openpete_frame;
                        game_on_frame_pre(&g_game.cpu, openpete_frame);
                        co_switch((cothread_t)openpete_game_fiber);
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
                            op_now(), (op_time_t)((OP_NS_PER_SEC * 1001) / 30000), &a, &pv, &cv);
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
                if (lag_re && !opts->headless &&
                    openpete_native_render_enabled() && !openpete_unlock_fps &&
                    g_openpete_interp_enabled && openpete_interp_enabled() &&
                    !(g_paused && !g_step_one)) {
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
                    lag_re(&g_game.cpu);
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
            openpete_mods_present_flush(&g_game.cpu, 1);

            /* The PTS arg tags this present for --record: the frame is queued
             * into the async readback ring inside (no GPU stall), so a vk
             * recording captures the on-screen frame at real speed. No-op unless
             * native is the display AND recording is active. */
            openpete_native_render_frame((uint64_t)(op_now() / 1000));

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
                    openpete_native_render_screenshot_all(base);
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
            ftl_record_present(&frame_time_log, frame_time_log_start_ns,
                               frame_time_log_target_period_ns, openpete_frame);

            /* Perf trace: close out this canonical present. Every CPU span
             * (SIM_TICK, the lag re-extract, native prepass/compose/gpu-sync)
             * and census emitted since the previous present binds here. */
            if (op_prof_on())
                op_prof_present((uint32_t)openpete_canon_tick, OP_PROF_CANONICAL);
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
         *    VBL 59.94, kept in sync at startup + on every live render-fps
         *    change), NOT the weaker s_render_fps > 0. At/below sim parity —
         *    e.g. render-fps 30 vs the ~29.97 Hz sim — sub-tick interpolation
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
        const bool native_intermediate =
            !headless_block && !openpete_unlock_fps &&
            g_openpete_interp_enabled &&
            openpete_interp_enabled() &&
            !did_present_this_iter &&
            openpete_native_render_enabled() &&
            native_re != NULL &&
            !(g_paused && !g_step_one);
        if (native_intermediate) {
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
                native_re(&g_game.cpu);       /* rebuild s_scene @ interpolated gamestate */
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
        bool unlock_fps = tab_unlock || sched_unlock ||
                          openpete_skip_present_until_level;
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
        } else if (s_render_fps == 0) {
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
            next_render_ns += s_render_period_ns;
            if (now2 - next_render_ns > slip_threshold_ns) {
                op_time_t skip = (now2 - next_render_ns) / s_render_period_ns;
                next_render_ns += skip * s_render_period_ns;
            }
            op_time_t next = (next_sim_ns < next_render_ns)
                             ? next_sim_ns : next_render_ns;
            op_sleep_until(next);
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

    openpete_native_render_shutdown();
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
