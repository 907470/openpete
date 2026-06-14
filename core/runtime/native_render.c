/* core/runtime/native_render.c — native Vulkan renderer A/B harness.
 *
 * Phase 1c of notes/plans/openpete-native-renderer.md. See
 * openpete_native_render.h for the contract + the two-window rationale.
 *
 * 1c.1: RHI in the runtime, Vulkan window alongside PsyCross.
 * 1c.2: PsyCross hidden; its frame read back + shown in the single window.
 * 1c.3a (this step): also mirror PsyCross's VRAM into a native R16_UINT
 *   texture each present and show it (decoded by the ported PSX sampler) as a
 *   second panel — proving real game VRAM (textures + the rendered
 *   framebuffer) reaches the native GPU path. Layout is now a row of
 *   aspect-fit panels: [VRAM | reference]. 1c.3b adds a native render of the
 *   display region + a diff panel and validates against the logo screen.
 *
 * Determinism: nothing here touches PSX RAM or the `.s` verdict producers.
 */

#include "openpete_native_render.h"

#include "openpete_log.h"
#include "rhi.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* PsyCross VRAM + present-FBO readback (extern "C" in PsyX_render.cpp);
 * forward-declared so this C TU need not include the C++-flavored header.
 * GR_ReadVRAM copies a rect of the 1024x512 CPU VRAM mirror tightly into dst;
 * GR_ReadPresentSceneRGBA returns the present FBO bottom-up (GL origin). */
extern int  GR_GetPresentSceneSize(int *out_w, int *out_h);
extern int  GR_ReadPresentSceneRGBA(unsigned char *pixels, int w, int h,
                                    unsigned int gl_format);
extern void GR_ReadVRAM(unsigned short *dst, int x, int y, int dst_w, int dst_h);

/* GL_RGBA enum — byte order R,G,B,A, matching VK_FORMAT_R8G8B8A8_UNORM. */
#define NR_GL_RGBA 0x1908u

#define NR_VRAM_W 1024
#define NR_VRAM_H 512

/* Default window size when the caller passes none. */
#define NR_DEFAULT_W 1280
#define NR_DEFAULT_H 720

/* Per-draw PSX material — must match the push_constant block in
 * shaders/psx_sample.frag (five tightly-packed 32-bit ints). */
typedef struct {
    int32_t tpage_x, tpage_y;
    int32_t clut_x, clut_y;
    int32_t bpp;
} psx_material;

/* How many panels the window shows (1c.3a: VRAM + reference). */
#define NR_PANELS 2

/* Single native renderer instance (one window per process). */
static struct {
    bool          enabled;
    SDL_Window   *window;
    rhi_device   *dev;
    int           win_w, win_h; /* last-known pixel size, for resize detection */

    /* Reference panel: PsyCross's read-back frame (RGBA8). */
    rhi_pipeline *blit_pipe;
    rhi_texture  *ref_tex;
    int           ref_w, ref_h;
    unsigned char *readback;    /* CPU readback scratch, ref_w*ref_h*4 */
    size_t        readback_cap;

    /* VRAM panel: PsyCross's 1024x512 VRAM mirror (R16_UINT), decoded by the
     * ported PSX sampler. */
    rhi_pipeline *vram_pipe;
    rhi_texture  *vram_tex;
    uint16_t     *vram_mirror;  /* NR_VRAM_W*NR_VRAM_H */

    /* Panel geometry: one VB of NR_PANELS*6 verts, rebuilt only when the
     * window or any panel's content size changes. */
    rhi_buffer   *panels_vb;
    int           layout_win_w, layout_win_h, layout_ref_w, layout_ref_h;
} s_nr;

/* Fill a panel quad: clip-space rect centered at (cx,cy) with half-extents
 * (hx,hy); v0 is the UV-v at the TOP edge, v1 at the BOTTOM. (VRAM is uploaded
 * top-down so it uses v0<v1; the GL readback is bottom-up so the reference
 * uses v0>v1.) rgba is unused by the textured shaders. */
static void build_panel(rhi_vertex out[6], float cx, float cy, float hx, float hy,
                        float u0, float v0, float u1, float v1)
{
    const rhi_vertex tl = { cx - hx, cy + hy, 0.0f, u0, v0, 0xFFFFFFFFu, 0 };
    const rhi_vertex tr = { cx + hx, cy + hy, 0.0f, u1, v0, 0xFFFFFFFFu, 0 };
    const rhi_vertex br = { cx + hx, cy - hy, 0.0f, u1, v1, 0xFFFFFFFFu, 0 };
    const rhi_vertex bl = { cx - hx, cy - hy, 0.0f, u0, v1, 0xFFFFFFFFu, 0 };
    out[0] = tl; out[1] = tr; out[2] = br;
    out[3] = tl; out[4] = br; out[5] = bl;
}

/* Aspect-fit content (content_w x content_h px) inside a clip-space cell
 * centered at (cx,cy) with clip half-extents (chx,chy), returning the fitted
 * half-extents (letterbox/pillarbox; the clear fills the bars). */
static void fit_cell(float cx, float cy, float chx, float chy,
                     int content_w, int content_h, int win_w, int win_h,
                     float *out_hx, float *out_hy)
{
    (void)cx; (void)cy;
    float cell_w = chx * (float)win_w;  /* cell size in px (full clip = win px) */
    float cell_h = chy * (float)win_h;
    float s = cell_w / (float)content_w;
    float sy = cell_h / (float)content_h;
    if (sy < s) s = sy;
    *out_hx = ((float)content_w * s) / (float)win_w;
    *out_hy = ((float)content_h * s) / (float)win_h;
}

int openpete_native_render_init(const openpete_options_t *opts, int win_w, int win_h)
{
    if (!opts || !opts->native_ab)
        return 0; /* not requested — inert no-op */

    if (opts->headless) {
        OPL_WARN_F("native", "--native-ab ignored under --headless (no display)");
        return 0;
    }

    /* SDL_INIT_VIDEO is already up (PsyX_Initialise). Create a SECOND window
     * for the Vulkan surface: a single SDL window can't carry both PsyCross's
     * GL context and our Vulkan surface, so the native renderer owns its own.
     * PsyCross's GL window is started hidden (see run.c), so this Vulkan
     * window is the single visible one. Match the game's resolved window size
     * so it isn't surprisingly larger than the PsyCross-only window. */
    int w = (win_w > 0) ? win_w : NR_DEFAULT_W;
    int h = (win_h > 0) ? win_h : NR_DEFAULT_H;

    SDL_Window *win = SDL_CreateWindow("openpete — native renderer (A/B)", w, h,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        OPL_ERROR_F("native", "SDL_CreateWindow(VULKAN) failed: %s", SDL_GetError());
        return 1;
    }

    rhi_device *dev = rhi_create(win, "openpete-native");
    if (!dev) {
        OPL_ERROR_F("native", "rhi_create failed; continuing without native A/B");
        SDL_DestroyWindow(win);
        return 1;
    }

    rhi_pipeline_desc blit_desc = {
        .vert_spv = "psx_sample.vert.spv", /* passthrough pos + UV */
        .frag_spv = "blit.frag.spv",       /* sampler2D at set 0 */
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = 0,
    };
    rhi_pipeline_desc vram_desc = {
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "psx_sample.frag.spv", /* usampler2D + material push const */
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    rhi_pipeline *blit_pipe = rhi_create_pipeline(dev, &blit_desc);
    rhi_pipeline *vram_pipe = rhi_create_pipeline(dev, &vram_desc);
    rhi_buffer   *panels_vb = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * NR_PANELS * 6);
    rhi_texture  *vram_tex  = rhi_create_vram(dev, NR_VRAM_W, NR_VRAM_H);
    uint16_t     *mirror    = calloc((size_t)NR_VRAM_W * NR_VRAM_H, sizeof(uint16_t));
    if (!blit_pipe || !vram_pipe || !panels_vb || !vram_tex || !mirror) {
        OPL_ERROR_F("native", "RHI setup failed; continuing without native A/B");
        free(mirror);
        if (vram_tex)  rhi_destroy_texture(dev, vram_tex);
        if (panels_vb) rhi_destroy_buffer(dev, panels_vb);
        if (vram_pipe) rhi_destroy_pipeline(dev, vram_pipe);
        if (blit_pipe) rhi_destroy_pipeline(dev, blit_pipe);
        rhi_destroy(dev);
        SDL_DestroyWindow(win);
        return 1;
    }

    s_nr.enabled     = true;
    s_nr.window      = win;
    s_nr.dev         = dev;
    s_nr.win_w       = w;
    s_nr.win_h       = h;
    s_nr.blit_pipe   = blit_pipe;
    s_nr.vram_pipe   = vram_pipe;
    s_nr.panels_vb   = panels_vb;
    s_nr.vram_tex    = vram_tex;
    s_nr.vram_mirror = mirror;
    OPL_INFO_F("native", "Vulkan A/B window up (%dx%d, device='%s'); PsyCross hidden",
               w, h, rhi_device_name(dev));
    return 0;
}

bool openpete_native_render_enabled(void)
{
    return s_nr.enabled;
}

/* Mirror PsyCross's whole VRAM (1024x512) into the native R16_UINT texture.
 * Full snapshot for now; 1c's dirty-rect sub-rect path (driven by hooking the
 * image ops) is a later efficiency step. Always available (reads PsyCross's
 * CPU mirror), even before the first scene. */
static void sync_vram_texture(void)
{
    GR_ReadVRAM(s_nr.vram_mirror, 0, 0, NR_VRAM_W, NR_VRAM_H);
    rhi_update_vram(s_nr.dev, s_nr.vram_tex, s_nr.vram_mirror);
}

/* Pull PsyCross's finished frame from its present FBO into s_nr.ref_tex.
 * Returns true if a reference frame is ready to draw this present. */
static bool sync_reference_texture(void)
{
    int pw = 0, ph = 0;
    if (!GR_GetPresentSceneSize(&pw, &ph) || pw <= 0 || ph <= 0)
        return false; /* PsyCross hasn't produced a scene yet */

    if (pw != s_nr.ref_w || ph != s_nr.ref_h) {
        if (s_nr.ref_tex) {
            rhi_wait_idle(s_nr.dev);
            rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
            s_nr.ref_tex = NULL;
        }
        s_nr.ref_tex = rhi_create_rgba8(s_nr.dev, (uint32_t)pw, (uint32_t)ph);
        s_nr.ref_w = pw;
        s_nr.ref_h = ph;

        size_t need = (size_t)pw * (size_t)ph * 4u;
        if (need > s_nr.readback_cap) {
            free(s_nr.readback);
            s_nr.readback = malloc(need);
            s_nr.readback_cap = s_nr.readback ? need : 0;
        }
        OPL_INFO_F("native", "reference FBO size %dx%d", pw, ph);
    }

    if (!s_nr.ref_tex || !s_nr.readback)
        return false;
    if (!GR_ReadPresentSceneRGBA(s_nr.readback, pw, ph, NR_GL_RGBA))
        return false;

    rhi_update_rgba8(s_nr.dev, s_nr.ref_tex, s_nr.readback);
    return true;
}

/* Rebuild the panel row (VRAM | reference), each aspect-fit into its cell, so
 * resizing never stretches. Only runs when the window or reference size
 * changes (the VRAM panel is a fixed 1024x512). */
static void update_layout(void)
{
    if (s_nr.win_w <= 0 || s_nr.win_h <= 0 || s_nr.ref_w <= 0 || s_nr.ref_h <= 0)
        return;
    if (s_nr.win_w == s_nr.layout_win_w && s_nr.win_h == s_nr.layout_win_h &&
        s_nr.ref_w == s_nr.layout_ref_w && s_nr.ref_h == s_nr.layout_ref_h)
        return;

    /* NR_PANELS cells across the window; cell i centered at cx_i, half-width
     * 1/NR_PANELS in clip space, full height. */
    const float chx = 1.0f / (float)NR_PANELS;
    rhi_vertex verts[NR_PANELS * 6];

    /* Panel 0 — VRAM (R16_UINT, top-down: v0=0 top .. v1=H bottom), UVs in
     * texel units (psx_sample uses texelFetch). */
    {
        float cx = -1.0f + chx, hx, hy;
        fit_cell(cx, 0.0f, chx, 1.0f, NR_VRAM_W, NR_VRAM_H, s_nr.win_w, s_nr.win_h, &hx, &hy);
        build_panel(&verts[0], cx, 0.0f, hx, hy,
                    0.0f, 0.0f, (float)NR_VRAM_W, (float)NR_VRAM_H);
    }
    /* Panel 1 — reference (RGBA8, bottom-up: v0=1 top .. v1=0 bottom), UVs
     * normalized (blit uses texture()). */
    {
        float cx = -1.0f + 3.0f * chx, hx, hy;
        fit_cell(cx, 0.0f, chx, 1.0f, s_nr.ref_w, s_nr.ref_h, s_nr.win_w, s_nr.win_h, &hx, &hy);
        build_panel(&verts[6], cx, 0.0f, hx, hy, 0.0f, 1.0f, 1.0f, 0.0f);
    }

    /* The VB may still be read by an in-flight frame; layout changes are rare
     * (startup / resize), so fully sync before rewriting it. */
    rhi_wait_idle(s_nr.dev);
    rhi_update_buffer(s_nr.panels_vb, verts, sizeof(verts));
    s_nr.layout_win_w = s_nr.win_w; s_nr.layout_win_h = s_nr.win_h;
    s_nr.layout_ref_w = s_nr.ref_w; s_nr.layout_ref_h = s_nr.ref_h;
}

void openpete_native_render_frame(void)
{
    if (!s_nr.enabled)
        return;

    /* Detect window-size changes WITHOUT polling SDL events (the host loop
     * owns the event queue, and stealing events would starve PsyCross input). */
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(s_nr.window, &pw, &ph) && pw > 0 && ph > 0 &&
        (pw != s_nr.win_w || ph != s_nr.win_h)) {
        s_nr.win_w = pw;
        s_nr.win_h = ph;
        rhi_notify_resize(s_nr.dev);
    }

    /* Mirror VRAM + read the reference back BEFORE opening our frame: these do
     * their own one-shot GPU submits, kept clear of our render pass. */
    sync_vram_texture();
    bool have_ref = sync_reference_texture();
    update_layout();

    if (!rhi_begin_frame(s_nr.dev))
        return; /* swapchain recreating or window minimized — skip this present */

    rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);

    /* Panel 0 — VRAM, decoded as 16bpp across the whole 1024x512 (the
     * framebuffer region reads correctly; paletted texture regions look
     * scrambled, which is expected for a raw 16bpp VRAM view). */
    if (s_nr.layout_ref_w > 0) { /* layout built (needs a known ref size) */
        const psx_material vram_mat = { 0, 0, 0, 0, 16 };
        rhi_bind_pipeline(s_nr.dev, s_nr.vram_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.vram_tex);
        rhi_push_constants(s_nr.dev, &vram_mat, sizeof(vram_mat));
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
        rhi_draw(s_nr.dev, 6, 0);

        /* Panel 1 — reference. */
        if (have_ref) {
            rhi_bind_pipeline(s_nr.dev, s_nr.blit_pipe);
            rhi_bind_vram(s_nr.dev, s_nr.ref_tex);
            rhi_bind_vertex_buffer(s_nr.dev, s_nr.panels_vb);
            rhi_draw(s_nr.dev, 6, 6);
        }
    }

    rhi_end_offscreen(s_nr.dev);
    rhi_blit_to_swapchain(s_nr.dev);
    rhi_end_frame(s_nr.dev);
}

void openpete_native_render_shutdown(void)
{
    if (!s_nr.enabled)
        return;
    rhi_wait_idle(s_nr.dev);
    if (s_nr.ref_tex)   rhi_destroy_texture(s_nr.dev, s_nr.ref_tex);
    if (s_nr.vram_tex)  rhi_destroy_texture(s_nr.dev, s_nr.vram_tex);
    if (s_nr.panels_vb) rhi_destroy_buffer(s_nr.dev, s_nr.panels_vb);
    if (s_nr.vram_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.vram_pipe);
    if (s_nr.blit_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.blit_pipe);
    rhi_destroy(s_nr.dev);
    SDL_DestroyWindow(s_nr.window);
    free(s_nr.readback);
    free(s_nr.vram_mirror);

    memset(&s_nr, 0, sizeof(s_nr));
    OPL_INFO_F("native", "Vulkan A/B window shut down");
}
