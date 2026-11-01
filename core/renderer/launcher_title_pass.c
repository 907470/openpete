/* core/renderer/launcher_title_pass.c - see launcher_title_pass.h. */
#include "launcher_title_pass.h"

#include "openpete_log.h"
#include "openpete_time.h"
#include "rhi.h"

#include <stddef.h>
#include <string.h>

/* Samples per output pixel along each axis: the target is this many times
 * the window per axis and the composite averages each block. */
#define LT_SS 2
/* The largest title the pass takes, in vertices. */
#define LT_MAX_VERTS 8192

/* The pass's state on the device it was first set on. */
struct lt_state {
    rhi_device   *dev;
    rhi_pipeline *glyph_pipe;
    rhi_pipeline *resolve_pipe;
    rhi_buffer   *vb;
    rhi_buffer   *quad_vb;
    rhi_texture  *rt;
    int           rt_w, rt_h;
    int           failed;     /* a pipeline, buffer or target could not be created */
    int           n;          /* vertices set for the next present */
    int           armed;      /* the prepass rendered the target this frame */
    unsigned      drawn;      /* presents the title was composited on */
    rhi_vertex    verts[LT_MAX_VERTS];
};
static struct lt_state s_lt;

/** @brief Fill the glyph pipeline's description: vertex-coloured, depth-tested, opaque.
 *
 * @param d Receives it. */
static void lt_glyph_desc(rhi_pipeline_desc *d) {
    *d = (rhi_pipeline_desc){
        .vert_spv = "quad.vert.spv",
        .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .uses_vram = false, .push_const_size = 0,
    };
}

/** @brief Fill the composite pipeline's description: the overlay box resolve, straight-alpha blended.
 *
 * @param d Receives it. */
static void lt_resolve_desc(rhi_pipeline_desc *d) {
    *d = (rhi_pipeline_desc){
        .vert_spv = "psx_sample.vert.spv",
        .frag_spv = "overlay_resolve.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = true, .push_const_size = 2 * sizeof(int),   /* ssaa, outline */
    };
}

void nr_launcher_title_describe(rhi_device *dev) {
    rhi_pipeline_desc d;
    void *marker = NULL;
    lt_glyph_desc(&d);
    (void)rhi_create_pipeline_slot(dev, &d, NULL, 0, NULL, 0, &marker, NULL);
    lt_resolve_desc(&d);
    (void)rhi_create_pipeline_slot(dev, &d, NULL, 0, NULL, 0, &marker, NULL);
}

/** @brief Create the pass's pipelines and buffers on a device unless they exist, logging the creation's time.
 *
 * @param dev The device.
 * @return 1 when every one exists, else 0 (and the pass stays off). */
static int lt_ensure_objects(rhi_device *dev) {
    if (s_lt.glyph_pipe && s_lt.resolve_pipe && s_lt.vb && s_lt.quad_vb) return 1;
    const op_time_t t0 = op_now();   /* clock-guard: wall: creation time for the log */
    rhi_pipeline_desc d;
    lt_glyph_desc(&d);
    if (!s_lt.glyph_pipe) s_lt.glyph_pipe = rhi_create_pipeline(dev, &d);
    lt_resolve_desc(&d);
    if (!s_lt.resolve_pipe) s_lt.resolve_pipe = rhi_create_pipeline(dev, &d);
    if (!s_lt.vb) s_lt.vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * LT_MAX_VERTS);
    if (!s_lt.quad_vb) s_lt.quad_vb = rhi_create_vertex_buffer_dynamic(dev, sizeof(rhi_vertex) * 6);
    if (!s_lt.glyph_pipe || !s_lt.resolve_pipe || !s_lt.vb || !s_lt.quad_vb) {
        OPL_WARN_F("native", "launcher title pass: %s could not be created; the pixel title stays",
                   !s_lt.glyph_pipe ? "the glyph pipeline" : !s_lt.resolve_pipe ? "the composite pipeline"
                                                                             : "a vertex buffer");
        s_lt.failed = 1;
        return 0;
    }
    OPL_INFO_F("native", "launcher title pass: pipelines created in %.1f ms",
               (double)(op_now() - t0) / 1e6);   /* clock-guard: wall: creation time for the log */
    return 1;
}

/** @brief Size the target at LT_SS times a window, recreating it after the device idles on a size change.
 *
 * @param dev The device.
 * @param w The window width in pixels.
 * @param h The window height in pixels.
 * @return 1 when the target exists at that size, else 0. */
static int lt_ensure_target(rhi_device *dev, int w, int h) {
    int want_w = w * LT_SS, want_h = h * LT_SS;
    const int maxdim = (int)rhi_max_image_dimension(dev);
    if (want_w > maxdim) want_w = maxdim;
    if (want_h > maxdim) want_h = maxdim;
    if (s_lt.rt && s_lt.rt_w == want_w && s_lt.rt_h == want_h) return 1;
    rhi_wait_idle(dev);
    if (s_lt.rt) rhi_destroy_texture(dev, s_lt.rt);
    s_lt.rt = rhi_create_render_target(dev, (uint32_t)want_w, (uint32_t)want_h);
    s_lt.rt_w = want_w;
    s_lt.rt_h = want_h;
    if (!s_lt.rt) {
        OPL_WARN_F("native", "launcher title pass: the %dx%d target could not be created", want_w, want_h);
        return 0;
    }
    OPL_INFO_F("native", "launcher title pass: target %dx%d (%dx per axis)", want_w, want_h, LT_SS);
    return 1;
}

int nr_launcher_title_set(rhi_device *dev, const openpete_hud_vert *v, int n, int w, int h) {
    s_lt.n = 0;
    if (!dev || !v || n < 3 || w <= 0 || h <= 0 || s_lt.failed) return 0;
    if (s_lt.dev && s_lt.dev != dev) return 0;
    s_lt.dev = dev;
    if (!lt_ensure_objects(dev) || !lt_ensure_target(dev, w, h)) return 0;
    if (n > LT_MAX_VERTS) n = LT_MAX_VERTS;
    n -= n % 3;
    const float sx = 2.0f / (float)w, sy = 2.0f / (float)h;
    for (int i = 0; i < n; i++) {
        /* Window pixels to clip space, +y up: the target's viewport is Y-flipped. */
        s_lt.verts[i] = (rhi_vertex){ v[i].x * sx - 1.0f, 1.0f - v[i].y * sy, v[i].z, 0.0f, 0.0f,
                                      v[i].rgba, 0.0f, 0.0f, 0.0f, v[i].z };
    }
    s_lt.n = n;
    return 1;
}

void nr_launcher_title_prepass(rhi_device *dev) {
    s_lt.armed = 0;
    if (!dev || dev != s_lt.dev || s_lt.n < 3 || !s_lt.rt) return;
    rhi_update_buffer(s_lt.vb, s_lt.verts, (size_t)s_lt.n * sizeof(rhi_vertex));
    rhi_begin_render_target(dev, s_lt.rt, 0.0f, 0.0f, 0.0f, 0.0f);   /* transparent */
    rhi_bind_pipeline(dev, s_lt.glyph_pipe);
    rhi_bind_vertex_buffer(dev, s_lt.vb);
    rhi_draw(dev, (uint32_t)s_lt.n, 0);
    rhi_end_render_target(dev, s_lt.rt);
    s_lt.armed = 1;
}

void nr_launcher_title_composite(rhi_device *dev) {
    if (!s_lt.armed || dev != s_lt.dev) return;
    s_lt.armed = 0;
    /* The whole window, UV 0..1 over the whole target, v 0 at the top. */
    const uint32_t white = 0xFFFFFFFFu;
    const rhi_vertex tl = { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f, white, 0.0f, 0.0f, 0.0f, 1.0f };
    const rhi_vertex tr = {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f, white, 0.0f, 0.0f, 0.0f, 1.0f };
    const rhi_vertex br = {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f, white, 0.0f, 0.0f, 0.0f, 1.0f };
    const rhi_vertex bl = { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, white, 0.0f, 0.0f, 0.0f, 1.0f };
    const rhi_vertex q[6] = { tl, tr, br, tl, br, bl };
    rhi_update_buffer(s_lt.quad_vb, q, sizeof q);
    rhi_reset_scissor(dev);
    rhi_bind_pipeline(dev, s_lt.resolve_pipe);
    rhi_bind_vram(dev, s_lt.rt);
    const int pc[2] = { LT_SS, 0 };   /* box over LT_SS x LT_SS, no outline */
    rhi_push_constants(dev, pc, sizeof pc);
    rhi_bind_vertex_buffer(dev, s_lt.quad_vb);
    rhi_draw(dev, 6, 0);
    if (s_lt.drawn++ == 0)
        OPL_INFO_F("native", "launcher title pass: first composite (%d vertices, target %dx%d)", s_lt.n, s_lt.rt_w,
                   s_lt.rt_h);
}

void nr_launcher_title_release(rhi_device *dev) {
    if (!dev || dev != s_lt.dev) return;
    rhi_wait_idle(dev);
    if (s_lt.glyph_pipe) rhi_destroy_pipeline(dev, s_lt.glyph_pipe);
    if (s_lt.resolve_pipe) rhi_destroy_pipeline(dev, s_lt.resolve_pipe);
    if (s_lt.vb) rhi_destroy_buffer(dev, s_lt.vb);
    if (s_lt.quad_vb) rhi_destroy_buffer(dev, s_lt.quad_vb);
    if (s_lt.rt) rhi_destroy_texture(dev, s_lt.rt);
    OPL_INFO_F("native", "launcher title pass: released after %u composited presents", s_lt.drawn);
    memset(&s_lt, 0, offsetof(struct lt_state, verts));
}
