/* core/renderer/launcher_title_pass.c - see launcher_title_pass.h. */
#include "launcher_title_pass.h"

#include "openpete_log.h"
#include "openpete_time.h"
#include "rhi.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Samples per output pixel along each axis: the target is this many times
 * the title's rect per axis and the composite averages each block. */
#define LT_SS 2
/* The target's largest side in texels; a rect whose LT_SS-times side
 * exceeds it takes fewer samples per pixel (down to 1). */
#define LT_MAX_SIDE 4096
/* Window pixels added around the vertices' bounds on every side. */
#define LT_PAD 2
/* The target's extent in window pixels is a multiple of this, so a title
 * whose bounds move by a few pixels between presents keeps its target. */
#define LT_ALIGN 64
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
    int           rt_w, rt_h;     /* the target in texels: ss times the rect */
    int           ss;             /* samples per window pixel along each axis */
    int           rx, ry;         /* the rect's top-left corner, window pixels */
    int           rw, rh;         /* the rect's extent, window pixels */
    int           win_w, win_h;   /* the window the rect lies in */
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
    const rhi_pipe_phase ph = rhi_pipe_phase_get();
    rhi_pipe_phase_set(RHI_PIPE_PHASE_LAUNCHER);   /* the launcher's pipelines, not the game's bring-up */
    lt_glyph_desc(&d);
    if (!s_lt.glyph_pipe) s_lt.glyph_pipe = rhi_create_pipeline(dev, &d);
    lt_resolve_desc(&d);
    if (!s_lt.resolve_pipe) s_lt.resolve_pipe = rhi_create_pipeline(dev, &d);
    rhi_pipe_phase_set(ph);
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

/** @brief Round a window-pixel extent up to a multiple of LT_ALIGN.
 *
 * @param v The extent.
 * @return The rounded extent. */
static int lt_align_up(int v) { return (v + LT_ALIGN - 1) / LT_ALIGN * LT_ALIGN; }

/** @brief Size the target for a rect of at least @p need_w x @p need_h window pixels.
 *
 * The target is recreated, after the device idles, when the current one is
 * too small or over twice the need.
 * The rect's extent is the need rounded up to LT_ALIGN; the samples per
 * pixel are LT_SS unless that puts a side past LT_MAX_SIDE or the device's
 * limit, then the most that fits (at least 1, the extent clamped to fit).
 * @param dev The device.
 * @param need_w The width the title's bounds take, window pixels.
 * @param need_h The height the title's bounds take, window pixels.
 * @return 1 when the target exists, else 0. */
static int lt_ensure_target(rhi_device *dev, int need_w, int need_h) {
    int maxdim = (int)rhi_max_image_dimension(dev);
    if (maxdim > LT_MAX_SIDE) maxdim = LT_MAX_SIDE;
    if (need_w > maxdim) need_w = maxdim;
    if (need_h > maxdim) need_h = maxdim;
    if (s_lt.rt && need_w <= s_lt.rw && need_h <= s_lt.rh && 2 * need_w >= s_lt.rw - LT_ALIGN &&
        2 * need_h >= s_lt.rh - LT_ALIGN)
        return 1;
    int rw = lt_align_up(need_w), rh = lt_align_up(need_h);
    if (rw > maxdim) rw = maxdim;
    if (rh > maxdim) rh = maxdim;
    int ss = LT_SS;
    while (ss > 1 && (rw * ss > maxdim || rh * ss > maxdim)) ss--;
    rhi_wait_idle(dev);
    if (s_lt.rt) rhi_destroy_texture(dev, s_lt.rt);
    s_lt.rt = rhi_create_render_target(dev, (uint32_t)(rw * ss), (uint32_t)(rh * ss));
    s_lt.rw = rw;
    s_lt.rh = rh;
    s_lt.ss = ss;
    s_lt.rt_w = rw * ss;
    s_lt.rt_h = rh * ss;
    if (!s_lt.rt) {
        OPL_WARN_F("native", "launcher title pass: the %dx%d target could not be created", s_lt.rt_w, s_lt.rt_h);
        return 0;
    }
    OPL_INFO_F("native", "launcher title pass: target %dx%d (%dx per axis over a %dx%d px rect)", s_lt.rt_w,
               s_lt.rt_h, ss, rw, rh);
    return 1;
}

/** @brief Find the window-pixel bounds of the title's vertices, padded by LT_PAD and clamped to the window.
 *
 * @param v The vertices.
 * @param n Their count, at least 1.
 * @param w The window width in pixels.
 * @param h The window height in pixels.
 * @param b Receives x0, y0, x1, y1 (x1 > x0 and y1 > y0 unless the title lies outside the window). */
static void lt_bounds(const openpete_hud_vert *v, int n, int w, int h, int b[4]) {
    float x0 = v[0].x, y0 = v[0].y, x1 = v[0].x, y1 = v[0].y;
    for (int i = 1; i < n; i++) {
        x0 = v[i].x < x0 ? v[i].x : x0;
        x1 = v[i].x > x1 ? v[i].x : x1;
        y0 = v[i].y < y0 ? v[i].y : y0;
        y1 = v[i].y > y1 ? v[i].y : y1;
    }
    const float lo[2] = { x0, y0 }, hi[2] = { x1, y1 }, lim[2] = { (float)w, (float)h };
    for (int k = 0; k < 2; k++) {
        float a = floorf(lo[k]) - LT_PAD, z = ceilf(hi[k]) + LT_PAD;
        a = a < 0.0f ? 0.0f : a > lim[k] ? lim[k] : a;
        z = z < 0.0f ? 0.0f : z > lim[k] ? lim[k] : z;
        b[k] = (int)a;
        b[k + 2] = (int)z;
    }
}

int nr_launcher_title_set(rhi_device *dev, const openpete_hud_vert *v, int n, int w, int h) {
    s_lt.n = 0;
    if (!dev || !v || n < 3 || w <= 0 || h <= 0 || s_lt.failed) return 0;
    if (s_lt.dev && s_lt.dev != dev) return 0;
    s_lt.dev = dev;
    if (n > LT_MAX_VERTS) n = LT_MAX_VERTS;
    n -= n % 3;
    int b[4];
    lt_bounds(v, n, w, h, b);
    if (b[2] <= b[0] || b[3] <= b[1]) return 1;   /* nothing of the title in the window: drawn as nothing */
    if (!lt_ensure_objects(dev) || !lt_ensure_target(dev, b[2] - b[0], b[3] - b[1])) return 0;
    s_lt.rx = b[0];
    s_lt.ry = b[1];
    s_lt.win_w = w;
    s_lt.win_h = h;
    const float sx = 2.0f / (float)s_lt.rw, sy = 2.0f / (float)s_lt.rh;
    const float ox = (float)s_lt.rx, oy = (float)s_lt.ry;
    for (int i = 0; i < n; i++) {
        /* Window pixels to the rect's clip space, +y up: the target's viewport is Y-flipped. */
        s_lt.verts[i] = (rhi_vertex){ .x = (v[i].x - ox) * sx - 1.0f, .y = 1.0f - (v[i].y - oy) * sy,
                                      .z = v[i].z, .rgba = v[i].rgba, .w = v[i].z };
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
    /* The rect in the window's clip space, UV 0..1 over the whole target, v 0
     * at the top; the part past the window's edge is clipped. */
    const uint32_t white = 0xFFFFFFFFu;
    const float x0 = 2.0f * (float)s_lt.rx / (float)s_lt.win_w - 1.0f;
    const float x1 = 2.0f * (float)(s_lt.rx + s_lt.rw) / (float)s_lt.win_w - 1.0f;
    const float y0 = 1.0f - 2.0f * (float)s_lt.ry / (float)s_lt.win_h;
    const float y1 = 1.0f - 2.0f * (float)(s_lt.ry + s_lt.rh) / (float)s_lt.win_h;
    const rhi_vertex tl = { .x = x0, .y = y0, .z = 0.0f, .u = 0.0f, .v = 0.0f, .rgba = white, .w = 1.0f };
    const rhi_vertex tr = { .x = x1, .y = y0, .z = 0.0f, .u = 1.0f, .v = 0.0f, .rgba = white, .w = 1.0f };
    const rhi_vertex br = { .x = x1, .y = y1, .z = 0.0f, .u = 1.0f, .v = 1.0f, .rgba = white, .w = 1.0f };
    const rhi_vertex bl = { .x = x0, .y = y1, .z = 0.0f, .u = 0.0f, .v = 1.0f, .rgba = white, .w = 1.0f };
    const rhi_vertex q[6] = { tl, tr, br, tl, br, bl };
    rhi_update_buffer(s_lt.quad_vb, q, sizeof q);
    rhi_reset_scissor(dev);
    rhi_bind_pipeline(dev, s_lt.resolve_pipe);
    rhi_bind_vram(dev, s_lt.rt);
    const int pc[2] = { s_lt.ss, 0 };   /* box over ss x ss, no outline */
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
