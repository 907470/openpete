// core/include/openpete.h — runtime entry point + program options.
//
// Each game's `main.cpp` is a tiny wrapper that fills in `openpete_options_t`
// and calls `openpete_run()`. All command-line parsing, window setup,
// screenshot logic, and the run loop live in core/.

#ifndef OPENPETE_H
#define OPENPETE_H

#include <stdbool.h>
#include <stdint.h>
#include "openpete_perf_stats.h"   /* OPENPETE_PERF_* levels for opts.perf_stats */
#include "openpete_overlay_anchor.h" /* openpete_anchor_t for opts.perf_position */

#ifdef __cplusplus
extern "C" {
#endif

/* Exit codes — see notes/plans/openpete-pc-port.md Phase 0b. */
#define OPENPETE_EXIT_OK           0  /* clean termination */
#define OPENPETE_EXIT_PANIC        1  /* panic / crash / timeout */
#define OPENPETE_EXIT_DIVERGENCE   2  /* state-divergence detected (Phase 1c+) */
#define OPENPETE_EXIT_SETUP_ERROR  3  /* disc not found, unknown flag, etc. */

/* Highest accepted --ssaa factor. The supersampled extent drives FIVE
 * device-local render targets — the scene target plus the four key-contest
 * targets (terrain/moby/actor key depth + the moby rank RT) — and each carries
 * a colour image AND a depth image, ~60 bytes per supersampled pixel on a
 * D32_SFLOAT_S8_UINT device. Since the factor is PER AXIS, the footprint grows
 * as N²: at 1080p, 4× is ~2.0 GB but 8× is ~8.0 GB, which exceeds an 8 GB card
 * outright. maxImageDimension2D alone is no guard (32768 permits 16× at 1080p,
 * ~32 GB), and drivers that oversubscribe device-local memory turn the overrun
 * into multi-second passes and a lost device rather than a clean allocation
 * failure the render-target step-down could catch. */
#define OPENPETE_SSAA_MAX 4

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
    /* PsyCross GL rasterizes; the native Vulkan window DISPLAYS its frame
     * (read back per present). No native scene is extracted. */
    OPENPETE_RENDERER_PSYCROSS = 0,
    /* Default: the native Vulkan renderer both rasterizes and displays. */
    OPENPETE_RENDERER_VK,
} openpete_renderer_t;

/* Vertex screen-coordinate precision (`--vertex-precision`). The native renderer
 * projects world geometry to sub-pixel FLOAT screen coordinates for smooth motion
 * (the PGXP-equivalent default). INTEGER snaps each 3D-world vertex to the PSX
 * 512x240 integer pixel grid before rasterization, reproducing the original
 * hardware's characteristic polygon "wobble"/jitter (the GTE stored screen X/Y as
 * int16). Display-only: it rounds only the emitted display geometry — never
 * gameplay-readable RAM — and only the 3D-world layers (2D HUD/overlay stays
 * smooth). Depth and UVs are untouched, so the true Z-buffer and perspective-
 * correct texturing are kept. See notes/plans/openpete-ps1-retro-look.md. */
typedef enum {
    OPENPETE_VERTEX_PRECISION_SUBPIXEL = 0,  /* default: sub-pixel float, smooth motion */
    OPENPETE_VERTEX_PRECISION_INTEGER,       /* PSX integer-grid snap (polygon wobble) */
} openpete_vertex_precision_t;

/* Texture/colour interpolation mode (`--texture-mapping`). The native renderer
 * reconstructs the homogeneous W from depth so the rasterizer interpolates
 * UV + gouraud colour perspective-correct (the modern default). AFFINE emits
 * w=1 instead (scene vertex spec-constant 1): the rasterizer interpolates
 * screen-linearly, exactly the PSX GPU's behaviour — the authentic texture
 * swim/warp on large near polygons (the game's own near-terrain subdivide,
 * which existed to tame it, still runs). Display-only; depth values are
 * identical in both modes. Boot-latched (pipelines bake the constant). */
typedef enum {
    OPENPETE_TEXTURE_MAPPING_PERSPECTIVE = 0,  /* default: perspective-correct */
    OPENPETE_TEXTURE_MAPPING_AFFINE,           /* PSX screen-linear interpolation */
} openpete_texture_mapping_t;

/* Sector level-of-detail policy (`--lod original|extended`). ORIGINAL is the
 * authentic PSX behaviour: distance-based LOD swaps + the stock fog curve.
 * EXTENDED collapses the sector LOD band so every visible sector draws from
 * its high-poly mesh at full quality, out to the cull distance and beyond
 * (the classic "see detail far away" look), and defaults --fog-scale to 100x
 * (fog effectively off, so the extended far detail is actually visible).
 * Native renderer only — the feeders derive detail from gamestate, never
 * perturbing gameplay-readable RAM (in PsyCross reference mode EXTENDED warns
 * and is ignored). See notes/plans/openpete-disable-lods.md. */
typedef enum {
    OPENPETE_LOD_ORIGINAL = 0,  /* default: authentic PSX LOD band + fog curve */
    OPENPETE_LOD_EXTENDED,      /* full detail at any distance (was --no-lod) */
} openpete_lod_mode_t;

/* All command-line options live here. Per-game main.cpp may override
 * `display_name` before passing to `openpete_run`. The struct tag exists so
 * headers can forward-declare it without pulling this whole file in. */
typedef struct openpete_options {
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

    /* Render-cadence target. -1 = unset (the default): resolved at startup
     * to the display's refresh rate for windowed runs, 0 for headless
     * (run_recomp_fiber, before the pacer is seeded). 0 = match sim
     * (condvar-paced ticker, the historical behavior). >0 = wall-clock
     * dual-pacer: sim still yields at the native PSX 60 Hz vblank, input
     * pump runs at this Hz, and present fires when the game produced new
     * draw commands. See notes/plans/openpete-fps-unlock.md "Phase 1". */
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

    /* Target display aspect ratio (width / height), native renderer only —
     * the Hor+ widening happens purely at the display layer from canonical
     * 4:3 producers (the PsyCross-mode substitution stack was retired
     * 2026-07-06). 0 = auto (`--aspect auto`, the default): the render
     * aspect follows the window / fullscreen shape live, filling it with no
     * bars (clamped ≥ 4:3 — Hor+ only ADDS world). >0 = fixed (`--aspect
     * W:H`): sizes the launch window to match and letterboxes/pillarboxes
     * whenever the window shape differs. */
    float aspect_w_over_h;

    /* --lod original|extended: sector level-of-detail policy (see
     * openpete_lod_mode_t above). ORIGINAL keeps the authentic PSX LOD band;
     * EXTENDED collapses it so every visible sector is drawn from its
     * high-poly mesh out to the cull distance and beyond. The native feeders
     * derive detail from gamestate and do not perturb gameplay-readable RAM.
     * `--no-lod` is a deprecated alias for `--lod extended`.
     * See notes/plans/openpete-disable-lods.md. */
    openpete_lod_mode_t lod;

    /* Fog-distance multiplier (`--fog-scale N`). Scales every channel of the
     * terrain fog model together (DPCS vertex curve, CLUT-row palette ladder,
     * near-tile select, fog-saturation LP handoff): N = 1 is the authentic
     * PSX curve (saturation at the LOD distance), larger N saturates N x
     * farther — the classic "see detail far away" look; ~100 pushes fog off
     * entirely. 0 = auto: 1 under --lod original, 100 under --lod extended. */
    float fog_scale;

    /* Vertex screen-coordinate precision (`--vertex-precision subpixel|integer`).
     * SUBPIXEL (default) keeps the native renderer's smooth sub-pixel projection;
     * INTEGER snaps 3D-world vertices to the PSX 512x240 pixel grid to reproduce
     * the original polygon wobble. Display-only (see openpete_vertex_precision_t). */
    openpete_vertex_precision_t vertex_precision;

    /* Raster output quantize (`--raster-quantize N`). N > 0 resamples the
     * composed 3D scene (pre-HUD) on the virtual PSX raster at N x scale — each
     * 512N x 240N cell takes ONE point sample (N=1 = authentic 240p), matching
     * the hardware rasterizer's single sample per output pixel: no downsample
     * filter, so textures and polygon edges stay hard. An integer MULTIPLIER of
     * the PSX grid rather than a row count, so every setting stays aligned with
     * the INTEGER vertex snap's 512x240 lattice by construction: snapped edges
     * land on cell boundaries, and the snap's silhouette/T-junction seams
     * (<= ~1 PSX pixel) are bounded to one cell, where a per-cell depth vote
     * moves the sample point off the leaking sliver. -1 = auto (1 when
     * vertex_precision is INTEGER, else off); 0 = off. Display-only; native
     * renderer only. */
    int raster_quantize;

    /* Extraction worker threads (`--extract-threads N`). Data-parallel fan-out
     * of the native render extraction (terrain sector walk) over a persistent
     * worker pool; results merge in item order, so output is byte-identical
     * to serial. 0 = auto (logical cores / 2, clamped to [1, 8]); 1 = serial
     * (the pre-pool code path, no threads created). Boot-latched;
     * display-only (workers read guest RAM only inside the extraction
     * window — see openpete_workpool.h contracts). */
    int extract_threads;

    /* PS1 15-bit colour (`--dither 0|1`). 1 = quantise the composed 3D scene
     * to RGB555 with the GPU's 4x4 ordered dither matrix on the PSX 512x240
     * framebuffer lattice — the authentic banding+crosshatch pair the native
     * float pipeline removed. Applied as an engine postfx below any mod chain
     * (a CRT filter sees the dithered image, as composite video did).
     * Display-only; native renderer only. Default 0. */
    int dither;

    /* Reference-renderer raster (`--psycross-native-raster 0|1`). 1 renders the
     * PsyCross scene at the console's own 512x240 display grid rather than a
     * square-pixel 4:3 grid derived from the window height, so a screenshot or
     * a --record capture carries one texel per PSX pixel — the authentic
     * raster and its aliasing, not a supersampled resample of it. The window
     * blit scales it up, so only the sampling grid changes. Ignored unless
     * PsyCross rasterizes (--renderer psycross). */
    int psycross_native_raster;

    /* Texture/colour interpolation (`--texture-mapping perspective|affine`).
     * AFFINE = the PSX GPU's screen-linear interpolation (authentic texture
     * swim). Boot-latched; display-only (see openpete_texture_mapping_t). */
    openpete_texture_mapping_t texture_mapping;

    /* Terrain sector-mesh GPU cache (`--terrain-cache 0|1`, EXPERIMENTAL,
     * default OFF): retain terrain as per-sector GPU meshes and move the
     * camera transform / fog / cull into the vertex shader, so the per-present
     * CPU cost is O(sectors) instead of O(faces). Display-only; falls back to
     * the CPU walk per present whenever an unsupported mode is active (debug
     * envs, portal previews, HD texture packs, mod material selectors, A/B
     * reference modes, software Vulkan, --vertex-precision integer). See
     * notes/plans/openpete-terrain-gpu-cache.md. */
    int         terrain_cache;

    /* Disc image resident in memory (default on): PsyCross reads the .bin
     * from a host array filled by a background loader instead of fread on
     * the sim thread. --disc-cache 0 / OPENPETE_DISC_CACHE=0 = the file
     * path, for A/B. Display/timing-only; bytes identical. */
    int         disc_cache;

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

    /* `--screenshot-native-only`: capture the native `-vk` variant ALONE —
     * skip the PsyCross OT rasterization (`openpete_psycross_renders`, which a
     * screenshot request otherwise forces on for the `-og` reference) and the
     * `-og`/`-vk` composition. Gameplay-neutral (the render→gameplay
     * channel writes come from the recompiled render funcs, upstream of OT
     * rasterization; this is also the normal windowed pure-vk play path). The
     * presentation-golden corpus's default capture path — its single biggest
     * speedup, since the discarded reference otherwise costs a full GL OT walk
     * per screenshot frame. */
    bool        screenshot_native_only;

    /* `--screenshot-intermediate-on-frame N[,...]`: deterministic sub-tick
     * (alpha = 0.5) native captures, headless-capable. At each listed VBL the
     * present clock is forced to the tick midpoint (curr_tick_ns + period/2) and
     * the re-extract callback is invoked once, so the shot lands on the exact
     * interpolated pose the sub-tick spine would present — reproducibly, with no
     * wall-clock pacing. VBL-keyed (openpete_frame), like --screenshot-on-frame.
     * Implies --screenshot-native-only capture. Fills a SEPARATE target list so a
     * run can carry canonical and intermediate legs together. */
    int         screenshot_intermediate_frames[OPENPETE_MAX_SCREENSHOT_FRAMES];
    int         screenshot_intermediate_count;   /* 0 = disabled */

    /* `--screenshot-freeze-backdrop`: during a headless capture run, render each
     * pre-freeze WORLD frame into the capture freeze texture so a freeze-frame
     * dump (pause / flight-results / level-exit) shows the green frozen-world
     * backdrop instead of a dark clear (frozen_tex is snapshotted only with a live
     * window). Off by default — it renders an extra frame per present, so enable
     * it only for bk2s/poses that reach a freeze state. */
    bool        screenshot_freeze_backdrop;

    /* Main-RAM dump — writes the 2MB g_ram to disk after frame N's game
     * code yields. Used as the openpete side of the PCSX-Redux comparison
     * loop (see scripts/pcsx-redux-ram-dump.lua). */
    int         ram_dump_on_frame;     /* -1 = disabled */
    const char* ram_dump_output;       /* path; default ./ram.bin */

    /* Scheduled savestate ops — fire once at the savestate quiescent point
     * (game fiber yielded) when openpete_frame (a VBL counter) reaches N.
     * Headless-friendly: this is the autonomous test vehicle for the
     * savestate plan's restore-correctness gates (save→tick→restore→tick
     * hash A/B) — see plans/openpete-savestate-frame-step-rewind.md P0/P3.
     * Load re-anchors openpete_frame exactly like the hotkey path. */
    int         state_save_on_frame;   /* -1 = disabled */
    const char* state_save_output;     /* path; default ./frameN.state */
    int         state_load_on_frame;   /* -1 = disabled */
    const char* state_load_input;      /* path; required when load armed */

    /* Savestate stress harnesses (P3 restore-at-yield gates; headless):
     * state_stress N       — every N ticked iterations: in-memory save +
     *                        immediate quiet restore. A clean run and a
     *                        stressed run must produce identical
     *                        --gameplay-hash-log timelines.
     * state_stress_rewind K— save, run K more iterations, restore, repeat
     *                        (the runahead rollback shape). The repeated
     *                        window's hash rows must self-match pass 1. */
    int         state_stress;          /* 0 = disabled */
    int         state_stress_rewind;   /* 0 = disabled */

    /* Rewind (savestate P4). rewind_window: seconds of keyframe ring
     * (0 = off). rewind_hold_frame/iters: headless harness — simulate
     * holding the rewind key for N iterations at frame A. */
    int         rewind_window;         /* 0 = disabled */
    int         rewind_hold_frame;     /* -1 = disabled */
    int         rewind_hold_iters;

    /* Runahead (savestate P5): lead the presented timeline by N sim
     * ticks to collapse internal input latency (measured L_submit =
     * 2 ticks; N=2 collapses it fully). 0 = off. Arms only when every
     * enabled mod declares a runahead capability. */
    int         runahead;              /* 0 = disabled; clamped 0..3 */

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
    /* --log-file PATH / --no-log-file (which sets "off"). NULL = unset, so
     * $OPENPETE_LOG_FILE and then the default state-dir path apply. */
    const char* log_file;

    /* Disc / data — Phase 1b uses these; Phase 0b ignores them. */
    const char* disc_path;
    const char* exe_path;          /* Direct PSX EXE bytes (overrides disc) */
    const char* ingest_path;       /* --ingest: copy this disc image into
                                       the content-addressed library next
                                       to the exe, then boot it. */
    const char* extract_recomp_inputs_dir; /* Extract PSX.EXE + overlay
                                       blobs from --disc into this dir and
                                       exit (first-boot pipeline stage /
                                       carve-equivalence gate). */
    const char* recomp_module_path; /* Shared-linkage builds: override the
                                       recomp-module path (default: platform
                                       library name next to the exe). Ignored
                                       by static builds. */

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

    /* Active renderer / display mode (--renderer psycross|vk). Default
     * OPENPETE_RENDERER_PSYCROSS = the original behavior (PsyCross owns the one
     * window, no native Vulkan window). The other modes open the native
     * renderer's Vulkan window and start PsyCross hidden. Pure display
     * selection — never perturbs gameplay state; native modes are no-ops under
     * --headless. See openpete_renderer_t above. */
    openpete_renderer_t renderer;

    /* The renderer this game starts with when --renderer is absent, which is
     * also the one print_help marks as the default. A game with no native
     * renderer walks sets both this and `renderer` through
     * openpete_options_set_default_renderer, before parsing, so an explicit
     * --renderer still wins and the help text still names what it will get. */
    openpete_renderer_t default_renderer;

    /* In-process continuation from the first-run splash: when non-NULL, an
     * SDL_Window* (void* keeps this header SDL-free) that the windowed vk
     * renderer adopts — retitle/resize/make-resizable — instead of opening
     * a second window, so one OS window carries splash and game. Set by the
     * game's main between splash teardown and openpete_run(); only handed
     * over in any windowed run, since every windowed mode displays through the
     * native window. Headless closes the splash instead. Display-only. */
    void* adopted_window;

    /* --ssaa N (default 1): supersampling factor for the native vk LIVE window.
     * The offscreen render target is allocated at N× the swapchain extent per
     * axis (N² samples/pixel) and the present blit box-downsamples to the
     * window. 1 = no-op (the offscreen matches the swapchain, a 1:1 blit).
     * Range 1..OPENPETE_SSAA_MAX, further capped to the GPU's max image size
     * (so a large value degrades gracefully). Display-only — no gameplay effect;
     * under --headless and on --renderer psycross. The native_rtt that backs
     * the -vk screenshots + DIFF mode stays at reference resolution
     * so the A/B-vs-PsyCross comparison stays 1:1 pixel-exact. */
    int ssaa;

    /* Performance-stats overlay level on the native vk window, one of the
     * OPENPETE_PERF_* values: off, the FPS counter (the default), or the
     * counter plus a pacing readout. --perf-stats at launch; the O menu's
     * SHOW FPS row and the M overlay's Performance stats row move it live.
     * Display-only; ignored under --headless / psycross. */
    int perf_stats;

    /* Which corner/edge the performance-stats block anchors to, an
     * openpete_anchor_t (openpete_overlay_anchor.h). --perf-position; the M
     * overlay's row moves it live. Display-only. */
    int perf_position;

    /* On-screen display (toast notifications). `osd` 0/1; `osd_position` an
     * openpete_anchor_t; `osd_timeout_ms` the default dwell in INTEGER
     * MILLISECONDS. The user-facing flag is in seconds (--osd-timeout 3.0)
     * because that is how a person thinks about a toast; everything past the
     * parser is ms, converted exactly once, right here. Display-only. */
    int      osd;
    int      osd_position;
    uint32_t osd_timeout_ms;

    /* --vsync 0|1: native vk swapchain present mode. 1 = FIFO (vsync ON, present
     * locked to the monitor refresh — the default); 0 = a vsync-OFF mode
     * (immediate/mailbox) for low latency / uncapped-throughput benchmarking;
     * -1 = unset (defer to the OPENPETE_PRESENT_MODE env, else FIFO). Display-only;
     * ignored under --headless / psycross. */
    int vsync;

    /* --fullscreen 0|1: launch the native window in borderless-desktop
     * fullscreen (same mode Alt+Enter / double-click / the O-menu DISPLAY row
     * toggle live). Default 0 = windowed. Display-only; ignored under
     * --headless / psycross (warned). */
    int fullscreen;

    /* --render-paths CSV (debug): restrict rendering to the named render paths
     * (e.g. "cyclorama,spyro") in BOTH renderers, so a path can be compared in
     * isolation. PsyCross drops the other paths' geometry prims during the OT
     * walk (presentation-only — never writes game RAM, so gameplay state is
     * unaffected); the native renderer feeds only the selected paths. The path
     * NAMES are game-specific, so core only carries the raw string; the per-game
     * impl parses it. NULL/empty = all paths (no filtering). */
    const char* render_paths;

    /* --devui-open 0|1 (debug): open the dev config overlay (the M panel) as
     * soon as it initializes, instead of waiting for the M key. Windowed
     * native-vk runs only — the overlay is never created headless or under
     * --renderer psycross, where the flag logs that it is inert. -1 = unset
     * (defer to the OPENPETE_DEVUI_OPEN env, else closed). Boot-latched:
     * openpete_devui_init reads it once. Display-only. */
    int devui_open;

    /* --devui-inject-mouse X,Y (debug): post this cursor position into the
     * dev overlay's imgui IO before every frame, so an unattended run can
     * park the pointer over a widget and read the hover state out of the
     * log. Windowed native-vk runs only. NULL/empty = unset (defer to
     * OPENPETE_DEVUI_INJECT_MOUSE, else no injection). Boot-latched:
     * openpete_devui_init parses it once. Display-only. */

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
     * on) — the shipping game has sound; pass --mute 1 (or set it in
     * openpete.toml) to silence a dev/CI build. Note this only clamps gain
     * on already-created AL sources; it never gates device/context init,
     * so a headless run with no audio device is silent either way. */
    bool mute;

    /* Preset the game's own in-game volume sliders at boot, on the scale its
     * options menu uses (0 = OFF .. 10 = max). -1 leaves each slider alone,
     * which is the default: the game picks its own value (and a loaded save
     * file restores the player's).
     *
     * Unlike --mute these are *gameplay-side* values, written into the game's
     * volume globals by the per-game audio-volume feature at a tick boundary.
     * The preset survives the clobbers the game does on its own (title-screen
     * init, save-file load) and is released the moment the player edits that
     * slider in the game's options menu. */
    int sfx_volume;     /* --sfx   0..10, -1 = leave the game's slider alone */
    int music_volume;   /* --music 0..10, -1 = leave the game's slider alone */

    /* --vibration: controller rumble, 0 (off) .. 10 (max), -1 = leave the
     * game's own vibration setting alone (the default, which means ON — the
     * game enables actuators at init and a save file restores the player's
     * choice).
     *
     * Two jobs from one value, the same shape as the sliders above: the
     * per-game vibration feature stamps the game's own enable flag at a tick
     * boundary, and the host scales motor amplitude by N/10 on top. The guest
     * toggle has no intensity of its own, so without the host scale a 3 and a
     * 10 would feel identical.
     *
     * 0 silences the motors host-side and writes NOTHING to guest RAM — the
     * game's own vibration setting keeps reading whatever the player chose.
     * That is deliberate: 0 is the value every unattended launcher sets, and a
     * guest write there would leave the oracle and determinism gates measuring
     * a quietly modified game. */
    int vibration;      /* --vibration 0..10, -1 = leave the game's setting alone */
    /* --mouse-sensitivity: multiplier on the freecam/camlock mouse look
     * (angle units per count = OPENPETE_FREECAM_MOUSE_K * this); < 0 = unset
     * (1.0). */
    float mouse_sensitivity;

    /* Content for the "<bk2_record_path>.args" sidecar written next to a
     * --bk2-record capture. Holds the launch flags passed *directly* on
     * the command line (excluding every --bk2-* flag and anything sourced
     * from openpete.toml) so replaying the recording via --bk2 re-applies
     * the same scenario. NULL = don't write a sidecar. Heap-owned and
     * intentionally leaked for program lifetime (built in
     * openpete_options_parse). */
    const char* bk2_record_args;

    /* Record live pad input (keyboard + gamepad, merged by PsyCross) into
     * a BizHawk-format .bk2 zip at this path. Sister of --bk2 replay;
     * combinable with it (the recorded frames include the replayed ones,
     * so a session can start with a canned .bk2 prefix and extend live
     * from where it ends). NULL = disabled. The file is written at exit,
     * so SIGTERM/Ctrl-C against the run still produces a usable movie as
     * long as core/runtime/run.c reaches its cleanup block.
     *
     * When --bk2-record is passed with no path, openpete_options_parse
     * resolves a default here: "<exe_dir>/bk2/<timestamp>.bk2" (and
     * mkdir -p's that directory). Heap-owned in that case (leaked for
     * program lifetime); otherwise it points at the CLI token. */
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
    /* --pace-log PATH: the unified pacing event log (every VBL, every
     * present, on one timebase). Superset of --frame-time-log; see
     * openpete_pace_log.h. */
    const char* pace_log_path;

    /* --profile-out PATH (default NULL): stream a binary `.opp` performance
     * trace (CPU scope spans + GPU zones + census, keyed per present /
     * canonical tick). Off by default. See core/include/openpete_profile.h,
     * scripts/profile-to-trace.py, scripts/profile-summary.py. */
    const char* profile_out_path;

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

    /* --record-fps-overlay 0|1: draw the native vk FPS counter INTO captured
     * video (--record). Off by default so recordings show the clean game image;
     * 1 burns the overlay in (verification captures). -1 = unset (defer to the
     * OPENPETE_FPS_IN_RECORD env, else off). Display-only. */
    int         record_fps_overlay;

    /* --record-cfr RATE: capture at a constant frame rate instead of true VFR.
     * RATE must be an integer multiple of the NTSC sim-tick rate (1125000/37609
     * = 29.913052…): 29.913, 59.826, 119.652, ... Any other value is rejected,
     * because a non-integer ratio makes the sub-tick interpolator emit an
     * uneven number of intermediate presents per tick — the very cadence jitter
     * CFR exists to remove. Rounded spellings (30, 60, 120) are accepted and
     * snapped to their true neighbour. 0 = off (VFR).
     *
     * The mode is more than a timestamping change: it drives the host clock
     * from the pacer (op_virtual_clock_enable) so exactly RATE/29.913 presents
     * are produced per sim tick regardless of how fast the machine renders, and
     * stamps frame N at exactly N/RATE. Two captures of the same window at
     * different rates therefore cover the identical span of game time.
     * Wall-clock never enters the output. */
/* Largest accepted --record-cfr multiple of the tick rate (16x = 478.6 fps).
 * Presents-per-tick above this is well past any display and just multiplies
 * encode cost. */
#define OPENPETE_RECORD_CFR_MAX_MULT 16
    double      record_cfr;

    /* --record-frames N: finish the recording (and exit) after exactly N
     * encoded frames. With --record-cfr this pins the captured window to an
     * exact game-time length: N / RATE seconds. 0 = unlimited. */
    int         record_frames;

    /* --texpack-dir PATH: directory of HD texture-replacement PNGs (the texture
     * pack). NULL = unset (defer to the OPENPETE_TEXPACK_DIR env, else the
     * built-in <exe>/assets/textures/). Native renderer only; display-only,
     * never perturbs gameplay state. */
    const char* texpack_dir;
    /* --sfxpack-dir PATH: directory of drop-in sound replacements (WAVs
     * named by the sfx census); NULL = env OPENPETE_SFXPACK_DIR /
     * <exe>/assets/sfx. Mods' assets/sfx/ always join. */
    const char* sfxpack_dir;

    /* --controller-db PATH: an extra SDL controller-mapping database, loaded
     * after the shipped and per-user ones and so outranking both. NULL =
     * unset (defer to the OPENPETE_CONTROLLER_DB env). Host input only;
     * a mapping decides which physical button reaches the decode path, never
     * what the guest reads for a given decoded button. */
    const char* controller_db;

    /* --mods-dir PATH: mods root (each subdir with a mod.toml is a mod;
     * enabled.txt fixes the order = priority). NULL = unset (defer to
     * env OPENPETE_MODS_DIR, else <exe>/mods). A missing directory means
     * zero mods and a byte-identical modless run. See
     * notes/plans/openpete-modding-framework.md. */
    const char* mods_dir;

    /* --mods ID[,ID...]: enabled modset override — replaces the mods dir's
     * enabled.txt entirely, same semantics (order IS priority, later =
     * higher). An empty list ("--mods \"\"") enables nothing, i.e. a
     * one-flag vanilla run without touching the install. NULL = unset
     * (defer to env OPENPETE_MODS, else enabled.txt, else all discovered
     * mods in lexicographic order). */
    const char* mods_enabled;

    /* --compile-mods: utility mode — compile EVERY mod with src/ in the
     * resolved mods dir (ignoring enabled.txt) against <exe>/sdk through
     * the loader's own key/stamp/compile path, then exit (nonzero if any
     * compile failed). The cmake `spyro1_mods` build step for native
     * presets; also pre-warms a dist install's compile-on-boot cache. */
    int         compile_mods;

    /* --dump-sfx-pack DIR: utility mode — walk every sample on --disc
     * (and the custom-level Data files in --dump-sfx-wads, comma list)
     * and write the sfx pack census (WAVs named by content hash +
     * index.txt) to DIR, then exit. */
    const char* dump_sfx_pack_dir;
    const char* dump_sfx_wads;

    /* openpete.toml utility modes — resolve, do the one thing, exit 0.
     * Handled by game main alongside --compile-mods, so the whole config
     * stack is already resolved by the time they run. */
    int         config_report;   /* --config-report: print the resolution table */
    int         keys_report;     /* --keys-report: print the resolved keymap */

    /* Host-backed memory card (save/load). The game reaches the card only
     * through the PSYQ MemCard* wrappers, which openpete backs with real
     * 128 KiB .mcr images in the `cards` directory openpete_paths.h
     * resolves. Pure host-side save I/O — never touches gameplay-readable
     * state. See openpete_memcard.h. */
    const char* card_dir;      /* --card-dir PATH: dir holding cardN.mcr (NULL = the resolved cards dir; env OPENPETE_CARD_DIR) */
    const char* card1_path;    /* --card1 PATH: explicit port-A image (NULL = <card_dir>/card1.mcr) */
    const char* card2_path;    /* --card2 PATH: explicit port-B image (NULL = <card_dir>/card2.mcr) */
    int         card_a_present;/* card in port A (default 1) */
    int         card_b_present;/* card in port B (default 1) */
    int         card_unformatted; /* --card-unformatted: provision a blank card so the in-game format UI runs (default 0 = pre-formatted) */
    int         adopt_saves;   /* --adopt-saves: re-tag card modset sidecars to the running modset at open */
    const char* resume_handoff;/* --resume-handoff: process-handoff checkpoint to restore at first quiescence */
    int         handoff_argc;  /* original argv snapshot for the handoff exec */
    char**      handoff_argv;

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

    /* --cheats a,b,...: comma-separated names of the game's OWN built-in
     * cheat codes to apply automatically, so the pad code never has to be
     * entered. The per-game feature module owns the name table and applies
     * the effects once gameplay is live (the globals they write don't exist
     * before a level loads, and New Game / save-load rewrite them). NULL =
     * none. Deliberately gameplay-affecting — the same class as the debug
     * free camera, not a presentation flag. */
    const char* cheats;
} openpete_options_t;

/* Defaults: 0 / unlimited / not-headless / log plain. */
void openpete_options_init(openpete_options_t* opts);

/* Set the game's starting renderer and the one help calls the default. Call
 * between openpete_options_init and openpete_options_parse. */
void openpete_options_set_default_renderer(openpete_options_t* opts,
                                           openpete_renderer_t r);

/* Parse argv into opts. Unknown flags return OPENPETE_EXIT_SETUP_ERROR;
 * caller should propagate the exit code. argv[0] is treated as program name. */
int  openpete_options_parse(openpete_options_t* opts, int argc, char** argv);

/* Validate that an EXE source exists — call AFTER any library/ingest
 * resolution (see disc-library, distribution D2.5). */
int openpete_options_require_exe_source(const openpete_options_t* opts);

/* Run the main loop. Returns one of OPENPETE_EXIT_*. */
int  openpete_run(const openpete_options_t* opts);

/* Ask for a clean shutdown from anywhere (settings-menu EXIT row, the RHI on an
 * unrecoverable device loss, the SDL window-close path). Sets the cooperative
 * flag the host-fiber loop polls once per iteration, so teardown runs through
 * the normal drain instead of exiting from inside a scene. */
void openpete_request_shutdown(void);

/* Live-set the render-fps target (present pacing) at runtime — e.g. from the
 * in-game settings menu. 0 = match sim (~30 Hz). The host loop recomputes the
 * pacer period and re-anchors its deadlines on the next iteration (no catch-up
 * burst). Pure pacing; never perturbs gameplay-readable state. See
 * notes/plans/openpete-ingame-settings-menu.md. */
void openpete_set_render_fps(int fps);

/* The live render-fps target the pacer is currently driving — the resolved
 * launch value (an unset --render-fps defaults to the display refresh rate,
 * windowed) or whatever openpete_set_render_fps last applied. 0 = legacy
 * match-sim pacer. The settings menu seeds its FPS row from this. */
int openpete_get_render_fps(void);

/* Live tick-scale (engine slow-mo/speed-up; sim and presents together).
 * Clamped to [0.05, 8]. The host loop re-derives every scaled period and
 * re-anchors its deadlines on the next iteration. Pacing only — the sim
 * executes identical ticks on a stretched wall clock. */
void   openpete_set_tick_scale(double scale);
double openpete_get_tick_scale(void);

/* Live runahead depth (0 = off, clamped 0..3). The host loop re-arms on its
 * next iteration: the mod-capability gate re-runs on first arm and the
 * ~2 MiB speculation snapshot allocates lazily; the getter returns the
 * APPLIED depth (0 when the gate refused). Presents snap for one tick on a
 * depth change (interp ring reset across the timeline swap). */
void openpete_runahead_set(int n);
int  openpete_runahead_get(void);

/* Live engine-level mute (SPU AL + XA gain + music WAV override) — the same
 * clamps --mute applies at boot, callable at runtime. Host audio only; the
 * F7 pause exit restores this baseline. */
void openpete_set_mute(int on);
int  openpete_get_mute(void);

/* Live in-game volume-slider presets (0..10, -1 = leave the game's slider
 * alone) — the --sfx / --music values, re-settable at runtime. These hold a
 * host-side REQUEST only: the per-game audio-volume feature reads them on its
 * own tick hook and performs every guest write there, so a UI-thread edit
 * never touches gameplay-readable RAM off-schedule. */
void openpete_set_sfx_volume(int v);
int  openpete_get_sfx_volume(void);
void openpete_set_music_volume(int v);
int  openpete_get_music_volume(void);

/* Live controller-vibration strength (0..10, -1 = leave the game's own
 * setting alone) — the --vibration value, re-settable at runtime. Like the
 * sliders above this is a host-side REQUEST: the per-game vibration feature
 * performs the guest write on its tick hook, while the host half (motor
 * amplitude) is applied on the pump thread. */
void openpete_set_vibration(int v);
int  openpete_get_vibration(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_H */
