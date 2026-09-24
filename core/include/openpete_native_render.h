/* core/include/openpete_native_render.h — native Vulkan renderer A/B harness.
 *
 * The host-side entry points for openpete's native renderer (the Phase 1c
 * foundation of notes/plans/openpete-native-renderer.md). The native
 * renderer is a pure *display* consumer: it reads gamestate + asset data +
 * the VRAM mirror and draws into its own Vulkan window. It NEVER writes
 * PSX-readable RAM, so it is determinism-safe by construction — the
 * original `.s` renderers keep running (here, via PsyCross) as the headless
 * gameplay-verdict producers, and the oracle gate is unaffected.
 *
 * Two-window model: a single SDL window cannot host both PsyCross's OpenGL
 * context and our Vulkan surface, so the native renderer creates its own
 * window. During development it runs ALONGSIDE a visible PsyCross window for
 * A/B comparison; the end state hides PsyCross (FBO-only, read back as the
 * "reference") and the Vulkan window is the only one shown.
 *
 * Gated entirely by --renderer vk. With --renderer psycross (the
 * default) every entry point is an inert no-op, so normal/headless runs are
 * byte-for-byte unaffected.
 */
#ifndef OPENPETE_NATIVE_RENDER_H
#define OPENPETE_NATIVE_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "openpete.h" /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the native Vulkan window + RHI device. Call AFTER PsyX_Initialise
 * (SDL_INIT_VIDEO must already be up). `win_w`/`win_h` are the resolved
 * window size (the same the caller hands PsyCross), so the native window
 * matches the game's window rather than a fixed default. Returns 0 on success
 * OR when the native path is simply not requested; returns non-zero only on a
 * real bring-up failure (the caller logs and continues without the native
 * path — it is never fatal to the game). */
/* Hand the window icon over before bring-up. RGBA32, w*4 stride. Applied to the
 * native window when it is created — in the default path PsyCross never makes a
 * window, so this is the only window an icon can land on. */
void openpete_native_set_icon(const void* rgba32, int w, int h);

/* Create the visible window up front, on the caller's thread, for bring-up to
 * adopt — the same hand-over the first-run splash uses. Win32 delivers a
 * window's messages only to the thread that created it, so the window must
 * belong to whichever thread runs the event pump. No-op unless this is the
 * single-window vk path, or a window was already handed over. */
void openpete_native_precreate_window(const openpete_options_t *opts,
                                      int win_w, int win_h);

int openpete_native_render_init(const openpete_options_t *opts, int win_w, int win_h);

/* True once native bring-up succeeded: a Vulkan device exists and this process
 * composes through it. Says nothing about where the picture comes from — it is
 * true in PsyCross display mode, where the window shows PsyCross's read-back
 * frame. Nor does it promise a window: headless bring-up is surfaceless and
 * still sets this, so a consumer that needs an actual display must check
 * headless separately. */
bool openpete_native_render_enabled(void);

/* True when a native SCENE is being extracted, interpolated and drawn: the
 * feature modules feed the ring, the sub-tick spine re-extracts, and compose
 * walks the tri list. Implies openpete_native_render_enabled(); the converse
 * does not hold. Every consumer that walks, extracts from, or composes the
 * native scene asks this one, not the display predicate above. */
bool openpete_native_scene_enabled(void);

/* Device identity for the pacing log's environment block. Both return a
 * static string valid for the process ("unknown" / "none" before the device
 * exists, and in PsyCross-only runs). The present mode is the one actually
 * granted, which is the only one a baseline may be keyed on. */
/* Presents dropped because the swapchain slot had not retired — see
 * rhi_fence_sheds. Monotonic. */
uint64_t openpete_native_render_fence_sheds(void);

/* Census (OPENPETE_SCENE_SIG=1): hash the composed scene's DISCRETE fields and
 * report them with the present's alpha. Answers whether re-extracting per
 * present ever decides differently within one tick's alpha span. */
void openpete_native_scene_sig_report(int canonical);

/* Wait (up to timeout_ns) for the last queued present to be DISPLAYED.
 * 1 = displayed, 0 = timed out, -1 = unsupported / nothing outstanding.
 * Presenting-thread only. */
int openpete_native_render_present_outstanding(void);
int openpete_native_render_present_wait(uint64_t timeout_ns, uint64_t *out_id,
                                        uint64_t *out_waited_ns);

/* The acquire path's refusals, and the granted swapchain depth. See
 * rhi_acquire_sheds / rhi_swapchain_image_count. */
uint64_t openpete_native_render_acquire_sheds(void);
uint64_t openpete_native_render_acquire_timeouts(void);
uint32_t openpete_native_render_swapchain_images(void);

const char* openpete_native_render_gpu_name(void);
const char* openpete_native_render_driver_version(void);
const char* openpete_native_render_present_mode(void);

/* Toggle the native window between windowed and borderless-desktop fullscreen.
 * Bound to Alt+Enter and double-click by run.c, and the O-menu DISPLAY row.
 * No-op until the native window is up (so it's inert in headless /
 * PsyCross-only runs). */
void openpete_native_toggle_fullscreen(void);
/* Thread-safe request form: raises a flag the host loop services on the
 * window-owning thread. Use this from the menu / overlay / any non-window
 * thread; call the toggle directly only from the window's own thread. */
void openpete_native_request_fullscreen_toggle(void);

/* Live fullscreen state (1 = borderless-desktop fullscreen, 0 = windowed or
 * no native window). The O-menu DISPLAY row reads this each frame instead of
 * keeping a shadow index, so it stays in sync with Alt+Enter / double-click. */
int openpete_native_fullscreen_active(void);

/* The native SDL window, or NULL when there is none. */
struct SDL_Window* openpete_native_render_window(void);

/* Performance-stats overlay (vk live window). Drawn only on the live vk
 * present (never screenshots). The level is one of the
 * OPENPETE_PERF_* values in openpete_perf_stats.h, which also owns the
 * numbers; the renderer holds the level because it gates the draw and sizes
 * the supersample region. Driven by --perf-stats, the Performance stats row
 * in the M overlay, and the O menu's SHOW FPS row. Display-only. */
void openpete_native_set_perf_stats(int level);
int  openpete_native_perf_stats_level(void);

/* Two-state view of the same setting, for the O menu row and the legacy
 * --show-fps flag: OFF is level 0, ON restores the last non-zero level, so a
 * toggle never quietly demotes a detailed readout to the bare counter. */
void openpete_native_set_fps_overlay(int on);

/* --record-fps-overlay: whether the FPS overlay is drawn INTO --record video.
 * -1 = unset (defer to the OPENPETE_FPS_IN_RECORD env, else off), 0/1 explicit.
 * Set once at init from opts. Only affects presents while a recording is active;
 * the live window always follows set_fps_overlay above. */
void openpete_native_set_fps_in_record(int pref);

/* Is the overlay on at all (any level)? The game-side glyph emitter reads this
 * to skip building geometry when the overlay is off. */
int  openpete_native_fps_overlay_enabled(void);
/* The current rolling FPS value the overlay displays. The game-side emitter
 * reads it to know which digit glyphs (0-9) to build. */
int  openpete_native_fps_value(void);

/* Live presentation setters (the in-game settings menu drives these). All
 * display-only: they mutate renderer/GPU state only, never gameplay-readable RAM,
 * and take effect on the next native present. Harmless (no visible effect) when
 * the native renderer isn't the active present path. See
 * notes/plans/openpete-ingame-settings-menu.md. */

/* Target display aspect (W/H, e.g. 16.0f/9.0f). <= 0 → AUTO (the default):
 * follow the window/fullscreen shape live, filling it with no bars. >0 →
 * FIXED: when windowed, resizes the window width to match (like --aspect at
 * launch); when fullscreen — or after the user resizes away — the window
 * letterboxes/pillarboxes the fixed aspect (vk-only Hor+). Forces a layout
 * recompute so the fitted panel rect updates next present. */
void openpete_native_set_aspect(float w_over_h);
/* The aspect as last requested (0 = AUTO). The config-knob row reads it so an
 * O-menu pick registers as an edit — same reason as the SSAA/VSync getters. */
float openpete_native_get_aspect(void);

/* SSAA supersampling factor (1 = off, 2/4/8/16). Clamped to [1,32]; ensure_ss_rtt
 * caps it to the GPU max image dimension and reallocs the supersampled scene
 * render target on the next present (a one-time wait-idle, like a window resize). */
void openpete_native_set_ssaa(int factor);
/* The requested factor (not the GPU-capped effective one), 1 when unset. Both
 * the O menu and the M overlay move this state, so the knob row must read it
 * back here rather than shadow it — otherwise a pick made in one surface is
 * invisible to the other (and to saving settings). */
int openpete_native_get_ssaa(void);

/* VSync on/off (1 = FIFO / vsync ON, 0 = immediate|mailbox / vsync OFF). Forwards
 * to the RHI, which flags the swapchain for recreation and rebuilds it (with the
 * new present mode) on the next frame. No-op until the native device is up. */
void openpete_native_set_vsync(int on);
/* Current vsync preference: 1 = FIFO, 0 = vsync-OFF, -1 = unset (no device yet,
 * or launched without --vsync so the env/surface decides). */
int openpete_native_get_vsync(void);

/* Vertex screen-coordinate precision (0 = smooth sub-pixel, 1 = PSX integer-grid
 * snap = the original polygon wobble). Rounds only 3D-world display geometry in
 * the next scene feed (2D HUD/overlay stays smooth; depth + UVs untouched).
 * Display-only: touches only renderer state, never gameplay RAM. */
void openpete_native_set_vertex_precision(int integer_snap);

/* Live vertex-snap state (1 = integer snap active). Feeders that synthesize
 * vertices ON another primitive's edge (the terrain near/HQ subdivide's
 * boundary midpoints) read this to emit snap-consistent screen coordinates,
 * keeping their T-vertices watertight against snapped neighbour edges. */
int openpete_native_vertex_snap_active(void);

/* Sticky feed state: tris added while set are ALREADY snap-consistent — the
 * renderer's integer-snap chokepoint leaves their coordinates untouched. Set
 * by feeders that must control vertex placement beyond per-vertex snapping
 * (terrain: subdivide midpoints live at exact off-integer positions ON
 * snapped parent edges). Clear after the feed, like set_tag. */
void openpete_native_scene_set_presnap(int on);

/* P6b: true when the terrain LP key-resolve contest is live (key pass +
 * key-capable pipelines came up). The terrain feeder emits scene-tri koff
 * only when this holds, so LP fragments can never contest a missing image. */
int openpete_native_key_resolve_on(void);

/* Raster output quantize: box-resample the composed 3D scene (pre-HUD) onto
 * the 512Nx240N virtual PSX raster (N=1 = authentic 240p; 0 = off). An integer
 * multiplier of the vertex-snap grid, so snapped edges land on cell boundaries
 * at every setting and the snap's seams shrink to <=1-cell tints.
 * Display-only: touches only renderer state, never gameplay RAM. */
void openpete_native_set_raster_quantize(int mult);
/* Current multiplier as resolved/last set (0 = off). */
int  openpete_native_get_raster_quantize(void);

/* PS1 15-bit colour (0 = off, 1 = RGB555 truncation + the GPU's 4x4 dither
 * matrix on the PSX framebuffer lattice), an engine postfx below the mod
 * chain. Live: the enable is checked per present, no pipeline rebuild.
 * Display-only: touches only renderer state, never gameplay RAM. */
void openpete_native_set_dither(int on);
int  openpete_native_get_dither(void);

/* Texture/colour interpolation mode (0 = perspective-correct, 1 = PSX affine
 * = the original screen-linear texture swim). The mode is a vertex spec
 * constant baked into every scene pipeline, so the setter QUEUES the change
 * and the renderer re-bakes all pipelines at the next present boundary (a
 * one-time device-idle hitch). Display-only: never touches gameplay RAM. */
void openpete_native_set_texture_mapping(int affine);
/* Live mode (the last APPLIED value; a queued change reads old until the
 * next present applies it). */
int  openpete_native_get_texture_mapping(void);

/* Untextured lighting view: textured scene fragments output the raw gouraud
 * vertex colour (texel replaced with modulation-neutral grey; transparency
 * cutouts keep real silhouettes). Same queued-rebuild contract as
 * openpete_native_set_texture_mapping. Debug view; display-only. */
void openpete_native_set_notex(int on);
int  openpete_native_get_notex(void);

/* Wireframe view: every 3D scene draw rasterizes as triangle edges coloured
 * by the gouraud vertex lighting (the NOTEX colouring); the 2D layers stay
 * filled. Seeded from --wireframe / OPENPETE_WIREFRAME at init; same
 * queued-rebuild contract as openpete_native_set_texture_mapping.
 * Display-only. */
void openpete_native_set_wireframe(int on);
int  openpete_native_get_wireframe(void);

/* Terrain candidate-set policy (openpete_terrain_visibility_t): GAME = the
 * game's own visibility verdict, FRUSTUM = every sector whose bounding sphere
 * is inside the displayed frame. Seeded from --terrain-visibility at init; the
 * terrain feeder reads it per walk, so the setter takes effect on the next
 * present. Display-only: the feeder reads the game's visibility flags and
 * never writes them. */
void openpete_native_set_terrain_visibility(int mode);
int  openpete_native_get_terrain_visibility(void);

/* One vertex of game-supplied FPS-overlay geometry, in a resolution-independent
 * overlay space: x/y are in units of WINDOW HEIGHT (origin = top-left, +y down,
 * so x and y share one scale → digits stay square at any aspect); z is a
 * view-Z (>= 1, the scene vertex unit) for the overlay's own mini depth pass
 * (smaller = nearer). rgba is byte order R,G,B,A. */
typedef struct { float x, y, z; uint32_t rgba; } openpete_hud_vert;

/* Hand the native renderer overlay geometry built from the game's own 3D
 * glyph models (game_layout.font). `verts` is a triangle list (3 verts per
 * tri), built each tick by the game-side emitter (text_moby_native). The
 * renderer copies them and draws them (depth-tested, on top) on the live vk
 * present. nverts==0 draws no readout: there is no other font (e.g. before
 * the glyph models have loaded). The performance readout's channel.
 * Display-only; never touches gameplay state. */
void openpete_native_set_overlay_glyphs(const openpete_hud_vert *verts, int nverts);

/* The overlay block's REGION rect in overlay space (window-height units), set
 * by the game-side builder — it is the side that can measure a string, so it
 * owns the number and the renderer reads it back rather than re-deriving it.
 * The supersample target, the composite quad and the glyph projection all use
 * this one rect; a disagreement lands the text in a shape other than the one
 * it was rasterized into. Glyphs are emitted REGION-LOCAL (origin at the
 * rect's top-left); the anchor decides where the rect goes on screen. */
void openpete_native_set_overlay_extent(float w, float h);
void openpete_native_get_overlay_extent(float *w, float *h);

/* The build-version line: the same 3D glyph vertices, plus the region rect
 * they were emitted region-local to, in ONE call — the two are read together
 * on the present thread and must describe the same push. The renderer pins it
 * to the bottom-left anchor, holds it briefly on a tagged build and fades it,
 * and keeps it up for the whole session on a build that is not exactly a
 * release. nverts==0 clears it. Display-only. */
void openpete_native_set_version_glyphs(const openpete_hud_vert *verts, int nverts,
                                        float region_w, float region_h);

/* --version-osd's own value. Live-settable. It governs the RELEASE build's
 * two-second line alone: an untagged shared build keeps its line for the
 * whole session and no knob suppresses it, and a build that is not
 * shared-recomp shows none either way. */
void openpete_native_set_version_osd(int on);
int  openpete_native_version_osd(void);

/* The three facts above resolved into one answer: whether a version line will
 * be drawn in this build. The game-side builder asks before spending glyph
 * geometry on it. */
int openpete_native_version_line_enabled(void);

/* One phrase naming why, for the shutdown census. Never NULL. */
const char *openpete_native_version_reason(void);

/* Version lines actually drawn, cumulative — the engagement counter a check
 * harness reads to tell a drawn line from a suppressed one. */
unsigned openpete_native_version_drawn(void);

/* Which corner/edge the performance-stats block anchors to
 * (openpete_overlay_anchor.h's openpete_anchor_t). Live-settable. The builder
 * reads it back to align each line inside the region — right-anchored text
 * right-aligns, centred text centres — so the block's slack sits on the side
 * away from the screen edge it hugs. */
void openpete_native_set_perf_anchor(int anchor);
int  openpete_native_perf_anchor(void);

/* Mod draw-service overlay (core/mods/mod_draw.c): rects + text glyphs in the
 * same overlay space, pushed once per present. Drawn on the vk variant in the
 * live present AND screenshot/record composes (mod HUD is content, not
 * chrome). nverts==0 clears it. Display-only. */
void openpete_native_set_mod_overlay(const openpete_hud_vert *verts, int nverts);

/* Window aspect (w/h) for overlay-space layout: x spans [0, aspect], y spans
 * [0, 1]. Falls back to 4:3 before the window exists. */
float openpete_native_overlay_aspect(void);

/* Output pixels per overlay unit — i.e. the window height. A game-side text
 * builder needs it to SNAP a computed origin to a whole pixel: text that is
 * right-aligned or centred derives its origin from the measured string, so
 * the origin moves by a fraction of a pixel whenever the text changes, and
 * the glyphs land at a different sub-pixel phase each time. Left-aligned text
 * never had the problem because its origin is a constant. 0 before the window
 * exists. */
float openpete_native_overlay_px_per_unit(void);

/* Capacity of the settings menu's 3D glyph layer, in vertices: the rows'
 * labels, values and title, the close glyph, and the lit frame. The emitter
 * and the renderer size their buffers from this one value. */
#define OPENPETE_MENU_GLYPH_MAX 65536

/* Hand the native renderer the in-game settings menu's glyph geometry (the row
 * labels/values/title, built game-side by text_moby_native from the same 3D gold
 * glyph mobys, with the selected row's per-glyph yaw baked in). Same overlay
 * space as set_overlay_glyphs; drawn by draw_menu_overlay on the live vk present
 * over the menu panel. nverts==0 clears it. Display-only. */
void openpete_native_set_menu_glyphs(const openpete_hud_vert *verts, int nverts);

/* ---- OSD toast geometry ------------------------------------------------
 * The OSD renders into a FIXED SLOT GRID: one supersample target holding
 * OPENPETE_OSD_MAX stacked slots of identical extent, reallocated only when
 * the window resizes. Content-sized targets were the obvious design and are
 * wrong — every SS target rebuild waits the device idle, and toast arrivals,
 * expiries and coalescing repaints happen many times a second.
 *
 * The builder emits each toast's border and text into its slot, in
 * TARGET-LOCAL overlay space (slot i's top-left is at y = i * slot height),
 * and reports which toast landed in which slot plus the box it actually
 * filled. The compositor samples that sub-rect and places it by the anchor
 * ledger at the toast's own alpha.
 *
 * The table is keyed by TOAST ID, never by index: the ring is mutated on
 * other threads between this push and the present that consumes it. Geometry
 * and table are published TOGETHER, under one lock — an id-keyed table is
 * worthless if it can itself be read half-copied. */
typedef struct {
    uint32_t id;         /* the toast this slot holds                     */
    int      slot;       /* its row in the grid                           */
    float    box_w;      /* extent actually filled, overlay units         */
    float    box_h;
} openpete_osd_slot_t;

void openpete_native_set_osd_glyphs(const openpete_hud_vert *verts, int nverts,
                                    const openpete_osd_slot_t *slots, int nslots);

/* The slot grid's geometry, so the builder lays out against the same numbers
 * the compositor samples with. Extents are overlay units (window-height
 * fractions); the count is OPENPETE_OSD_MAX. */
void openpete_native_osd_slot_extent(float *w, float *h);

/* Toasts composited so far this run. An engagement counter: a capture-based
 * gate reading zero here never drew a toast, whatever its pixels say. */
unsigned openpete_native_osd_composited(void);

/* Path-isolation mode (--render-paths). When on, a frame with no native scene
 * geometry presents the scene background instead of re-displaying the PSX VRAM
 * framebuffer — the VRAM is the UNFILTERED frame, so falling back to it would
 * show every path and defeat the isolation. The game enables this when a
 * --render-paths selection is in effect. Generic core toggle; no-op until set. */
void openpete_native_set_isolation(bool on);

/* Record the active PSX display framebuffer (DISPENV `disp` rect, VRAM px —
 * the scanned-out double-buffer page) and the drawn region (DRAWENV `clip`
 * rect). The native renderer crops the disp vertically to the clip so it
 * drops the overscan letterbox and matches the bars-free reference. Called
 * from the PutDispEnv / PutDrawEnv thunks each frame; cheap stashes that are
 * inert when the native path is disabled. */
void openpete_native_set_display_rect(int x, int y, int w, int h);
void openpete_native_set_draw_clip(int x, int y, int w, int h);

/* ---- native 3D scene (Phase 2: cyclorama/sky from gamestate) --------
 *
 * The game-specific layer (games/<game>/) re-derives 3D geometry from
 * gamestate in float and pushes screen-space triangles here; core draws
 * them natively into panel 0 (the "native render", replacing the
 * re-displayed PsyCross framebuffer). Coordinates are PSX screen pixels
 * (the same space the GTE projects into: x in [0,disp_w], y in [0,disp_h],
 * OFX/OFY-centred); core maps them into the panel, cropping the overscan
 * band like the reference. rgba is byte order R,G,B,A.
 *
 * Determinism-safe: this is host-side display state only; the game's push
 * reads gamestate but writes nothing PSX-readable. Inert unless the native
 * path is live. */
/* z is the ORDERING view-Z the native depth buffer is written from (3a.3):
 * full-world view-Z, >= 1 (see openpete_native_scene_viewz). Actors map their
 * view-space Z through openpete_native_scene_viewz; the cyclorama/sky passes
 * OPENPETE_DEPTH_VIEWZ_FAR (always the backdrop); screen-space 2D content
 * passes 1.0. */
/* u,v are texel coords within the texture page (PSX integer texel space, 0..255)
 * — only meaningful when the tri is textured. */
/* pvz is the TRUE full-world view-Z of this vertex — the perspective W the
 * rasterizer divides UV/colour by. It is separate from `z` because `z` is an
 * ORDERING quantity: feeders shift it (openpete_worldvz_pull/_bias, the OT-slot
 * mirrors) to reproduce the PSX's per-object OT placement, and a shifted depth
 * implies a wrong W. Where the shift is large — the level-transition flight
 * pulls the whole player 2488 world units so its nearest vertex sits on the
 * near plane — W varies 2:1 across a face that truly spans 1.01:1, smearing one
 * texel over the snout. State the real view-Z here and `z` stays free to encode
 * ordering. MANDATORY (> 0): an unshifted feeder states the same value as `z`;
 * screen-space 2D content states 1.0. A vertex that reaches
 * openpete_native_scene_add_tri with pvz <= 0 is counted
 * (openpete_native_scene_pvz_missing) and takes its `z`. */
typedef struct { float sx, sy, z; float u, v; uint32_t rgba; float pvz; } openpete_scene_vert;
/* A scene triangle. When `textured` is 0 it is gouraud-shaded from the vertex
 * rgba (the cyclorama, and untextured moby faces). When 1, it samples the PSX
 * VRAM mirror via the material below (tpage/CLUT/bpp in VRAM-pixel units, exactly
 * the GTE/GPU encoding) and modulates by rgba (tex*colour/128). 3a.3.
 * A textured tri's vertex ALPHA byte is not a blend alpha. 0xFF, which every
 * feeder but the moby walk states, draws the tri's CLUT row alone. A value
 * with a high nibble h below 15 and a nonzero low nibble q, constant over the
 * tri, blends the drawn colour by q/30 toward the colour the same CLUT index
 * h - 7 rows from clut_y draws (the moby fog band between two ticks;
 * psx_scene.frag clut_fog_mix). */
/* blend: 0 = opaque; 1 = PSX semi-transparent ABR mode 0 ((B+F)/2) -> alpha blend;
 * 2 = ABR mode 1 (B+F) -> additive blend (3b.1, flame/glows: src ONE + dst ONE);
 * 3 = ABR mode 2 (B-F) -> subtractive blend (3b.2, shadows: reverse-subtract,
 * src ONE + dst ONE — the shadow colour darkens the framebuffer). All
 * transparent classes are depth_write_off (test vs opaque, never occlude
 * another transparent layer). ABR mode 3 (B+F/4) + back-to-front sort are still
 * TODO (no consumer yet).
 *   TEXTURED tris go through the VRAM sampler (tex*col/128) for every class.
 *   UNTEXTURED gouraud tris support classes 0 (opaque), 1 (alpha — uses the
 * vertex rgba ALPHA byte; set 128 for (B+F)/2), 3 (subtract — reverse-
 * subtracts the vertex colour, alpha ignored) and 2 (additive, B+F — the glow
 * rings: gouraud fan, bright centre fading to black edge, alpha ignored). */
/* category: which draw layer an OPAQUE (blend==0) tri belongs to. The opaque
 * pass is split WORLD-opaque -> shadows(subtract) -> ACTOR-opaque so a shadow
 * (a ground decal pulled slightly toward the camera to clear terrain bumps)
 * draws OVER the terrain it falls on but UNDER the actors standing on it — the
 * PSX draw order (terrain, then shadow, then Spyro on top). Ignored for
 * non-opaque tris (blend!=0 routes by blend). Set by add_tri from its explicit
 * `category` argument; feeders that don't care pass OPENPETE_SCENE_WORLD. */
#define OPENPETE_SCENE_WORLD 0
/* ACTOR — regular + shaded mobys (the rescued dragon, level mobys, gems, …). Drawn
 * after shadows, then the HUD/overlay layer goes ON TOP of them. */
#define OPENPETE_SCENE_ACTOR 1
/* HUD/overlay mobys — the elements the PSX forces into the moby OT's NEAR slot:
 * the treasure/lives HUD icons, the "DEMO MODE" banner, and the level-intro /
 * "RESCUED <dragon>" text. Drawn with the depth test OFF, in the layer BETWEEN the
 * ACTOR group (regular mobys) and the PLAYER group (Spyro), so they sit on top of
 * the world/terrain AND other mobys (not Z-occluded; the near-OT slot draws over
 * other mobys like the rescued dragon) but write no depth — so the player Spyro,
 * drawn next, still covers them (Spyro flies in front of the intro text). Mirrors
 * the PSX moby OT: near-slot UI over other mobys, the player rendered on top.
 * WITHIN the layer, untextured tris draw back-to-front by centroid z (stable:
 * equal-z 2D content keeps submission order) — the depth-off analogue of the PSX
 * OT bucket order, so the rotating near-OT mobys (the inventory screen's spinning
 * letters/dragon icons) self-occlude correctly instead of painting inside-out.
 * Routed independently of `blend`. Tagged by the feeder from the UI-moby classifier
 * OR the .s billboard OT-bias (see moby_native near_ot). */
#define OPENPETE_SCENE_HUD   2
/* PLAYER — the player Spyro's body. An opaque actor like ACTOR, but drawn AFTER the
 * HUD/overlay layer so Spyro covers the near-OT HUD text (the level-intro fly-in),
 * whereas regular mobys (ACTOR) stay UNDER it. The only player feeder is
 * spyro_native; everything else uses ACTOR. */
#define OPENPETE_SCENE_PLAYER 3
/* OVERLAY — 2D prims from the HUD ordering table (func_800168DC), which the PSX
 * draws ON TOP OF THE ENTIRE 3D scene (a separate OT presented over the scene OT):
 * the cutscene letterbox bars, and any full-screen 2D flat quad. Drawn DEAD LAST
 * with the depth test off, OVER everything incl. the player. (The level-intro /
 * "RESCUED" text is NOT here — that is moby near-OT geometry = HUD, under the
 * player.) Untextured opaque; routed by the feeder. */
#define OPENPETE_SCENE_OVERLAY 4
/* HUD_PANEL — a semi-transparent 2D HUD-OT quad that sits UNDER the HUD text/icons
 * but OVER the world/backdrop: the dim panel behind the pause / inventory menu text
 * (func_8001A40C, setRGB0(64,64,64)) and the "shaded box" behind in-game HUD text
 * (func_8001860C, setRGB0(112,112,112)), both emitted as a POLY_F4 code 0x2A via
 * func_800168DC. Depth-test OFF, drawn at the START of the HUD layer (before the HUD
 * text), so the text reads on top. Distinct from OVERLAY (dead-last, over text) and
 * from the full-screen fade (also a 0x2A F4, but emitted by func_800190D4 — the
 * feeder excludes that via a re-entry flag). Untextured. The 0x2A code only means
 * "semi-transparent"; the actual ABR RATE comes from the box's DR_MODE (getTPage abr)
 * — BOTH these boxes set ABR mode 2 (B-F subtractive), which DARKENS the framebuffer
 * by the box colour (NOT (B+F)/2 alpha — that washes it toward grey). The feeder reads
 * the ABR per-prim into `blend` (3 = subtractive, 1 = alpha), and the HUD-panel draw
 * picks the matching pipe per box. */
#define OPENPETE_SCENE_HUD_PANEL 5
/* HUD3D — the in-game 3D HUD icon mobys (g_Hud.m_Mobys: the spinning gem/treasure,
 * dragon-head, Spyro-head/lives, key, and number digits). Unlike the flat 2D HUD
 * layer, these are real 3D models, so they need a Z-buffer to sort their own faces
 * (depth-test OFF makes them paint inside-out). Drawn LAST, on top of everything,
 * after a mid-pass depth clear (rhi_clear_depth) so they sort internally yet are
 * never occluded by world geometry. Only the static g_Hud range routes here
 * (openpete_ui_moby_is_addr_static); the polybuf text mobys stay flat 2D = HUD. */
#define OPENPETE_SCENE_HUD3D 6
/* PORTAL_CYCLORAMA — the destination-level cyclorama preview shown inside a homeworld
 * portal arch (portal.c). It is a far BACKGROUND seen through the arch
 * window: the arch stone must occlude it, but it must never occlude actors or
 * Spyro flying into/through the portal. Drawn in its own pass AFTER the WORLD
 * group (so it depth-tests against the already-written arch/terrain depth) but
 * with depth-WRITE OFF, so the actors/player drawn next are never per-pixel cut
 * by it. This mirrors the PSX OT, where the portal sky occupies one coarse
 * bucket and the player's whole model sorts in front of it (never intersecting
 * it per-pixel the way a shared Z-buffer otherwise would). Untextured only;
 * self-occlusion comes from the feeder's own far→near painter sort. */
#define OPENPETE_SCENE_PORTAL_CYCLORAMA 7
/* PORTAL_MASK — the stencil aperture mask for a portal-sky group (W2.6). The
 * portal feeder emits each portal's sky FOOTPRINT again under this category
 * (same geometry+z, colour irrelevant): core rasterizes it through an
 * invisible (colour-write-off) depth-TESTED stencil-MARK pass, so the mask is
 * set exactly where the sky is visible past the world depth (the arch stone /
 * a wall in front punches it out). The PORTAL_CYCLORAMA colours then draw
 * depth-FREE, gated on the mask — so dome / flat base / backstop within one
 * portal composite by submission order (painter) instead of z-arithmetic, and
 * portals are isolated from each other per group (rhi_clear_stencil between
 * groups; groups drawn far→near by their plane depth). Untextured only. */
#define OPENPETE_SCENE_PORTAL_MASK 8
/* PORTAL_GUARD — near-band terrain PROTECTION (the PSX mask bit, 2026-07-03).
 * The .s drains deferred terrain (depthSum < 0x2000 = mean world vz < 8192,
 * minus word3&0x80 faces) via immediate DrawPrim, which the PSX renders with
 * the mask bit SET; every OT prim — including the portal sky — draws with
 * mask-CHECK and is REJECTED on those pixels. So near-band terrain always
 * beats the portal preview on hardware, regardless of OT order. The terrain
 * feeder re-emits exactly those faces (its face_is_near classifier) under
 * this category when a portal fed sky recently; core re-draws them right
 * AFTER the portal-sky sequence (the sky writes no depth, so the redraw
 * re-wins its own pixels — the depth-buffer realization of the mask).
 * Opaque only by construction (the defer test excludes ABE faces). */
#define OPENPETE_SCENE_PORTAL_GUARD 9
/* SKY — the main cyclorama dome (cyclorama.c). Drawn exactly like WORLD
 * (opaque, depth-tested, before everything else) EXCEPT it does not stencil-MARK
 * terrain coverage (W2.6-c): the WORLD group's mark defines "pixels real world
 * geometry covered", and the hole-fill LP backing (WORLD_FILL) paints only where
 * that mark is absent — which is precisely where the sky shows through a terrain
 * seam. If the dome marked, every hole would be self-masked and the fill dead. */
#define OPENPETE_SCENE_SKY 10
/* WORLD_FILL — the terrain LP backing layer for sectors whose HP is drawn
 * (W2.6-c hole-fill). The PSX draws the coarse LP list first and the fine HP
 * over it, so LP shows through wherever HP leaves a T-junction seam against an
 * LP-only neighbour (the artisans distant-water gaps). Natively, drawing LP
 * behind HP with a depth bias poked through on non-flat sectors (the coarse
 * mesh bulges in front of the fine one — tried and reverted 2026-06-26).
 * Instead the WORLD group stencil-MARKs its coverage and this layer draws right
 * after it with RHI_STENCIL_TEST_ZERO: it can only touch pixels NO world
 * geometry covered, so poke-through is impossible by construction and every
 * genuine hole is filled with the same LP the PSX shows there. Depth-tested and
 * depth-written at TRUE LP depth (no bias needed). Untextured only (LP is
 * gouraud G4/G3). EXCLUDED from the occlusion depth pre-pass: rasterized
 * unstencilled there, a bulging LP would be a phantom occluder. */
#define OPENPETE_SCENE_WORLD_FILL 11

/* SHADOW: the drop-shadow decals (Spyro silhouette + moby shadows, blend==3).
 * Split from plain blend-3 so the SUBTRACT draw group's DECAL special-casing
 * (one shared shadow-sprite material for the whole textured range +
 * shadow_decal_tex.frag's ground-height/wall discard) applies ONLY to real
 * shadows. Genuinely subtractive world content — the dragon-fragment SMOKE
 * PUFFS (particle sel-2 billboards, gray ramp, ABR 2) — stays category WORLD
 * and draws through the plain subtractive pipes in the same slot; routing it
 * through the decal path discarded every sky-borne puff (height test) and
 * sampled the wrong texture (the range-uniform shadow sprite material). */
#define OPENPETE_SCENE_SHADOW 12
/* ACTOR_FRONT: the dragon-rescue CUTSCENE actors — the mobys the game keeps in
 * g_DragonCutscene (m_RescuedDragonMoby / m_CutsceneDragon / m_CutsceneSpyro).
 * They render like ACTOR (opaque, depth-tested) EXCEPT during a rescue cutscene,
 * when draw_native_scene front-passes them: a colour-write-off PUNCH pass
 * (depth compare ALWAYS) replaces the stored depth on exactly the pixels they
 * rasterize, then the ordinary opaque draw wins over the terrain it
 * interpenetrates (Delvin's spread wing inside the tower) while keeping exact
 * self-occlusion. Punching per PIXEL, not over a screen bbox, is what keeps the
 * fix local: every pixel the actors do not cover keeps its true depth, so
 * distant level mobys (--lod extended un-culls statues / chests / the level-end
 * vortex + critters) and semi-transparent terrain — both drawn after — keep
 * true occlusion. The pass runs BEFORE the PLAYER group so Spyro sorts against
 * the dragon by real Z instead of being painted over. Tagged by the moby feeder
 * via pointer match against g_DragonCutscene (the game's own actor set — no
 * heuristic). */
#define OPENPETE_SCENE_ACTOR_FRONT 13
/* TERRAIN_KEY: RETIRED (number reserved — do not reuse). Formerly the
 * portal-aperture og-key fan (P6c): the sky bucket's key rastered into the
 * HP key image to discard og-covered terrain, plus the WORLD_FILL stencil
 * pre-mark. Replaced by the plane-cover rule: the aperture mask and preview
 * colours test at opening plane − margin, covering everything behind it per
 * pixel — the .s's OT pushes are tier-asymmetric (fan +64 slots, LP +64+8b,
 * HP only +4b), so the og-key verdict breaks wherever tier and distance
 * decouple. No feeder emits this category any more. */
#define OPENPETE_SCENE_TERRAIN_KEY 14
/* carry: 1 = a tri is screen-space 2D-UI that the sub-tick re-extract canNOT
 * re-derive (it has no 2D-prim chokepoint to hook), so it must be snapshotted
 * before the rebuild and re-applied after — like the OVERLAY/HUD_PANEL carry, but
 * for tris that share a re-derivable category (the 2D-UI sprites/lines live in
 * OPENPETE_SCENE_HUD, which the moby feeder ALSO populates with re-derived near-OT
 * billboards). A category check can't tell them apart; this flag can. Passed as
 * add_tri's explicit `carry` argument (0 unless a feeder opts in). */
/* The FEEDER-FACING tri: geometry + material only. The routing state —
 * category / carry / portal group — is NOT a struct field: it travels
 * exclusively through the add_tri arguments and lives on the renderer's
 * internal stored tri (native_render.c). A feeder that tries to write
 * `tri.category = …` gets a compile error instead of a silently-ignored
 * field (W0.7b). */
typedef struct {
    openpete_scene_vert v[3];
    int textured;
    int tpage_x, tpage_y, clut_x, clut_y, bpp;
    int blend;
    float koff;  /* og OT KEY: the face's ABSOLUTE .s insertion key in world
                  * units + 0.5 sentinel; sign<0 = near-band contest-only
                  * (|koff| = key), 0 = not keyed. Stored depth (v[].z) is
                  * TRUE view-Z either way — og's keys select WHICH surface
                  * owns a pixel, they never enter the depth buffer.
                  * Per-face constant.                                     */
    int prop;    /* authored embedded PROP (pedestal discs, statue bases…):
                  * this face additionally rasters TRUE per-vertex depth
                  * into the prop image, and near-band terrain always
                  * yields to a prop physically within the embed bound —
                  * the class-curated "props render on top of the ground
                  * they are set into" divergence.                         */
    float rank;  /* og OT paint RANK (walk order, >=1; 0 = unranked): the
                  * game paints a moby's faces whole, in OT walk order —
                  * never per-pixel between same-moby faces. Ranked opaque
                  * faces raster (rank, ordinal) into the rank image and a
                  * fragment loses its pixel iff a LATER-ranked face of the
                  * SAME moby covers it. Stored depth stays TRUE per-vertex
                  * — rank decides WHICH same-moby face exists at a pixel,
                  * never what depth it writes. Cross-moby ordering stays
                  * true per-pixel z (the ordinal gate).                   */
    int mordinal;/* present-local moby ordinal (1..255) identifying "same
                  * moby" in the rank image. Present-LOCAL only: a pool
                  * ordinal is not an identity across presents.            */
    float pkey;  /* PLAYER ordering key: the avatar draw's own
                  * max(0, (part-origin view-Z >> 7) - sortDepth) slot + 1
                  * (0 = none). The face rasters that slot into the ACTOR
                  * key image's player band (psx_key_contest.glsl), where
                  * the additive glow fan is its only reader; the face
                  * itself contests nothing — its koff stays 0 and its
                  * stored depth stays true, so an ordering pull can never
                  * show the avatar through geometry in front of him.     */
} openpete_scene_tri;

/* Rank sentinels for faces that are not ranked but must still be told
 * apart by the key contest (openpete_scene_tri::rank): -1 = an opaque
 * actor face, -2 = an additive glow fan. Every rank consumer reads both as
 * unranked; the contest reads them as the class whose arm to run. */
#define OPENPETE_SCENE_RANK_ACTOR (-1.0f)
#define OPENPETE_SCENE_RANK_AURA  (-2.0f)

/* ---- scene view-Z: the ONE named point (W0.7b) ------------------------------
 *
 * The scene vertex stream carries depth as FULL-WORLD view-space Z (the GTE
 * MAC3 of full-world camera-relative coords; larger = farther, 1.0 == one GTE
 * unit), unencoded. The vertex stages clamp it to [1, OPENPETE_DEPTH_VIEWZ_FAR]
 * and derive the depth-buffer value from it: 1/vz on the float depth format
 * (reversed: larger is nearer, GREATER_OR_EQUAL, clear 0), 1 - 1/vz on the
 * D24 fallback (LESS_OR_EQUAL, clear 1). Both are affine in 1/vz, so they
 * interpolate exactly in screen space.
 *
 * The feed takes an openpete_worldvz, which can ONLY be built through
 * the unit-claiming constructors below — a bare float is a compile error. A
 * GTE path that produces sub-scaled Z must normalize through _scaled naming
 * its unit. This enforces the "every feeder hands in the SAME absolute scale"
 * invariant whose prose-only form shipped ×4 / ×2^m_scale sorting bugs
 * (world/4 feeders drew through terrain). Known per-feeder units:
 *   terrain            world/16 (camera>>4, verts /16)  → _scaled(vz, 16)
 *   moby shadows       world/4  (cam+pos >>2)           → _scaled(vz, 4)
 *   moby records       world/2^m_scale — normalized at project time; feeds _full
 *   particles (world)  world/4 — normalized at build (PART_VZ_WORLD4); feeds _full
 *   spyro / flame / tracer / portal / glows  full-world  → _full
 *
 * GPU mirrors of the clamp and the depth curves: core/rhi/shaders/psx_viewz.glsl
 * (included by psx_scene.vert, quad.vert, prepass.vert, terrain_key.vert,
 * shadow_decal_tex.vert and terrain.vert). */
/* The hyperbolic curve's scale: 1.0 == 4096 GTE units spreads typical actor
 * depths across the usable range. The key images store
 * z = vz / (vz + OPENPETE_DEPTH_VIEWZ_SCALE). */
#define OPENPETE_DEPTH_VIEWZ_SCALE 4096.0f
/* The farthest view-Z the vertex stages keep: the clamp bounds W. Its value is
 * 4096 * 0.9999 / (1 - 0.9999) in float, the view-Z whose hyperbolic depth is
 * 0.9999. It is the backdrop view-Z the cyclorama feeds: reversed depth
 * 1/W = 2.4e-8, above the clear of 0 and below every nearer surface; forward
 * depth 1 - 1/W rounds to the clear of 1. */
#define OPENPETE_DEPTH_VIEWZ_FAR 0x1.386abap+25f

/* A full-world view-Z. Only the constructors below produce one. */
typedef struct { float vz; } openpete_worldvz;

/* vz is ALREADY full-world view-Z (raw GTE MAC3 of full-world coords, or a
 * value normalized to full-world upstream where the unit is known). */
static inline openpete_worldvz openpete_worldvz_full(float vz) {
    openpete_worldvz w; w.vz = vz; return w;
}
/* vz is full-world DIVIDED BY mul (vz*mul == full-world): the unit claim for
 * sub-scaled GTE paths — 4 for >>2 camera paths, 16 for terrain cam>>4,
 * (1<<m_scale) for moby record scale. */
static inline openpete_worldvz openpete_worldvz_scaled(float vz, float mul) {
    openpete_worldvz w; w.vz = vz * mul; return w;
}
/* An INTENTIONAL world-proportional ordering bias (mul>1 pushes behind, mul<1
 * pulls nearer), applied after unit normalization — the terrain LP tier-pull,
 * the shadow A/B bias envs. Distinct from a unit claim: use _scaled to state
 * what unit a value is in, _bias to state ordering policy. */
static inline openpete_worldvz openpete_worldvz_bias(openpete_worldvz z, float mul) {
    z.vz *= mul; return z;
}
/* _bias with the offset SATURATED at `cap` world units (cap <= 0 = uncapped).
 *
 * A proportional bias grows without bound, so at distance it eventually exceeds
 * the real clearance between whatever it is meant to separate and something
 * else entirely — it stops being a tie-break and starts reordering the scene.
 * Capping bounds that collateral while keeping the two properties the tie-break
 * needs: the biased surface is still strictly behind a coincident unbiased one
 * at every distance, and the map vz -> vz' stays strictly INCREASING, so the
 * relative order of two biased surfaces is preserved exactly. That second
 * property is what a per-face or per-sector exemption cannot offer — exempting
 * some faces of one continuous surface and not others tears it at the boundary. */
static inline openpete_worldvz openpete_worldvz_bias_capped(openpete_worldvz z, float mul, float cap) {
    float d = z.vz * (mul - 1.0f);
    if (cap > 0.0f) { if (d > cap) d = cap; else if (d < -cap) d = -cap; }
    z.vz += d; return z;
}
/* An INTENTIONAL ordering pull by a FIXED world-unit distance (toward the
 * camera), applied after unit normalization. For OT-tie-break emulation: the
 * PSX links a whole moby's prims at its CENTRE-depth bucket, so late-added
 * attachments (sparkles) draw over the entire model regardless of per-pixel
 * depth; a constant pull that clears the body's protrusion is the Z-buffer
 * realization. Use _bias for proportional policy, _pull for absolute. */
static inline openpete_worldvz openpete_worldvz_pull(openpete_worldvz z, float world_units) {
    z.vz -= world_units; return z;
}
/* One slot of the 3D ordering table in full-world view-Z: the moby, glow and
 * effect routines all key on MAC3 >> 7. */
#define OPENPETE_OT_SLOT_VZ 128
/* The moby feeders' one-slot tie pull, in full-world view-Z units: every moby
 * vertex is fed this much nearer than its true depth, so a moby wins the
 * surface it sits on, as the ordering table resolves same-slot moby-versus-
 * terrain ties. Less than one slot, so it flips only pairs the table calls a
 * tie. */
#define OPENPETE_MOBY_TIE_PULL_VZ 127.0
/* An ordering-table key as a full-world view-Z on the moby feeders' depth
 * scale. The moby path keys each face at its average-depth slot minus the
 * owning moby's authored depth offset, and feeds the face at its true depth
 * less the tie pull, so the faces of slot s land in [128*s - 127, 128*s + 1).
 * A primitive linked at key k over a moby whose offset is d sorts against that
 * moby's faces as the table does when fed at 128*(k + d) - 127: in front of
 * every face of slot k + d and beyond, behind every nearer one. `owner_slot` is
 * k + d. The quantization is the key's own. */
static inline openpete_worldvz openpete_worldvz_owner_slot(int owner_slot) {
    return openpete_worldvz_full((float)((double)owner_slot * OPENPETE_OT_SLOT_VZ
                                         - OPENPETE_MOBY_TIE_PULL_VZ));
}
/* The scene vertex depth (openpete_scene_vert.z / .pvz) of a view-Z:
 * near-guarded at 1 (behind-camera geometry is culled upstream). */
static inline float openpete_native_scene_viewz(openpete_worldvz z) {
    return z.vz < 1.0f ? 1.0f : z.vz;
}

/* ---- depth-aware shadow decal (height-above-ground wall rejection) ----------
 *
 * The Spyro silhouette shadow is a FLAT fan; over uneven near-terrain it sits
 * behind the bumps, so a pure depth test/band can't tell "my own bumpy ground"
 * (keep) from "a wall in front" (discard) — both put terrain nearer than the fan.
 * The fix keeps the flat fan's LOOK unchanged and only changes the keep/discard
 * decision: per pixel the shadow covers, the shader reconstructs the opaque
 * terrain's world HEIGHT from the depth pre-pass and discards where it sits well
 * ABOVE the shadow's ground plane (a wall). Bumps/ramps near ground height stay
 * shadowed.
 *
 * The reconstruction needs the camera transform, which only the game can read, so
 * the game passes it here each frame (display-only; never written back to PSX RAM).
 * `cam_col0..2` are 4096²·C⁻¹ rows (pushed numerical inverse — the Spyro camera
 * matrix is NOT orthonormal); the inverse recovers V = C⁻¹·MAC with the Spyro GTE
 * swizzle V=(-Δy,-Δz,Δx). AXIS NOTE (2026-07-02): Spyro's VERTICAL world axis is
 * the **z field** — V1 carries -Δz, so world height = cam_pos_up − V1 (cam_col1).
 * The original decal tested cam_pos_y − V0, i.e. HORIZONTAL-Y proximity, which
 * rejected walls incidentally and left the wall-base notch. `ofx/ofy/h` are the
 * GTE projection regs. `ground_up` is the shadow's ground plane (SpyroShadow
 * world z). `wall_height` is the height slack above ground before a pixel counts
 * as a wall. valid=0 disables the test (shader draws the fan unmasked).
 *
 * A pure height threshold has one blind band: the BOTTOM wall_height units of a
 * wall rise from ground level, so the fan still painted on a wall's base
 * (dark-hollow f2700 notch). The classifier for that band is surface
 * ORIENTATION — own ground/bumps/slopes are walkable (up-normal large), a wall
 * face is near-vertical (up-normal ≈ 0). The shader reconstructs the terrain
 * normal from neighbouring pre-pass texels (differential — no camera position
 * needed) and discards steep surfaces IN FRONT of the fan fragment
 * (`steep_nup` = |up normal| below which a surface counts as a wall). */
typedef struct {
    float cam_col0[3];
    float cam_col1[3];
    float cam_col2[3];
    float cam_pos_up;     /* camera world height (the game's z field) */
    float ofx, ofy, h;
    float ground_up;      /* shadow ground-plane height (SpyroShadow z) */
    float wall_height;
    float steep_nup;
    int   valid;
} openpete_shadow_decal_params;
void openpete_native_set_shadow_decal_params(const openpete_shadow_decal_params *p);

/* Begin a fresh native-scene frame (clears the triangle list). Called by
 * the game at the start of its scene extraction. */
void openpete_native_scene_begin(void);
/* Mark the scene now accumulating as a sub-tick / lag re-extract's rebuild
 * rather than the game render's own. The blended-world block of a replay is
 * reordered into the emission order the game render last recorded, because
 * the blend equations do not commute once a channel clamps and the replay
 * emits one producer at a time. Called by the re-extract wrapper after the
 * game's body has fed. */
void openpete_native_scene_mark_replay(void);
/* Frame-keyed begin: clears the scene only when `frame` differs from the last
 * begin_frame call, otherwise a no-op. This lets MULTIPLE independent feeders
 * (cyclorama, then the world, then mobys — whatever order they hook in within
 * one game frame) compose into a single scene without any one of them wiping
 * the others: the first feeder of each frame clears, the rest append. Pass
 * `openpete_frame` (the per-yield frame counter). Order-independent and robust
 * to a feeder being absent on a given level. */
void openpete_native_scene_begin_frame(unsigned frame);
/* Frame stamp of the currently accumulating scene (0xFFFFFFFF = none).
 * Late feeders must check this and append only when the scene is live —
 * begin_frame with a NEWER frame wipes a held scene (load stalls hold the
 * last composed frame on purpose). */
unsigned openpete_native_scene_frame(void);

/* Register the frame-close notification: it fires immediately before a frame's
 * accumulation is snapshotted and the next frame started, which is the last
 * moment an append still reaches that scene. A feeder whose data is not final
 * when its primitives are produced releases whatever it still holds for
 * `frame` here. Runs on the thread doing the rotation. */
void openpete_native_scene_register_frame_close(void (*fn)(unsigned frame));

/* The ordering-table traversal rank of the tris added next. The 2D layer is
 * screen-space with the depth test off, so compose order is painter order, and
 * its two emitters reach the scene at different points of the frame, one at a
 * primitive's link, one when a glyph chain is built, one of them from a
 * deferred release. Both stamp the number the game emitted at, and the compose
 * orders by it. Sticky, cleared at each frame boundary; a feeder that never
 * sets it leaves 0. */
void openpete_native_scene_set_seq(uint32_t seq);

/* PSX address of the primitive the tris added next were decoded from. Sticky,
 * cleared at each frame boundary. */
void openpete_native_scene_set_prim(uint32_t prim);

/* Texel STP selection of a textured scene triangle: which texels of its
 * texture the fragment stage keeps (psx_scene.frag STP_SELECT). The GPU
 * applies a semi-transparent primitive's ABR equation only to texels whose
 * bit 15 (STP) is set and draws its STP-clear texels opaque. ABR 0 reproduces
 * that through the texel alpha; the ABR 1-3 equations read no alpha, so a 2D
 * feeder emits such a primitive as two triangle sets: its STP-clear texels
 * through the opaque pipe, then its STP-set texels through the ABR's pipe. */
enum {
    OPENPETE_STP_ALL = 0,      /* every non-zero texel (the default) */
    OPENPETE_STP_CLEAR_ONLY,   /* texels with bit 15 clear */
    OPENPETE_STP_SET_ONLY,     /* texels with bit 15 set */
    OPENPETE_STP_SELS
};

/* Sticky, like set_prim: every tri added after the call carries `sel` until
 * the next call. A feeder that sets it resets it to OPENPETE_STP_ALL after its
 * triangles. */
void openpete_native_scene_set_stp_select(int sel);

/* Stamp every 2D tri of this frame (live scene and carry accumulation) with its
 * ordering-table traversal rank, looked up by the primitive address each was
 * decoded from. Lets every 2D emitter keep feeding where it does and still be
 * composed in the order the GPU draws. */
void openpete_native_scene_rank_2d(uint32_t (*rank_of)(uint32_t prim));

/* The primitive addresses this frame's 2D was decoded from, so a feeder can
 * tell which of the frame's ordering-table walks is the 2D one. */
int openpete_native_scene_collect_2d_prims(uint32_t* out, int max);
/* Append one screen-space triangle to the current native scene. `category`
 * (OPENPETE_SCENE_WORLD/_ACTOR/_HUD/…) and `carry` are passed EXPLICITLY per
 * call — there is NO ambient/global scene category (the old set_category /
 * set_carry globals are gone), so a feeder can't leak a stale layer into the
 * next feeder and EVERY emit path (incl. the sub-tick re-extract) is
 * category-correct by construction. `category` is ignored for non-opaque tris
 * (blend!=0 routes by blend). `carry` = 1 marks sub-tick-uncarryable 2D-UI (the
 * title logo / "PRESS START" / HUD glyph FT4 sprites + UI separator lines the
 * re-extract can't re-derive) so the overlay-carry snapshot preserves them
 * across the sub-tick rebuild instead of strobing at 30 Hz; 0 otherwise. */
/* Sticky CLUT-ladder row for subsequent add_tri calls (0 = unfogged base row;
 * out-of-range values are clamped to 0). Spyro's terrain fogs by SELECTING a
 * pre-baked, progressively-greyer palette row — clut_y + delta — rather than
 * blending at raster time, so identical texels reach the screen through up to
 * 16 different palettes depending on distance.
 *
 * Declaring the row lets a consumer recover the BASE palette, which is the
 * texture's stable identity: the replacement registry resolves against that
 * base so one pack file covers a texture at every fog distance, and re-applies
 * the ladder's colour transform at draw time. Purely descriptive — it does not
 * affect the material batch key (clut_y already carries the distinction) and
 * does not change how unreplaced terrain rasterizes.
 *
 * Sticky like set_tag: set it per emit, and reset to 0 when leaving a laddered
 * emit loop so the value cannot leak into another feeder's tris. */
void openpete_native_scene_set_fog_delta(int delta);

/* ── Portal cover invariant (plane-cover rule) ────────────────────────
 * The portal feeder's opening planes, republished per feed in core terms so
 * the OPENPETE_PORTAL_COVER_CHECK sweep can verify the plane-cover rule
 * against the COMPLETE scene (terrain AND actors): every pixel inside an
 * opening's footprint must resolve to the preview or to content nearer than
 * plane − margin. 1/view-Z of the plane at screen (x,y) = a·x + b·y + c. */
typedef struct {
    double a, b, c;            /* screen-space inv-vz field of the plane */
    float  px[16], py[16];     /* projected opening polygon (walk coords) */
    int    pn;
} openpete_native_cover_plane;
/* Replaces the previous set (call once per feed; n=0 clears). margin_world
 * is the plane-cover near margin M in world units. */
void openpete_native_set_cover_planes(const openpete_native_cover_plane *planes,
                                      int n, float margin_world);

void openpete_native_scene_add_tri(const openpete_scene_tri *tri, int category, int carry);
/* add_tri with an explicit grouping id (stored on the renderer's internal
 * tri — plain add_tri stores 0). Used by the portal feeder to associate each
 * portal's PORTAL_MASK footprint with its PORTAL_CYCLORAMA colours so core
 * can stencil-isolate portals per group. No ambient state (same principle as
 * the explicit category/carry args). */
void openpete_native_scene_add_tri_grouped(const openpete_scene_tri *tri, int category,
                                           int carry, int group);
/* Scene vertices that reached add_tri without a true view-Z (pvz <= 0) since
 * boot; each took its ordering view-Z. 0 when every feeder states pvz. */
unsigned openpete_native_scene_pvz_missing(void);

/* Sticky per-tri origin stamped by add_tri until changed (same ambient-state
 * pattern as set_tag; set 0 when leaving the emit loop). 1 marks a triangle the
 * host added beyond what the game's own render produced — the extended-LOD moby
 * reveal. The verdicts "this frame has content" and "this frame is a gameplay
 * world frame" are read off the game's geometry alone, so host-added geometry
 * never turns a frame the game drew nothing on into a rendered scene. */
void openpete_native_scene_set_host_added(int on);

/* Marks the start of the terrain feed in the scene being accumulated: every
 * triangle added after the call, until the scene resets, is stamped as emitted
 * at or after the feed. The retained terrain cache draws geometry the feed does
 * not emit; the mark places the feed in the scene's emission order when the
 * feed emits no triangle of its own. */
void openpete_native_scene_mark_terrain_feed(void);

/* ── Provenance tag (mod-shaders S6) ─────────────────────────────────
 * Sticky per-tri provenance stamped by add_tri until changed (same
 * ambient-state pattern as the terrain feeder's s_emit_* context; set
 * 0 when leaving a tagged emit loop). Carries what the material key
 * LACKS — the batcher already splits on tpage/clut, so texture-level
 * selectors ride the material key; the tag adds source identity:
 *
 *   bits 31:28  path (0 = untagged, 1 = moby, 2 = terrain)
 *   moby:       bits 15:0  m_Class
 *   terrain:    bit 27 env-anim member (func_8002A6FC rewrite set),
 *               bit 26 LP tier, bits 8:0 sector id; textured faces add
 *               the Tiledef/texture-record id in [15:9] (word2 & 0x7F —
 *               the texture-selector key, S9) and a per-present material
 *               slot in [24:16] (texture-keyed per-instance resolve)
 *
 * INERT BY DEFAULT: the tag joins the material sort/coalesce key ONLY
 * when a split consumer is armed (a registered material selector — S8 —
 * or the OPENPETE_TAG_SPLIT=1 dev flag), so the ~70-draw batching is
 * byte-identical without mods. OPENPETE_TAG_TINT=1 tints vertex colours
 * by tag hash (visual coverage instrument, no split needed). */
#define OPENPETE_TAG_MOBY(cls) \
    (0x10000000u | ((uint32_t)(cls) & 0xFFFFu))
/* moby + a per-instance material slot (S8b) in the spare bits [27:16];
 * slot 0 == OPENPETE_TAG_MOBY (declarative / no per-instance winner). */
#define OPENPETE_TAG_MOBY_SLOT(cls, slot) \
    (0x10000000u | (((uint32_t)(slot) & 0xFFFu) << 16) | ((uint32_t)(cls) & 0xFFFFu))
#define OPENPETE_TAG_TERRAIN(lp, env, sec) \
    (0x20000000u | ((env) ? 0x08000000u : 0u) | ((lp) ? 0x04000000u : 0u) | \
     ((uint32_t)(sec) & 0x1FFu))
/* Textured terrain face (S9): [15:9] = the face's Tiledef/texture-record id
 * (word2 & 0x7F — the id texture selectors key on), [24:16] = the
 * per-present material slot from the texture-keyed per-instance resolve
 * (0 = declarative resolve, 0x1FF = the callback declined -> stock). */
#define OPENPETE_TAG_TERRAIN_TEX(lp, env, sec, tex, slot) \
    (OPENPETE_TAG_TERRAIN(lp, env, sec) | \
     (((uint32_t)(slot) & 0x1FFu) << 16) | (((uint32_t)(tex) & 0x7Fu) << 9))
/* Untextured terrain band (S8c): bit 25 marks the flat/gouraud tris, and
 * [24:16] carries the per-extraction material slot (0 = declarative resolve,
 * 0x1FF = the callback declined -> stock). */
#define OPENPETE_TAG_TERRAIN_UNTEX(lp, env, sec, slot) \
    (OPENPETE_TAG_TERRAIN(lp, env, sec) | 0x02000000u | \
     (((uint32_t)(slot) & 0x1FFu) << 16))

/* ── The remaining feeder paths ───────────────────────────────────────
 * Every path below copies the MOBY layout exactly — [31:28] path,
 * [27:16] material slot (12 bits), [15:0] source identity (16 bits) —
 * which fills the word with no spare bits. Copy it for any new feeder
 * rather than inventing a payload: one shared shape keeps the decode
 * and the slot-width reasoning uniform across channels.
 *
 * The identity half is whatever the feeder actually HAS at emit time,
 * which varies sharply by path. Where a feeder has none, the field is
 * 0 and the tag identifies the channel only — that is a real limit,
 * not a placeholder to fill in later: a per-element selector cannot be
 * built on a feeder that never knew which element it was drawing.
 * `docs/render-channel-census.md` records the per-feeder evidence. */
#define OPENPETE_TAG_PATH(p, ident, slot) \
    (((uint32_t)(p) & 0xFu) << 28 | (((uint32_t)(slot) & 0xFFFu) << 16) | \
     ((uint32_t)(ident) & 0xFFFFu))

/* Player (Spyro) — drawn by r_pete.s, NOT the moby pool walk, so no
 * moby-class selector can ever reach him. The emitted tri carries no
 * sub-model marker (the walk keeps per-vertex indices, not a segment
 * id), so identity is 0 = the whole player; splitting head/body/wing
 * would mean threading the segment through the walk first. */
#define OPENPETE_TAG_PLAYER(slot)         OPENPETE_TAG_PATH(3u, 0u, (slot))
/* Flame breath. Identity distinguishes the ADDITIVE ribbon from the
 * OPAQUE G3 caps — one feeder, two blend modes, so a material that
 * reshades the ribbon must be able to leave the caps alone. */
#define OPENPETE_TAG_FLAME_RIBBON  0u
#define OPENPETE_TAG_FLAME_CAPS    1u
#define OPENPETE_TAG_FLAME(part, slot)    OPENPETE_TAG_PATH(4u, (part), (slot))
/* Particles. The feeder covers six distinct effects and only two of them
 * carry the game's own `sel` class — glows, flares, sparkle lines and the
 * dragon star are not in that space at all. So identity is a pair:
 * [15:8] a feeder-local KIND naming the effect, [7:0] the game's `sel`
 * where one exists (0 otherwise).
 *
 * Both halves CLASSIFY without IDENTIFYING: the pool stores no slot or
 * pointer and emit order is pool-walk order. Enough for "all smoke
 * puffs", never enough for "this puff" — which is also why the channel
 * cannot be geometry-cached. */
#define OPENPETE_TAG_PART_QUAD    0u   /* FT4 billboards (sel-classified) */
#define OPENPETE_TAG_PART_LINE    1u   /* streak lines (sel-classified) */
#define OPENPETE_TAG_PART_GLOW    2u   /* glow rings */
#define OPENPETE_TAG_PART_FLARE   3u   /* sparkle flares */
#define OPENPETE_TAG_PART_SPARK   4u   /* sparkle lines */
#define OPENPETE_TAG_PART_STAR    5u   /* dragon-rescue star fan */
#define OPENPETE_TAG_PARTICLE(kind, sel, slot) \
    OPENPETE_TAG_PATH(5u, (((uint32_t)(kind) & 0xFFu) << 8) | \
                          ((uint32_t)(sel) & 0xFFu), (slot))
/* Sky / cyclorama dome. No per-tri identity exists (the feeder carries a
 * loop index only), so the identity field is 0 = the whole dome. */
#define OPENPETE_TAG_SKY(slot)            OPENPETE_TAG_PATH(6u, 0u, (slot))
/* Drop shadows. Identity is the caster KIND, not the caster: the moby
 * pointer is read while building but never reaches the tri. */
#define OPENPETE_TAG_SHADOW_MOBY   0u
#define OPENPETE_TAG_SHADOW_PLAYER 1u
#define OPENPETE_TAG_SHADOW(kind, slot)   OPENPETE_TAG_PATH(7u, (kind), (slot))
/* Tracer/beam streaks. The .s drains its source point arrays every
 * frame, so nothing survives to identify a streak. */
#define OPENPETE_TAG_TRACER(slot)         OPENPETE_TAG_PATH(8u, 0u, (slot))
/* Portal destination-sky previews. Identity is the preview KIND, because
 * the game draws a portal three ways across the approach and they are
 * genuinely different art: a flat colour chip far out, a DPCS-fogged dome
 * at medium range, the full gouraud dome up close. Anything that restyles
 * one rung and not the others pops as the player walks up.
 *
 * The owning portal is NOT in the tag — it rides the per-tri GROUP id
 * (the destination skybox pointer), which the portal pass already uses. */
#define OPENPETE_TAG_PORTAL_FLAT     0u
#define OPENPETE_TAG_PORTAL_DOME     1u
#define OPENPETE_TAG_PORTAL_BLENDED  2u
#define OPENPETE_TAG_PORTAL(kind, slot)   OPENPETE_TAG_PATH(9u, (kind), (slot))

void openpete_native_scene_set_tag(uint32_t tag);
/* Accepted-tri count per tag family (bits 31:28, index 0 = untagged) of the
 * scene being accumulated; reset with the scene. */
void openpete_native_scene_tag_family_counts(uint32_t out[16]);
/* Arm/disarm the tag as a batch-split key (material-selector registry). */
void openpete_native_set_tag_split(int on);
/* Set the panel-0 background fill colour (rgba, byte order R,G,B,A) — drawn
 * before the scene so uncovered areas show it instead of black, mirroring the
 * PSX cyclorama's frame clear to m_BackgroundColor. 0 = no fill (black).
 * Reset to 0 by openpete_native_scene_begin. */
void openpete_native_scene_set_bg(uint32_t rgba);
/* Capture the gamestate draw-env clear colour (the framebuffer background the
 * canonical render auto-clears to) from the PutDrawEnv path. `isbg` is the PSX
 * DRAWENV auto-clear flag (0 = no clear → defer to the scene-fed bg); r/g/b are
 * the clear colour 0..255. This is the PRIMARY background source in the native
 * renderer (preferred over openpete_native_scene_set_bg / g_Cyclorama, which is
 * stale in cyclorama-less states like the black level-name flyby). Persists
 * across sub-tick presents (PutDrawEnv only fires on canonical frames). */
void openpete_native_set_clear_color(int isbg, int r, int g, int b);
/* Full-screen fade overlay (the game's func_800190D4): every transition the game
 * darkens/brightens the WHOLE frame with one semi-transparent full-screen POLY_F4
 * whose ABR mode is the fade direction — mode 2 (B-F, subtractive) fades to BLACK,
 * mode 1 (B+F, additive) fades to WHITE. The native renderer reproduces it as a
 * full-panel quad drawn DEAD LAST — over the world, actors, HUD and 2D UI — in the
 * matching blend mode with the depth test passing everywhere (z=near), so it tints
 * the whole composited frame exactly as the PSX F4 does. `mode` is the PSX ABR mode
 * (0 = (B+F)/2 alpha, 1 = add, 2 = subtract; 3 is unused by the game). r/g/b are
 * the fade colour 0..255. Call it every frame the fade is active; the overlay
 * auto-clears each frame (reset by openpete_native_scene_begin / begin_frame), so a
 * frame with no set_fade call shows no fade. Pass mode<0 to clear it explicitly.
 * Determinism-safe (display-only). */
void openpete_native_scene_set_fade(int mode, int r, int g, int b);
/* Freeze-frame green overlay (the pause / inventory / flight-results backdrop).
 * The game freezes the rendered frame, desaturates it to grayscale and re-displays
 * it green-tinted (setRGB0(0x4C,0x80,0x40)) via a full-screen POLY_FT4 that samples
 * a VRAM scratch copy. The native renderer instead snapshots its OWN last world
 * frame (frozen_tex) and draws it desaturated + tinted — so the green overlay shows
 * the native HQ render and survives the end-state where PsyCross's VRAM is gone.
 * The feeder detects the backdrop FT4 (its unique green tint + grayscale-CLUT
 * signature), calls this with the FT4's r/g/b tint, and SKIPS feeding the FT4. Call
 * every frame the backdrop is active; it auto-clears each frame (reset by
 * scene_begin / begin_frame). Pass r<0 to clear explicitly. Determinism-safe
 * (display-only).
 *
 * `fade` is the fade-to-black shift in 0..127 (subtractive luma shift x127, the
 * GS_ExitLevel case-0 fade where the game shrinks the grayscale CLUT each tick).
 * Pass 0 for the static pause / inventory / flight-results backdrop. */
void openpete_native_scene_set_freeze_backdrop(int r, int g, int b, int fade);

/* Freeze-backdrop production mode, set once from the game descriptor.
 *
 * 0 (default): the game emits a full-screen quad that re-displays the frozen
 * frame through a grayscale CLUT with a flat tint, and the backdrop reproduces
 * that desaturation and tint. Two such emitters can hand over with a one-tick
 * gap between them, which the per-tick freeze carry bridges.
 *
 * 1: the game produces the frozen frame by writing DRAWENV.isbg = 0 and drawing
 * no world, so the framebuffer simply keeps the last rendered frame. The
 * backdrop then draws the snapshot unaltered — no desaturation, no tint — and
 * the carry bridges no gap, because the arming signal is a latched draw-
 * environment word that the game clears in one write rather than a primitive it
 * stops emitting. */
void openpete_native_scene_set_freeze_retained(int retained);

/* Flat 2D HUD/text draw order vs the player for this frame. Default (0) draws the
 * flat HUD/text OVER the player (Spyro behind the HUD/text) — the normal case. Pass
 * 1 ONLY during the GS_LevelTransition flyby ("THE ADVENTURE BEGINS"/"ENTERING
 * <level>"), where Spyro flies in front of the text being revealed, so the flat HUD
 * draws UNDER the player. The 3D HUD icons (HUD3D) and the letterbox/fade always draw
 * on top regardless. Auto-clears each frame (reset by begin_frame); the feeder sets
 * it from g_Gamestate every frame. Determinism-safe (display-only). */
void openpete_native_scene_set_hud_under_player(int under);
/* Actors-in-front draw order for this frame (dragon-rescue cutscene). Pass 1 to
 * draw the ACTOR group depth-ISOLATED after the world+player (mid-pass depth
 * clear, the HUD3D trick): actors still Z-sort among their own faces but are
 * never occluded by terrain. The PSX's coarse OT bucketing painted the rescue
 * actors over walls they interpenetrate (the rescued dragon's spread wing inside
 * a wall) — true per-pixel Z would amputate them. Default (0) = normal depth
 * order. Auto-clears each frame; the feeder sets it from the game's own
 * dragon-cutscene state. Determinism-safe (display-only). */
void openpete_native_scene_set_actor_front(int front);
/* True when the native display is live (so the game only does the extra
 * extraction work when it will actually be shown). */
/* (openpete_native_render_enabled, above, serves this gate.) */

/* ---- widescreen (Hor+) display policy --------------------------------
 *
 * In vk-only mode (--renderer vk), when the window is WIDER than the 4:3 PSX
 * display, the native renderer fills the window and reveals more world at the
 * left/right while keeping the vertical FOV unchanged (Hor+). It is purely a
 * display policy: the .s verdict producers still run canonical 4:3 (gameplay is
 * untouched), and the native renderer just un-scissors the side strips and maps
 * the same projected geometry into the wider window — the centre 4:3 region is
 * pixel-identical to the 4:3 render. The A/B and diff compare modes stay 4:3 so
 * they line up 1:1 with the PsyCross reference; widescreen is inactive there.
 *
 * The widen FACTOR is window_aspect / display_aspect (1.0 = no widen, ~1.33 for
 * 16:9). Feeders that pre-cull geometry to the 4:3 screen-X rect must widen
 * those horizontal bounds by this factor so the revealed side geometry isn't
 * dropped before it reaches the (now full-width) scissor. Vertical bounds stay
 * unchanged. Determinism-safe (display-only); returns 1.0 when inactive. */
float openpete_native_widescreen_factor(void);
/* The visible PSX screen-X range under the active widescreen factor: [0,512] at
 * 4:3, widened symmetrically about the screen centre (256) — e.g. ~[-85,597] at
 * 16:9. Feeders use it as the left/right bound of their horizontal trivial
 * reject (replacing the hard 0 / 512 edges). */
void  openpete_native_widescreen_x_bounds(float *xlo, float *xhi);

/* ---- field of view display policy ------------------------------------
 *
 * --fov DEG is the vertical FOV of the visible band (clip_h rows), the one
 * FOV number Hor+ keeps aspect-invariant. The feeders project with
 * H_eff = H_canon · s, s = tan(vref/2) / tan(fov/2), vref = 2·atan((clip_h/2)
 * / (y_row · H_canon)), where H_canon is the GTE's H and y_row the game's
 * camera-matrix Y scale (the feeder passes both). deg <= 0 is the default:
 * the scale is the literal 1.0 and H_eff == H_canon bit-for-bit. The
 * tangent is latched once per present (scene begin), so a live change
 * cannot split one present across two H values. Renderer state only; the
 * game's verdict producers keep the canonical H. */
void   openpete_native_set_fov(float deg);
float  openpete_native_fov_deg(void);             /* 0 = default */
double openpete_native_fov_h_scale(double H_canon, double y_row);
/* Rows of the visible band the panel shows (224 on Spyro 1; 224 before the
 * first frame). */
int    openpete_native_visible_rows(void);
/* Horizontal FOV, degrees, the current window displays for the canonical
 * H_canon under the active Hor+ factor and FOV scale: 2·atan(256·F / H_eff). */
double openpete_native_fov_horizontal_deg(double H_canon, double y_row);
/* Label inputs: the feeders note the projection they build views from
 * (H_canon and the camera's Y-row scale); the angles derive the vertical FOV
 * shown (the setting, or the game's own at the default) and the horizontal
 * FOV at the live Hor+ factor. Returns 0 while no view has been built (the
 * angles then assume H 341, Y row 0.625). */
void  openpete_native_fov_note_canon(double H_canon, double y_row);
int   openpete_native_fov_angles(double* v_deg, double* h_deg);

/* Present one native frame to the Vulkan window. Call after PsyX_EndScene so
 * PsyCross's present FBO is freshly rendered (the future "reference"). Does
 * NOT pump SDL events — the host loop owns the event queue, which PsyCross's
 * input path depends on; window-size changes are detected by polling. No-op
 * if the native path is disabled.
 *
 * `record_pts_us` is the wall-clock capture instant (microseconds) tagged onto
 * this present for --record: the frame is queued into the async readback ring
 * here (between the blit and present) and retrieved a couple presents later, so
 * a vk recording captures the on-screen SSAA/widescreen/sub-tick frame at real
 * speed. Unused when recording is inactive. */
void openpete_native_render_frame(uint64_t record_pts_us);

/* Re-present the last composed frame (offscreen -> swapchain, no recompose).
 * Issues no scene work and reads no guest state, so it is the one present a
 * thread other than the guest's may make. Returns 1 if a present went out. */
int openpete_native_render_repeat_last(void);

/* ---- eager scene ring ----------------------------------------------------
 * The thread running the guest extracts every scene of a span ahead of time
 * and publishes each into a ring; a present thread composes ring slots on
 * the display grid. See native_render.c "eager scene ring" for the slot
 * lifecycle and the ownership rule. Times are op_now() nanoseconds. */
/* Call before the re-extract of an eager scene: the terrain builder stamps
 * its build with a scene id (top bit set) instead of the VBL counter. */
void     openpete_native_scene_eager_begin(void);
uint32_t openpete_native_scene_build_stamp(void);
/* Copy the accumulator + every per-present companion into the next free
 * slot. kind = OP_PACE_CANONICAL / OP_PACE_INTERMEDIATE. Returns the slot,
 * or -1 (ring full / allocation failed) — a refused publish, never a cut. */
int      openpete_native_scene_publish(int64_t target_ns, int kind, double alpha, int paired);
int64_t  openpete_native_scene_next_target(void);      /* earliest pending, 0 = none */
int      openpete_native_scene_pick(int64_t at);       /* nearest pending -> PRESENTING */
int      openpete_native_render_scene(int slot, uint64_t record_pts_us);
/* Staged form: render the scene into stage image `stage` now, present it
 * later with openpete_native_present_stage (see rhi_present_stage). The
 * slot's lifecycle is the same as a direct present's. */
int      openpete_native_render_scene_to_stage(int slot, int stage, uint64_t record_pts_us);
int      openpete_native_present_stage_acquire(void);      /* hold a swapchain image for the next present */
int      openpete_native_present_stage(int stage);         /* 1 = presented (acquires if not held) */
int      openpete_native_present_stage_release(void);      /* give back a held acquire on display handoff */
void     openpete_native_display_claim(void);              /* seam fence: calling thread owns the display now */
int      openpete_native_stage_count(void);
uint32_t openpete_native_stage_gen(void);                  /* bumps when the stages are rebuilt */
void     openpete_native_scene_retire(uint64_t begun_serial);
int      openpete_native_scene_stamp_live(uint32_t stamp);
void     openpete_native_scene_ring_flush(void);
int      openpete_native_scene_ring_pending(void);
int      openpete_native_scene_ring_busy(void);
void     openpete_native_scene_slot_info(int slot, int64_t *target_ns, double *alpha, int *paired, int *kind);
void     openpete_native_scene_ring_report(void);
/* Per-present accounting: what each composed frame showed (native scene,
 * VRAM framebuffer fallback, PsyCross reference, bare clear) and which
 * scene kind it came from. Printed at exit. */
void     openpete_native_present_census_report(void);
float    openpete_native_display_refresh_hz(void);   /* current swapchain's display, 0 = unknown */
int      openpete_native_display_vsync(void);        /* FIFO present mode in effect */
int      openpete_native_scene_ring_free(void);
/* Refresh the VRAM mirror from the guest's (dirty rects only). The eager
 * producer calls it while the guest is idle; the inline present path calls
 * it itself. */
void     openpete_native_vram_sync(void);

/* --- sub-tick re-feed spine (high-FPS, native-renderer plan §Movement) ----
 *
 * For smooth presentation above the 29.913 Hz sim, the host loop presents
 * intermediate (sub-tick) frames. Each one rebuilds the native scene against
 * an INTERPOLATED view of gamestate (camera re-run / object lerp / animation
 * clock-advance), keyed off the openpete_interp snapshot ring. The rebuild is
 * game-specific (it knows g_Camera / the feeders), so the game registers a
 * "re-extract" callback; core invokes it on each intermediate present (then
 * presents the rebuilt scene via openpete_native_render_frame).
 *
 * The callback receives the host CPUState (opaque here — the game casts it).
 * It must: query openpete_interp for prev/curr+alpha, compute the interpolated
 * camera/objects, bracket any PSX-RAM writes (save → write-interpolated →
 * re-extract → restore) entirely on the host fiber so the next sim tick reads
 * canonical RAM, and rebuild s_scene. Read-only against gameplay state by the
 * restore discipline; proven by the oracle gate. NULL disables intermediates. */
typedef void (*openpete_native_reextract_fn)(void* cpu);
void openpete_native_register_reextract(openpete_native_reextract_fn fn);
openpete_native_reextract_fn openpete_native_get_reextract(void);

/* Optional per-canonical-present commit callback. The host present loop invokes
 * it ONCE per canonical present (after the game render, before the sub-tick
 * re-extract), for per-frame render bookkeeping the GAME owns but that must be
 * driven from the renderer-agnostic host boundary rather than a game-internal
 * one. Spyro uses it to latch which render paths the canonical frame drew (the
 * mask the spine gates each feeder on) and clear its per-frame interval table —
 * deliberately NOT done inside psyq_DrawOTag so it survives the planned removal
 * of the PsyCross OT submission. NULL = no commit step. */
typedef void (*openpete_native_commit_fn)(void);
void openpete_native_register_commit(openpete_native_commit_fn fn);
openpete_native_commit_fn openpete_native_get_commit(void);

/* ---- moby occlusion culling (native-side, display-only) ---------------------
 *
 * A per-present GPU reduction of the WORLD-opaque depth pre-pass (the shadow
 * decal's pre-pass, shared) into a coarse conservative "farthest view-Z per
 * cell" grid, ring-read-back to the CPU a few presents later. The moby feeder
 * queries it BEFORE re-extracting a cull record: a moby whose whole
 * (conservatively inflated) screen rect is provably behind nearer terrain is
 * skipped — no model walk, no scene tris, no draws — because the depth test
 * would erase every pixel of it anyway. The CANONICAL cull pass and every
 * gameplay-readable byte it writes are untouched (this only skips native
 * drawing), so it is determinism-safe by construction.
 *
 * openpete_native_occl_query: sx/sy in PSX screen px (the scene_vert space),
 * vz_near = the moby's nearest possible view-Z (centre − radius, full-world
 * units). Returns 1 ⇒ provably occluded (safe to skip), 0 ⇒ visible/unknown.
 * Conservative under grid staleness: rect inflation, view-Z slack
 * (OPENPETE_MOBY_OCCL_SLACK), edge/age refusal. OPENPETE_MOBY_OCCLUSION=0
 * disables. openpete_native_occl_suspend(n) disables culling for the next n
 * presents — the feeder calls it on camera cuts/teleports, where a
 * few-presents-old grid is wrong everywhere.
 *
 * Camera bound: the grid is the reduction of the depth pre-pass of the scene
 * one present composed, so it encodes the world as seen from THAT scene's
 * camera. A query evaluated under a different camera reads cell depths at the
 * wrong screen positions; the error is lateral parallax, which the depth slack
 * does not cover, and it removes visible geometry for the one tick the mismatch
 * lasts. Both feeders therefore pass the camera their own walk projects with,
 * and the depth verdict is only taken while that camera is within
 * OPENPETE_OCCL_CAM_MOVE world units and OPENPETE_OCCL_CAM_TURN degrees of the
 * grid's. Outside the bound the query returns 0 (keep): the cull only ever
 * removes work, never a draw. openpete_native_occl_set_camera publishes the
 * producing walk's camera; it is snapshotted per scene, so a present's grid is
 * tagged with the camera whose geometry it reduced. */
typedef struct openpete_occl_camera {
    float pos[3];   /* camera position, full-world units */
    float M[9];     /* the game's camera matrix, rows unit-normalized, row-major */
    int   valid;
} openpete_occl_camera;
void openpete_native_occl_set_camera(const openpete_occl_camera *c);
int  openpete_native_occl_query(float sx0, float sy0, float sx1, float sy1, float vz_near,
                                const openpete_occl_camera *cam);
/* The frustum half of openpete_native_occl_query alone: 1 when the rect lies
 * wholly outside the visible extent, a test on the rect's own geometry that
 * reads no depth and no camera. It returns 0 whenever the query would refuse
 * the grid (absent, suspended, stale), since the extent affine arrives with
 * the grid. A caller that reuses a depth verdict across cameras applies this
 * to its current rect instead of reusing a stored frustum verdict. */
int  openpete_native_occl_offscreen(float sx0, float sy0, float sx1, float sy1, float vz_near);
void openpete_native_occl_suspend(int presents);
/* Query-outcome census (cumulative counters; diagnostics). OFFSCREEN = culled
 * because the rect lies fully outside the visible extent (native frustum cull
 * — the game's cull cone is wider than the screen, so it queues off-screen
 * mobys that contribute zero pixels); CULLED = culled by the depth grid;
 * MOVED = kept because the caller's camera is outside the grid camera's
 * move/turn bound. */
enum { OPENPETE_OCCL_Q_NOGRID = 0, OPENPETE_OCCL_Q_SUSPENDED, OPENPETE_OCCL_Q_STALE,
       OPENPETE_OCCL_Q_TOONEAR, OPENPETE_OCCL_Q_EDGE, OPENPETE_OCCL_Q_VISIBLE,
       OPENPETE_OCCL_Q_CULLED, OPENPETE_OCCL_Q_OFFSCREEN, OPENPETE_OCCL_Q_MOVED,
       OPENPETE_OCCL_Q_COUNT };
void openpete_native_occl_qstats(long out[OPENPETE_OCCL_Q_COUNT]);
/* Diagnostics: the farthest grid view-Z seen in the last CULLED rect scan
 * (how decisively the depth verdict held). Valid right after a query→1. */
float openpete_native_occl_last_scan_max(void);
/* Diagnostics: presents elapsed since the live grid was captured, or -1 when
 * no grid is held. The query refuses a grid older than 8 presents. */
int  openpete_native_occl_grid_age(void);

/* ---- per-present phase timers (OPENPETE_PHASE_TIMERS=1) ---------------------
 *
 * Perf diagnosis: where does a present's CPU time go? Feeders/host-loop add
 * per-phase µs into the current present; openpete_native_render_frame rolls
 * the present into a ring and prints a p50/p99/max summary once per second
 * (stderr), plus the worst-present counts (moby records, scene tris, draw
 * calls). Everything below is inert (single branch) unless the env is set.
 * Diagnostic-only; never touches gameplay state. */
enum {
    OPPT_REX_TOTAL = 0,  /* whole sub-tick re-extract callback (run.c)          */
    OPPT_REX_TERRAIN,    /* terrain feeder pass                                 */
    OPPT_REX_MOBY,       /* moby feeder pass (REG+SPEC+SHD walks)               */
    OPPT_REX_SHADOW,     /* shadow feeder pass                                  */
    OPPT_REX_SPYRO,      /* Spyro player feeder pass                            */
    OPPT_PREPASS,        /* WORLD-opaque depth pre-pass record                  */
    OPPT_COMPOSE,        /* scene bucket + material qsort + draw recording      */
    OPPT_GPU_SYNC,       /* begin_frame fence/acquire + blit + end_frame submit */
    OPPT_SIM_TICK,       /* game-fiber yield: game logic + canonical .s render +
                          * feeder-hook extraction (30Hz cost, folded into the
                          * canonical present; NOT reducible by native culling) */
    OPPT_COUNT
};
enum { OPPC_MOBYS = 0, OPPC_TRIS, OPPC_DRAWS, OPPC_CULLED, OPPC_COUNT };
int      openpete_phase_timers_on(void);
uint64_t openpete_phase_now_us(void);
void     openpete_phase_add(int phase, uint64_t us);
void     openpete_phase_set_count(int which, int n);

/* 2D-overlay carry for the sub-tick re-extract: the fade + cutscene-letterbox +
 * menu-panel overlays are produced by the 2D-UI feeder during the game render
 * and can't be re-derived sub-tick. Carried tris are ACCUMULATED at add_tri
 * time (feed-time capture — the live scene is wiped by every sub-tick
 * re-extract, so scanning it at capture time raced the wipes and dropped any
 * 2D feed an intermediate present landed between; the collected-egg HUD sprite
 * froze/vanished at high --render-fps). capture_overlay() commits the
 * accumulated set as the carry (call on the CANONICAL present, before the
 * re-extract); apply_overlay() re-applies it after the 3D feeders (call at the
 * end of every re-extract). See native_render.c. */
void openpete_native_scene_capture_overlay(void);
void openpete_native_scene_apply_overlay(double alpha);
/* Pre-capture hook: called at the top of capture_overlay so the 2D-UI feeder
 * can flush a deferred ("feed-previous") prim into this tick's accumulation
 * before the carry commits — by the canonical present every post-link prim
 * mutation is done, so the flush is exact. */
void openpete_native_scene_register_overlay_precapture(void (*fn)(void));
/* Roll the fade ramp's prev/curr — call on each CANONICAL present (before the
 * re-extract) so apply_overlay can interpolate the fade sub-tick. */
void openpete_native_scene_commit_fade(void);

/* Capture the offscreen target — whatever the last present blitted, which is
 * the native frame in vk mode and PsyCross's read-back frame in psycross
 * mode, chrome and postfx included — to a PNG at `path`. Call after openpete_native_render_frame so the offscreen holds
 * the latest frame. `what` labels the log line (NULL reads as "offscreen").
 * Returns 0 on success, nonzero if disabled or on failure. */
int openpete_native_render_screenshot(const char *path, const char *what);

/* As above, additionally reporting the written image's extent in `out_w`/
 * `out_h` (both set to 0 when nothing was written). Either pointer may be
 * NULL. Lets a caller that captures a sequence of presents state each
 * present's extent and detect a sequence whose images are not comparable. */
int openpete_native_render_screenshot_ext(const char *path, const char *what,
                                          int *out_w, int *out_h);

/* --record shutdown drain: flush the async readback ring's still-queued tail
 * frames to the encoder (in PTS order) before openpete_record_shutdown()
 * finalizes the file. Call once, after the run loop, while the native device is
 * still alive. No-op unless the native renderer is the display AND recording is
 * active. Blocks (the device is torn down right after). */
void openpete_native_render_record_flush(void);

/* Dump the native view of one --screenshot-on-frame target, writing it to
 * `base` with "-vk" inserted before the extension. In a windowed run the -vk
 * image is a READBACK of the just-presented off_image — ground truth for what
 * the window showed (sub-tick interp scene, mods, postfx, overlays). Headless
 * recomposes the canonical lagged scene instead, the only option without a
 * live present; psycross mode, which has no native scene to recompose, writes
 * the readback and stops. The reference ("-og") is captured separately by the
 * caller. Restores the live layout afterwards. Returns 0 if all variants were written,
 * nonzero otherwise (including when the native path is disabled). */
int openpete_native_render_screenshot_all(const char *base);

/* In-memory twin of the above: the same compose (same capture target, same
 * widescreen widen, same scene selection) read back into a malloc'd top-down
 * RGBA buffer instead of written as a PNG. *out is the caller's to free; *w and
 * *h receive the capture extent. Returns 0 on success. */
int openpete_native_render_capture_rgba(unsigned char **out, int *w, int *h);

/* Save / restore the accumulating scene (s_scene and its per-frame scalars).
 * A caller that drives a re-extract only to inspect its output brackets it with
 * these: the re-extract rebuilds s_scene in place, and the next frame's
 * begin_frame would otherwise snapshot that rebuilt scene into the frame-history
 * ring in place of the canonical one. save returns 0 on success; restore is a
 * no-op when nothing is saved. */
int  openpete_native_scene_save(void);
void openpete_native_scene_restore(void);

/* Intermediate (sub-tick alpha=0.5) capture: make the next
 * openpete_native_render_screenshot_all compose the CURRENT scene (depth 0)
 * instead of the lagged frame-history slot. The re-extract builds the
 * interpolated scene into the current scene and never pushes it to history, so
 * the normal shot-compose delay would read an empty history frame. Set on before
 * the dump, off after. No effect on the canonical dump (leave it 0). */
void openpete_native_render_set_capture_nodelay(int on);

/* Capture-time freeze backdrop: render the current world frame into the capture
 * freeze texture so a later headless dump of a freeze frame (pause / flight-
 * results / level-exit) shows the green frozen-world backdrop. frozen_tex is
 * snapshotted only with a live window, so a headless capture must maintain this
 * itself. No-op on freeze / empty frames, so once the game pauses it holds the
 * last world frame. Call each present during a freeze-backdrop capture run;
 * cap_wf is the capture's widescreen factor (openpete_native_widescreen_factor). */
void openpete_native_render_snapshot_frozen(float cap_wf);

/* Compose this present's frame in memory and give every pending framebuffer
 * window (see openpete_fb_snapshot.h) the crop of it that the game's MoveImage
 * copied, so the quad re-displaying that copy draws the native frame instead of
 * an emulated framebuffer the native render never writes. Runs on the host loop
 * with the display in hand, after the present. */
void openpete_native_render_snapshot_fb_window(void);

/* Tear down the device + window. Safe to call unconditionally. */
void openpete_native_render_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NATIVE_RENDER_H */
