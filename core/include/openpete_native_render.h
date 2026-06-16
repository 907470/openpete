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
/* blend: 0 = opaque; 1 = PSX semi-transparent ABR mode 0 ((B+F)/2) -> alpha blend
 * (the textured frag's per-texel STP-derived alpha — 0.5 for STP texels, 1.0
 * otherwise — reproduces mode 0 WITH per-texel STP gating); 2 = ABR mode 1 (B+F)
 * -> additive blend (3b.1, flame/glows: src ONE + dst ONE, depth_write_off).
 * ABR modes 2/3 (subtract/quarter) + back-to-front sort are still TODO (no
 * consumer yet). Only meaningful for textured tris; untextured gouraud is opaque. */
typedef struct {
    openpete_scene_vert v[3];
    int textured;
    int tpage_x, tpage_y, clut_x, clut_y, bpp;
    int blend;
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
/* Append one screen-space triangle to the current native scene. */
void openpete_native_scene_add_tri(const openpete_scene_tri *tri);
/* Set the panel-0 background fill colour (rgba, byte order R,G,B,A) — drawn
 * before the scene so uncovered areas show it instead of black, mirroring the
 * PSX cyclorama's frame clear to m_BackgroundColor. 0 = no fill (black).
 * Reset to 0 by openpete_native_scene_begin. */
void openpete_native_scene_set_bg(uint32_t rgba);
/* True when the native display is live (so the game only does the extra
 * extraction work when it will actually be shown). */
/* (openpete_native_render_enabled, above, serves this gate.) */

/* Present one native frame to the Vulkan window. Call after PsyX_EndScene so
 * PsyCross's present FBO is freshly rendered (the future "reference"). Does
 * NOT pump SDL events — the host loop owns the event queue, which PsyCross's
 * input path depends on; window-size changes are detected by polling. No-op
 * if the native path is disabled. */
void openpete_native_render_frame(void);

/* Capture the native A/B window (the offscreen target — both panels, the
 * native render + the reference) to a PNG at `path`. Call after
 * openpete_native_render_frame so the offscreen holds the latest frame.
 * Returns 0 on success, nonzero if disabled or on failure. */
int openpete_native_render_screenshot(const char *path);

/* Tear down the device + window. Safe to call unconditionally. */
void openpete_native_render_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NATIVE_RENDER_H */
