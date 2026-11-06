/* core/renderer/launcher_shape_pass.c - see launcher_shape_pass.h. */
#include "launcher_shape_pass.h"

#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_time.h"
#include "rhi.h"

#include <stddef.h>
#include <string.h>

/* The most shapes one present draws: the game page's chips, bar and
 * panels with room to spare. */
#define LS_MAX 64

/* The fragment stage's push block (launcher_shape.frag Shape), std430. */
typedef struct {
    float rect[4];
    float col[4];
    float track[4];
    float fill[4];
    float border[4];
    float r[2];
    float aa;
    float blur;
    float fill_x;
    float bw;
    float off_y;
    int   kind;     /* 0 fill, 1 bar, 2 shadow */
} ls_push;
_Static_assert(sizeof(ls_push) == 112, "ls_push must match launcher_shape.frag's push block");

/* The pass's state on the device it was first drawn on. */
struct ls_state {
    rhi_device   *dev;
    rhi_pipeline *pipe;
    rhi_buffer   *vb;
    int           failed;      /* the pipeline or the buffer could not be created */
    int           n;           /* shapes added for the present being drawn */
    unsigned      dropped;     /* shapes past LS_MAX, over the pass's life */
    unsigned      drawn;       /* presents that drew a shape */
    openpete_launcher_shape shapes[LS_MAX];
    rhi_vertex    verts[LS_MAX * 2 * 6];
};
static struct ls_state s_ls;

/** @brief Fill the shape pipeline's description: straight-alpha blended, no depth, the push block in the fragment stage.
 *
 * @param d Receives it. */
static void ls_desc(rhi_pipeline_desc *d) {
    *d = (rhi_pipeline_desc){
        .vert_spv = "launcher_shape.vert.spv",
        .frag_spv = "launcher_shape.frag.spv",
        .blend = RHI_BLEND_ALPHA, .depth_test = false,
        .uses_vram = false, .push_const_size = sizeof(ls_push),
    };
}

void nr_launcher_shapes_describe(rhi_device *dev) {
    rhi_pipeline_desc d;
    void *marker = NULL;
    ls_desc(&d);
    (void)rhi_create_pipeline_slot(dev, &d, NULL, 0, NULL, 0, &marker, NULL);
}

int openpete_native_launcher_shape_add(const openpete_launcher_shape *s) {
    if (!s) return 0;
    if (s_ls.n >= LS_MAX) {
        if (s_ls.dropped++ == 0) OPL_WARN_F("native", "launcher shape pass: more than %d shapes in a present", LS_MAX);
        return 0;
    }
    s_ls.shapes[s_ls.n++] = *s;
    return 1;
}

void nr_launcher_shapes_end(void) { s_ls.n = 0; }

/** @brief Create the pipeline and the vertex buffer on a device unless they exist, logging the creation's time.
 *
 * @param dev The device.
 * @return 1 when both exist on @p dev, else 0 (and the pass stays off on a failure). */
static int ls_ensure(rhi_device *dev) {
    if (s_ls.failed || (s_ls.dev && s_ls.dev != dev)) return 0;
    if (s_ls.pipe && s_ls.vb) return 1;
    s_ls.dev = dev;
    const op_time_t t0 = op_now();   /* clock-guard: wall: creation time for the log */
    rhi_pipeline_desc d;
    ls_desc(&d);
    const rhi_pipe_phase ph = rhi_pipe_phase_get();
    rhi_pipe_phase_set(RHI_PIPE_PHASE_LAUNCHER);   /* the launcher's pipeline, not the game's bring-up */
    if (!s_ls.pipe) s_ls.pipe = rhi_create_pipeline(dev, &d);
    rhi_pipe_phase_set(ph);
    if (!s_ls.vb) s_ls.vb = rhi_create_vertex_buffer_dynamic(dev, sizeof s_ls.verts);
    if (!s_ls.pipe || !s_ls.vb) {
        OPL_WARN_F("native", "launcher shape pass: %s could not be created; panels, chips and the bar do not draw",
                   !s_ls.pipe ? "the pipeline" : "the vertex buffer");
        s_ls.failed = 1;
        return 0;
    }
    OPL_INFO_F("native", "launcher shape pass: pipeline created in %.1f ms",
               (double)(op_now() - t0) / 1e6);   /* clock-guard: wall: creation time for the log */
    return 1;
}

/** @brief Write the quad of a rectangle grown by a margin as two clip-space triangles.
 *
 * @param v Receives 6 vertices.
 * @param r The rectangle, framebuffer pixels.
 * @param m The margin on every side.
 * @param w The framebuffer width.
 * @param h The framebuffer height. */
static void ls_quad(rhi_vertex *v, const float r[4], float m, float w, float h) {
    const float x0 = (r[0] - m) * 2.0f / w - 1.0f, x1 = (r[2] + m) * 2.0f / w - 1.0f;
    /* Clip y is up: the offscreen viewport is Y-flipped. */
    const float y0 = 1.0f - (r[1] - m) * 2.0f / h, y1 = 1.0f - (r[3] + m) * 2.0f / h;
    const uint32_t white = 0xFFFFFFFFu;
    const rhi_vertex tl = { .x = x0, .y = y0, .z = 0.0f, .u = 0.0f, .v = 0.0f, .rgba = white, .w = 1.0f };
    const rhi_vertex tr = { .x = x1, .y = y0, .z = 0.0f, .u = 0.0f, .v = 0.0f, .rgba = white, .w = 1.0f };
    const rhi_vertex br = { .x = x1, .y = y1, .z = 0.0f, .u = 0.0f, .v = 0.0f, .rgba = white, .w = 1.0f };
    const rhi_vertex bl = { .x = x0, .y = y1, .z = 0.0f, .u = 0.0f, .v = 0.0f, .rgba = white, .w = 1.0f };
    v[0] = tl; v[1] = tr; v[2] = br; v[3] = tl; v[4] = br; v[5] = bl;
}

/** @brief Fill a shape's push block for its body or its shadow.
 *
 * @param p Receives it.
 * @param s The shape.
 * @param shadow 1 for the shadow, 0 for the body. */
static void ls_push_fill(ls_push *p, const openpete_launcher_shape *s, int shadow) {
    memset(p, 0, sizeof *p);
    memcpy(p->rect, s->rect, sizeof p->rect);
    p->r[0] = s->rx > 0.01f ? s->rx : 0.01f;
    p->r[1] = s->ry > 0.01f ? s->ry : 0.01f;
    p->aa = s->aa > 0.0f ? s->aa : 1.0f;
    if (shadow) {
        p->kind = 2;
        p->col[3] = s->shadow_a;
        p->off_y = s->shadow_dy;
        p->blur = s->shadow_blur > 0.0f ? s->shadow_blur : 1.0f;
        return;
    }
    p->kind = s->kind == OPENPETE_LAUNCHER_SHAPE_BAR ? 1 : 0;
    memcpy(p->col, s->col, sizeof p->col);
    memcpy(p->track, s->track, sizeof p->track);
    memcpy(p->fill, s->fill, sizeof p->fill);
    memcpy(p->border, s->border, sizeof p->border);
    p->fill_x = s->fill_x;
    p->bw = s->border_w;
}

void nr_launcher_shapes_draw(rhi_device *dev) {
    if (!dev || s_ls.n <= 0 || !ls_ensure(dev)) return;
    int w = 0, h = 0;
    rhi_offscreen_extent(dev, &w, &h);
    if (w <= 0 || h <= 0) return;
    /* Draw order: every shadow (a draw of its own), then every body. */
    int order[LS_MAX * 2], shadow[LS_MAX * 2], nq = 0;
    for (int pass = 1; pass >= 0; pass--)
        for (int i = 0; i < s_ls.n; i++) {
            const openpete_launcher_shape *s = &s_ls.shapes[i];
            if (pass == 1 && s->shadow_a <= 0.0f) continue;
            const float m = pass ? s->shadow_blur + s->shadow_dy + s->aa + 1.0f : s->aa + 1.0f;
            ls_quad(&s_ls.verts[nq * 6], s->rect, m, (float)w, (float)h);
            order[nq] = i;
            shadow[nq++] = pass;
        }
    rhi_update_buffer(s_ls.vb, s_ls.verts, (size_t)nq * 6 * sizeof(rhi_vertex));
    rhi_reset_scissor(dev);
    rhi_bind_pipeline(dev, s_ls.pipe);
    rhi_bind_vertex_buffer(dev, s_ls.vb);
    for (int q = 0; q < nq; q++) {
        ls_push p;
        ls_push_fill(&p, &s_ls.shapes[order[q]], shadow[q]);
        rhi_push_constants(dev, &p, sizeof p);
        rhi_draw(dev, 6, (uint32_t)(q * 6));
    }
    if (s_ls.drawn++ == 0)
        OPL_INFO_F("native", "launcher shape pass: first draw (%d shapes, %d draws, %dx%d)", s_ls.n, nq, w, h);
}

void nr_launcher_shapes_release(rhi_device *dev) {
    if (!dev || dev != s_ls.dev) return;
    rhi_wait_idle(dev);
    if (s_ls.pipe) rhi_destroy_pipeline(dev, s_ls.pipe);
    if (s_ls.vb) rhi_destroy_buffer(dev, s_ls.vb);
    OPL_INFO_F("native", "launcher shape pass: released after %u presents with shapes (%u shapes dropped)",
               s_ls.drawn, s_ls.dropped);
    memset(&s_ls, 0, offsetof(struct ls_state, shapes));
}
