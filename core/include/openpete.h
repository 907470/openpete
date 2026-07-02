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

/* Active renderer / display mode (--renderer). VK (the native Vulkan renderer)
 * is the DEFAULT visible display; the PsyCross GL window is started hidden and,
 * in pure-vk mode, does NOT rasterize the ordering table at all (the native
 * renderer draws everything from gamestate). PSYCROSS selects the legacy GL
 * path as the sole visible window. AB/DIFF bring up the native window AND keep
 * PsyCross rendering (to its hidden FBO) so the native output can be compared
 * against it:
 *   VK   — native renderer is the sole, full-window display (default).
 *   PSYCROSS — legacy: PsyCross GL drives the single visible window.
 *   AB   — native | PsyCross side-by-side, for visual comparison.
 *   DIFF — native renderer live full-window; --screenshot-on-frame writes a
 *          per-pixel colour diff of native vs the PsyCross reference.
 * All native modes are pure display consumers — they never perturb gameplay
 * state. Native modes are ignored under --headless (PsyCross renders instead),
 * so the oracle / determinism gates (which run headless) are unaffected by the
 * default. See notes/plans/openpete-native-renderer.md. */
typedef enum {
    OPENPETE_RENDERER_PSYCROSS = 0,  /* legacy: PsyCross GL, no native window */
    OPENPETE_RENDERER_VK,            /* default: native Vulkan renderer, sole display */
    OPENPETE_RENDERER_AB,            /* native | PsyCross side-by-side */
    OPENPETE_RENDERER_DIFF,          /* native live; screenshot = colour diff */
} openpete_renderer_t;

/* All command-line options live here. Per-game main.cpp may override
 * `display_name` before passing to `openpete_run`. */
typedef struct {
    /* Identity (filled by per-game main; rest from CLI) */
    const char* display_name;     /* window title etc. */

    /* Per-game default entry point. Set to the game's `main` PSX vaddr (or
     * whatever entry the user is expected to dispatch). When --run-recomp is
     * omitted, the parser falls back to this value, so a populated hint makes
     * --run-recomp optional; --run-recomp on the CLI still overrides it. If 0
     * and --run-recomp is omitted, the parser errors with a generic message.
     * core/ stays game-agnostic by reading this field; per-game main.cpp
     * populates it. */
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

    /* --tick-scale F (default 1.0): wall-clock SPEED multiplier for the whole
     * engine — scales the sim cadence AND the render cadence by the same factor,
     * so the number of interpolated sub-tick presents PER sim tick is invariant.
     * 0.5 = half-speed slow motion (sim 14.985 Hz, present rate halved), 2.0 =
     * double speed. A debug/inspection aid for high-FPS interpolation: slow the
     * world down without changing how many intermediate frames render between
     * ticks, so each interpolated frame is easy to scrutinize. Display/pacing
     * only — every sim tick still computes byte-identical gameplay state (the
     * tick LOGIC and count are unchanged; only their wall-clock spacing scales),
     * so it cannot perturb gameplay vs PSX. Inert under --headless (which runs
     * unpaced) and when there is no window. Must be > 0. */
    double tick_scale;

    /* SDL window size override. 0/0 = use the per-game default (640x480
     * for Spyro). The window *height* drives the internal render resolution:
     * PsyCross sizes its present FBO from g_windowHeight (× the disp/clip
     * overscan ratio) in GR_BeginScene and rasterizes the GTE-projected
     * geometry directly into it with a real depth buffer — a taller window
     * genuinely renders the scene at higher resolution, not an upscale
     * (empirically ~5x the edge detail at 4x height; see
     * notes/plans/openpete-record-video.md). Window width is decoupled:
     * the FBO width is derived from height × aspect, so it only affects the
     * letterbox/pillarbox margins of the final present-blit to the window. */
    int window_w;
    int window_h;

    /* Horizontal FOV override in degrees (0 = no override; game's native
     * SetGeomScreen value is used). Implemented by intercepting
     * psyq_SetGeomScreen and replacing the GTE H register with
     * 160 / tan(fov/2) — i.e. the H that produces the requested
     * horizontal FOV at the PSX's 320-wide projection plane. See
     * notes/plans/openpete-widescreen-fov.md "FOV slider". */
    float fov_deg;

    /* PGXP (Parallel/Precision Geometry Transform Pipeline) toggle.
     * -1 = unset (defaults applied at boot); 0 = off; 1 = on.
     * When 1, PsyCross's g_cfg_pgxpTextureCorrection and g_cfg_pgxpZBuffer
     * are enabled — gameplay GTE calls populate a sub-pixel-precision
     * vertex cache that the renderer uses for perspective-correct UV
     * interpolation and a real Z-buffer (vs PSX's ordering-table-only
     * depth). When 0, the cache stays unread and rendering is bit-
     * identical to USE_PGXP=0. Defaults to 0 (off): the perspective-
     * correct look diverges from canonical PSX and hurts 1:1 comparison
     * shots, so it's opt-in via --pgxp 1 (which passed the per-level soak
     * — see the run.c boot block). Display-only; gameplay state is the
     * same either way.
     * See notes/plans/openpete-internal-resolution.md "Phase 2". */
    int pgxp_enable;

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

    /* --no-lod: collapse the sector LOD band so every visible sector is
     * drawn from its high-poly mesh. Implemented by overriding the
     * Environment's m_LodDistance to match m_CullingDistance for the
     * duration of func_800258F0; cells past cull stay invisible — this is
     * an LOD-band collapse, not a cull-distance push. m_LodDistance is
     * renderer-only (verified: only writer is initialization.c, only
     * reader is r_environment.s), so the override does not perturb
     * gameplay state. See notes/plans/openpete-disable-lods.md. */
    bool no_lod;

    /* Screenshot. Pass `--screenshot-on-frame N[,N2,...]` to fire on each
     * listed frame; the path template can contain `{n}` to substitute the
     * frame number per shot. Output extension `.png` writes PNG via
     * stb_image_write; any other extension writes BMP via SDL_SaveBMP. */
#define OPENPETE_MAX_SCREENSHOT_FRAMES 64
    int         screenshot_frames[OPENPETE_MAX_SCREENSHOT_FRAMES];
    int         screenshot_frame_count;   /* 0 = disabled */
    /* `--screenshot-on-tick N[,...]` fills the same targets but keys them
     * on `openpete_canon_tick` (canonical presents = game draw-ticks)
     * instead of `openpete_frame` (VBL yields). Tick-keyed captures sample
     * the SAME sim moment run-to-run — the frame↔tick mapping drifts with
     * host timing (see openpete_fiber.h) — so the visual-regression gates
     * use ticks. Mutually exclusive with --screenshot-on-frame. */
    bool        screenshot_ticks_keyed;
    const char* screenshot_output;        /* path template; default ./screenshot.png */

    /* Multi-present capture for the transform-interp visual regression
     * harness (`scripts/check_interp_visual.sh`): once the sim reaches
     * `screenshot_presents_start`, capture the next
     * `screenshot_presents_count` *presents* (canonical + intermediates)
     * with sequential indices in `{p}`. Distinct from
     * `screenshot_on_frame` which fires once per sim frame on the
     * canonical present only. Used to compare alpha-1.0 (canonical) vs
     * alpha<1.0 (intermediate) outputs at different --render-fps
     * settings — the latter is the only way to observe whether the
     * interp delta is reaching the GL submit. */
    int         screenshot_presents_start;  /* -1 = disabled */
    int         screenshot_presents_count;  /* default 1 */

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

    /* Active renderer / display mode (--renderer psycross|vk|ab|diff). Default
     * OPENPETE_RENDERER_PSYCROSS = the original behavior (PsyCross owns the one
     * window, no native Vulkan window). The other modes open the native
     * renderer's Vulkan window and start PsyCross hidden. Pure display
     * selection — never perturbs gameplay state; native modes are no-ops under
     * --headless. See openpete_renderer_t above. */
    openpete_renderer_t renderer;

    /* --ssaa N (default 1): supersampling factor for the native vk LIVE window.
     * The offscreen render target is allocated at N× the swapchain extent per
     * axis (N² samples/pixel) and the present blit box-downsamples to the
     * window. 1 = no-op (the offscreen matches the swapchain, a 1:1 blit).
     * Range 1..32, auto-capped to the GPU's max image size / device-local VRAM
     * (so a large value degrades gracefully). Display-only — no gameplay effect;
     * under --headless and on --renderer psycross. The native_rtt that backs
     * the -vk/-ab/-diff screenshots + DIFF mode stays at reference resolution
     * so the A/B-vs-PsyCross comparison stays 1:1 pixel-exact. */
    int ssaa;

    /* FPS overlay on the native vk window (default on; --show-fps 0 to start
     * off, F11 to toggle live). Display-only; ignored under --headless / psycross. */
    bool fps_overlay;

    /* --render-stats (debug): in --renderer diff, read the per-pixel diff back
     * each frame and log colour-closeness statistics (per-channel mean/max abs
     * diff, % within tolerance, differing-pixel count) over the panel, plus an
     * aggregate at exit. Pairs with --render-paths to measure a single path's
     * native-vs-PsyCross accuracy. Opt-in (the per-frame readback is a GPU
     * stall) — the live diff view stays fast without it. Display-only. */
    bool render_stats;

    /* --render-paths CSV (debug): restrict rendering to the named render paths
     * (e.g. "cyclorama,spyro") in BOTH renderers, so a path can be compared in
     * isolation. PsyCross drops the other paths' geometry prims during the OT
     * walk (presentation-only — never writes game RAM, so gameplay state is
     * unaffected); the native renderer feeds only the selected paths. The path
     * NAMES are game-specific, so core only carries the raw string; the per-game
     * impl parses it. NULL/empty = all paths (no filtering). */
    const char* render_paths;

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

    /* Force-mute all audio (SPU SFX + XA CD music + music WAV override)
     * at the OpenAL layer via PsyX_SPUAL_SetMute + PsyX_XA_ForceGain +
     * music_override_set_forced_gain. Engine-level, so no game-specific
     * globals or per-frame stamping is needed. Defaults to false (audio
     * on) — the shipping game has sound; pass --mute 1 (or drop it in an
     * openpete.opts) to silence a dev/CI build. Note this only clamps gain
     * on already-created AL sources; it never gates device/context init,
     * so a headless run with no audio device is silent either way. */
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

    /* PCSX-Redux oracle verifier. When set, opens a .psxtrace produced by
     * scripts/pcsx-redux-trace.lua; on every dispatch of psxtrace_anchor_pc
     * the verifier consumes the next trace record and value-equality
     * compares its per-region hashes against openpete's live
     * gameplay_hash_compute_ex(). The anchor PC defaults to whatever PC
     * is recorded in the trace's first record (so the same anchor
     * scripts/pcsx-redux-trace.lua used). First mismatch is logged with
     * (frame, region_index, region_name, oracle_hash, op_hash); the run
     * exits non-zero unless --psxtrace-keep-going is set. NULL = disabled.
     * Format spec in tools/psxtrace_format.py. */
    const char* psxtrace_verify_path;
    bool        psxtrace_keep_going;
    uint32_t    psxtrace_anchor_pc;        /* 0 = inherit from trace      */
    uint32_t    psxtrace_anchor_hit;       /* 0/1 = first hit; N = Nth    */

    /* --frame-time-log PATH (default NULL): write one 32-byte record
     * per present (frame, slip_count, op_now_ns, deadline_ns, slack_ns)
     * for the pacing measurement harness. Off by default. See
     * notes/plans/openpete-frame-pacing.md and
     * scripts/measure-pacing.py. */
    const char* frame_time_log_path;

    /* --vertex-trace-log PATH (default NULL): write one 72-byte record
     * per matcher batch per OT walk (sim-due + intermediate). Used by
     * scripts/analyze-vtrace.py to diagnose substitute-pipeline issues
     * empirically. Off by default. */
    const char* vertex_trace_log_path;

    /* --substitute-clamp-px N (default 8): max per-axis per-vertex
     * applied delta in pixels. 0 disables substitute entirely. Bounds
     * the worst visible flicker from constant-velocity over-prediction
     * at intermediate renders. <0 = unset (use compiled default). */
    float substitute_clamp_px;

    /* --debug-highlight-smoothed: paint actively-smoothed batches solid
     * untextured red (diagnostic for the rigid/animated split).
     * -1 = unset (fall back to env), 0 = off, 1 = on. Live-toggle F9. */
    int debug_highlight_smoothed;

    /* --debug-black-intermediates: present a single black prim instead
     * of the interpolated OT on intermediate renders (presentation
     * isolation test). -1 unset / 0 off / 1 on. Live-toggle F10. */
    int debug_black_intermediates;

    /* --record PATH: capture gameplay video. True VFR — one encoded frame
     * per game-presented frame, PTS = wall-clock capture time, so playback
     * matches real game cadence (frame drops show as fewer frames/sec).
     * Frames are read from the present FBO at internal resolution (no
     * letter/pillarbox bars). Links libav* in-process. NULL = disabled.
     * Container auto-picked by extension (.mkv recommended; .mp4 supported).
     * Written incrementally + trailer on shutdown, so Ctrl-C still yields a
     * playable file. See notes/plans/openpete-record-video.md. */
    const char* record_path;
    const char* record_encoder;   /* libavcodec video encoder. Default libx264. */
    int         record_crf;       /* x264/x265 quality. Default 23. */
    int         record_gop;       /* keyframe interval, frames. Default 120. */
    const char* record_pixfmt;    /* encoder pixfmt. Default yuv420p. */

    /* Audio capture into the recording. The mixed SPU PCM is pulled via an
     * OpenAL loopback device (only while recording; normal playback is
     * unaffected) and encoded as a second stream. See
     * notes/plans/openpete-record-audio.md. */
    bool        record_no_audio;     /* --no-record-audio: video-only */
    const char* record_audio_encoder; /* libavcodec audio encoder. Default aac. */

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
