/* core/renderer/launcher_sky_pass.c - the launcher's sky pass
 * (openpete_launcher_sky.h): a level's dome mesh drawn behind the launcher's
 * screens, its clear colour filling the pixels the mesh leaves. */
#include "openpete_launcher_sky.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "openpete_log.h"
#include "rhi.h"

/* The push-constant block of launcher_sky.glsl: six vec4s. */
typedef struct {
    float cam[4];     /* yaw, pitch (radians), tan half-FOV x, tan half-FOV y */
    float bg[4];      /* clear colour, 1 for the fill draw */
    float screen[4];  /* width, height, aspect, clock */
    float wipe[4];    /* centre u, v, far radius, soft edge */
    float layer[4];   /* progress, opacity, reveal flag, grain seed */
    float look[4];    /* vignette, grain, 0, 0 */
} nr_sky_push;

_Static_assert(sizeof(nr_sky_push) == 96, "launcher_sky.glsl's block is six vec4s");

/* One mesh's GPU copy: the triangles expanded to rhi_vertex records. */
typedef struct {
    uint64_t    serial;
    rhi_buffer *vb;
    uint32_t    verts;
    uint64_t    last_draw;   /* s_sky.draws at its last use */
} nr_sky_copy;

#define NR_SKY_COPIES 8

static struct {
    openpete_sky_view view;
    int               have_view;
    rhi_pipeline     *pipe;
    int               pipe_failed;
    nr_sky_copy       copy[NR_SKY_COPIES];
    uint64_t          draws;
    /* Engagement, logged at the release: draws that drew a layer, layers
     * drawn, of them revealed and crossfaded, meshes uploaded, triangles
     * drawn. */
    uint64_t          drew, layers, revealed, faded, uploads, tris;
} s_sky;

/** @brief Fill the sky pipeline's description: launcher_sky.vert/.frag over rhi_vertex, alpha blend, no depth, one fragment per pixel (RHI_STENCIL_ONCE), push constants in both stages.
 *
 * @param d Receives the description. */
static void nr_sky_desc(rhi_pipeline_desc *d)
{
    memset(d, 0, sizeof *d);
    d->vert_spv = "launcher_sky.vert.spv";
    d->frag_spv = "launcher_sky.frag.spv";
    d->blend = RHI_BLEND_ALPHA;
    d->stencil = RHI_STENCIL_ONCE;
    d->push_const_size = (uint32_t)sizeof(nr_sky_push);
    d->push_vertex = true;
}

void launcher_sky_describe(struct rhi_device *dev)
{
    rhi_pipeline_desc d;
    nr_sky_desc(&d);
    void *marker = NULL;
    (void)rhi_create_pipeline_slot(dev, &d, NULL, 0, NULL, 0, &marker, NULL);
}

void openpete_native_launcher_sky_next(const openpete_sky_view *v)
{
    s_sky.have_view = v != NULL;
    if (v) s_sky.view = *v;
}

/** @brief Read a little-endian int16 direction component as a float in -1..1.
 *
 * @param p Its two bytes.
 * @return The component. */
static float nr_sky_unit(const uint8_t *p)
{
    const int16_t v = (int16_t)(uint16_t)(p[0] | (p[1] << 8));
    return (float)v / 32767.0f;
}

/** @brief Upload one mesh: each triangle's three vertices as rhi_vertex records (direction in x, y, z; colour in rgba) in a new buffer.
 *
 * @param dev The device.
 * @param m The mesh.
 * @param out Receives the buffer and its vertex count.
 * @return 1 on success, 0 when the mesh is empty or an index is out of range or the buffer cannot be created. */
static int nr_sky_upload(rhi_device *dev, const openpete_sky_mesh *m, nr_sky_copy *out)
{
    const size_t n = (size_t)m->ntri * 3u;
    if (n == 0 || !m->verts || !m->tris) return 0;
    rhi_vertex *v = calloc(n, sizeof *v);
    if (!v) return 0;
    for (size_t k = 0; k < n; k++) {
        const uint32_t i = m->tris[k];
        if (i >= m->nv) { free(v); return 0; }
        const uint8_t *r = m->verts + (size_t)i * 9u;
        v[k].x = nr_sky_unit(r);
        v[k].y = nr_sky_unit(r + 2);
        v[k].z = nr_sky_unit(r + 4);
        v[k].rgba = (uint32_t)r[6] | ((uint32_t)r[7] << 8) | ((uint32_t)r[8] << 16) | 0xFF000000u;
        v[k].w = 1.0f;
    }
    rhi_buffer *b = rhi_create_vertex_buffer_device(dev, n * sizeof *v);
    if (b) rhi_update_buffer(b, v, n * sizeof *v);
    free(v);
    if (!b) return 0;
    out->serial = m->serial;
    out->vb = b;
    out->verts = (uint32_t)n;
    return 1;
}

/** @brief Return a mesh's GPU copy, uploading it into a free slot on its first use; NULL when no slot is free or the upload fails.
 *
 * @param dev The device.
 * @param m The mesh.
 * @return The copy, its last use set to this draw. */
static nr_sky_copy *nr_sky_copy_of(rhi_device *dev, const openpete_sky_mesh *m)
{
    nr_sky_copy *free_slot = NULL;
    for (int i = 0; i < NR_SKY_COPIES; i++) {
        nr_sky_copy *c = &s_sky.copy[i];
        if (c->vb && c->serial == m->serial) { c->last_draw = s_sky.draws; return c; }
        if (!c->vb && !free_slot) free_slot = c;
    }
    const Uint64 t0 = SDL_GetTicksNS();  /* clock-guard: wall: upload time for the log */
    if (!free_slot || !nr_sky_upload(dev, m, free_slot)) {
        OPL_WARN_F("launcher", "sky: the dome mesh %llu could not be uploaded (%u triangles)",
                   (unsigned long long)m->serial, (unsigned)m->ntri);
        return NULL;
    }
    free_slot->last_draw = s_sky.draws;
    if (!s_sky.uploads)
        OPL_INFO_F("launcher", "sky: first dome uploaded in %.1f ms (%u triangles)",
                   (double)(SDL_GetTicksNS() - t0) / 1e6, (unsigned)m->ntri);  /* clock-guard: wall */
    s_sky.uploads++;
    return free_slot;
}

/** @brief Free the copies no draw used for LAUNCHER_SKY_RETIRE_DRAWS draws.
 *
 * Every launcher present begins a frame, and a frame begin waits for the
 * frame RHI_FRAMES_IN_FLIGHT before it, so a copy unused for more draws
 * than that is read by no submitted frame.
 * @param dev The device. */
static void nr_sky_retire(rhi_device *dev)
{
    _Static_assert(LAUNCHER_SKY_RETIRE_DRAWS > RHI_FRAMES_IN_FLIGHT, "a retired copy is idle");
    for (int i = 0; i < NR_SKY_COPIES; i++) {
        nr_sky_copy *c = &s_sky.copy[i];
        if (!c->vb || s_sky.draws - c->last_draw < LAUNCHER_SKY_RETIRE_DRAWS) continue;
        rhi_destroy_buffer(dev, c->vb);
        memset(c, 0, sizeof *c);
    }
}

/** @brief Fill the push block shared by a layer's two draws.
 *
 * @param v The view.
 * @param l The layer.
 * @param w Target width in pixels.
 * @param h Target height in pixels.
 * @param pc Receives the block, the fill flag clear. */
static void nr_sky_push_fill(const openpete_sky_view *v, const openpete_sky_layer *l, int w, int h,
                             nr_sky_push *pc)
{
    const float deg = 3.14159265f / 180.0f;
    const float aspect = (float)w / (float)h;
    const float tx = tanf(v->hfov_deg * 0.5f * deg);
    memset(pc, 0, sizeof *pc);
    pc->cam[0] = v->yaw_deg * deg;
    pc->cam[1] = v->pitch_deg * deg;
    pc->cam[2] = tx;
    pc->cam[3] = tx / aspect;
    for (int i = 0; i < 3; i++) pc->bg[i] = (float)l->mesh->bg[i] / 255.0f;
    pc->screen[0] = (float)w;
    pc->screen[1] = (float)h;
    pc->screen[2] = aspect;
    pc->screen[3] = v->clock_s;
    pc->wipe[0] = v->reveal_u;
    pc->wipe[1] = v->reveal_v;
    float far = 0.0f;   /* the farthest corner from the centre, in target heights */
    for (int k = 0; k < 4; k++) {
        const float dx = ((float)(k & 1) - v->reveal_u) * aspect, dy = (float)(k >> 1) - v->reveal_v;
        far = fmaxf(far, sqrtf(dx * dx + dy * dy));
    }
    pc->wipe[2] = far;
    pc->wipe[3] = v->reveal_soft;
    pc->layer[0] = v->reveal;
    pc->layer[1] = l->alpha;
    pc->layer[2] = l->reveal ? 1.0f : 0.0f;
    pc->layer[3] = v->seed;
    pc->look[0] = v->vignette;
    pc->look[1] = v->grain;
}

/** @brief Return the sky pipeline, creating it on the first call; NULL once its creation has failed.
 *
 * @param dev The device.
 * @return The pipeline or NULL. */
static rhi_pipeline *nr_sky_pipe(rhi_device *dev)
{
    if (s_sky.pipe || s_sky.pipe_failed) return s_sky.pipe;
    rhi_pipeline_desc d;
    nr_sky_desc(&d);
    const Uint64 t0 = SDL_GetTicksNS();  /* clock-guard: wall: creation time for the log */
    (void)rhi_create_pipeline_slot(dev, &d, NULL, 0, NULL, 0, (void **)&s_sky.pipe, NULL);
    if (s_sky.pipe)
        OPL_INFO_F("launcher", "sky: pipeline created in %.1f ms",
                   (double)(SDL_GetTicksNS() - t0) / 1e6);  /* clock-guard: wall */
    if (!s_sky.pipe) {
        s_sky.pipe_failed = 1;
        OPL_WARN_F("launcher", "%s", "sky: the pipeline could not be created; the background stays flat");
    }
    return s_sky.pipe;
}

int launcher_sky_draw(struct rhi_device *dev)
{
    if (!s_sky.have_view) return 0;
    s_sky.have_view = 0;
    s_sky.draws++;
    const openpete_sky_view *v = &s_sky.view;
    int drawn = 0;
    int w = 0, h = 0;
    rhi_offscreen_extent(dev, &w, &h);
    rhi_pipeline *pipe = (w > 0 && h > 0) ? nr_sky_pipe(dev) : NULL;
    for (int i = 0; pipe && i < 2; i++) {
        const openpete_sky_layer *l = &v->layer[i];
        if (!l->mesh || (!l->reveal && l->alpha <= 0.0f)) continue;
        nr_sky_copy *c = nr_sky_copy_of(dev, l->mesh);
        if (!c) continue;
        nr_sky_push pc;
        nr_sky_push_fill(v, l, w, h, &pc);
        rhi_clear_stencil(dev);
        rhi_bind_pipeline(dev, pipe);
        rhi_bind_vertex_buffer(dev, c->vb);
        rhi_push_constants(dev, &pc, (uint32_t)sizeof pc);
        rhi_draw(dev, c->verts, 0);
        pc.bg[3] = 1.0f;
        rhi_push_constants(dev, &pc, (uint32_t)sizeof pc);
        rhi_draw(dev, 3, 0);
        drawn++;
        s_sky.layers++;
        s_sky.tris += l->mesh->ntri;
        if (l->reveal) s_sky.revealed++;
        else if (l->alpha < 1.0f) s_sky.faded++;
    }
    if (drawn) s_sky.drew++;
    nr_sky_retire(dev);
    return drawn;
}

void launcher_sky_release(struct rhi_device *dev)
{
    int held = s_sky.pipe != NULL;
    for (int i = 0; i < NR_SKY_COPIES; i++) held |= s_sky.copy[i].vb != NULL;
    if (held) rhi_wait_idle(dev);
    if (s_sky.draws)
        OPL_INFO_F("launcher", "sky: %llu of %llu presents handed a view drew the sky: %llu layers (%llu revealed, "
                   "%llu crossfaded), %llu triangles, %llu meshes uploaded",
                   (unsigned long long)s_sky.drew, (unsigned long long)s_sky.draws,
                   (unsigned long long)s_sky.layers, (unsigned long long)s_sky.revealed,
                   (unsigned long long)s_sky.faded, (unsigned long long)s_sky.tris,
                   (unsigned long long)s_sky.uploads);
    for (int i = 0; i < NR_SKY_COPIES; i++)
        if (s_sky.copy[i].vb) rhi_destroy_buffer(dev, s_sky.copy[i].vb);
    if (s_sky.pipe) rhi_destroy_pipeline(dev, s_sky.pipe);
    memset(&s_sky, 0, sizeof s_sky);
}
