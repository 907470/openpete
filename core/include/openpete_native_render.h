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
 * Gated entirely by --renderer vk|ab|diff. With --renderer psycross (the
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

/* True once init succeeded and the native A/B display is live. */
bool openpete_native_render_enabled(void);

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

/* Live fullscreen state (1 = borderless-desktop fullscreen, 0 = windowed or
 * no native window). The O-menu DISPLAY row reads this each frame instead of
 * keeping a shadow index, so it stays in sync with Alt+Enter / double-click. */
int openpete_native_fullscreen_active(void);

/* Performance-stats overlay (vk live window). Drawn only on the live vk
 * present (never ab/diff/screenshots). The level is one of the
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

/* One vertex of game-supplied FPS-overlay geometry, in a resolution-independent
 * overlay space: x/y are in units of WINDOW HEIGHT (origin = top-left, +y down,
 * so x and y share one scale → digits stay square at any aspect); z is a depth
 * in [0,1) for the overlay's own mini depth pass (smaller = nearer). rgba is
 * byte order R,G,B,A. */
typedef struct { float x, y, z; uint32_t rgba; } openpete_hud_vert;

/* Hand the native renderer overlay geometry built from Spyro's own 3D HUD text
 * mobys (the gold extruded glyph models, g_Models[260..269] digits / 426..451
 * letters / punctuation). `verts` is a triangle list (3 verts per tri), built
 * each game frame by the game-side emitter (text_moby_native). The renderer
 * copies them and draws them (depth-tested, on top) on the live vk present in
 * place of the 5x7 bitmap FPS font. nverts==0 → fall back to the bitmap font
 * (e.g. before the glyph models have loaded). Currently the FPS counter; reusable
 * for other text overlays. Display-only; never touches gameplay state. */
void openpete_native_set_overlay_glyphs(const openpete_hud_vert *verts, int nverts);

/* Mod draw-service overlay (core/mods/mod_draw.c): rects + text glyphs in the
 * same overlay space, pushed once per present. Drawn on the vk variant in the
 * live present AND screenshot/record composes (mod HUD is content, not
 * chrome). nverts==0 clears it. Display-only. */
void openpete_native_set_mod_overlay(const openpete_hud_vert *verts, int nverts);

/* Window aspect (w/h) for overlay-space layout: x spans [0, aspect], y spans
 * [0, 1]. Falls back to 4:3 before the window exists. */
float openpete_native_overlay_aspect(void);

/* Hand the native renderer the in-game settings menu's glyph geometry (the row
 * labels/values/title, built game-side by text_moby_native from the same 3D gold
 * glyph mobys, with the selected row's per-glyph yaw baked in). Same overlay
 * space as set_overlay_glyphs; drawn by draw_menu_overlay on the live vk present
 * over the menu panel. nverts==0 clears it. Display-only. */
void openpete_native_set_menu_glyphs(const openpete_hud_vert *verts, int nverts);

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
/* z is the depth used for the native depth buffer (3a.3): NDC [0,1], 0=near,
 * 1=far. Actors map their view-space Z through openpete_native_depth_from_viewz;
 * the cyclorama/sky passes 1.0 (always the backdrop). */
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
 * ordering. 0 = no shift applied: W is derived from `z` (the historical
 * behaviour, exact for every unbiased feeder). */
typedef struct { float sx, sy, z; float u, v; uint32_t rgba; float pvz; } openpete_scene_vert;
/* A scene triangle. When `textured` is 0 it is gouraud-shaded from the vertex
 * rgba (the cyclorama, and untextured moby faces). When 1, it samples the PSX
 * VRAM mirror via the material below (tpage/CLUT/bpp in VRAM-pixel units, exactly
 * the GTE/GPU encoding) and modulates by rgba (tex*colour/128). 3a.3. */
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
 * portal arch (portal_native.c). It is a far BACKGROUND seen through the arch
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
/* SKY — the main cyclorama dome (cyclorama_native.c). Drawn exactly like WORLD
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
} openpete_scene_tri;

/* ---- view-Z → depth-buffer conversion: the ONE named point (W0.7b) --------
 *
 * The native depth buffer stores z = vz / (vz + OPENPETE_DEPTH_VIEWZ_SCALE),
 * 0(near)..→1(far), monotonic in distance, where vz is FULL-WORLD view-space Z
 * (the GTE MAC3 of full-world camera-relative coords; larger = farther,
 * 1.0 == 4096 GTE units). Not perspective-exact — the native render needs
 * correct ORDERING, not PSX-bit-identical Z; 4096 spreads typical actor depths
 * across the usable range.
 *
 * The conversion takes an openpete_worldvz, which can ONLY be built through
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
 * GPU mirrors of this mapping / its inverse (keep in sync if the constant
 * ever changes): core/rhi/shaders/psx_scene.vert, quad.vert, prepass.vert,
 * shadow_decal.frag (viewz_from_scene_depth). */
#define OPENPETE_DEPTH_VIEWZ_SCALE 4096.0f

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
static inline float openpete_native_depth_from_viewz(openpete_worldvz z) {
    float vz = z.vz;
    if (vz < 1.0f) vz = 1.0f;              /* near guard (behind-camera is culled) */
    return vz / (vz + OPENPETE_DEPTH_VIEWZ_SCALE);  /* 0(near) .. ->1(far), never reaches 1 */
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
    /* Raised-occluder-in-front clause (PSX painter parity for curbs /
     * raised platforms): discard fan pixels whose pre-pass terrain is
     * nearer than the fan fragment by more than plat_slack view-Z units
     * AND raised more than plat_eps above the shadow plane. */
    float plat_eps;
    float plat_slack;
    int   valid;
} openpete_shadow_decal_params;
void openpete_native_set_shadow_decal_params(const openpete_shadow_decal_params *p);

/* Begin a fresh native-scene frame (clears the triangle list). Called by
 * the game at the start of its scene extraction. */
void openpete_native_scene_begin(void);
/* Frame-keyed begin: clears the scene only when `frame` differs from the last
 * begin_frame call, otherwise a no-op. This lets MULTIPLE independent feeders
 * (cyclorama, then the world, then mobys — whatever order they hook in within
 * one game frame) compose into a single scene without any one of them wiping
 * the others: the first feeder of each frame clears, the rest append. Pass
 * `openpete_frame` (the per-yield frame counter). Order-independent and robust
 * to a feeder being absent on a given level. */
void openpete_native_scene_begin_frame(unsigned frame);
/* Frame stamp of the currently accumulating scene (0xFFFFFFFF = none).
 * Late feeders (mod actor render) must check this and append only when
 * the scene is live — begin_frame with a NEWER frame wipes a held scene
 * (load stalls hold the last composed frame on purpose). */
unsigned openpete_native_scene_frame(void);
/* Mod actor render: hide the PLAYER-category scene tris for the frames
 * where a stand-in actor is drawn instead (the flutter's hover window).
 * Explicit per-present state — the mod draw service's flush sets it
 * both ways every present. Draw-only (occlusion/census unaffected). */
void openpete_native_set_player_suppress(int on);
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
int      openpete_native_stage_count(void);
uint32_t openpete_native_stage_gen(void);                  /* bumps when the stages are rebuilt */
void     openpete_native_scene_retire(uint64_t begun_serial);
int      openpete_native_scene_stamp_live(uint32_t stamp);
void     openpete_native_scene_ring_flush(void);
int      openpete_native_scene_ring_pending(void);
int      openpete_native_scene_ring_busy(void);
void     openpete_native_scene_slot_info(int slot, int64_t *target_ns, double *alpha, int *paired, int *kind);
void     openpete_native_scene_ring_report(void);
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
 * few-presents-old grid is wrong everywhere. */
int  openpete_native_occl_query(float sx0, float sy0, float sx1, float sy1, float vz_near);
void openpete_native_occl_suspend(int presents);
/* Query-outcome census (cumulative counters; diagnostics). OFFSCREEN = culled
 * because the rect lies fully outside the visible extent (native frustum cull
 * — the game's cull cone is wider than the screen, so it queues off-screen
 * mobys that contribute zero pixels); CULLED = culled by the depth grid. */
enum { OPENPETE_OCCL_Q_NOGRID = 0, OPENPETE_OCCL_Q_SUSPENDED, OPENPETE_OCCL_Q_STALE,
       OPENPETE_OCCL_Q_TOONEAR, OPENPETE_OCCL_Q_EDGE, OPENPETE_OCCL_Q_VISIBLE,
       OPENPETE_OCCL_Q_CULLED, OPENPETE_OCCL_Q_OFFSCREEN, OPENPETE_OCCL_Q_COUNT };
void openpete_native_occl_qstats(long out[OPENPETE_OCCL_Q_COUNT]);
/* Diagnostics: the farthest grid view-Z seen in the last CULLED rect scan
 * (how decisively the depth verdict held). Valid right after a query→1. */
float openpete_native_occl_last_scan_max(void);

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
/* Drop the pending accumulation without committing it. Call on canonical
 * presents that SKIP capture_overlay (spine off / unlock) so a long spine-off
 * stretch doesn't pool stale 2D-UI tris that would flash when it re-engages. */
void openpete_native_scene_overlay_accum_reset(void);
/* Roll the fade ramp's prev/curr — call on each CANONICAL present (before the
 * re-extract) so apply_overlay can interpolate the fade sub-tick. */
void openpete_native_scene_commit_fade(void);

/* Capture the native A/B window (the offscreen target — both panels, the
 * native render + the reference) to a PNG at `path`. Call after
 * openpete_native_render_frame so the offscreen holds the latest frame.
 * Returns 0 on success, nonzero if disabled or on failure. */
int openpete_native_render_screenshot(const char *path);

/* --record shutdown drain: flush the async readback ring's still-queued tail
 * frames to the encoder (in PTS order) before openpete_record_shutdown()
 * finalizes the file. Call once, after the run loop, while the native device is
 * still alive. No-op unless the native renderer is the display AND recording is
 * active. Blocks (the device is torn down right after). */
void openpete_native_render_record_flush(void);

/* Dump every native renderer variant for one --screenshot-on-frame target,
 * regardless of the live --renderer mode, writing each to `base` with
 * "-vk"/"-ab"/"-diff" inserted before the extension. In a windowed pure-vk run
 * the -vk variant is a READBACK of the just-presented off_image — ground truth
 * for what the window showed (sub-tick interp scene, mods, postfx, overlays);
 * headless / ab / diff runs recompose the canonical lagged scene instead (the
 * only option without a live present). -ab (native|reference) and -diff
 * (|native - ref|) are always recomposed, -og-aligned, for the PsyCross
 * compare. The reference ("-og") is captured separately by the caller.
 * Restores the live layout afterwards. Returns 0 if all variants were written,
 * nonzero otherwise (including when the native path is disabled). */
int openpete_native_render_screenshot_all(const char *base);

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

/* Tear down the device + window. Safe to call unconditionally. */
void openpete_native_render_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NATIVE_RENDER_H */
