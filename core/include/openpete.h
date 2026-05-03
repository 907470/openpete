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

    /* Per-game hint surfaced in the "missing --run-recomp" diagnostic.
     * Set to the game's `main` PSX vaddr (or whatever entry the user is
     * expected to dispatch) so the parse error names a concrete value to
     * pass. core/ stays game-agnostic by reading this field; per-game
     * main.cpp populates it. 0 = no hint, message stays generic. */
    uint32_t run_recomp_hint_addr;

    /* Run-loop control */
    int frame_limit;              /* 0 = unlimited */
    int max_runtime_seconds;      /* 0 = unlimited */
    bool headless;                /* no on-screen window (offscreen video driver) */

    /* Render-cadence target. 0 = match sim (condvar-paced ticker, the
     * historical behavior). >0 = wall-clock dual-pacer: sim still yields at
     * the native PSX 60 Hz vblank, input pump runs at this Hz, and present
     * fires when the game produced new draw commands. See
     * notes/plans/openpete-fps-unlock.md "Phase 1". */
    int render_fps;

    /* SDL window size override. 0/0 = use the per-game default (640x480
     * for Spyro). PsyCross still renders the PSX framebuffer at 512x240
     * internal; this only controls the on-screen window dimensions GL
     * stretches to. */
    int window_w;
    int window_h;

    /* Horizontal FOV override in degrees (0 = no override; game's native
     * SetGeomScreen value is used). Implemented by intercepting
     * psyq_SetGeomScreen and replacing the GTE H register with
     * 160 / tan(fov/2) — i.e. the H that produces the requested
     * horizontal FOV at the PSX's 320-wide projection plane. See
     * notes/plans/openpete-widescreen-fov.md "FOV slider". */
    float fov_deg;

    /* Target display aspect ratio (width / height). 0 = no override
     * (4:3 native). >0 widens the DISPENV / DRAWENV horizontally and
     * re-centres SetGeomOffset so the GTE projects geometry across the
     * wider framebuffer — Hor+ widescreen. PsyCross's non-PGXP render
     * path reads activeDispEnv.disp.w directly into GR_Ortho2D, so no
     * PsyCross commit is needed (PGXP is currently off; if it gets
     * re-enabled per Step 7 the PSX_SCREEN_ASPECT macro will need
     * promoting to a runtime variable). HUD draws stay at native
     * 512-wide pixel coords — Phase 2 re-anchors per-element. */
    float aspect_w_over_h;

    /* Screenshot. Pass `--screenshot-on-frame N[,N2,...]` to fire on each
     * listed frame; the path template can contain `{n}` to substitute the
     * frame number per shot. Output extension `.png` writes PNG via
     * stb_image_write; any other extension writes BMP via SDL_SaveBMP. */
#define OPENPETE_MAX_SCREENSHOT_FRAMES 64
    int         screenshot_frames[OPENPETE_MAX_SCREENSHOT_FRAMES];
    int         screenshot_frame_count;   /* 0 = disabled */
    const char* screenshot_output;        /* path template; default ./screenshot.png */

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

    /* BizHawk .bk2 movie replay. Full pad-bitstream fidelity (holds,
     * releases, gamepad, analog). NULL = disabled. See
     * notes/plans/openpete-bk2-input.md. */
    const char* bk2_path;

    /* Map bk2 frame 0 to openpete_frame `bk2_frame_offset`. Negative is
     * legal — a positive value here means "skip the first N bk2 frames".
     * INT_MIN = "unset"; the host loop then auto-picks
     * -bk2.first_input_frame so the first pressed bk2 frame lands at
     * openpete_frame=0 (which is itself anchored to the first DrawOTag,
     * so splash frames are naturally excluded from the bk2 timeline). */
    int  bk2_frame_offset;
    bool bk2_frame_offset_set;

    /* When set, exit after the last bk2 frame instead of holding the last
     * input forever. */
    bool bk2_stop_on_end;

    /* Force-mute all audio (SPU SFX + XA CD music) at the OpenAL layer
     * via PsyX_SPUAL_SetMute + PsyX_XA_ForceGain. Engine-level, so no
     * game-specific globals or per-frame stamping is needed. Defaults
     * to true so headless / CI runs are silent unless --mute off. */
    bool mute;

    /* Record live pad input (keyboard + gamepad, merged by PsyCross) into
     * a BizHawk-format .bk2 zip at this path. Sister of --bk2 replay;
     * combinable with it (the recorded frames include the replayed ones,
     * so a session can start with a canned .bk2 prefix and extend live
     * from where it ends). NULL = disabled. The file is written at exit,
     * so SIGTERM/Ctrl-C against the run still produces a usable movie as
     * long as core/runtime/run.c reaches its cleanup block. */
    const char* bk2_record_path;

    /* Scheduled fast-forward windows: forces openpete_unlock_fps = 1
     * while openpete_frame is inside any [lo,hi] range. Used by the
     * BK2 determinism harness to inject pacing variation without
     * requiring a human to hold Tab. NULL = disabled. Format:
     *   "100-200,500-600"
     * Inclusive on both ends. Frame numbers are openpete_frame (yield-
     * driven), the same anchor BK2 uses. */
    const char* unlock_frames_spec;
#define OPENPETE_UNLOCK_RANGES_MAX 32
    struct { int lo, hi; } unlock_ranges[OPENPETE_UNLOCK_RANGES_MAX];
    int  unlock_ranges_count;

    /* Position log: every yield where the game is past fast-boot, append
     * one CSV line `frame,vbl,x,y,z` with g_Spyro.m_Position to this
     * path. Used for cross-run determinism diffs. NULL = disabled. The
     * Spyro address is hardcoded in run.c (game-specific) — when we add
     * a second game we'll route this through game_extras. */
    const char* position_log_path;

    /* --gameplay-hash-log PATH (default NULL): per-yield, hash the
     * gameplay-readable region defined by manifest/gameplay_state_regions.toml
     * with FNV1a-64 and append `frame,vbl,hex_hash` to PATH. The
     * regions are loaded statically (compiled-in copy of the manifest)
     * so the runtime has zero runtime-config requirement. Used by
     * scripts/check_aspect_determinism.sh as the primary determinism
     * gate — the per-position log is a coarse proxy that misses leaks
     * not yet propagated to Spyro's m_Position. NULL = disabled. */
    const char* gameplay_hash_log_path;

    /* Skip the title screen / save-file picker and load directly into a
     * specific level. The game's natural post-intro path puts the title
     * screen in TSM_Demo + TSD_Level mode, which loads whatever
     * g_LevelId is set to. We hook that exact moment in the per-game
     * impl, override g_LevelId/g_NextLevelId, and let the state machine
     * do its normal flyby + load — no state corruption. 0 = disabled.
     * Spyro 1 level IDs: 10/20/30/40/50/60 are homeworlds, +1..3 are
     * regular levels, +4 is the flight/speedway level, 63 is Gnasty's
     * lair. Splash sequence is auto-skipped (host-yield is bypassed until
     * the first DrawOTag), so this loads as fast as the EXE allows. */
    int skip_to_level;
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
