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
int openpete_native_render_init(const openpete_options_t *opts, int win_w, int win_h);

/* True once init succeeded and the native A/B display is live. */
bool openpete_native_render_enabled(void);

/* Toggle the native window between windowed and borderless-desktop fullscreen.
 * Bound to Alt+Enter and double-click by run.c. No-op until the native window
 * is up (so it's inert in headless / PsyCross-only runs). */
void openpete_native_toggle_fullscreen(void);

/* FPS overlay (vk live window). Default on; drawn only on the live vk present
 * (never ab/diff/screenshots). set() forces a state; toggle() flips and returns
 * the new state (for the F11 hotkey). Display-only. */
void openpete_native_set_fps_overlay(int on);
int  openpete_native_toggle_fps_overlay(void);

/* Is the FPS overlay currently on? The game-side digit-model emitter reads this
 * to skip building geometry when the overlay is off. */
int  openpete_native_fps_overlay_enabled(void);
/* The current rolling FPS value the overlay displays. The game-side emitter
 * reads it to know which digit glyphs (0-9) to build. */
int  openpete_native_fps_value(void);

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
typedef struct { float sx, sy, z; float u, v; uint32_t rgba; } openpete_scene_vert;
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
 * non-opaque tris (blend!=0 routes by blend). Stamped by add_tri from the
 * current category (see openpete_native_scene_set_category), so feeders that
 * don't care default to WORLD. */
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
/* PORTAL_SKY — the destination-level cyclorama preview shown inside a homeworld
 * portal arch (portal_native.c). It is a far BACKGROUND seen through the arch
 * window: the arch stone must occlude it, but it must never occlude actors or
 * Spyro flying into/through the portal. Drawn in its own pass AFTER the WORLD
 * group (so it depth-tests against the already-written arch/terrain depth) but
 * with depth-WRITE OFF, so the actors/player drawn next are never per-pixel cut
 * by it. This mirrors the PSX OT, where the portal sky occupies one coarse
 * bucket and the player's whole model sorts in front of it (never intersecting
 * it per-pixel the way a shared Z-buffer otherwise would). Untextured only;
 * self-occlusion comes from the feeder's own far→near painter sort. */
#define OPENPETE_SCENE_PORTAL_SKY 7
/* carry: 1 = this tri is screen-space 2D-UI that the sub-tick re-extract canNOT
 * re-derive (it has no 2D-prim chokepoint to hook), so it must be snapshotted
 * before the rebuild and re-applied after — like the OVERLAY/HUD_PANEL carry, but
 * for tris that share a re-derivable category (the 2D-UI sprites/lines live in
 * OPENPETE_SCENE_HUD, which the moby feeder ALSO populates with re-derived near-OT
 * billboards). A category check can't tell them apart; this flag can. Stamped by
 * add_tri from openpete_native_scene_set_carry (0 unless a feeder opts in). */
typedef struct {
    openpete_scene_vert v[3];
    int textured;
    int tpage_x, tpage_y, clut_x, clut_y, bpp;
    int blend;
    int category;
    int carry;
} openpete_scene_tri;

/* Map a GTE view-space Z (larger = farther; same units the GTE projects with,
 * 1.0 == 4096) to an NDC depth in [0,1) that is monotonic in distance, so the
 * depth buffer sorts actors near→far. Not perspective-exact (the native render
 * only needs correct ORDERING, not PSX-bit-identical Z); the 4096 scale spreads
 * typical actor depths across the usable range. Shared by every feeder so their
 * depths are mutually comparable. */
static inline float openpete_native_depth_from_viewz(float vz) {
    if (vz < 1.0f) vz = 1.0f;              /* near guard (behind-camera is culled) */
    return vz / (vz + 4096.0f);            /* 0(near) .. ->1(far), never reaches 1 */
}

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
/* Append one screen-space triangle to the current native scene. The tri's
 * `category` is overwritten with the current scene category (set_category). */
void openpete_native_scene_add_tri(const openpete_scene_tri *tri);
/* Set the layer that subsequent add_tri calls are stamped with
 * (OPENPETE_SCENE_WORLD / _ACTOR). Actor feeders (Spyro body, mobys) bracket
 * their emit with set_category(ACTOR) ... set_category(WORLD). Reset to WORLD at
 * each frame's first begin_frame and by scene_begin, so a feeder that never
 * calls it always emits WORLD geometry. */
void openpete_native_scene_set_category(int category);
/* Mark subsequent add_tri calls as carry=1 (sub-tick-uncarryable 2D-UI: the title
 * logo / "PRESS START" / HUD glyph FT4 sprites + UI separator lines that the 2D-UI
 * feeder builds during the game render and the re-extract can't re-derive). The
 * overlay-carry snapshots these (alongside OVERLAY/HUD_PANEL) so they survive the
 * sub-tick rebuild instead of strobing at 30 Hz. Reset to 0 at each frame's first
 * begin_frame and by scene_begin; the 2D-UI feeder brackets its HUD-sprite emit
 * with set_carry(1) ... set_carry(0). */
void openpete_native_scene_set_carry(int on);
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

/* --- sub-tick re-feed spine (high-FPS, native-renderer plan §Movement) ----
 *
 * For smooth presentation above the 29.97 Hz sim, the host loop presents
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

/* 2D-overlay carry for the sub-tick re-extract: the fade + cutscene-letterbox +
 * menu-panel overlays are produced by the 2D-UI feeder during the game render
 * and can't be re-derived sub-tick. capture_overlay() snapshots them from the
 * live scene (call before scene_begin); apply_overlay() re-applies them after
 * the 3D feeders (call at the end of the re-extract). See native_render.c. */
void openpete_native_scene_capture_overlay(void);
void openpete_native_scene_apply_overlay(double alpha);
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
 * regardless of the live --renderer mode: re-renders the scene as vk (single
 * panel), A/B (native|reference), and diff (|native - ref|), writing each to
 * `base` with "-vk"/"-ab"/"-diff" inserted before the extension. The PsyCross
 * reference ("-og") is captured separately by the caller. Restores the live
 * layout afterwards. Returns 0 if all variants were written, nonzero otherwise
 * (including when the native path is disabled). */
int openpete_native_render_screenshot_all(const char *base);

/* Tear down the device + window. Safe to call unconditionally. */
void openpete_native_render_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NATIVE_RENDER_H */
