/* core/runtime/native_render.c — native Vulkan renderer A/B harness.
 *
 * Phase 1c of notes/plans/openpete-native-renderer.md. See
 * openpete_native_render.h for the contract + the two-window rationale.
 *
 * 1c.1: stood the RHI up inside the real runtime (Vulkan window presenting
 * alongside PsyCross) — proved GL and Vulkan coexist in one process.
 *
 * 1c.2 (this step): PsyCross's GL window is now HIDDEN (FBO-only) and the
 * Vulkan window is the single visible one. Each present we read PsyCross's
 * finished frame back from its FBO (GR_ReadPresentSceneRGBA), upload it to an
 * RGBA8 texture, and draw it fullscreen in the Vulkan window — the
 * "reference" the native render will sit beside in 1c.3. This proves the
 * GL->VK readback bridge end to end: the live game, displayed through Vulkan.
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

/* PsyCross present-FBO readback. These are `extern "C"` in PsyCross
 * (PsyX_render.cpp); forward-declared here so this C TU need not include the
 * C++-flavored PsyX_render.h. GR_ReadPresentSceneRGBA returns rows bottom-up
 * (GL origin); we correct that in the fullscreen quad's UVs, not on the CPU. */
extern int GR_GetPresentSceneSize(int *out_w, int *out_h);
extern int GR_ReadPresentSceneRGBA(unsigned char *pixels, int w, int h,
                                   unsigned int gl_format);

/* GL_RGBA enum value — byte order R,G,B,A, matching VK_FORMAT_R8G8B8A8_UNORM.
 * Spelled as a literal so we don't pull a GL header into a C core TU. */
#define NR_GL_RGBA 0x1908u

/* Default A/B window size when the user didn't pin --window. Wide enough for
 * the native/reference/diff three-panel composite that lands in 1c.3. */
#define NR_DEFAULT_W 1280
#define NR_DEFAULT_H 720

/* Single native renderer instance (one window per process). */
static struct {
    bool          enabled;
    SDL_Window   *window;
    rhi_device   *dev;
    int           win_w, win_h; /* last-known pixel size, for resize detection */

    /* Reference panel: PsyCross's read-back frame, drawn fullscreen. */
    rhi_pipeline *blit_pipe;    /* samples an RGBA8 texture over a fullscreen quad */
    rhi_buffer   *fs_vb;        /* the fullscreen quad (V flipped for GL bottom-up) */
    rhi_texture  *ref_tex;      /* RGBA8, (re)created to the present-FBO size */
    int           ref_w, ref_h;
    unsigned char *readback;    /* CPU readback scratch, ref_w*ref_h*4 */
    size_t        readback_cap;

    /* Aspect-fit quad geometry: the window/ref sizes the current fs_vb was
     * built for, so it's only rebuilt when the fit actually changes. */
    int           quad_win_w, quad_win_h, quad_ref_w, quad_ref_h;
} s_nr;

/* Fill a quad scaled to (sx,sy) in clip space, UVs flipped vertically:
 * PsyCross's readback is bottom-up (row 0 = image bottom), so the top screen
 * vertices sample V=1 and the bottom vertices V=0, displaying it upright.
 * (Matches the smoke test's +Y-up offscreen convention, inverted in V.)
 * sx<1 pillarboxes; sy<1 letterboxes; the offscreen clear fills the bars.
 * rgba is unused by blit.frag. */
static void build_fs_quad(rhi_vertex out[6], float sx, float sy)
{
    const rhi_vertex tl = { -sx,  sy, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0 };
    const rhi_vertex tr = {  sx,  sy, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu, 0 };
    const rhi_vertex br = {  sx, -sy, 0.0f, 1.0f, 0.0f, 0xFFFFFFFFu, 0 };
    const rhi_vertex bl = { -sx, -sy, 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu, 0 };
    out[0] = tl; out[1] = tr; out[2] = br;
    out[3] = tl; out[4] = br; out[5] = bl;
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

    /* Pipeline + geometry for the reference blit. The reference texture is
     * created lazily on the first readback (its size = PsyCross's present
     * FBO, unknown until the game has rendered a scene). */
    rhi_pipeline_desc blit_desc = {
        .vert_spv = "psx_sample.vert.spv", /* passthrough pos + UV */
        .frag_spv = "blit.frag.spv",       /* sampler2D at set 0 */
        .blend = RHI_BLEND_NONE,
        .depth_test = false,
        .uses_vram = true,                 /* set-0 combined-image-sampler layout */
        .push_const_size = 0,
    };
    rhi_pipeline *blit_pipe = rhi_create_pipeline(dev, &blit_desc);
    rhi_buffer   *fs_vb     = rhi_create_vertex_buffer(dev, sizeof(rhi_vertex) * 6);
    if (!blit_pipe || !fs_vb) {
        OPL_ERROR_F("native", "blit pipeline/buffer setup failed; no native A/B");
        if (fs_vb) rhi_destroy_buffer(dev, fs_vb);
        if (blit_pipe) rhi_destroy_pipeline(dev, blit_pipe);
        rhi_destroy(dev);
        SDL_DestroyWindow(win);
        return 1;
    }
    /* fs_vb is filled per the aspect fit on the first frame (update_aspect_quad). */

    s_nr.enabled   = true;
    s_nr.window    = win;
    s_nr.dev       = dev;
    s_nr.win_w     = w;
    s_nr.win_h     = h;
    s_nr.blit_pipe = blit_pipe;
    s_nr.fs_vb     = fs_vb;
    OPL_INFO_F("native", "Vulkan A/B window up (%dx%d, device='%s'); PsyCross hidden",
               w, h, rhi_device_name(dev));
    return 0;
}

bool openpete_native_render_enabled(void)
{
    return s_nr.enabled;
}

/* Pull PsyCross's finished frame from its present FBO into s_nr.ref_tex.
 * Returns true if a reference frame is ready to draw this present. */
static bool sync_reference_texture(void)
{
    int pw = 0, ph = 0;
    if (!GR_GetPresentSceneSize(&pw, &ph) || pw <= 0 || ph <= 0)
        return false; /* PsyCross hasn't produced a scene yet */

    if (pw != s_nr.ref_w || ph != s_nr.ref_h) {
        /* (Re)create the reference texture to match the present FBO size.
         * Rare (effectively once) — wait the GPU idle before freeing the old
         * one so no in-flight frame still references it. */
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

/* Rebuild the display quad to fit the reference's aspect inside the window
 * (letterbox/pillarbox), so resizing never stretches the image. Cheap and
 * only runs when the window or reference size actually changes. */
static void update_aspect_quad(void)
{
    if (s_nr.ref_w <= 0 || s_nr.ref_h <= 0 || s_nr.win_w <= 0 || s_nr.win_h <= 0)
        return;
    if (s_nr.win_w == s_nr.quad_win_w && s_nr.win_h == s_nr.quad_win_h &&
        s_nr.ref_w == s_nr.quad_ref_w && s_nr.ref_h == s_nr.quad_ref_h)
        return; /* fit unchanged */

    float content = (float)s_nr.ref_w / (float)s_nr.ref_h;
    float window  = (float)s_nr.win_w / (float)s_nr.win_h;
    float sx = 1.0f, sy = 1.0f;
    if (window > content) sx = content / window; /* window wider  → pillarbox */
    else                  sy = window / content; /* window taller → letterbox */

    rhi_vertex q[6];
    build_fs_quad(q, sx, sy);
    /* The vertex buffer may still be read by an in-flight frame; geometry
     * changes are rare (startup / resize), so fully sync before rewriting. */
    rhi_wait_idle(s_nr.dev);
    rhi_update_buffer(s_nr.fs_vb, q, sizeof(q));
    s_nr.quad_win_w = s_nr.win_w; s_nr.quad_win_h = s_nr.win_h;
    s_nr.quad_ref_w = s_nr.ref_w; s_nr.quad_ref_h = s_nr.ref_h;
}

void openpete_native_render_frame(void)
{
    if (!s_nr.enabled)
        return;

    /* Detect window-size changes WITHOUT polling SDL events (the host loop
     * owns the event queue, and stealing events would starve PsyCross's
     * input). A per-frame size query is cheap. */
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(s_nr.window, &pw, &ph) && pw > 0 && ph > 0 &&
        (pw != s_nr.win_w || ph != s_nr.win_h)) {
        s_nr.win_w = pw;
        s_nr.win_h = ph;
        rhi_notify_resize(s_nr.dev);
    }

    /* Read PsyCross's frame back BEFORE opening our frame: the readback +
     * texture upload do their own one-shot GPU submits, kept clear of our
     * frame's render pass. */
    bool have_ref = sync_reference_texture();
    update_aspect_quad();

    if (!rhi_begin_frame(s_nr.dev))
        return; /* swapchain recreating or window minimized — skip this present */

    rhi_begin_offscreen(s_nr.dev, 0.0f, 0.0f, 0.0f, 1.0f);
    if (have_ref) {
        rhi_bind_pipeline(s_nr.dev, s_nr.blit_pipe);
        rhi_bind_vram(s_nr.dev, s_nr.ref_tex); /* bind RGBA8 ref to set 0 */
        rhi_bind_vertex_buffer(s_nr.dev, s_nr.fs_vb);
        rhi_draw(s_nr.dev, 6, 0);
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
    if (s_nr.fs_vb)     rhi_destroy_buffer(s_nr.dev, s_nr.fs_vb);
    if (s_nr.blit_pipe) rhi_destroy_pipeline(s_nr.dev, s_nr.blit_pipe);
    rhi_destroy(s_nr.dev);
    SDL_DestroyWindow(s_nr.window);
    free(s_nr.readback);

    s_nr.dev       = NULL;
    s_nr.window    = NULL;
    s_nr.ref_tex   = NULL;
    s_nr.fs_vb     = NULL;
    s_nr.blit_pipe = NULL;
    s_nr.readback  = NULL;
    s_nr.readback_cap = 0;
    s_nr.ref_w = s_nr.ref_h = 0;
    s_nr.enabled = false;
    OPL_INFO_F("native", "Vulkan A/B window shut down");
}
