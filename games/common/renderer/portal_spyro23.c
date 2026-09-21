// games/common/renderer/portal_spyro23.c : the homeworld portal previews of
// the two games whose portal pass and preview leaf are the same routines at
// different addresses. Each game's renderer/portal_game.c supplies the three
// addresses; the contract and the asm evidence are renderer/portal_spyro23.h.
//
// The game's portal pass gates each portal record, projects
// its five pentagon vertices with the sky matrix, writes the clip rect, the
// visible pentagon edges, the depth and the fade terms into the parameter
// block, and calls the preview leaf for the background quad and, when
// the portal is nearer than 0x5000 units, for the baked picture of the
// destination level. Both meshes are pre-projected 2-D triangle lists in
// level RAM: a 0x18-byte header {vertex count, vertex offset, colour count,
// colour offset, face count, face offset}, vertices as (y << 16 | x) << 5
// with the low five bits draw-owned scratch, colours 0x00BBGGRR, faces of two
// words (vertex indices in 12/10/10 bits; word 1 bit 31 flat with the colour
// index in bits 29-20, else three colour byte offsets). The leaf drops a face
// whose three vertices share a rect outcode or lie outside one pentagon edge,
// clips the rest to the edges, fades the colours toward the block's far
// colour by its IR0 and then toward the environment far colour by the sky
// fog word, and links the prims at OT index 3 + depth.
//
// This feeder hooks the leaf: it copies the block and the argument before
// the real leaf runs (the block is rewritten per portal), and after the pass
// re-derives every preview in float: the same outcode and half-plane
// rejects, a Sutherland-Hodgman clip to the pentagon, the two colour fades,
// and a depth at the portal's own distance. The rect footprint goes out as
// the portal's stencil mask and the picture as portal-cyclorama triangles,
// so terrain in front of the portal plane occludes the picture per pixel.
// The picture is authored in the 4:3 frame; under a wider frame it and the
// pentagon shift together by the frame's centre offset, and a full-screen
// clip rect follows the window instead (p3_bounds_of). Sub-tick presents
// replay the captured previews with the rect and edges lerped between the
// previous and the current tick when the portal set matches.
//
// Never writes PSX RAM: the real pass and leaf run unchanged, and the feeder
// only reads afterwards.

#include "feature_registry.h"
#include "openpete.h"          /* openpete_options_t */
#include "psx_runtime_internal.h"
#include "openpete_psx.h"
#include "openpete_native_render.h"
#include "openpete_recomp_module.h"
#include "openpete_fiber.h"           /* openpete_frame */
#include "renderer/render_paths.h"
#include "renderer/native_gte.h"
#include "renderer/portal_spyro23.h"
#include "game_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P3_MAX_PORTALS 16
#define P3_MAX_EDGES   8
#define P3_MAX_VERTS   1024
#define P3_MAX_COLS    256

typedef struct {
    uint32_t mesh;                 /* the preview mesh header vaddr */
    int      arg;                  /* 1 = background record, 0 = the +0x3C picture */
    int      ymin, ymax, xmin, xmax;
    int      depth7;               /* portal depth >> 7, as captured */
    float    depth_w;              /* the same depth in world units, lerpable */
    uint32_t far_rgb;              /* packed 8-bit far colour */
    int      ir0;                  /* portal fade factor */
    int      nedge;
    float    ex[P3_MAX_EDGES][2], ey[P3_MAX_EDGES][2];
    int      slot;                 /* portal index within the pass */
} p3_preview;

typedef struct {
    p3_preview p[P3_MAX_PORTALS * 2];
    int n;
    unsigned frame;
    int valid;
} p3_tick;

static p3_tick s_curr, s_prev;
static int s_in_pass = 0;
static int s_leaf_calls = 0;      /* engagement: leaf hooks seen this tick */
static unsigned s_sky_fed_frame = 0;  /* the last present a preview actually emitted */
static uint32_t s_pass_start = 0;

static int p3_dbg(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL3_DBG"); v = (e && *e && *e != '0'); }
    return v;
}
/* OPENPETE_PORTAL3_DBG_FROM: the first VBL the census reports from. */
static unsigned p3_dbg_from(void) {
    static unsigned v = 0; static int init = 0;
    if (!init) { const char* e = getenv("OPENPETE_PORTAL3_DBG_FROM"); v = e ? (unsigned)strtoul(e, NULL, 0) : 0u; init = 1; }
    return v;
}
static int s_cen_faces, s_cen_idx, s_cen_oc, s_cen_edge, s_cen_tris;

/* OPENPETE_PORTAL3_INTERP=0 replays every preview at the state its tick
 * captured, with no lerp toward the previous tick. */
static int p3_interp_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL3_INTERP"); v = !(e && *e == '0'); }
    return v;
}

/* What the census names the feed that is running: the canonical pass, or the
 * sub-tick replay at its alpha. */
static const char* s_feed_src = "canon";
static float s_feed_alpha = 1.0f;

/* ---- capture: the block, before the leaf overwrites nothing and after the
 * pass rewrote it for this portal ---------------------------------------- */
static void p3_capture(CPUState* cpu, int arg) {
    if (s_curr.n >= P3_MAX_PORTALS * 2) return;
    p3_preview* p = &s_curr.p[s_curr.n];
    memset(p, 0, sizeof *p);
    uint32_t B = portal_game_s23.block;
    p->ymin = (int32_t)cpu->read_word(B + 0x00);
    p->ymax = (int32_t)cpu->read_word(B + 0x04);
    p->xmin = (int32_t)cpu->read_word(B + 0x08);
    p->xmax = (int32_t)cpu->read_word(B + 0x0C);
    p->mesh = cpu->read_word(B + 0x10);
    p->arg  = arg;
    if (arg == 0) p->mesh += 0x3C;
    p->depth7 = (int32_t)cpu->read_word(B + 0x14);
    p->depth_w = (float)(p->depth7 << 7);
    uint32_t r = (cpu->read_half(B + 0x18) >> 4) & 0xFF, g = (cpu->read_half(B + 0x1A) >> 4) & 0xFF,
             b = (cpu->read_half(B + 0x1C) >> 4) & 0xFF;
    p->far_rgb = r | (g << 8) | (b << 16);
    p->ir0 = (int16_t)cpu->read_half(B + 0x1E);
    int ne = (int32_t)cpu->read_word(B + 0x20);
    if (ne < 0) ne = 0; if (ne > P3_MAX_EDGES) ne = P3_MAX_EDGES;
    p->nedge = ne;
    for (int i = 0; i < ne; i++) {
        uint32_t a = cpu->read_word(B + 0x24 + i * 8), b2 = cpu->read_word(B + 0x28 + i * 8);
        p->ex[i][0] = (float)(int16_t)(a & 0xFFFF);  p->ey[i][0] = (float)(int16_t)(a >> 16);
        p->ex[i][1] = (float)(int16_t)(b2 & 0xFFFF); p->ey[i][1] = (float)(int16_t)(b2 >> 16);
    }
    /* Portal slot: the leaf pair (1 then 0) belongs to one portal. */
    p->slot = (s_curr.n == 0) ? 0 : (s_curr.p[s_curr.n - 1].arg == 1 && arg == 0
                                     ? s_curr.p[s_curr.n - 1].slot
                                     : s_curr.p[s_curr.n - 1].slot + 1);
    s_curr.n++;
}

/* ---- extraction ------------------------------------------------------ */
typedef struct { float x, y; uint32_t rgb; } p3_v;

static float p3_cross(float ax, float ay, float bx, float by, float vx, float vy) {
    return (bx - ax) * (vy - ay) - (by - ay) * (vx - ax);
}

/* Clip polygon `in` (n verts) against the half-plane cross >= 0 of edge e. */
static int p3_clip_edge(const p3_v* in, int n, p3_v* out, float ax, float ay, float bx, float by) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const p3_v *cur = &in[i], *nxt = &in[(i + 1) % n];
        float dc = p3_cross(ax, ay, bx, by, cur->x, cur->y);
        float dn = p3_cross(ax, ay, bx, by, nxt->x, nxt->y);
        int ci = dc >= 0.0f, ni = dn >= 0.0f;
        if (ci) out[m++] = *cur;
        if (ci != ni) {
            float t = dc / (dc - dn);
            p3_v v;
            v.x = cur->x + (nxt->x - cur->x) * t;
            v.y = cur->y + (nxt->y - cur->y) * t;
            uint32_t c = 0;
            for (int k = 0; k < 3; k++) {
                float c0 = (float)((cur->rgb >> (k * 8)) & 0xFF), c1 = (float)((nxt->rgb >> (k * 8)) & 0xFF);
                int cv = (int)(c0 + (c1 - c0) * t + 0.5f);
                if (cv < 0) cv = 0; else if (cv > 255) cv = 255;
                c |= (uint32_t)cv << (k * 8);
            }
            v.rgb = c;
            out[m++] = v;
        }
        if (m >= 30) break;
    }
    return m;
}

/* The screen bounds a preview's colours are admitted within, in the 2-D
 * screen space the meshes are authored in.
 *
 * The pass's clip rect is the GPU scissor, so it is written in the 4:3
 * framebuffer's coordinates and can never exceed [0,512]. A rect that spans
 * the whole of it while no pentagon edge cuts the frame is the camera standing
 * inside the portal: nothing bounds the preview, so it is the frame's entire
 * backdrop rather than an aperture cut into the world. Under a frame wider
 * than 4:3 that backdrop has to reach the window's edges, so the bounds become
 * the visible screen-X range.
 *
 * Both tests are needed. A portal approached head on grows until its rect
 * clamps to the frame's sides while its arch is still an opening with the
 * world around it, and there the pentagon still bounds the preview: widening
 * the rect would spill the preview over the world beside the arch. At 4:3 the
 * visible range is [0,512] and every bound is the rect's own. */
typedef struct { float xmin, xmax, ymin, ymax; int widened; } p3_bounds;

#define P3_FRAME_X0 0      /* the 4:3 framebuffer's screen-X range, the rect's */
#define P3_FRAME_X1 512    /* clamp and the authored frame quad's edges */

/* Every corner of the rect's frame on the inside of every pentagon edge. */
static int p3_edges_clear_frame(const p3_preview* p) {
    const float cx[4] = { (float)P3_FRAME_X0, (float)P3_FRAME_X1, (float)P3_FRAME_X1, (float)P3_FRAME_X0 };
    const float cy[4] = { (float)p->ymin, (float)p->ymin, (float)p->ymax, (float)p->ymax };
    for (int e = 0; e < p->nedge; e++)
        for (int k = 0; k < 4; k++)
            if (p3_cross(p->ex[e][0], p->ey[e][0], p->ex[e][1], p->ey[e][1], cx[k], cy[k]) < 0.0f)
                return 0;
    return 1;
}

static p3_bounds p3_bounds_of(const p3_preview* p) {
    p3_bounds b = { (float)p->xmin, (float)p->xmax, (float)p->ymin, (float)p->ymax, 0 };
    float xlo = 0.0f, xhi = 0.0f;
    openpete_native_widescreen_x_bounds(&xlo, &xhi);
    if (p->xmin <= P3_FRAME_X0 && p->xmax >= P3_FRAME_X1 && xlo < (float)P3_FRAME_X0 &&
        p3_edges_clear_frame(p)) {
        b.xmin = xlo; b.xmax = xhi; b.widened = 1;
    }
    return b;
}

static int s_emitted = 0;

static void p3_emit_tri(const p3_v* a, const p3_v* b, const p3_v* c, float offx, float offy,
                        float depth_world, int category, uint32_t tag, int group) {
    openpete_scene_tri t = {0};
    const p3_v* v[3] = { a, b, c };
    openpete_worldvz wz = openpete_worldvz_scaled(depth_world, 1.0f);
    float z = openpete_native_depth_from_viewz(wz);
    for (int k = 0; k < 3; k++) {
        t.v[k].sx = v[k]->x + offx; t.v[k].sy = v[k]->y + offy;
        t.v[k].z = z; t.v[k].pvz = wz.vz;
        t.v[k].u = 0.0f; t.v[k].v = 0.0f;
        t.v[k].rgba = (v[k]->rgb & 0xFFFFFFu) | 0xFF000000u;
    }
    t.textured = 0; t.blend = 0;
    (void)tag;
    openpete_native_scene_add_tri_grouped(&t, category, 0, group);
    if (p3_dbg() && category == OPENPETE_SCENE_PORTAL_CYCLORAMA) {
        static unsigned s_dumps = 0;
        if (s_dumps < 12 && openpete_frame >= p3_dbg_from()) {
            s_dumps++;
            fprintf(stderr, "[portal3-native f=%u] rgb=%06X/%06X/%06X xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) off=(%.0f,%.0f)\n",
                    openpete_frame, a->rgb & 0xFFFFFF, b->rgb & 0xFFFFFF, c->rgb & 0xFFFFFF,
                    a->x, a->y, b->x, b->y, c->x, c->y, offx, offy);
        }
    }
    s_emitted++;
}

/* One mesh face to its clipped screen polygon: the index check, the shared-side
 * outcode reject, the flat-or-gouraud colour indices and the clip to the
 * pentagon's inside half-planes. Returns the polygon's vertex count in `poly`,
 * 0 when the face is dropped; the census counters name which test dropped it.
 *
 * The pentagon test is the leaf's own: a face with every vertex outside one
 * edge is dropped whole, a face straddling an edge is clipped to it, and a
 * clip that collapses the polygon drops it. */
typedef struct {
    const float*    vx;
    const float*    vy;
    const uint8_t*  oc;
    const uint32_t* col;
    uint32_t        vc, cc;
} p3_mesh;

static int p3_clip_face(const p3_preview* p, const p3_mesh* m, uint32_t w0, uint32_t w1, p3_v* poly) {
    uint32_t i0 = (w0 >> 20) & 0xFFF, i1 = (w0 >> 10) & 0x3FF, i2 = w0 & 0x3FF;
    if (i0 >= m->vc || i1 >= m->vc || i2 >= m->vc) { s_cen_idx++; return 0; }
    if (m->oc[i0] & m->oc[i1] & m->oc[i2]) { s_cen_oc++; return 0; }
    uint32_t k0, k1, k2;
    if (w1 & 0x80000000u) { k0 = k1 = k2 = (w1 >> 20) & 0x3FF; }
    else { k0 = ((w1 >> 18) & 0xFFC) >> 2; k1 = ((w1 >> 8) & 0xFFC) >> 2; k2 = ((w1 << 2) & 0xFFC) >> 2; }
    const uint32_t idx[3] = { i0, i1, i2 }, kidx[3] = { k0, k1, k2 };
    for (int k = 0; k < 3; k++) {
        poly[k].x = m->vx[idx[k]]; poly[k].y = m->vy[idx[k]];
        poly[k].rgb = kidx[k] < m->cc ? m->col[kidx[k]] : 0;
    }
    p3_v tmp[32];
    int n = 3;
    for (int e = 0; e < p->nedge; e++) {
        const float ax = p->ex[e][0], ay = p->ey[e][0], bx = p->ex[e][1], by = p->ey[e][1];
        int outside = 0;
        for (int k = 0; k < n; k++)
            if (p3_cross(ax, ay, bx, by, poly[k].x, poly[k].y) < 0.0f) outside++;
        if (outside == 0) continue;
        if (outside == n) { s_cen_edge++; return 0; }
        n = p3_clip_edge(poly, n, tmp, ax, ay, bx, by);
        memcpy(poly, tmp, sizeof(p3_v) * (size_t)n);
        if (n < 3) { s_cen_edge++; return 0; }
    }
    return n;
}

static void p3_extract_one(CPUState* cpu, const p3_preview* p, const ngte_view* vw,
                           int sky_ir0, uint32_t env_far, int mask_done[P3_MAX_PORTALS]) {
    static float vx[P3_MAX_VERTS], vy[P3_MAX_VERTS];
    static uint8_t oc[P3_MAX_VERTS];
    static uint32_t col[P3_MAX_COLS];
    uint32_t M = p->mesh;
    if (M < 0x80000000u || M >= 0x80200000u) return;
    uint32_t vc = cpu->read_word(M + 0x00), vo = cpu->read_word(M + 0x04);
    uint32_t cc = cpu->read_word(M + 0x08), co = cpu->read_word(M + 0x0C);
    uint32_t fc = cpu->read_word(M + 0x10), fo = cpu->read_word(M + 0x14);
    if (vc > P3_MAX_VERTS || cc > P3_MAX_COLS || fc > 4096) return;
    const float offx = (float)(vw->ofx - 256.0), offy = (float)(vw->ofy - 120.0);
    const float depth_world = p->depth_w;
    const int group = 1 + p->slot;
    const p3_bounds B = p3_bounds_of(p);
    /* The background record is one quad spanning the 4:3 frame, the backdrop
     * the picture is painted over; the picture itself is authored well past the
     * frame's sides. Widened bounds therefore reach real picture geometry, and
     * the backdrop's two side edges move out with them so the band the picture
     * does not cover keeps its colour instead of the frame's clear. The edge
     * colours travel with the vertices, so the band continues the frame's edge
     * exactly and the interior interpolates as it did. */
    const int widen_quad = B.widened && p->arg == 1;

    for (uint32_t i = 0; i < vc; i++) {
        /* (y << 16 | x) << 5, the low five bits draw-owned scratch: x is
         * the signed 16 bits above them, y the signed 11 bits above x (a
         * picture reaches a few dozen rows above and below the frame). */
        uint32_t w = cpu->read_word(M + vo + i * 4);
        int x = (int16_t)((w >> 5) & 0xFFFF);
        int y = (int)((int32_t)w >> 21);
        float fx = (float)x;
        if (widen_quad && x <= P3_FRAME_X0) fx = B.xmin;
        else if (widen_quad && x >= P3_FRAME_X1) fx = B.xmax;
        vx[i] = fx; vy[i] = (float)y;
        uint8_t o = 0;
        if (y <= p->ymin) o |= 1; if (y >= p->ymax) o |= 2;
        if (fx <= B.xmin) o |= 4; if (fx >= B.xmax) o |= 8;
        oc[i] = o;
    }
    for (uint32_t j = 0; j < cc; j++) {
        uint32_t c = cpu->read_word(M + co + j * 4) & 0xFFFFFF;
        if (p->ir0) c = ngte_depth_cue(c, p->far_rgb, p->ir0);
        if (sky_ir0) c = ngte_depth_cue(c, env_far, sky_ir0);
        col[j] = c;
    }
    /* The portal's stencil footprint: the rect at the portal depth, once per
     * portal (the background record comes first). */
    if (p->slot < P3_MAX_PORTALS && !mask_done[p->slot]) {
        mask_done[p->slot] = 1;
        const float mask_depth = depth_world;
        p3_v r0 = { B.xmin, B.ymin, 0 }, r1 = { B.xmax, B.ymin, 0 };
        p3_v r2 = { B.xmax, B.ymax, 0 }, r3 = { B.xmin, B.ymax, 0 };
        p3_emit_tri(&r0, &r1, &r2, offx, offy, mask_depth, OPENPETE_SCENE_PORTAL_MASK, 0, group);
        p3_emit_tri(&r0, &r2, &r3, offx, offy, mask_depth, OPENPETE_SCENE_PORTAL_MASK, 0, group);
    }
    const uint32_t tag = OPENPETE_TAG_PORTAL(p->arg ? OPENPETE_TAG_PORTAL_FLAT : OPENPETE_TAG_PORTAL_DOME, p->slot);
    openpete_native_scene_set_tag(tag);
    const float pic_depth = depth_world;
    const p3_mesh mesh = { vx, vy, oc, col, vc, cc };
    for (uint32_t f = 0; f < fc; f++) {
        uint32_t w0 = cpu->read_word(M + fo + f * 8), w1 = cpu->read_word(M + fo + f * 8 + 4);
        s_cen_faces++;
        p3_v poly[32];
        const int n = p3_clip_face(p, &mesh, w0, w1, poly);
        if (n < 3) continue;
        s_cen_tris += n - 2;
        for (int k = 1; k + 1 < n; k++)
            p3_emit_tri(&poly[0], &poly[k], &poly[k + 1], offx, offy, pic_depth,
                        OPENPETE_SCENE_PORTAL_CYCLORAMA, tag, group);
    }
    openpete_native_scene_set_tag(0);
}

static int p3_opening_poly(const p3_preview* p, p3_v* out, int maxn);

/* The aperture census: one uncapped line per fed preview, holding the opening
 * polygon in the screen space p3_emit_tri draws into and the per-record
 * channels the replay carries. It is the statistic
 * scripts/check_feeder_subtick.sh --path portal judges, the aperture being far
 * larger than the compact blob a pixel mask models. */
static void p3_census(const p3_tick* T, const ngte_view* vw) {
    const float offx = (float)(vw->ofx - 256.0), offy = (float)(vw->ofy - 120.0);
    fprintf(stderr, "[portal3-feed f=%u src=%s a=%.3f portals=%d tris=%d emitted=%d]\n",
            openpete_frame, s_feed_src, (double)s_feed_alpha, T->n, s_cen_tris, s_emitted);
    for (int i = 0; i < T->n; i++) {
        const p3_preview* p = &T->p[i];
        p3_v poly[32];
        const int pn = p3_opening_poly(p, poly, 32);
        fprintf(stderr, "[portal3-ap f=%u src=%s a=%.3f slot=%d arg=%d n=%d d7=%d dw=%.1f ir0=%d far=%06X]",
                openpete_frame, s_feed_src, (double)s_feed_alpha, p->slot, p->arg, pn,
                p->depth7, (double)p->depth_w, p->ir0, p->far_rgb);
        for (int k = 0; k < pn; k++)
            fprintf(stderr, " (%.2f,%.2f)", (double)(poly[k].x + offx), (double)(poly[k].y + offy));
        fprintf(stderr, "\n");
    }
}

static void p3_feed(CPUState* cpu, const p3_tick* T) {
    if (!openpete_native_scene_enabled() || !render_path_enabled(RP_PORTAL)) return;
    s_emitted = 0;
    s_cen_faces = s_cen_idx = s_cen_oc = s_cen_edge = s_cen_tris = 0;
    /* A pass that captured no preview emits nothing, so it must not open the
     * frame's scene: opening snapshots and clears whatever the frame holds. */
    if (T->n == 0) return;
    /* In a game state that draws neither sky nor terrain this pass is the
     * frame's first native feeder, so it opens the frame's scene itself.
     * Without that, the previews append to the previous frame's already
     * presented scene and the next feeder's frame-keyed clear discards them.
     * The call is keyed on the frame: a scene an earlier feeder opened is
     * left as it is and the previews append to it. */
    openpete_native_scene_begin_frame(openpete_frame);
    ngte_view vw; ngte_view_build(cpu, &vw);
    int sky_ir0 = game_layout.fog.factor ? (int)cpu->read_word(game_layout.fog.factor) : 0;
    uint32_t env_far = ngte_fog_far_colour(cpu);
    int mask_done[P3_MAX_PORTALS]; memset(mask_done, 0, sizeof mask_done);
    for (int i = 0; i < T->n; i++) p3_extract_one(cpu, &T->p[i], &vw, sky_ir0, env_far, mask_done);
    if (s_emitted) s_sky_fed_frame = openpete_frame;
    if (p3_dbg() && openpete_frame >= p3_dbg_from()) p3_census(T, &vw);
}

/* ---- opening planes for the shared terrain feeder ----------------------
 *
 * The terrain feeder drops a transparent face whose centroid falls inside an
 * opening and whose mean view-Z is at or behind the opening plane minus the
 * cover margin: blended terrain draws AFTER the portal sequence, so without
 * the drop it paints over a preview that has already covered it.
 *
 * Unlike Spyro 1's arch previews : a dome rendered per pixel at the depth of
 * a 3-D opening polygon : a Spyro 3 preview is a pre-projected 2-D picture
 * pasted at one constant depth (the pass's depth >> 7, restored by
 * p3_extract_one as `depth7 << 7` full-world view-Z). So the plane is
 * constant in screen space: inv-vz = 1 / (depth7 << 7), a = b = 0. The sky's
 * render depth carries no OT slot push, so push_vz is 0.
 *
 * The footprint is where the preview's colours actually land: the clip rect
 * intersected with the pass's pentagon half-planes, in the same recentred
 * screen space p3_emit_tri draws into. The mask rect alone is too wide : it
 * is the stencil aperture, and the colour tris are pentagon-clipped inside
 * it, so a face outside the pentagon is not covered by anything.
 *
 * The planes come from the tick's capture, not from a live re-derivation:
 * the pass rewrites the parameter block per portal and it means nothing outside
 * the pass, so the rect and edges exist only as captured. The terrain feed
 * runs before the portal pass within a present, so a canonical present reads
 * the previous tick's rect : one tick of lag while a portal moves on screen,
 * zero while the camera holds. */
static double p3_plane_depth_world(const p3_preview* p) {
    double d = (double)p->depth_w;
    return d > 1.0 ? d : 1.0;
}

/* The preview's painted footprint as a screen polygon: the clip rect clipped
 * by each pentagon edge's inside half-plane (the same cross >= 0 rule the
 * colour extraction clips with). Returns the vertex count, 0 if it collapses. */
static int p3_opening_poly(const p3_preview* p, p3_v* out, int maxn) {
    p3_v poly[32], tmp[32];
    const p3_bounds B = p3_bounds_of(p);
    poly[0].x = B.xmin; poly[0].y = B.ymin; poly[0].rgb = 0;
    poly[1].x = B.xmax; poly[1].y = B.ymin; poly[1].rgb = 0;
    poly[2].x = B.xmax; poly[2].y = B.ymax; poly[2].rgb = 0;
    poly[3].x = B.xmin; poly[3].y = B.ymax; poly[3].rgb = 0;
    int n = 4;
    for (int e = 0; e < p->nedge; e++) {
        const float ax = p->ex[e][0], ay = p->ey[e][0], bx = p->ex[e][1], by = p->ey[e][1];
        int outside = 0;
        for (int k = 0; k < n; k++)
            if (p3_cross(ax, ay, bx, by, poly[k].x, poly[k].y) < 0.0f) outside++;
        if (outside == 0) continue;
        if (outside == n) return 0;
        n = p3_clip_edge(poly, n, tmp, ax, ay, bx, by);
        memcpy(poly, tmp, sizeof(p3_v) * (size_t)n);
        if (n < 3) return 0;
    }
    if (n > maxn) return 0;
    memcpy(out, poly, sizeof(p3_v) * (size_t)n);
    return n;
}

int portal_s23_sky_recent(void) {
    return s_sky_fed_frame != 0u && (openpete_frame - s_sky_fed_frame) <= 2u;
}

int portal_s23_sky_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max) {
    (void)cpu;
    if (!s_curr.valid || s_curr.n == 0 || max <= 0) return 0;
    if (openpete_frame - s_curr.frame > 3u) return 0;
    const float offx = (float)(v->ofx - 256.0), offy = (float)(v->ofy - 120.0);
    int n = 0;
    for (int i = 0; i < s_curr.n && n < max; i++) {
        const p3_preview* p = &s_curr.p[i];
        if (p->arg != 1) continue;            /* one entry per portal, on its base record */
        if (p->nedge < 3) continue;           /* no pentagon: no bounded opening */
        p3_v poly[16];
        const int pn = p3_opening_poly(p, poly, 16);
        if (pn < 3) continue;
        openpete_portal_skyplane* o = &out[n];
        memset(o, 0, sizeof *o);
        const double dz = p3_plane_depth_world(p);
        o->portal  = p->mesh;                 /* the preview mesh: this portal's identity */
        o->a = 0.0; o->b = 0.0; o->c = 1.0 / dz;
        o->back_vz = dz;
        o->ot_vz   = dz;
        o->push_vz = 0.0;
        o->pn = pn;
        float x0 = poly[0].x + offx, x1 = x0, y0 = poly[0].y + offy, y1 = y0;
        for (int k = 0; k < pn; k++) {
            o->px[k] = poly[k].x + offx;
            o->py[k] = poly[k].y + offy;
            if (o->px[k] < x0) x0 = o->px[k];
            if (o->px[k] > x1) x1 = o->px[k];
            if (o->py[k] < y0) y0 = o->py[k];
            if (o->py[k] > y1) y1 = o->py[k];
        }
        o->x0 = x0; o->x1 = x1; o->y0 = y0; o->y1 = y1;
        /* No view-space outline: the preview is a flat picture at one depth,
         * so the opening has no measurable orientation in the world and the
         * rim classifier, which reasons in the opening plane, has nothing to
         * reason about. vn = 0 leaves its frames invalid. */
        o->vn = 0;
        if (p3_dbg() && openpete_frame >= p3_dbg_from()) {
            static unsigned s_lines = 0;
            if (s_lines < 24) {
                s_lines++;
                fprintf(stderr, "[portal3-plane f=%u] mesh=%08X vz=%.0f pn=%d bbox=(%.0f,%.0f)-(%.0f,%.0f)\n",
                        openpete_frame, o->portal, dz, pn, o->x0, o->y0, o->x1, o->y1);
            }
        }
        n++;
    }
    return n;
}

/* ---- hooks ------------------------------------------------------------ */
static int on_portal_leaf(CPUState* cpu, uint32_t addr) {
    int feed = openpete_native_scene_enabled();
    if (!feed && !p3_dbg()) return 0;
    if (!s_in_pass) return 0;
    static int s_in = 0;              /* the dispatch below re-enters this hook */
    if (s_in) return 0;
    s_in = 1;
    int arg = (int32_t)cpu->a0;
    p3_capture(cpu, arg);
    s_leaf_calls++;
    uint32_t start = cpu->read_word(game_layout.polybuf.cursor);
    openpete_recomp_module_get()->dispatch(cpu, addr);
    uint32_t end = cpu->read_word(game_layout.polybuf.cursor);
    if (p3_dbg()) {
        /* The leaf's prims, for the census: F3 0x14 bytes, G3 0x1C, the size
         * byte of the link word naming the type. */
        static unsigned s_dumps = 0;
        if (s_dumps < 6 && openpete_frame >= p3_dbg_from()) {
            s_dumps++;
            fprintf(stderr, "[portal3-prims f=%u arg=%d] bytes=%u:", openpete_frame, arg, end - start);
            uint32_t q = start; int n = 0;
            while (q + 0x14 <= end && n < 6) {
                uint32_t w0 = cpu->read_word(q), code = w0 >> 24;
                if (code == 0x04) {
                    fprintf(stderr, " F3 rgb=%06X xy=(%d,%d)(%d,%d)(%d,%d)", cpu->read_word(q + 4) & 0xFFFFFF,
                            (int16_t)(cpu->read_word(q + 8) & 0xFFFF), (int16_t)(cpu->read_word(q + 8) >> 16),
                            (int16_t)(cpu->read_word(q + 12) & 0xFFFF), (int16_t)(cpu->read_word(q + 12) >> 16),
                            (int16_t)(cpu->read_word(q + 16) & 0xFFFF), (int16_t)(cpu->read_word(q + 16) >> 16));
                    q += 0x14;
                } else if (code == 0x06) {
                    fprintf(stderr, " G3 rgb=%06X/%06X/%06X xy=(%d,%d)(%d,%d)(%d,%d)",
                            cpu->read_word(q + 4) & 0xFFFFFF, cpu->read_word(q + 12) & 0xFFFFFF, cpu->read_word(q + 20) & 0xFFFFFF,
                            (int16_t)(cpu->read_word(q + 8) & 0xFFFF), (int16_t)(cpu->read_word(q + 8) >> 16),
                            (int16_t)(cpu->read_word(q + 16) & 0xFFFF), (int16_t)(cpu->read_word(q + 16) >> 16),
                            (int16_t)(cpu->read_word(q + 24) & 0xFFFF), (int16_t)(cpu->read_word(q + 24) >> 16));
                    q += 0x1C;
                } else { fprintf(stderr, " ?%02X", code); q += 4; }
                n++;
            }
            fprintf(stderr, "\n");
        }
    }
    s_in = 0;
    return 1;
}

static int on_portal_pass(CPUState* cpu, uint32_t addr) {
    int feed = openpete_native_scene_enabled();
    if (!feed && !render_paths_active() && !p3_dbg()) return 0;
    static int s_in = 0;
    if (s_in) return 0;
    s_in = 1;
    if (openpete_frame != s_curr.frame) {
        s_prev = s_curr;
        s_prev.valid = s_curr.valid && (openpete_frame - s_curr.frame <= 3u);
        s_curr.n = 0; s_curr.frame = openpete_frame; s_curr.valid = 1;
    }
    s_leaf_calls = 0;
    s_in_pass = 1;
    s_pass_start = cpu->read_word(game_layout.polybuf.cursor);
    openpete_recomp_module_get()->dispatch(cpu, addr);
    uint32_t end = cpu->read_word(game_layout.polybuf.cursor);
    s_in_pass = 0;
    render_path_record(RP_PORTAL, s_pass_start, end);
    if (feed) p3_feed(cpu, &s_curr);
    if (p3_dbg()) {
        static unsigned s_reports = 0, s_passes = 0;
        s_passes++;
        /* The first passes report even with nothing captured: a pass that runs
         * and captures nothing is the reading that separates "no portal in
         * view" from "the hook never fired". */
        if (s_reports < 64 && (s_curr.n || s_passes <= 4) && openpete_frame >= p3_dbg_from()) {
            s_reports++;
            fprintf(stderr, "[portal3 f=%u] leaf-calls=%d captured=%d prim-bytes=%u emitted=%d faces=%d idx-drop=%d oc-drop=%d edge-drop=%d tris=%d",
                    openpete_frame, s_leaf_calls, s_curr.n, end - s_pass_start, s_emitted,
                    s_cen_faces, s_cen_idx, s_cen_oc, s_cen_edge, s_cen_tris);
            for (int i = 0; i < s_curr.n; i++) {
                const p3_preview* p = &s_curr.p[i];
                fprintf(stderr, " | slot%d arg%d mesh=%08X rect=[%d,%d]x[%d,%d] d7=%d ir0=%d edges=%d",
                        p->slot, p->arg, p->mesh, p->xmin, p->xmax, p->ymin, p->ymax, p->depth7, p->ir0, p->nedge);
            }
            fprintf(stderr, "\n");
        }
    }
    s_in = 0;
    return 1;
}

/* The previous tick's record for one preview, or NULL when the portal was not
 * previewed then. The portal's identity is its preview mesh, since the pass's
 * slot numbering is the order of the portals it gated this tick and shifts
 * whenever one enters or leaves the set; the leaf argument separates the
 * backdrop record from the picture record of the same portal. A slot that
 * agrees as well settles the case of two portals previewing one destination. */
static const p3_preview* p3_pair(const p3_tick* prev, const p3_preview* c) {
    const p3_preview* hit = NULL;
    int n = 0;
    for (int i = 0; i < prev->n; i++) {
        const p3_preview* p = &prev->p[i];
        if (p->arg != c->arg || p->mesh != c->mesh) continue;
        if (p->slot == c->slot) return p;
        hit = p;
        n++;
    }
    return n == 1 ? hit : NULL;
}

/* One preview's captured state at the instant `a` between its two ticks.
 *
 * The pentagon edges pair by index over the count the two ticks share; a tick
 * that shows more edges than its partner holds the extra ones at its own,
 * since the partner has no line for them. The clip rect and the two fade terms
 * pair outright. The depth is lerped in world units rather than in the
 * captured depth >> 7: the native path takes it as a view-Z, so the step the
 * capture quantizes to 128 units is a step in the order the preview occludes
 * terrain, and the interval between the two ticks is continuous.
 *
 * The preview mesh itself is not interpolated: it is baked screen-space art
 * whose vertices are identical from tick to tick, and the aperture is the only
 * thing that moves. */
static void p3_lerp_preview(p3_preview* O, const p3_preview* P, const p3_preview* C,
                            float a) {
    O->xmin = (int)lroundf((float)P->xmin + (float)(C->xmin - P->xmin) * a);
    O->xmax = (int)lroundf((float)P->xmax + (float)(C->xmax - P->xmax) * a);
    O->ymin = (int)lroundf((float)P->ymin + (float)(C->ymin - P->ymin) * a);
    O->ymax = (int)lroundf((float)P->ymax + (float)(C->ymax - P->ymax) * a);
    O->depth_w = P->depth_w + (C->depth_w - P->depth_w) * a;
    O->ir0 = (int)lroundf((float)P->ir0 + (float)(C->ir0 - P->ir0) * a);
    for (int k = 0; k < 24; k += 8) {
        const int pc = (int)((P->far_rgb >> k) & 0xFFu), cc = (int)((C->far_rgb >> k) & 0xFFu);
        const uint32_t lc = (uint32_t)(int)lroundf((float)pc + (float)(cc - pc) * a);
        O->far_rgb = (O->far_rgb & ~(0xFFu << k)) | (lc << k);
    }
    const int ne = P->nedge < C->nedge ? P->nedge : C->nedge;
    for (int e = 0; e < ne; e++)
        for (int k = 0; k < 2; k++) {
            O->ex[e][k] = P->ex[e][k] + (C->ex[e][k] - P->ex[e][k]) * a;
            O->ey[e][k] = P->ey[e][k] + (C->ey[e][k] - P->ey[e][k]) * a;
        }
}

/* Sub-tick re-extract: replay the tick's previews, each lerped toward the
 * record the previous tick captured for the same portal. A portal with no
 * partner there holds its own capture while the rest of the set lerps. */
void portal_s23_reextract(CPUState* cpu, double alpha, int interp_on) {
    if (!openpete_native_scene_enabled()) return;
    if (!render_path_canon_drawn(RP_PORTAL)) return;
    if (!s_curr.valid || s_curr.n == 0) return;
    p3_tick T = s_curr;
    /* At alpha 1 the present is the current tick's own view: replay the
     * capture unchanged, so the identity present reproduces the canonical one
     * bit for bit (a lerp evaluated at 1 rounds differently). */
    int can_lerp = interp_on && p3_interp_on() && alpha < 0.999 && s_prev.valid;
    if (can_lerp) {
        const float a = (float)alpha;
        for (int i = 0; i < T.n; i++) {
            const p3_preview* P = p3_pair(&s_prev, &s_curr.p[i]);
            if (P) p3_lerp_preview(&T.p[i], P, &s_curr.p[i], a);
        }
    }
    s_feed_src = "sub"; s_feed_alpha = (float)alpha;
    p3_feed(cpu, &T);
    s_feed_src = "canon"; s_feed_alpha = 1.0f;
}

/* The two hook addresses come from the game's own table, so the table is
 * filled at boot rather than written out here. A game that names neither
 * registers nothing and never enters this feeder. */
static feature_dispatch_entry_t portal_dispatch[3];

static void portal_apply_options(const openpete_options_t* opts) {
    (void)opts;
    const struct { uint32_t addr; feature_dispatch_fn fn; } hooks[] = {
        { portal_game_s23.pass, on_portal_pass },
        { portal_game_s23.leaf, on_portal_leaf },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++)
        if (hooks[i].addr) { portal_dispatch[n].addr = hooks[i].addr; portal_dispatch[n].fn = hooks[i].fn; n++; }
    portal_dispatch[n].addr = 0u;
    portal_dispatch[n].fn   = NULL;
}

const feature_module_t portal_module = {
    .name           = "portal_native",
    .apply_options  = portal_apply_options,
    .dispatch_table = portal_dispatch,
};
