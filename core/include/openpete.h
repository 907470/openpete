// core/include/openpete.h — runtime entry point + program options.
//
// Each game's `main.cpp` is a tiny wrapper that fills in `openpete_options_t`
// and calls `openpete_run()`. All command-line parsing, window setup,
// screenshot logic, and the run loop live in core/.

#ifndef OPENPETE_H
#define OPENPETE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exit codes — see notes/plans/openpete-pc-port.md Phase 0b. */
#define OPENPETE_EXIT_OK           0  /* clean termination */
#define OPENPETE_EXIT_PANIC        1  /* panic / crash / timeout */
#define OPENPETE_EXIT_DIVERGENCE   2  /* state-divergence detected (Phase 1c+) */
#define OPENPETE_EXIT_SETUP_ERROR  3  /* disc not found, unknown flag, etc. */

/* Log format. */
typedef enum {
    OPENPETE_LOG_PLAIN = 0,
    OPENPETE_LOG_JSON  = 1,
} openpete_log_format_t;

/* All command-line options live here. Per-game main.cpp may override
 * `display_name` before passing to `openpete_run`. */
typedef struct {
    /* Identity (filled by per-game main; rest from CLI) */
    const char* display_name;     /* window title etc. */

    /* Run-loop control */
    int frame_limit;              /* 0 = unlimited */
    int max_runtime_seconds;      /* 0 = unlimited */
    bool headless;                /* no on-screen window (offscreen video driver) */

    /* Screenshot */
    int         screenshot_on_frame;   /* -1 = disabled */
    const char* screenshot_output;     /* path; default ./shot.png */

    /* Main-RAM dump — writes the 2MB g_ram to disk after frame N's game
     * code yields. Used as the openpete side of the PCSX-Redux comparison
     * loop (see scripts/pcsx-redux-ram-dump.lua). */
    int         ram_dump_on_frame;     /* -1 = disabled */
    const char* ram_dump_output;       /* path; default ./ram.bin */

    /* Seed g_ram from a 2 MB file before the recomp dispatches. Used to
     * pin-prime openpete with PCSX-Redux's RAM at the same instant — gives
     * the recompiler a byte-identical starting state, so any subsequent
     * divergence is purely the recomp's fault, not BIOS / _96_init / heap-
     * init asymmetry between the two engines. NULL = disabled. */
    const char* seed_ram_path;

    /* Seed CPUState (32 GPRs + pc/hi/lo + cop0 + GTE) from a 528-byte file
     * captured by scripts/pcsx-redux-ram-dump.lua. Applied AFTER seed-ram
     * and after the boot's EXE-header init, so it overrides everything.
     * Lets openpete start from the exact register state pcsxr was in at
     * the same PC — the partner of --seed-ram for register-aligned diffs.
     * NULL = disabled. File format: see core/runtime/run.c (load_cpu_seed). */
    const char* seed_cpu_path;

    /* Halt at recompiled-function entry. When the recomp's per-function
     * intercept (psx_override_dispatch) sees this PSX vaddr, dump 2MB RAM
     * to ram_dump_output and exit cleanly. 0 = disabled. Pair with the
     * PCSX-Redux Lua dumper using the same PC to capture both engines at
     * the *same* logical instant (function-entry boundary). A VBL count
     * names different instants on the two sides: the Lua dumper counts
     * vblank events inside the BIOS vblank interrupt, openpete counts
     * VSync(0) calls. */
    uint32_t halt_at_pc;

    /* Optional CPU-state dump at halt-at-pc. NULL = disabled. Format
     * matches scripts/pcsx-redux-ram-dump.lua's CSv1 (528 bytes:
     * magic + 32 GPRs + pc/hi/lo + cop0 + GTE). Used to align stack-
     * region diffs: knowing both sides' sp at the dump instant lets a
     * post-process zero "below sp" before bytewise comparison. */
    const char* halt_cpu_dump_output;

    /* Halt on the Nth (1-indexed) call to halt_at_pc instead of the
     * first. Use to land the dump at a specific frame boundary by
     * pinning to a per-frame game function. 0 or 1 = dump on first
     * hit; 2+ = skip preceding hits. */
    uint32_t halt_hit_target;

    /* Logging */
    openpete_log_format_t log_format;
    const char* log_level_spec;        /* e.g. "cd:debug,*:info" or NULL */

    /* Disc / data — Phase 1b uses these; Phase 0b ignores them. */
    const char* disc_path;
    const char* exe_path;          /* Direct PSX EXE bytes (overrides disc) */

    /* Phase 1b bring-up smoke test: skip the run loop and instead boot the
     * PSX EXE, dispatch a single recompiled function at this address, and
     * exit reporting cpu->v0. 0 = disabled. */
    uint32_t test_recomp_call_addr;
    uint32_t test_recomp_expected_v0;  /* 0 = don't check */
    uint32_t test_recomp_gp;            /* override cpu->gp before dispatch (0 = leave alone) */
    uint32_t probe_addr;                /* read 4 bytes at this PSX vaddr post-dispatch (0 = off) */
    bool     test_recomp_segv;          /* deliberately fault to verify crash handler */

    /* Phase 1d main-loop driver. Dispatches the function at this PSX vaddr on a
     * dedicated fiber; PSYQ thunks (VSync etc.) yield back to the host fiber,
     * which presents via PsyCross and advances `openpete_frame` until exit. */
    uint32_t run_recomp_addr;

    /* Phase 1e infra-triangle. When set, the host fiber emits a hardcoded
     * POLY_F3 each frame between PsyX_BeginScene and PsyX_EndScene. Used to
     * validate the full GL render path before real-game draw code is online
     * (the title-screen overlay is untranslated until Phase 1d.5/Phase 2). */
    bool debug_triangle;

    /* Test harness: scripted key presses. Comma-separated FRAME:KEY pairs,
     * e.g. "600:return,1200:x,1800:return". Each press is held for ~10
     * frames (~166ms at 60Hz) — long enough for the game's PadVSync to
     * see m_Down rise and commit the press, short enough to release before
     * the next event. Keys (matches default kbd map): return/enter, space,
     * x, c, z, v, up, down, left, right. NULL = no scripted input. */
    const char* press_script;

    /* Test harness: fast-forward VSync(0) calls until openpete_frame >= N.
     * Bumps the frame counter without yielding to the host fiber, so the
     * game advances as fast as possible without presenting/pacing. Skips
     * Spyro's two 3.5s VSync busy-waits in Initialize() (Sony+Universal
     * splash windows) — saves ~7s of black-screen boot per iteration. 0 =
     * disabled. */
    int fast_boot_until_frame;

    /* BizHawk .bk2 movie replay (alternative to --press, with full pad
     * fidelity instead of keyboard scancodes). NULL = disabled. See
     * notes/plans/openpete-bk2-input.md. Mutually exclusive with
     * press_script. */
    const char* bk2_path;

    /* Map bk2 frame 0 to openpete_frame `bk2_frame_offset`. Negative is
     * legal — a positive value here means "skip the first N bk2 frames".
     * INT_MIN = "unset"; the host loop then auto-picks
     * -bk2.first_input_frame so the first pressed bk2 frame lands at
     * openpete_frame=0 (use --fast-boot-until-frame to skip splashes). */
    int  bk2_frame_offset;
    bool bk2_frame_offset_set;

    /* When set, exit after the last bk2 frame instead of holding the last
     * input forever. */
    bool bk2_stop_on_end;
} openpete_options_t;

/* Defaults: 0 / unlimited / not-headless / log plain. */
void openpete_options_init(openpete_options_t* opts);

/* Parse argv into opts. Unknown flags return OPENPETE_EXIT_SETUP_ERROR;
 * caller should propagate the exit code. argv[0] is treated as program name. */
int  openpete_options_parse(openpete_options_t* opts, int argc, char** argv);

/* Run the main loop. Returns one of OPENPETE_EXIT_*. */
int  openpete_run(const openpete_options_t* opts);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_H */
