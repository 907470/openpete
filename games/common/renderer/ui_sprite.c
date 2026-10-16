// games/common/renderer/ui_sprite.c - native renderer feed for the 2D UI
// sprite/text layer (2D-from-OT).
//
// The 3D scene (terrain, mobys, the player avatar, particles, ...) is
// re-derived from gamestate by the other native feeders. The 2D UI - the
// title logo, "PRESS START", HUD sprites, dialog boxes, the level-name
// banner - is a separate layer the game builds as screen-space primitives
// (no GTE), linking each into the shared ordering table through ONE helper
// (ui_game.ot_insert; a0 = primitive pointer). The 3D renderers use an
// INLINED insert, so that helper is the clean 2D chokepoint.
//
// This module hooks it, decodes each 2D primitive it inserts, and feeds it to
// the native scene's HUD overlay layer (OPENPETE_SCENE_HUD - screen-space,
// depth-test off, on top of the world). Textures sample the per-frame VRAM
// mirror the native renderer already uploads, so the font/logo art is
// available for free. Determinism-safe: the hook runs the real routine
// unchanged (the OT link still happens for the PsyCross reference), only
// READING the primitive.
//
// Every address the module reads is a ui_game field
// (games/common/renderer/ui_game.h); a game that fills none registers no
// hook.

#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "renderer/render_paths.h"
#include "renderer/native_gte.h"      /* the shared texture-page decode */
#include "renderer/ui_game.h"
#include "renderer/orchestrator.h"  /* spyro_native_canon_capture_open */
#include "openpete_recomp_module.h"   /* the game's own routine, through the module dispatch */
#include "texture_pack.h"
#include "openpete_fb_snapshot.h"
#include "game_layout.h"          /* the draw-buffer pair the retained freeze reads */
#include "openpete_subtick_view.h"   /* PSX RAM through the sub-tick-consistent view */
#include "openpete_fiber.h"          /* openpete_feed_key_now */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */
#include "renderer/env_knob.h"

extern unsigned openpete_frame;


static unsigned s_cap_frame = 0xFFFFFFFFu;  /* build frame whose batch the last capture released */
/* VBL stamp of the scene the last canonical capture took; 0xFFFFFFFF before
 * the first capture. */
static unsigned s_presented_scene = 0xFFFFFFFFu;
static unsigned s_presented_reopen;         /* 2D feeds that opened a new scene because the
                                             * live one was already captured (engagement) */
static int s_last_abr;                      /* the tracked draw-mode ABR (defined with the dispatch state) */
static unsigned s_post_claim, s_post_sprt, s_post_fade;

/* Primitives fed, by class: the engagement counters a harness reads to tell
 * "the layer drew nothing" from "the layer drew identically" (a zero count is
 * a vacuous pass, not a clean one). */
static unsigned s_fed_prims, s_fed_sprts;
unsigned ui_sprite_prim_count(void) { return s_fed_prims; }
unsigned ui_sprite_sprt_count(void) { return s_fed_sprts; }

/* OPENPETE_UISPRITE_BBOX=<VBL>: at that VBL, the screen bounding box and count
 * of every 2D class fed, so a capture comparison can be confined to the box an
 * element actually occupies instead of a guessed rectangle. */
enum { UICLS_FT4 = 0, UICLS_BAR, UICLS_PANEL, UICLS_LINE, UICLS_SPRT, UICLS_FADE, UICLS_POLYG, UICLS_N };
static const char* const kUiClsName[UICLS_N] = { "ft4", "bar", "panel", "line", "sprt", "fade", "polyg" };
static struct { float x0, y0, x1, y1; unsigned n; } s_bbox[UICLS_N];
static unsigned s_cls_n[UICLS_N];        /* fed per class since boot (always counted) */
/** @brief Returns the VBL named by OPENPETE_UISPRITE_BBOX, or -1 when it is unset or empty.
 *
 * Latched on the first call. A non-numeric value parses as VBL 0.
 * @return The bounding-box capture VBL, or -1 for none. */
static long bbox_vbl(void) {
    static long v = -2;
    if (v == -2) { const char* e = getenv("OPENPETE_UISPRITE_BBOX"); v = (e && *e) ? atol(e) : -1; }
    return v;
}
/** @brief Counts one fed 2D primitive of a class and grows that class's box at the capture VBL.
 *
 * s_cls_n always counts. The box and its count accumulate only while
 * openpete_frame equals bbox_vbl().
 * @param cls A UICLS_* class.
 * @param x0 The left edge, in PSX pixels.
 * @param y0 The top edge, in PSX pixels.
 * @param x1 The right edge, in PSX pixels.
 * @param y1 The bottom edge, in PSX pixels. */
static void bbox_note(int cls, float x0, float y0, float x1, float y1) {
    s_cls_n[cls]++;
    if ((long)openpete_frame != bbox_vbl()) return;
    if (!s_bbox[cls].n) { s_bbox[cls].x0 = x0; s_bbox[cls].y0 = y0;
                          s_bbox[cls].x1 = x1; s_bbox[cls].y1 = y1; }
    else { if (x0 < s_bbox[cls].x0) s_bbox[cls].x0 = x0;
           if (y0 < s_bbox[cls].y0) s_bbox[cls].y0 = y0;
           if (x1 > s_bbox[cls].x1) s_bbox[cls].x1 = x1;
           if (y1 > s_bbox[cls].y1) s_bbox[cls].y1 = y1; }
    s_bbox[cls].n++;
}
/** @brief Prints one bounding-box line per class on the first call after the capture VBL.
 *
 * A no-op when OPENPETE_UISPRITE_BBOX is unset or the VBL has not passed;
 * it prints once per boot. */
static void bbox_report(void) {
    if (bbox_vbl() < 0) return;
    static long done = -1;
    if (done == bbox_vbl() || (long)openpete_frame <= bbox_vbl()) return;
    done = bbox_vbl();
    for (int c = 0; c < UICLS_N; c++)
        fprintf(stderr, "[bbox vbl=%ld %s] n=%u x=[%.0f,%.0f] y=[%.0f,%.0f]\n",
                bbox_vbl(), kUiClsName[c], s_bbox[c].n,
                s_bbox[c].x0, s_bbox[c].x1, s_bbox[c].y0, s_bbox[c].y1);
}

/* Semi-transparent 2D primitives fed, by class and PSX ABR mode (0 (B+F)/2,
 * 1 B+F, 2 B-F, 3 B+F/4): the engagement counter of the blend classes the 2D
 * layers draw (a zero count on a screen that links semi-transparent 2D is a
 * feed that never reached them). `split` counts the textured ones fed as an
 * STP-clear / STP-set pair (feed_hud_tex_quad). Counted always; printed under
 * OPENPETE_UISPRITE_CENSUS once per tick that fed any, the line emitted at
 * the first semi-transparent feed of a later tick. */
enum { SEMI_SPRT = 0, SEMI_FT4, SEMI_F4, SEMI_G, SEMI_LINE, SEMI_N };
static const char* const kSemiName[SEMI_N] = { "sprt", "ft4", "f4", "g", "line" };
static unsigned s_semi[SEMI_N][4];        /* since boot */
static unsigned s_semi_tick[SEMI_N][4];   /* the tick s_semi_frame */
static unsigned s_semi_frame = 0xFFFFFFFFu;
static unsigned s_split_tick;             /* split primitives in the tick s_semi_frame */
/** @brief Prints the finished tick's UI-SEMI census line, then clears the per-tick counters.
 *
 * The line prints only under OPENPETE_UISPRITE_CENSUS and after a tick has
 * been counted; the per-tick counters and the split count clear in every
 * case. */
static void semi_flush_tick(void) {
    static int cen = -1;
    ENV_FLAG_LATCH(cen, "OPENPETE_UISPRITE_CENSUS");
    if (cen && s_semi_frame != 0xFFFFFFFFu) {
        char line[256]; int o = 0;
        for (int c = 0; c < SEMI_N; c++)
            o += snprintf(line + o, sizeof(line) - (size_t)o, " %s=%u/%u/%u/%u", kSemiName[c],
                          s_semi_tick[c][0], s_semi_tick[c][1], s_semi_tick[c][2], s_semi_tick[c][3]);
        fprintf(stderr, "[census f=%u UI-SEMI]%s split=%u\n", s_semi_frame, line, s_split_tick);
    }
    memset(s_semi_tick, 0, sizeof(s_semi_tick));
    s_split_tick = 0;
}
/** @brief Counts one semi-transparent 2D primitive fed, by class and ABR.
 *
 * The first note of a later tick flushes the previous tick's census line.
 * @param cls A SEMI_* class.
 * @param abr The draw-mode ABR; only its low two bits are used.
 * @param split Nonzero when the textured primitive was fed as an STP-clear
 *        and STP-set pair. */
static void semi_note(int cls, unsigned abr, int split) {
    if (openpete_frame != s_semi_frame) { semi_flush_tick(); s_semi_frame = openpete_frame; }
    s_semi[cls][abr & 3u]++; s_semi_tick[cls][abr & 3u]++;
    s_split_tick += split ? 1u : 0u;
}

/* The native blend of a 2D primitive: the scene blend class
 * (openpete_scene_tri.blend) and the vertex alpha. An opaque primitive is
 * class 0. A semi-transparent one blends at the ABR of its draw mode: ABR 0
 * (B+F)/2 is the alpha class at vertex alpha 0x80, which halves an untextured
 * quad; ABR 1 B+F, 2 B-F and 3 B+F/4 are the additive, reverse-subtract and
 * quarter-additive classes, which read no alpha. A textured primitive takes
 * the class alone and the vertex alpha is not read. The GPU blends only its
 * texels with STP set and draws the STP-clear ones opaque: at ABR 0 the
 * fragment alpha carries that (0.5 for an STP texel, 1.0 otherwise); at ABR
 * 1-3 the primitive is split by texel STP (feed_hud_tex_quad). */
typedef struct { int blend; uint32_t alpha; } ui_blend_t;
/** @brief Returns the native blend class and vertex alpha of a 2D primitive.
 *
 * @param semi Nonzero for a semi-transparent primitive.
 * @param abr The draw-mode ABR; only its low two bits are used.
 * @return Class 0 at alpha 0xFF when opaque, else the class of the ABR. */
static ui_blend_t ui_blend(int semi, unsigned abr) {
    static const ui_blend_t kAbr[4] = { { 1, 0x80u }, { 2, 0xFFu }, { 3, 0xFFu }, { 4, 0xFFu } };
    if (!semi) return (ui_blend_t){ 0, 0xFFu };
    return kAbr[abr & 3u];
}

/* games/common/renderer/psx_uv.h - the texel the PSX GPU reads at each pixel
 * of a textured triangle, as a plane over screen position. */

/* The fields are the implementation's to choose; callers use
 * psx_uv_plane_setup and psx_uv_plane_at only. */
typedef struct psx_uv_plane {
    double u_org, v_org;   /* u and v at screen position (0, 0) */
    double dudx, dudy;     /* u per pixel in x and in y */
    double dvdx, dvdy;     /* v per pixel in x and in y */
} psx_uv_plane;

/** @brief Builds the plane of texel coordinates the PSX GPU reads across a textured triangle.
 *
 * The plane is affine in screen position. At every pixel (x, y) the GPU
 * draws for the triangle, floor(psx_uv_plane_at(p, x, y, 0)) mod 256 and
 * floor(psx_uv_plane_at(p, x, y, 1)) mod 256 are the u and v the GPU reads
 * (before the texture window). For a sprite (x, y, w, h, u, v) the same
 * holds at every pixel of the sprite on each of the two triangles it is fed
 * as: (x, y, u, v), (x + w, y, u + w, v), (x, y + h, u, v + h) and
 * (x + w, y, u + w, v), (x + w, y + h, u + w, v + h), (x, y + h, u, v + h).
 * The expected output is the measured table tests/unit/data/gpu-uv/uv.texels
 * (gpu_uv_table_test).
 * @param iv The triangle's integer vertices in the GP0 command's order, each {x, y, u, v}.
 * @param p Receives the plane.
 * @return 1 on success; 0 when the triangle's screen area is zero and the
 *         GPU draws nothing. */
static int psx_uv_plane_setup(const int iv[3][4], psx_uv_plane* p);

/** @brief Evaluates the plane at a screen position, in texels, unwrapped.
 *
 * @param p The plane from psx_uv_plane_setup.
 * @param x The screen x: pixel (x, y) is the integer position, fractions interpolate.
 * @param y The screen y.
 * @param which_v Nonzero for v, zero for u.
 * @return The plane's value. */
static double psx_uv_plane_at(const psx_uv_plane* p, double x, double y, int which_v);

/* games/common/renderer/psx_uv.c - the texel the PSX GPU reads at each pixel
 * of a textured triangle.
 *
 * Measured rule (tests/unit/data/gpu-uv/uv.texels: census triangles of the
 * three games, sprites, plain, mirrored, scaled and skewed quads, random
 * triangles and their reorderings):
 *
 * - Each of u and v has two gradients, per pixel in x and in y, in fixed
 *   point with 12 fraction bits: the exact gradient of the plane through
 *   the three vertices, scaled by 4096 and truncated toward zero.
 * - The plane starts at one vertex, the base: the vertex with the smallest
 *   x. When two vertices share the smallest x, the base is the one that
 *   follows the other in the command's cyclic vertex order (v0 -> v1 ->
 *   v2 -> v0). This tie is the only way vertex order changes the texels.
 * - At pixel (x, y) the value is base.u * 4096 + 2048 + Gx * (x - base.x) +
 *   Gy * (y - base.y), and the texel is that value shifted right by 12
 *   (floor), mod 256. The 2048 rounds the base texel to the nearest.
 *
 * The hardware documentation states only that the GPU interpolates texture
 * coordinates linearly in screen space; the precision, the base vertex and
 * the rounding are taken from the table.
 *
 * The plane is stored as doubles holding integers divided by 4096. For
 * integer x and y, and for every value an on-screen triangle produces, each
 * product and sum in psx_uv_plane_at is exact in double precision, so the
 * floor of the result equals the integer computation regardless of the
 * floating-point rounding mode. */

#define PSX_UV_FRAC_BITS 12
#define PSX_UV_ONE (1LL << PSX_UV_FRAC_BITS)

/** @brief Returns the index of the vertex the texel plane starts from.
 *
 * The vertex with the smallest x; of two sharing it, the one that follows
 * the other in cyclic vertex order.
 * @param iv The triangle's vertices, each {x, y, u, v}.
 * @return 0, 1 or 2. */
static int psx_uv_base_vertex(const int iv[3][4])
{
    int b = 0;
    for (int i = 1; i < 3; i++)
        if (iv[i][0] < iv[b][0]) b = i;
    for (int i = 0; i < 3; i++)
        if (i != b && iv[i][0] == iv[b][0] && i == (b + 1) % 3) return i;
    return b;
}

static int psx_uv_plane_setup(const int iv[3][4], psx_uv_plane* p)
{
    const long long ex1 = iv[1][0] - iv[0][0], ey1 = iv[1][1] - iv[0][1];
    const long long ex2 = iv[2][0] - iv[0][0], ey2 = iv[2][1] - iv[0][1];
    const long long area = ex1 * ey2 - ex2 * ey1;   /* twice the signed area */
    p->u_org = p->v_org = p->dudx = p->dudy = p->dvdx = p->dvdy = 0.0;
    if (area == 0) return 0;
    const int b = psx_uv_base_vertex(iv);
    long long g[2][2];   /* [u or v][x or y], 12 fraction bits */
    for (int w = 0; w < 2; w++) {
        const long long d1 = iv[1][2 + w] - iv[0][2 + w], d2 = iv[2][2 + w] - iv[0][2 + w];
        /* C division truncates toward zero, as the measured gradients do. */
        g[w][0] = (d1 * ey2 - d2 * ey1) * PSX_UV_ONE / area;
        g[w][1] = (ex1 * d2 - ex2 * d1) * PSX_UV_ONE / area;
    }
    const long long bx = iv[b][0], by = iv[b][1];
    const long long u0 = iv[b][2] * PSX_UV_ONE + PSX_UV_ONE / 2 - g[0][0] * bx - g[0][1] * by;
    const long long v0 = iv[b][3] * PSX_UV_ONE + PSX_UV_ONE / 2 - g[1][0] * bx - g[1][1] * by;
    const double one = (double)PSX_UV_ONE;
    p->u_org = (double)u0 / one;
    p->v_org = (double)v0 / one;
    p->dudx = (double)g[0][0] / one;
    p->dudy = (double)g[0][1] / one;
    p->dvdx = (double)g[1][0] / one;
    p->dvdy = (double)g[1][1] / one;
    return 1;
}

static double psx_uv_plane_at(const psx_uv_plane* p, double x, double y, int which_v)
{
    return which_v ? p->v_org + p->dvdx * x + p->dvdy * y
                   : p->u_org + p->dudx * x + p->dudy * y;
}



/** @brief Rounds a fed 2D triangle's screen positions and texel coordinates back to the primitive's integer words.
 *
 * @param t The fed triangle.
 * @param iv Receives each vertex as {x, y, u, v}, rounded by lrintf. */
static void tri_int_verts(const openpete_scene_tri* t, int iv[3][4]) {
    for (int k = 0; k < 3; k++) {
        iv[k][0] = (int)lrintf(t->v[k].sx); iv[k][1] = (int)lrintf(t->v[k].sy);
        iv[k][2] = (int)lrintf(t->v[k].u);  iv[k][3] = (int)lrintf(t->v[k].v);
    }
}

/* psx_uv_plane_at's values at integer pixels are multiples of 2^-12 texel;
 * a fed value 2^-13 above the plane keeps the fragment stage's truncation on
 * the GPU's texel under the float error of the interpolation. */
#define UI_TEXEL_GUARD (1.0 / 8192.0)

/** @brief Rewrites a textured triangle's vertex u and v so the fragment stage reads the GPU's texel per pixel.
 *
 * Builds the GPU stepper from the triangle's rounded integer words and
 * sets each vertex to the plane value at (sx - 0.5, sy - 0.5) plus
 * UI_TEXEL_GUARD. A triangle of zero screen area is left unchanged.
 * @param t The triangle, rewritten in place. */
static void tex_tri_psx_texel_centre(openpete_scene_tri* t) {
    int iv[3][4]; psx_uv_plane s;
    tri_int_verts(t, iv);
    if (!psx_uv_plane_setup((const int (*)[4])iv, &s)) return;
    for (int j = 0; j < 3; j++) {
        const double x = (double)t->v[j].sx - 0.5, y = (double)t->v[j].sy - 0.5;
        t->v[j].u = (float)(psx_uv_plane_at(&s, x, y, 0) + UI_TEXEL_GUARD);
        t->v[j].v = (float)(psx_uv_plane_at(&s, x, y, 1) + UI_TEXEL_GUARD);
    }
}

/* uvedge census (OPENPETE_UISPRITE_CENSUS): textured 2D triangles fed whose
 * native texel on the first or last covered pixel of any row differs from the
 * GPU's, the GPU's read off psx_uv_plane_setup's stepper, the native's by
 * truncating the fed triangle's interpolated u, v at the pixel centre as the
 * fragment stage does. Printed once per tick, at the first textured feed of a
 * later tick. */
static unsigned s_uvedge_frame = 0xFFFFFFFFu, s_uvedge_tris, s_uvedge_bad;

typedef struct { double dudx, dudy, dvdx, dvdy; } tri_uv_grad_t;

/** @brief Computes the affine u and v gradients of a fed triangle in PSX pixel units.
 *
 * @param t The fed triangle.
 * @param g Receives the four gradients.
 * @return 1 on success, 0 when the triangle's screen area is below 1e-9. */
static int tri_uv_grad(const openpete_scene_tri* t, tri_uv_grad_t* g) {
    const openpete_scene_vert *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
    const double ex1 = (double)b->sx - a->sx, ey1 = (double)b->sy - a->sy;
    const double ex2 = (double)c->sx - a->sx, ey2 = (double)c->sy - a->sy;
    const double det = ex1 * ey2 - ex2 * ey1;
    if (fabs(det) < 1e-9) return 0;
    const double du1 = (double)b->u - a->u, du2 = (double)c->u - a->u;
    const double dv1 = (double)b->v - a->v, dv2 = (double)c->v - a->v;
    g->dudx = (du1 * ey2 - du2 * ey1) / det;  g->dudy = (ex1 * du2 - ex2 * du1) / det;
    g->dvdx = (dv1 * ey2 - dv2 * ey1) / det;  g->dvdy = (ex1 * dv2 - ex2 * dv1) / det;
    return 1;
}

/** @brief Returns the texel coordinate the GPU reads at an integer pixel of a triangle's texel plane.
 *
 * @param p The plane from psx_uv_plane_setup.
 * @param x The pixel column.
 * @param y The pixel row.
 * @param which_v Nonzero for v, zero for u.
 * @return floor of the plane's value, wrapped to 0..255. */
static int psx_texel(const psx_uv_plane* p, int x, int y, int which_v) {
    return (int)floor(psx_uv_plane_at(p, (double)x, (double)y, which_v)) & 0xFF;
}

/** @brief Returns the texel coordinate the fragment stage reads at a pixel of a fed triangle.
 *
 * Interpolates u or v from vertex 0 with the gradients at the pixel centre
 * (x + 0.5, y + 0.5) and truncates toward zero.
 * @param t The fed triangle.
 * @param g The triangle's gradients from tri_uv_grad.
 * @param x The pixel column.
 * @param y The pixel row.
 * @param which_v Nonzero for v, zero for u.
 * @return The truncated texel coordinate. */
static int native_texel(const openpete_scene_tri* t, const tri_uv_grad_t* g, int x, int y, int which_v) {
    const double px = x + 0.5 - t->v[0].sx, py = y + 0.5 - t->v[0].sy;
    const double a = which_v ? t->v[0].v + g->dvdx * px + g->dvdy * py
                             : t->v[0].u + g->dudx * px + g->dudy * py;
    return (int)a;
}

/** @brief Tests whether the centre of pixel (x, y) lies inside a triangle, edges inclusive.
 *
 * @param t The triangle.
 * @param x The pixel column.
 * @param y The pixel row.
 * @return 1 when no two edge functions disagree in sign at (x + 0.5, y + 0.5),
 *         else 0. */
static int tri_covers(const openpete_scene_tri* t, int x, int y) {
    const double px = x + 0.5, py = y + 0.5;
    int pos = 0, neg = 0;
    for (int i = 0; i < 3; i++) {
        const openpete_scene_vert *a = &t->v[i], *b = &t->v[(i + 1) % 3];
        const double e = ((double)b->sx - a->sx) * (py - a->sy) - ((double)b->sy - a->sy) * (px - a->sx);
        pos |= e > 0; neg |= e < 0;
    }
    return !(pos && neg);
}

/** @brief Tests whether the native and GPU texels differ at one pixel.
 *
 * @param fed The fed triangle.
 * @param g The fed triangle's gradients.
 * @param s The GPU stepper of the original triangle.
 * @param x The pixel column.
 * @param y The pixel row.
 * @return Nonzero when u or v differs. */
static int uvedge_pixel_differs(const openpete_scene_tri* fed, const tri_uv_grad_t* g,
                                const psx_uv_plane* s, int x, int y) {
    return native_texel(fed, g, x, y, 0) != psx_texel(s, x, y, 0)
        || native_texel(fed, g, x, y, 1) != psx_texel(s, x, y, 1);
}

/** @brief Tests whether a fed triangle reads another texel than the GPU on the first or last covered pixel of any row.
 *
 * The GPU's texel comes from psx_uv_plane_setup's stepper over the original
 * triangle's integer words; the native texel truncates the fed triangle's
 * interpolated u and v at the pixel centre, as the fragment stage does.
 * Rows run over the original's integer bounding box.
 * @param orig The triangle as decoded from the primitive.
 * @param fed The triangle as fed, after the texel-centre adjustment.
 * @return 1 when any row edge differs, else 0 (also for a degenerate
 *         triangle). */
static int uvedge_tri_differs(const openpete_scene_tri* orig, const openpete_scene_tri* fed) {
    int iv[3][4]; psx_uv_plane s; tri_uv_grad_t g;
    tri_int_verts(orig, iv);
    if (!psx_uv_plane_setup((const int (*)[4])iv, &s) || !tri_uv_grad(fed, &g)) return 0;
    const int x0 = (int)fminf(fminf(orig->v[0].sx, orig->v[1].sx), orig->v[2].sx);
    const int x1 = (int)fmaxf(fmaxf(orig->v[0].sx, orig->v[1].sx), orig->v[2].sx);
    const int y0 = (int)fminf(fminf(orig->v[0].sy, orig->v[1].sy), orig->v[2].sy);
    const int y1 = (int)fmaxf(fmaxf(orig->v[0].sy, orig->v[1].sy), orig->v[2].sy);
    for (int y = y0; y < y1; y++) {
        int lo = x1, hi = x0 - 1;
        for (int x = x0; x < x1; x++)
            if (tri_covers(orig, x, y)) { if (x < lo) lo = x; hi = x; }
        if (hi >= lo && (uvedge_pixel_differs(fed, &g, &s, lo, y) || uvedge_pixel_differs(fed, &g, &s, hi, y)))
            return 1;
    }
    return 0;
}

/** @brief Adds one fed textured triangle to the per-tick UI-UVEDGE census.
 *
 * Active under OPENPETE_UISPRITE_CENSUS. The first note of a later tick
 * prints the previous tick's line; each note counts the triangle and
 * whether uvedge_tri_differs reports a row-edge texel difference.
 * @param orig The triangle as decoded from the primitive.
 * @param fed The triangle as fed. */
static void uvedge_note(const openpete_scene_tri* orig, const openpete_scene_tri* fed) {
    static int cen = -1;
    ENV_FLAG_LATCH(cen, "OPENPETE_UISPRITE_CENSUS");
    if (!cen) return;
    if (openpete_frame != s_uvedge_frame) {
        if (s_uvedge_frame != 0xFFFFFFFFu)
            fprintf(stderr, "[census f=%u UI-UVEDGE] tris=%u edge=%u\n", s_uvedge_frame, s_uvedge_tris, s_uvedge_bad);
        s_uvedge_frame = openpete_frame; s_uvedge_tris = s_uvedge_bad = 0;
    }
    s_uvedge_tris++;
    s_uvedge_bad += (unsigned)uvedge_tri_differs(orig, fed);
}

/** @brief Redirects a quad that samples a framebuffer window to the stored native snapshot of that window.
 *
 * A quad sampling a MoveImage'd copy of a draw buffer reads texels the
 * native render never writes. The quad's texel bounding box is looked up
 * with openpete_fb_snapshot_find; on a hit both triangles take the
 * snapshot's page and OPENPETE_FB_SNAPSHOT_BPP, and each UV is scaled and
 * offset into a pixel position in that image, or into a texel of the rect
 * while its capture is pending (tpage_y 1).
 * @param quad The quad's two triangles, rewritten in place on a hit.
 * @return 1 when redirected, 0 with the quad unchanged when no snapshot holds
 *         its texel box (the stock VRAM path). */
static int quad_to_fb_snapshot(openpete_scene_tri quad[2]) {
    float su = 1.0f, sv = 1.0f, ox = 0.0f, oy = 0.0f;
    int texel_space = 0;
    float lo_u = quad[0].v[0].u, hi_u = lo_u, lo_v = quad[0].v[0].v, hi_v = lo_v;
    for (int t = 0; t < 2; t++)
        for (int j = 0; j < 3; j++) {
            lo_u = fminf(lo_u, quad[t].v[j].u); hi_u = fmaxf(hi_u, quad[t].v[j].u);
            lo_v = fminf(lo_v, quad[t].v[j].v); hi_v = fmaxf(hi_v, quad[t].v[j].v);
        }
    const int snap = openpete_fb_snapshot_find(quad[0].tpage_x, quad[0].tpage_y, quad[0].bpp,
                                               (int)lo_u, (int)lo_v, (int)hi_u, (int)hi_v,
                                               &su, &sv, &ox, &oy, &texel_space);
    if (snap < 0) return 0;
    for (int t = 0; t < 2; t++) {
        openpete_scene_tri* tri = &quad[t];
        tri->tpage_x = snap; tri->tpage_y = texel_space; tri->clut_x = 0; tri->clut_y = 0;
        tri->bpp = OPENPETE_FB_SNAPSHOT_BPP;
        for (int j = 0; j < 3; j++) {
            tri->v[j].u = ox + tri->v[j].u * su;
            tri->v[j].v = oy + tri->v[j].v * sv;
        }
    }
    return 1;
}

/** @brief Returns the framebuffer cell key of a quad triangle that samples a draw buffer's texels, or 0.
 *
 * A 16 bpp triangle whose texel box lies inside a live draw area samples
 * pixels the game rendered into that buffer. The key hashes the texture page
 * and the three texel coordinates as the primitive holds them, which a copy
 * keeps from tick to tick while its screen box scales (the pause window's
 * grow back to full size draws the same 4x4 grid of buffer cells at a larger
 * size each tick, func_8004DF84). The bit 0 is forced so a key is never 0.
 * @param t The triangle with the primitive's own texel coordinates.
 * @return The key, or 0 when the triangle samples no draw buffer. */
static uint32_t fb_cell_key(const openpete_scene_tri* t) {
    if (!t->textured || t->bpp != 16) return 0;
    float lo_u = t->v[0].u, hi_u = lo_u, lo_v = t->v[0].v, hi_v = lo_v;
    for (int j = 1; j < 3; j++) {
        lo_u = fminf(lo_u, t->v[j].u); hi_u = fmaxf(hi_u, t->v[j].u);
        lo_v = fminf(lo_v, t->v[j].v); hi_v = fmaxf(hi_v, t->v[j].v);
    }
    if (!openpete_fb_snapshot_area_holds(t->tpage_x + (int)lo_u, t->tpage_y + (int)lo_v,
                                         (int)(hi_u - lo_u), (int)(hi_v - lo_v)))
        return 0;
    uint32_t h = 2166136261u;
    const uint32_t w[2] = { (uint32_t)t->tpage_x, (uint32_t)t->tpage_y };
    for (int k = 0; k < 2; k++) { h ^= w[k]; h *= 16777619u; }
    for (int j = 0; j < 3; j++) {
        h ^= (uint32_t)t->v[j].u; h *= 16777619u;
        h ^= (uint32_t)t->v[j].v; h *= 16777619u;
    }
    return h | 1u;
}

/** @brief Adds a textured 2D quad's two triangles to the HUD layer, split by texel STP where the blend class needs it.
 *
 * A quad sampling a framebuffer window is first redirected to its snapshot
 * (quad_to_fb_snapshot), whatever primitive it came from; its UVs are then
 * coordinates in the stored image (or rect) and are fed unchanged. A quad that
 * samples VRAM has each triangle's UVs set to the texel the GPU samples on
 * every pixel (tex_tri_psx_texel_centre). A VRAM quad of an ABR 1-3 class
 * is fed as a pair: its STP-clear texels through the opaque class, then its
 * STP-set texels through its own class, since those blend equations read no
 * alpha and the GPU applies them to STP-set texels only. Both halves precede
 * the next primitive's triangles, so the draw order of overlapping
 * primitives holds. Every other quad (opaque, ABR 0, a framebuffer-snapshot
 * window, whose image carries no STP bit) is fed once. A VRAM triangle that
 * samples a draw buffer carries its framebuffer cell key (fb_cell_key).
 * @param in The quad's two triangles.
 * @return 1 for a quad fed as an STP pair, else 0. */
static int feed_hud_tex_quad(const openpete_scene_tri in[2]) {
    openpete_scene_tri quad[2] = { in[0], in[1] };
    const int vram = !quad_to_fb_snapshot(quad);
    uint32_t cell[2] = { 0, 0 };
    for (int t = 0; vram && t < 2; t++) {
        cell[t] = fb_cell_key(&in[t]);
        tex_tri_psx_texel_centre(&quad[t]);
        uvedge_note(&in[t], &quad[t]);
    }
    /* ui_blend's classes 2-4 are ABR 1-3. */
    const int split = quad[0].blend > 1 && vram;
    if (split) {
        openpete_scene_tri clear[2] = { quad[0], quad[1] };
        clear[0].blend = clear[1].blend = 0;
        openpete_native_scene_set_stp_select(OPENPETE_STP_CLEAR_ONLY);
        for (int t = 0; t < 2; t++) {
            openpete_native_scene_set_fb_cell(cell[t]);
            openpete_native_scene_add_tri(&clear[t], OPENPETE_SCENE_HUD, 1);
        }
        openpete_native_scene_set_stp_select(OPENPETE_STP_SET_ONLY);
    }
    for (int t = 0; t < 2; t++) {
        openpete_native_scene_set_fb_cell(cell[t]);
        openpete_native_scene_add_tri(&quad[t], OPENPETE_SCENE_HUD, 1);
    }
    openpete_native_scene_set_fb_cell(0);
    openpete_native_scene_set_stp_select(OPENPETE_STP_ALL);
    return split;
}

/* ---- 2D prim decode + feed ----------------------------------------------- */
/** @brief Opens the scene a 2D feed appends to: the live one unless absent or already presented.
 *
 * A claimed primitive is fed from a batch release (the canonical capture,
 * the frame close or the ordering-table walk), where openpete_frame may
 * already be the next VBL, and a glyph is fed at emission. Beginning the
 * scene of openpete_frame at a late release would push the finished scene
 * into the history ring a VBL early and start a new scene holding only this
 * primitive, and the lagged headless compose would read the wrong slot. The
 * primitive is appended to the live scene instead, the one of the VBL that
 * built it.
 *
 * A late release always precedes the capture of the scene it belongs to,
 * since the capture releases the pending batch first (on_overlay_precapture).
 * A live scene whose stamp equals the one the last capture took
 * (s_presented_scene) was therefore already presented, and the feed belongs
 * to a later VBL whose render opened no scene of its own: a tick that draws
 * 2D alone with no 3D producer and no claim, such as Spyro 3's loading
 * picture (func_80059358, SPRTs fed at emission). Appending there would put
 * the primitive into the presented scene, under that scene's fade and over
 * its world, so the scene of openpete_frame is opened instead. */
static void scene_begin_for_feed(void) {
    unsigned live = openpete_native_scene_frame();
    if (live != 0xFFFFFFFFu && (live != s_presented_scene || live == openpete_frame)) {
        openpete_native_scene_begin_frame(live);
        return;
    }
    if (live != 0xFFFFFFFFu) {
        s_presented_reopen++;
        static int tr = -1;
        ENV_FLAG_LATCH(tr, "OPENPETE_UISPRITE_TRACE");
        if (tr) fprintf(stderr, "[uireopen f=%u key=%u live=%u]\n", openpete_frame,
                        openpete_feed_key_now(), live);
    }
    openpete_native_scene_begin_frame(openpete_frame);
}

/** @brief Feeds one screen-space POLY_FT4 (textured quad) into the native HUD layer.
 *
 * Layout: word[1]=rgb|code, [2]=xy0, [3]=uv0|clut, [4]=xy1, [5]=uv1|tpage,
 * [6]=xy2, [7]=uv2, [8]=xy3, [9]=uv3; XY is x=lo16, y=hi16, signed. The
 * PSX corner order TL, TR, BL, BR becomes the triangles (0,1,2) and
 * (1,3,2). Code bit 1 makes it semi-transparent at the ABR of its own
 * texture page. The quad goes through feed_hud_tex_quad and is counted in
 * the engagement, semi-transparency and bounding-box counters.
 * @param cpu The guest CPU state.
 * @param p The primitive's guest address.
 * @param w1 The primitive's word 1 (colour and command). */
static void feed_ft4(CPUState* cpu, uint32_t p, uint32_t w1) {
    int sx[4], sy[4], u[4], v[4];
    uint32_t xyw[4], uvw[4];
    xyw[0]=cpu->read_word(p+0x08); uvw[0]=cpu->read_word(p+0x0C);
    xyw[1]=cpu->read_word(p+0x10); uvw[1]=cpu->read_word(p+0x14);
    xyw[2]=cpu->read_word(p+0x18); uvw[2]=cpu->read_word(p+0x1C);
    xyw[3]=cpu->read_word(p+0x20); uvw[3]=cpu->read_word(p+0x24);
    for (int k=0;k<4;k++){ sx[k]=(int16_t)(xyw[k]&0xFFFF); sy[k]=(int16_t)(xyw[k]>>16);
                           u[k]=(int)(uvw[k]&0xFF); v[k]=(int)((uvw[k]>>8)&0xFF); }
    unsigned clut = (uvw[0]>>16)&0xFFFF, tp = (uvw[1]>>16)&0xFFFF;
    ngte_tpage tpd;
    ngte_decode_tpage(tp, clut, 0u, &tpd);
    int tpx=tpd.tpx, tpy=tpd.tpy, bpp=tpd.bpp;
    int clx=tpd.clx, cly=tpd.cly;
    uint32_t rgba = (w1 & 0xFFFFFFu) | 0xFF000000u;
    int abe = (int)((w1>>24)&0x2u);   /* code bit1 = semi-transparent, at the page's ABR */
    const int blend = ui_blend(abe, (tp >> 5) & 3u).blend;

    scene_begin_for_feed();
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };   /* TL,TR,BL,BR -> 2 tris */
    openpete_scene_tri quad[2]; memset(quad, 0, sizeof(quad));
    for (int t=0;t<2;t++) {
        openpete_scene_tri* tri = &quad[t];
        for (int j=0;j<3;j++){ int c=idx[t][j];
            tri->v[j].sx=(float)sx[c]; tri->v[j].sy=(float)sy[c];
            tri->v[j].z=1.0f; tri->v[j].pvz=1.0f; tri->v[j].u=(float)u[c]; tri->v[j].v=(float)v[c]; tri->v[j].rgba=rgba; }
        tri->textured=1; tri->tpage_x=tpx; tri->tpage_y=tpy; tri->clut_x=clx; tri->clut_y=cly;
        tri->bpp=bpp; tri->blend = blend;
    }
    const int split = feed_hud_tex_quad(quad);
    if (abe) semi_note(SEMI_FT4, (tp >> 5) & 3u, split);
    bbox_note(UICLS_FT4, (float)sx[0], (float)sy[0], (float)sx[3], (float)sy[3]);
    s_fed_prims++;
}

/* The PSX 4:3 frame the 2D layer is authored in: x runs 0..512 whatever the
 * window is, and the widescreen widen mirrors about its centre column. */
#define UI_FRAME_W 512

/** @brief Feeds one screen-space untextured POLY_F4 (flat quad) into a native 2D layer.
 *
 * Three uses, chosen by the caller:
 *   - the cutscene letterbox: a frame-width opaque bar (code 0x28) on
 *     OPENPETE_SCENE_OVERLAY, drawn last over everything, alpha 0xFF,
 *     widened for widescreen;
 *   - a menu box's backing: a narrower opaque quad (code 0x28) on
 *     OPENPETE_SCENE_HUD_PANEL at alpha 0xFF, under the box's own tiles,
 *     frame and text;
 *   - a dark panel (pause or inventory panel, HUD shaded box, dialog-box
 *     slab): a semi-transparent quad (code 0x2A) on OPENPETE_SCENE_HUD_PANEL,
 *     under the text and over the world, blended at the tracked ABR.
 * Layout: word[1]=code|rgb, [2]=xy0, [3]=xy1, [4]=xy2, [5]=xy3 (no UV words
 * between them, unlike FT4); XY is x=lo16, y=hi16, signed; corners TL, TR,
 * BL, BR.
 * @param cpu The guest CPU state.
 * @param p The primitive's guest address.
 * @param w1 The primitive's word 1 (colour and command).
 * @param category The scene layer (OPENPETE_SCENE_*).
 * @param alpha The vertex alpha.
 * @param blend The scene blend class.
 * @param full_width Nonzero to widen the quad about x 256 by the widescreen
 *        factor, with a 4-pixel edge overshoot the scene scissor clips. */
static void feed_f4(CPUState* cpu, uint32_t p, uint32_t w1, int category, uint32_t alpha, int blend,
                    int full_width) {
    int sx[4], sy[4];
    uint32_t xyw[4];
    xyw[0]=cpu->read_word(p+0x08); xyw[1]=cpu->read_word(p+0x0C);
    xyw[2]=cpu->read_word(p+0x10); xyw[3]=cpu->read_word(p+0x14);
    for (int k=0;k<4;k++){ sx[k]=(int16_t)(xyw[k]&0xFFFF); sy[k]=(int16_t)(xyw[k]>>16); }
    /* Widescreen: a FULL-SCREEN bar (the cutscene letterbox) is authored at the PSX
     * 4:3 width (x 0..512), which scene_to_clip maps to the centred 4:3 inset - so
     * in a wide window the bars stop at the inset and the revealed side strips show
     * the world at top/bottom. Widen such a bar about screen-centre 256 by the Hor+
     * factor F so x 0->xlo and 512->xhi map to the full panel [-1,+1] (same vertical
     * extent - the widened bar stays continuous with the central one). No-op at 4:3
     * (F=1). Vertical bars are unchanged.
     *
     * Over-extend each side edge a few PSX px past the content rect: the exact
     * widened edge lands sub-pixel-precisely on clip ±1, where the rasteriser drops
     * a 1px sliver along the window border, leaving the letterbox short of the
     * edge. The scene scissor clips the overshoot, so the bleed is never visible
     * beyond the edge - it only guarantees the border column is covered. */
    if (full_width) {
        float F = openpete_native_widescreen_factor();
        if (F > 1.0f) for (int k=0;k<4;k++) {
            const float kBleed = 4.0f;   /* PSX px of edge bleed (scissor-clipped) */
            float w = ((float)sx[k] - (float)(UI_FRAME_W / 2)) * F;
            if (w < 0.0f)      w -= kBleed;
            else if (w > 0.0f) w += kBleed;
            sx[k] = UI_FRAME_W / 2 + (int)w;
        }
    }
    uint32_t rgba = (w1 & 0xFFFFFFu) | (alpha << 24);

    scene_begin_for_feed();
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };   /* TL,TR,BL,BR -> 2 tris */
    for (int t=0;t<2;t++) {
        openpete_scene_tri tri; memset(&tri,0,sizeof(tri));
        for (int j=0;j<3;j++){ int c=idx[t][j];
            tri.v[j].sx=(float)sx[c]; tri.v[j].sy=(float)sy[c];
            tri.v[j].z=1.0f; tri.v[j].pvz=1.0f; tri.v[j].rgba=rgba; }
        tri.textured=0; tri.blend=blend;
        openpete_native_scene_add_tri(&tri, category, 0);
    }
    bbox_note(category == OPENPETE_SCENE_OVERLAY ? UICLS_BAR : UICLS_PANEL,
              (float)sx[0], (float)sy[0], (float)sx[3], (float)sy[3]);
    s_fed_prims++;
}

/* The layer, blend class and vertex alpha of an untextured 2D primitive. An
 * opaque one draws on the HUD layer at class 0. A semi-transparent one (code
 * bit 1) blends at the ABR of the draw mode linked before it, on the panel
 * layer, as a dark panel's F4 does. */
typedef struct { int category; int blend; uint32_t alpha; } ui_untex_t;
/** @brief Returns the layer, blend class and alpha of an untextured 2D primitive.
 *
 * A semi-transparent primitive is counted through semi_note at the
 * tracked ABR s_last_abr.
 * @param w1 The primitive's second word; bits 24-31 hold the command byte.
 * @param semi_cls The SEMI_* class counted when the primitive is
 *        semi-transparent.
 * @return HUD at class 0 when opaque, else HUD_PANEL at the ui_blend
 *         result. */
static ui_untex_t ui_untex_layer(uint32_t w1, int semi_cls) {
    if (!((w1 >> 24) & 0x2u)) return (ui_untex_t){ OPENPETE_SCENE_HUD, 0, 0xFFu };
    semi_note(semi_cls, (unsigned)s_last_abr, 0);
    const ui_blend_t b = ui_blend(1, (unsigned)s_last_abr);
    return (ui_untex_t){ OPENPETE_SCENE_HUD_PANEL, b.blend, b.alpha };
}

/** @brief Returns the signed x in the low halfword of a packed PSX XY word.
 *
 * @param xy The packed word.
 * @return The x coordinate. */
static float psx_x(uint32_t xy) { return (float)(int16_t)(xy & 0xFFFFu); }
/** @brief Returns the signed y in the high halfword of a packed PSX XY word.
 *
 * @param xy The packed word.
 * @return The y coordinate. */
static float psx_y(uint32_t xy) { return (float)(int16_t)(xy >> 16); }

/** @brief Feeds one screen-space line into a native 2D layer as a gouraud quad.
 *
 * The scene is triangles only. The GPU draws a line one pixel per step of
 * its major axis from the first endpoint's pixel through the second's, both
 * included. The quad covers that footprint: the line between the two
 * endpoint pixels' centres, extended half a pixel past each end along the
 * major axis and half a pixel to each side along the minor axis, so a
 * horizontal or vertical line covers exactly its pixels. Each end takes its
 * endpoint's colour.
 * @param xy0 The first endpoint's packed XY (x low, y high, signed).
 * @param xy1 The second endpoint's packed XY.
 * @param rgba0 The first endpoint's colour and alpha.
 * @param rgba1 The second endpoint's colour and alpha.
 * @param lay The layer and blend class. */
static void feed_line_quad(uint32_t xy0, uint32_t xy1, uint32_t rgba0, uint32_t rgba1, ui_untex_t lay) {
    const float x0 = psx_x(xy0) + 0.5f, y0 = psx_y(xy0) + 0.5f;
    const float x1 = psx_x(xy1) + 0.5f, y1 = psx_y(xy1) + 0.5f;
    const float dx = x1 - x0, dy = y1 - y0;
    const int xmajor = fabsf(dx) >= fabsf(dy);
    const float major = fmaxf(fabsf(xmajor ? dx : dy), 1.0f);
    const float ex = 0.5f * (xmajor ? (dx < 0.0f ? -1.0f : 1.0f) : dx / major);   /* half-pixel end extension */
    const float ey = 0.5f * (xmajor ? dy / major : (dy < 0.0f ? -1.0f : 1.0f));
    const float sx = xmajor ? 0.0f : 0.5f, sy = xmajor ? 0.5f : 0.0f;         /* half-pixel side offset */

    scene_begin_for_feed();
    /* corners: TL,TR,BL,BR = start-side, end-side, start+side, end+side. */
    const float cx[4] = { x0 - ex - sx, x1 + ex - sx, x0 - ex + sx, x1 + ex + sx };
    const float cy[4] = { y0 - ey - sy, y1 + ey - sy, y0 - ey + sy, y1 + ey + sy };
    const uint32_t cc[4] = { rgba0, rgba1, rgba0, rgba1 };
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };
    for (int t=0;t<2;t++) {
        openpete_scene_tri tri; memset(&tri,0,sizeof(tri));
        for (int j=0;j<3;j++){ int c=idx[t][j];
            tri.v[j].sx=cx[c]; tri.v[j].sy=cy[c]; tri.v[j].z=1.0f; tri.v[j].pvz=1.0f; tri.v[j].rgba=cc[c]; }
        tri.textured=0; tri.blend=lay.blend;
        openpete_native_scene_add_tri(&tri, lay.category, 1);
    }
    bbox_note(UICLS_LINE, psx_x(xy0), psx_y(xy0), psx_x(xy1), psx_y(xy1));
    s_fed_prims++;
}

/** @brief Feeds one opaque LINE_G2 (code 0x50) into the native HUD layer.
 *
 * The UI separator lines (the rules on a results screen, gem-UI dividers)
 * and the dialog name underline, each with a per-endpoint colour. Layout:
 * word[1]=code|rgb0, [2]=xy0, [3]=pad|rgb1, [4]=xy1; XY is x=lo16, y=hi16,
 * signed. Fed at alpha 0xFF, blend class 0.
 * @param cpu The guest CPU state.
 * @param p The primitive's guest address.
 * @param w1 The primitive's word 1 (code and first colour). */
static void feed_line_g2(CPUState* cpu, uint32_t p, uint32_t w1) {
    const uint32_t c1 = cpu->read_word(p + 0x0C);
    feed_line_quad(cpu->read_word(p + 0x08), cpu->read_word(p + 0x10),
                   (w1 & 0xFFFFFFu) | 0xFF000000u, (c1 & 0xFFFFFFu) | 0xFF000000u,
                   (ui_untex_t){ OPENPETE_SCENE_HUD, 0, 0xFFu });
}

/** @brief Feeds one LINE_F2 (flat two-point line) into a native 2D layer.
 *
 * One colour for both endpoints: the border of Spyro 3's boss and opponent
 * health bars. Layout: word[1]=code|rgb, [2]=xy0, [3]=xy1. The layer and
 * blend come from ui_untex_layer, which puts a code with bit 1 set on the
 * panel layer at the tracked ABR; the claim test is_line_f2 admits codes
 * 0x40 and 0x41 only, so that branch is not reached from the chokepoint.
 * @param cpu The guest CPU state.
 * @param p The primitive's guest address.
 * @param w1 The primitive's word 1 (code and colour). */
static void feed_line_f2(CPUState* cpu, uint32_t p, uint32_t w1) {
    const ui_untex_t lay = ui_untex_layer(w1, SEMI_LINE);
    const uint32_t rgba = (w1 & 0xFFFFFFu) | (lay.alpha << 24);
    feed_line_quad(cpu->read_word(p + 0x08), cpu->read_word(p + 0x0C), rgba, rgba, lay);
}

/** @brief Feeds one untextured POLY_G3 or POLY_G4 (gouraud triangle or quad) into a native 2D layer.
 *
 * The fill of Spyro 3's health bars (green to red by health). Code bit 3
 * selects the quad. Layout: vertex k's colour at word 1 + 2k, rgb in the low
 * 24 bits (word 1's top byte is the code), and its xy in the next word. A
 * quad's corners are TL, TR, BL, BR, split (0,1,2) (1,3,2) as the GPU splits
 * it. The layer and blend come from ui_untex_layer: a code with bit 1 set
 * goes on the panel layer at the tracked ABR.
 * @param cpu The guest CPU state.
 * @param p The primitive's guest address.
 * @param w1 The primitive's word 1 (code and first colour). */
static void feed_poly_g(CPUState* cpu, uint32_t p, uint32_t w1) {
    const ui_untex_t lay = ui_untex_layer(w1, SEMI_G);
    const int nv = (w1 >> 24) & 0x08u ? 4 : 3;
    openpete_scene_vert vx[4]; memset(vx, 0, sizeof(vx));
    for (int k = 0; k < nv; k++) {
        const uint32_t c  = k ? cpu->read_word(p + 0x04u + (uint32_t)k * 8u) : w1;
        const uint32_t xy = cpu->read_word(p + 0x08u + (uint32_t)k * 8u);
        vx[k].sx = psx_x(xy); vx[k].sy = psx_y(xy);
        vx[k].z = 1.0f; vx[k].pvz = 1.0f;   /* screen space: view-Z 1, W 1 */
        vx[k].rgba = (c & 0xFFFFFFu) | (lay.alpha << 24);
    }
    scene_begin_for_feed();
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };
    for (int t = 0; t < nv - 2; t++) {
        openpete_scene_tri tri; memset(&tri, 0, sizeof(tri));
        for (int j = 0; j < 3; j++) tri.v[j] = vx[idx[t][j]];
        tri.textured = 0; tri.blend = lay.blend;
        openpete_native_scene_add_tri(&tri, lay.category, 1);
    }
    bbox_note(UICLS_POLYG, vx[0].sx, vx[0].sy, vx[nv - 1].sx, vx[nv - 1].sy);
    s_fed_prims++;
}

/** @brief Feeds one screen-space SPRT (textured sprite rectangle) into the native HUD layer as two triangles.
 *
 * An SPRT carries no texture page: the caller passes the draw-mode word in
 * effect, whose ABR (bits 5-6) also sets the blend of a semi-transparent
 * SPRT. Layout: word[1]=code|rgb, [2]=xy0 (x=lo16, y=hi16, signed),
 * [3]=u0|v0|clut (u at byte 0, v at byte 1, CLUT in the high half),
 * [4]=(h<<16)|w. Code bit 1 = semi-transparent; bit 0 = raw texture (the
 * modulation colour is ignored), fed as the neutral modulation 0x808080. A
 * zero width or height feeds nothing. The quad goes through
 * feed_hud_tex_quad and is counted in the engagement, semi-transparency and
 * bounding-box counters.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @param tpage The GPU draw-mode word in effect. */
static void feed_sprt_now(CPUState* cpu, uint32_t prim, unsigned tpage) {
    uint32_t w1 = cpu->read_word(prim + 0x04);
    uint32_t xy = cpu->read_word(prim + 0x08);
    uint32_t uv = cpu->read_word(prim + 0x0C);
    uint32_t wh = cpu->read_word(prim + 0x10);
    int x0 = (int16_t)(xy & 0xFFFF), y0 = (int16_t)(xy >> 16);
    int u0 = (int)(uv & 0xFFu),      v0 = (int)((uv >> 8) & 0xFFu);
    unsigned clut = (uv >> 16) & 0xFFFFu;
    int w = (int)(wh & 0xFFFFu),     h = (int)((wh >> 16) & 0xFFFFu);
    if (w <= 0 || h <= 0) return;

    int code = (int)((w1 >> 24) & 0xFFu);
    uint32_t rgba = (code & 1) ? 0xFF808080u                 /* raw texture: no modulation */
                               : ((w1 & 0xFFFFFFu) | 0xFF000000u);
    int abe = code & 2;
    /* The GPU blends a semi-transparent primitive at the draw mode's ABR rate,
     * which for an SPRT lives in the DR_MODE that set the page, not in the
     * packet. */
    const int blend = ui_blend(abe, (tpage >> 5) & 3u).blend;
    ngte_tpage tpd;
    ngte_decode_tpage((unsigned)tpage, (unsigned)clut, 0u, &tpd);
    int tpx = tpd.tpx, tpy = tpd.tpy, bpp = tpd.bpp;
    int clx = tpd.clx, cly = tpd.cly;

    float sx[4] = { (float)x0, (float)(x0 + w), (float)x0, (float)(x0 + w) };
    float sy[4] = { (float)y0, (float)y0, (float)(y0 + h), (float)(y0 + h) };
    float su[4] = { (float)u0, (float)(u0 + w), (float)u0, (float)(u0 + w) };
    float sv[4] = { (float)v0, (float)v0, (float)(v0 + h), (float)(v0 + h) };

    scene_begin_for_feed();   /* the live scene: this runs both at emission (the
                               * self-linking glyph chains) and from a released
                               * batch (a HUD sprite linked through the chokepoint) */
    static const int idx[2][3] = { {0,1,2}, {1,3,2} };
    openpete_scene_tri quad[2]; memset(quad, 0, sizeof(quad));
    for (int t = 0; t < 2; t++) {
        openpete_scene_tri* tri = &quad[t];
        for (int j = 0; j < 3; j++) { int c = idx[t][j];
            tri->v[j].sx = sx[c]; tri->v[j].sy = sy[c]; tri->v[j].z = 1.0f; tri->v[j].pvz = 1.0f;
            tri->v[j].u = su[c]; tri->v[j].v = sv[c]; tri->v[j].rgba = rgba; }
        tri->textured = 1; tri->tpage_x = tpx; tri->tpage_y = tpy;
        tri->clut_x = clx; tri->clut_y = cly; tri->bpp = bpp;
        tri->blend = blend;
    }
    const int split = feed_hud_tex_quad(quad);
    if (abe) semi_note(SEMI_SPRT, (tpage >> 5) & 3u, split);
    bbox_note(UICLS_SPRT, sx[0], sy[0], sx[3], sy[3]);
    s_fed_prims++; s_fed_sprts++;
}

/* Unclaimed 2D primitives, by GP0 command byte. The chain below claims a
 * closed list of encodings; anything else is linked for the reference and
 * never fed, so it is missing from the native frame. This histogram names the
 * codes the game actually links that the chain does not decode, the
 * engagement counter for adding a decoder (nonzero before, zero after).
 * Counted always (one increment); printed per tick under
 * OPENPETE_UISPRITE_CENSUS, the tick's line emitted at the first insert of
 * the next tick, so a line holds a whole tick's unclaimed prims. */
static unsigned s_unclaimed[256];        /* since boot */
static unsigned s_unclaimed_tick[256];   /* current tick */
static unsigned s_unclaimed_frame = 0xFFFFFFFFu;
unsigned ui_sprite_unclaimed_count(void) {
    unsigned n = 0; for (int c = 0; c < 256; c++) n += s_unclaimed[c]; return n;
}
/** @brief Prints the finished tick's UI-UNCLAIMED census line and clears the per-tick histogram.
 *
 * Active under OPENPETE_UISPRITE_CENSUS and after a tick has been counted;
 * a tick with no unclaimed primitive prints nothing. */
static void unclaimed_flush_tick(void) {
    static int cen = -1;
    ENV_FLAG_LATCH(cen, "OPENPETE_UISPRITE_CENSUS");
    if (!cen || s_unclaimed_frame == 0xFFFFFFFFu) return;
    char line[512]; int o = 0; int any = 0;
    for (int c = 0; c < 256 && o < (int)sizeof(line) - 24; c++) {
        if (!s_unclaimed_tick[c]) continue;
        any = 1;
        o += snprintf(line + o, sizeof(line) - (size_t)o, " %02X=%u", c, s_unclaimed_tick[c]);
    }
    if (any)
        fprintf(stderr, "[census f=%u UI-UNCLAIMED]%s\n", s_unclaimed_frame, line);
    memset(s_unclaimed_tick, 0, sizeof(s_unclaimed_tick));
}
/** @brief Counts a linked primitive whose GP0 command byte no decoder claims.
 *
 * Reads the command byte from the high byte of the primitive's second
 * word. An address below 0x80000000 is ignored.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address. */
static void unclaimed_note(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return;
    unsigned code = (unsigned)((cpu->read_word(prim + 4) >> 24) & 0xFFu);
    s_unclaimed[code]++; s_unclaimed_tick[code]++;
}

/* ---- dispatch ------------------------------------------------------------
 * A 2D primitive's words are NOT final when it is linked. Two callers rewrite
 * a linked primitive: a title-screen sprite draw mirrors a sprite by swapping
 * its U coords right after the insert returns (the right-hand memory-card
 * border), and the atlas page turn skews a whole page of already-linked quads
 * a tick after linking them (the quads carry the turning sheet; read at link
 * time every one of them is still the flat page). The words ARE final when the
 * ordering table holding them is handed to the GPU.
 *
 * So the chokepoint claims each primitive (records its address and kind) and
 * the batch is fed at its release, in link order. The release is the
 * canonical capture of the VBL that built the batch (on_overlay_precapture),
 * else the scene's frame close (on_scene_frame_close), else the OT walk
 * (`ui_sprite_ot_draw_begin`), whichever comes first; ui_sprite_ot_verify
 * re-reads the released primitives at the walk to count words that changed
 * after their release. The primitives linked between two DrawOTag calls are
 * one ordering table's worth. Feeding in link order and then ranking every 2D
 * triangle by the table traversal (build_ot_rank_map) keeps the native
 * layering equal to the OT walk order. The chokepoint is the clean 2D-vs-3D discriminator (the 3D
 * renderers inline the insert). */
enum { QK_FT4 = 0, QK_FREEZE, QK_DR_MODE, QK_F4_BAR, QK_F4_PANEL, QK_LINE_G2, QK_SPRT, QK_POLY_G, QK_LINE_F2 };
typedef struct { uint32_t addr; uint32_t sum; uint32_t seq; uint32_t aux; uint32_t xy_link; uint32_t xy_rel; uint8_t kind; } ui_claim_t;

/* One drawn tick's 2D. Spyro 3's atlas links ~370 quads per tick, the widest
 * screen in either game; the cap is well clear of that and an overflow is
 * counted rather than silently dropped. */
#define UI_CLAIM_MAX 2048
static ui_claim_t s_claim[UI_CLAIM_MAX];
static unsigned s_claim_n;            /* claimed, awaiting release */
static unsigned s_claim_frame;        /* openpete_frame the batch's first claim was made in */
static unsigned s_claim_lost;         /* claims dropped on overflow (engagement: 0) */
static int      s_ot_ranked;          /* 2D tris the table walk ranked at the last release: the
                                       * engagement counter for the traversal rank (0 means the
                                       * walk reached none of this frame's 2D) */
static unsigned s_claim_late;         /* batches released at the frame close rather than at the
                                       * ordering-table walk (engagement counter for that hook) */
/** @brief Prints and resets the counts of 2D emissions that arrived after their VBL's canonical capture.
 *
 * The capture is the release point on the assumption that it follows the
 * VBL's last 2D emission; s_post_claim, s_post_sprt and s_post_fade count
 * chokepoint claims, glyph feeds and fade emissions stamped with the VBL of
 * the batch the capture released (s_cap_frame). An emission stamped with a
 * later VBL belongs to the next batch and is not counted. Under
 * OPENPETE_UISPRITE_TRACE one line is printed per capture while s_cap_frame
 * is set, zeros included. */
static void post_capture_report(void) {
    static int tr = -1;
    ENV_FLAG_LATCH(tr, "OPENPETE_UISPRITE_TRACE");
    if (tr && s_cap_frame != 0xFFFFFFFFu)
        fprintf(stderr, "[uipost built=%u] claim=%u sprt=%u fade=%u reopen=%u\n",
                s_cap_frame, s_post_claim, s_post_sprt, s_post_fade, s_presented_reopen);
    s_post_claim = s_post_sprt = s_post_fade = 0;
}

static unsigned s_claim_at_capture;   /* batches released at their own frame's canonical capture:
                                       * the in-phase release, and the one that should carry
                                       * every batch on a frame that composes an image */
/* Verification read. A batch is released inside its own frame, before the game
 * has handed the ordering table holding it to the GPU. The walk is where the
 * words are beyond doubt final, so the released batch is re-read there and
 * compared with what was fed: `s_verify_stale` counts primitives whose words
 * changed after the release (must be 0, a nonzero count means a store lands
 * after the capture and the release point is too early), against
 * `s_verify_n` primitives checked. */
static ui_claim_t s_verify[UI_CLAIM_MAX];
static unsigned s_verify_pending;     /* entries awaiting their walk */
static unsigned s_verify_n, s_verify_stale;
static unsigned s_claim_spec_refused; /* claims and glyph feeds refused because the tick was
                                       * speculative: the engagement counter for that guard
                                       * (0 under --runahead means it never engaged) */
static unsigned s_mutated;            /* primitives whose words changed between the claim
                                       * and the flush: the deferral's engagement counter
                                       * (0 means no caller ever rewrites a linked prim
                                       * and the deferral is buying nothing) */
static CPUState* s_cpu = NULL;        /* the game CPU (stable), for the OT-walk flush */
static int      s_in_fade = 0;        /* set across the fade emitter's real-function call so
                                       * the chokepoint hook can tell the fade's 0x2A F4 from
                                       * a dark panel's (both go through the chokepoint) */
static int      s_in_grid = 0;        /* set across the framebuffer grid emitter's real call
                                       * (ui_game.fb_grid_emitter) */
static unsigned s_grid_ord;           /* primitives linked through the chokepoint since that
                                       * call began: the emission ordinal */
static int      s_last_abr = 0;       /* semi-transparency rate (ABR) of the most recent
                                       * DR_MODE seen - the GPU draw-mode in effect for the
                                       * next semi-transparent F4 (the game links a box's
                                       * DR_MODE immediately before its 0x2A F4). PSX ABR:
                                       * 0 (B+F)/2, 1 B+F, 2 B-F, 3 B+F/4. */
static unsigned s_last_tpage = 0u;    /* the whole draw-mode word of that same DR_MODE. An
                                       * SPRT carries no texture page, so the page the GPU
                                       * samples it through is the one the last draw mode
                                       * set. */

/** @brief Folds the words a feeder reads from a primitive into one FNV-1a value.
 *
 * The release and the verify pass compare it against the value taken at the
 * claim to tell a rewritten primitive from an untouched one without keeping
 * every word. Words 1..9 cover the longest encoding claimed here (POLY_FT4:
 * the code and colour word and the four xy/uv pairs). Of word 0 only the top
 * byte, the tag length the decode chain reads, is folded in: its low 24 bits
 * are the ordering table's link pointer, rewritten whenever another
 * primitive is linked in front of this one, which changes nothing drawn.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return The folded value. */
static uint32_t claim_sum(CPUState* cpu, uint32_t prim) {
    uint32_t h = 2166136261u;
    h = (h ^ (cpu->read_word(prim) >> 24)) * 16777619u;
    for (int k = 1; k < 10; k++) h = (h ^ cpu->read_word(prim + (uint32_t)k * 4u)) * 16777619u;
    return h;
}

/** @brief Tests whether a primitive is a POLY_FT4 (code 0x2C or 0x2E) with a full tag.
 *
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 for code 0x2C or 0x2E with a tag length of at least 9, else 0
 *         (also for an address below 0x80000000). */
static int is_ft4(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return ((uint8_t)(w1 >> 24) & 0xFD) == 0x2C   /* 0x2C / 0x2E = POLY_FT4 */
        && (uint8_t)(w0 >> 24) >= 9;              /* tag length >= 9 (full FT4) */
}

/** @brief Tests whether a primitive is an opaque POLY_F4 (code 0x28) with a full tag.
 *
 * The cutscene letterbox bars and menu-box backings; feed_one_claim tells
 * them apart by width.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 for code 0x28 with a tag length of at least 5, else 0 (also for
 *         an address below 0x80000000). */
static int is_f4_opaque(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return (uint8_t)(w1 >> 24) == 0x28            /* POLY_F4 opaque */
        && (uint8_t)(w0 >> 24) >= 5;              /* tag length >= 5 (full F4) */
}

/** @brief Tests whether a primitive is a semi-transparent POLY_F4 (code 0x2A) with a full tag.
 *
 * A dark panel (the pause or inventory panel, the HUD shaded box, a
 * dialog-box slab) or the full-screen fade, which share the code. The test
 * does not tell them apart: the chokepoint claims a match only while
 * s_in_fade is clear, since on_fade_emitter sets it across the fade
 * emitter's own call and feeds the fade from its arguments.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 for code 0x2A with a tag length of at least 5, else 0. */
static int is_f4_semitrans(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return (uint8_t)(w1 >> 24) == 0x2A            /* POLY_F4 semi-transparent */
        && (uint8_t)(w0 >> 24) >= 5;              /* tag length >= 5 (full F4) */
}

/** @brief Tests whether a primitive is a DR_MODE or DR_TPAGE (GP0 command 0xE1).
 *
 * The command sets the GPU draw mode, including the semi-transparency rate
 * (ABR) in bits 5-6 of the tpage word (getTPage packs abr as
 * ((abr&3)<<5)). code[0] is at +4 (the tag occupies +0), so the GP0 byte
 * and the ABR are in read_word(prim+4). feed_one_claim mirrors the word so
 * that a later semi-transparent 2D primitive or SPRT of the batch reads the
 * game's own draw mode.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 when word 1's top byte is 0xE1, else 0. */
static int is_dr_mode(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    return (uint8_t)(cpu->read_word(prim + 4) >> 24) == 0xE1;   /* code[0] = GP0 0xE1 */
}

/** @brief Tests whether a primitive is one of the freeze-frame backdrop POLY_FT4s.
 *
 * The pause, inventory and flight-results screens re-display the frozen
 * image desaturated, as POLY_FT4s sampling a VRAM scratch copy with a flat
 * tint and a grayscale CLUT at a fixed VRAM position. The tint and CLUT pair
 * (ui_game.freeze_tint, freeze_clut_x, freeze_clut_y) is the signature; the
 * title logo samples the same page but with a white tint and its own CLUT.
 * A match is claimed as QK_FREEZE, which drives the native freeze backdrop
 * from the native renderer's own last world image instead of feeding the
 * quad.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 for a full-tag POLY_FT4 (0x2C or 0x2E) with the freeze tint and
 *         CLUT, else 0 (always 0 when ui_game.freeze_tint is 0). */
static int is_freeze_backdrop(CPUState* cpu, uint32_t prim) {
    if (!ui_game.freeze_tint) return 0;                   /* no such quad on this game */
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    if (((uint8_t)(w1 >> 24) & 0xFD) != 0x2C) return 0;   /* POLY_FT4 (0x2C/0x2E) */
    if ((uint8_t)(w0 >> 24) < 9) return 0;                /* full FT4 tag */
    if ((w1 & 0xFFFFFFu) != ui_game.freeze_tint) return 0;/* freeze tint (B,G,R) */
    uint32_t clut = (cpu->read_word(prim + 0x0C) >> 16) & 0xFFFFu;
    int clx = (int)(clut & 0x3Fu) * 16, cly = (int)((clut >> 6) & 0x1FFu);
    return clx == ui_game.freeze_clut_x && cly == ui_game.freeze_clut_y;
}

/* The band the freeze-backdrop FT4s linked this VBL cover: x from the leftmost
 * quad's left edge to the rightmost's right edge, and the y of their bottom
 * vertices. */
static int is_line_f2(CPUState* cpu, uint32_t prim);
static struct { unsigned frame; int valid; int x0, x1, y; } s_freeze_band;
static unsigned s_freeze_mask_dropped;   /* engagement count, UI-SPRITE census */

/** @brief Extends the current VBL's freeze-backdrop band with one FT4's horizontal extent.
 *
 * The first quad of a VBL sets the band to its vertex 0 x, vertex 3 x and
 * vertex 3 y; later quads only widen x0 and x1, and y keeps the first
 * quad's value.
 * @param cpu The guest CPU state.
 * @param prim The POLY_FT4's guest address. */
static void freeze_band_note(CPUState* cpu, uint32_t prim) {
    const uint32_t xy0 = cpu->read_word(prim + 0x08), xy3 = cpu->read_word(prim + 0x20);
    const int x0 = (int16_t)xy0, x1 = (int16_t)xy3, y = (int16_t)(xy3 >> 16);
    if (!s_freeze_band.valid || s_freeze_band.frame != openpete_frame) {
        s_freeze_band.frame = openpete_frame; s_freeze_band.valid = 1;
        s_freeze_band.x0 = x0; s_freeze_band.x1 = x1; s_freeze_band.y = y;
        return;
    }
    if (x0 < s_freeze_band.x0) s_freeze_band.x0 = x0;
    if (x1 > s_freeze_band.x1) s_freeze_band.x1 = x1;
}

/** @brief Tests whether a primitive is the black LINE_F2 the pause renderer links under its freeze-backdrop FT4s.
 *
 * The line spans the band the VBL's backdrop FT4s cover (freeze_band_note),
 * in either direction, on the band's bottom row. A quad leaves its bottom
 * row unfilled, so the FT4s stop one row short of their bottom vertices and
 * the line covers that row. The native backdrop is the renderer's own
 * frozen image over the whole display rect, which fills that row at every
 * aspect, so the chokepoint drops the line with the FT4s it pairs with.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 for a black LINE_F2 on the current VBL's band, else 0. */
static int is_freeze_mask_line(CPUState* cpu, uint32_t prim) {
    if (!s_freeze_band.valid || s_freeze_band.frame != openpete_frame) return 0;
    if (!is_line_f2(cpu, prim)) return 0;
    if ((cpu->read_word(prim + 4) & 0xFFFFFFu) != 0u) return 0;          /* black */
    const uint32_t xy0 = cpu->read_word(prim + 0x08), xy1 = cpu->read_word(prim + 0x0C);
    const int ya = (int16_t)(xy0 >> 16), yb = (int16_t)(xy1 >> 16);
    const int xa = (int16_t)xy0, xb = (int16_t)xy1;
    return ya == s_freeze_band.y && yb == s_freeze_band.y &&
           ((xa == s_freeze_band.x0 && xb == s_freeze_band.x1) ||
            (xb == s_freeze_band.x0 && xa == s_freeze_band.x1));
}

/** @brief Tests whether a primitive is an SPRT (screen-space textured sprite, codes 0x64-0x67) with a full tag.
 *
 * A HUD icon or digit linked one primitive at a time through the
 * chokepoint, as opposed to the glyph chains that link themselves and are
 * fed by text_sprt.c. The packet carries no texture page: feed_one_claim
 * passes the word of the last DR_MODE it mirrored, which may come from an
 * earlier batch when none precedes the SPRT in its own.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 for codes 0x64-0x67 with a tag length of at least 4, else 0. */
static int is_sprt(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return ((uint8_t)(w1 >> 24) & 0xFCu) == 0x64u   /* SPRT, any abe/raw combination */
        && (uint8_t)(w0 >> 24) >= 4;                /* tag length >= 4 (full SPRT) */
}

/** @brief Tests whether a primitive is an opaque LINE_G2 (gouraud two-point line, code 0x50) with a full tag.
 *
 * UI separator lines and the dialog name underline. A particle streak line
 * is linked by the particle path, not the chokepoint, so it never reaches
 * this test.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 for code 0x50 with a tag length of at least 4, else 0. */
static int is_line_g2(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return (uint8_t)(w1 >> 24) == 0x50            /* LINE_G2 */
        && (uint8_t)(w0 >> 24) >= 4;              /* tag length >= 4 (full LINE_G2) */
}

/** @brief Tests whether a primitive is an untextured POLY_G3 or POLY_G4 with a full tag.
 *
 * Codes 0x30-0x33 and 0x38-0x3B: bit 3 selects the quad, bit 1
 * semi-transparency, and bit 2 (texturing) is excluded. The gouraud fill of
 * a health bar.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 on a match with a tag length of at least 8 (quad) or 6
 *         (triangle), else 0. */
static int is_poly_g(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    const unsigned code = (uint8_t)(w1 >> 24);
    return (code & 0xF4u) == 0x30u
        && (uint8_t)(w0 >> 24) >= ((code & 0x08u) ? 8 : 6);   /* full G4 / G3 tag */
}

/** @brief Tests whether a primitive is a LINE_F2 (flat two-point line) with a full tag.
 *
 * The mask 0xFE admits codes 0x40 and 0x41; the semi-transparent 0x42 does
 * not match. A health bar's border and the freeze mask line.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 for codes 0x40-0x41 with a tag length of at least 3, else 0. */
static int is_line_f2(CPUState* cpu, uint32_t prim) {
    if (prim < 0x80000000u) return 0;
    uint32_t w0 = cpu->read_word(prim), w1 = cpu->read_word(prim + 4);
    return ((uint8_t)(w1 >> 24) & 0xFEu) == 0x40u
        && (uint8_t)(w0 >> 24) >= 3;              /* tag length >= 3 (full LINE_F2) */
}


/* The ordering table's own traversal, as an address-to-rank map.
 *
 * The GPU draws a 2D primitive when the table walk reaches it, and the walk is
 * a flat singly-linked list: each tag's low 24 bits point at the next
 * primitive, 0x00FFFFFF ends it. AddPrim links a primitive at the head of its
 * entry, so within an entry the last-linked primitive is drawn first, the
 * order a primitive is emitted in is not the order it is drawn in, and no
 * counter kept at emission can stand in for the walk. Reproduce the walk itself
 * (the game's own list, not a proxy for it).
 *
 * The table being fed is walked by the game a tick after this runs, so every
 * recent base in the DrawOTag ring is walked. Every 2D primitive of the scene
 * found in a chain is recorded, not just the batch's, so a glyph fed at
 * emission and a quad fed from the deferred batch rank against each other. */
#define PSYQ_OT_BASE_RING 8
#define UI_OT_WALK_MAX 16384
/* PSX main RAM. A tag's next pointer is 24 bits of a word the game is still
 * writing when this runs, so it can name anything; only an address that is a
 * word inside RAM may be followed. */
#define UI_RAM_LO 0x80000000u
#define UI_RAM_HI 0x801FFFFCu
/** @brief Tests whether an address is a word-aligned address inside main RAM.
 *
 * @param a The guest address.
 * @return 1 for 0x80000000..0x801FFFFC at a multiple of 4, else 0. */
static int ui_ram_word(uint32_t a) { return a >= UI_RAM_LO && a <= UI_RAM_HI && !(a & 3u); }

static uint32_t s_walk_addr[UI_OT_WALK_MAX];   /* sorted by address */
static uint32_t s_walk_rank[UI_OT_WALK_MAX];   /* traversal position of that address */
static int      s_walk_n;
static int      s_walk_hits;                   /* tris the map ranked at the last release:
                                                * the engagement counter (0 = the walk found
                                                * none of this frame's 2D) */

/** @brief Binary-searches a sorted address list for one primitive address.
 *
 * build_ot_rank_map ranks only the addresses the scene's 2D was decoded
 * from, and counts a chain's hits for the ring probe.
 * @param known The primitive addresses, sorted ascending.
 * @param kn The count of addresses.
 * @param a The address sought.
 * @return 1 when present, else 0. */
static int known_has(const uint32_t* known, int kn, uint32_t a) {
    int lo = 0, hi = kn - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2;
        if (known[mid] == a) return 1;
        if (known[mid] < a) lo = mid + 1; else hi = mid - 1; }
    return 0;
}

/** @brief Builds the address-to-rank map of the scene's 2D primitives from the recent ordering-table chains.
 *
 * Walks every recent DrawOTag base (psyq_ot_recent_base) in issue order,
 * oldest first, following each tag's 24-bit next pointer while it names a
 * word in main RAM, up to UI_OT_WALK_MAX steps. Each known address met is
 * given the next rank, 1-based, on its first sighting; an address seen in an
 * earlier chain keeps that rank. The union is taken because the game may
 * split its 2D across more than one table, and a single best chain would
 * leave the others' primitives unranked. The map is then sorted by address
 * for ot_rank_of. OPENPETE_OT_RING_PROBE prints each chain's length and hits.
 * @param cpu The guest CPU state.
 * @param known The scene's 2D primitive addresses, sorted ascending.
 * @param kn The count of known addresses. */
static void build_ot_rank_map(CPUState* cpu, const uint32_t* known, int kn) {
    s_walk_n = 0;
    static int pz = -1;
    ENV_FLAG_LATCH(pz, "OPENPETE_OT_RING_PROBE");
    for (int b = PSYQ_OT_BASE_RING - 1; b >= 0; b--) {      /* issue order */
        uint32_t base = psyq_ot_recent_base(b);
        if (!ui_ram_word(base)) continue;
        int len = 0, score = 0, added = 0;
        uint32_t addr = base;
        for (int step = 0; step < UI_OT_WALK_MAX && ui_ram_word(addr); step++) {
            len++;
            if (known_has(known, kn, addr)) {
                score++;
                if (s_walk_n < UI_OT_WALK_MAX) {            /* first chain to hold it wins */
                    int dup = 0;
                    for (int i = 0; i < s_walk_n && !dup; i++) if (s_walk_addr[i] == addr) dup = 1;
                    if (!dup) { s_walk_addr[s_walk_n] = addr;
                                s_walk_rank[s_walk_n] = (uint32_t)(s_walk_n + 1); s_walk_n++; added++; }
                }
            }
            uint32_t next = cpu->read_word(addr) & 0x00FFFFFFu;
            if (next == 0x00FFFFFFu) break;
            addr = next | 0x80000000u;
            if (!ui_ram_word(addr)) break;
        }
        if (pz) fprintf(stderr, "[otring f=%u] slot=%d base=%08X len=%d hits=%d added=%d\n",
                        openpete_frame, b, base, len, score, added);
    }
    for (int i = 1; i < s_walk_n; i++) {          /* sort by address for the lookup */
        uint32_t a = s_walk_addr[i], r = s_walk_rank[i];
        int j = i - 1;
        while (j >= 0 && s_walk_addr[j] > a) { s_walk_addr[j+1] = s_walk_addr[j];
                                               s_walk_rank[j+1] = s_walk_rank[j]; j--; }
        s_walk_addr[j+1] = a; s_walk_rank[j+1] = r;
    }
    if (pz) fprintf(stderr, "[otring f=%u] union=%d of known=%d\n", openpete_frame, s_walk_n, kn);
}

/* The link rank (ui_game.rank_by_link): the order the game's own stitch
 * draws its 2D in, from where each primitive was linked rather than from the
 * table. A ring of the most recent links, oldest overwritten; the scene's 2D
 * of one VBL is a few hundred links, far inside it. Each link carries the
 * table period it was made in (the stitches before it): a scene can hold 2D
 * linked on both sides of a stitch, which the game draws as two successive
 * tables, the earlier one first. */
#define UI_LINK_RING 16384
typedef struct { uint32_t addr; int32_t slot; uint64_t period; uint64_t ord; } ui_link_t;
static ui_link_t s_link[UI_LINK_RING];
static uint64_t  s_link_n;          /* links recorded since boot */
static uint64_t  s_link_period;     /* stitches since boot */

void ui_sprite_link_note(uint32_t prim, int slot) {
    if (!ui_game.rank_by_link) return;
    ui_link_t* l = &s_link[s_link_n % UI_LINK_RING];
    l->addr = prim; l->slot = slot;
    l->period = s_link_period;
    l->ord = s_link_n++;
}

uint64_t ui_sprite_link_count(void) { return s_link_n; }

/** @brief Reports whether the 2D is ranked by link (ui_game.rank_by_link) rather than by the DrawOTag walk.
 *
 * OPENPETE_UI_RANK_WALK, read once, forces the walk on a link-rank game:
 * the in-binary A/B of the two ranks.
 * @return 1 for the link rank, else 0. */
static int rank_by_link_on(void) {
    static int walk = -1;
    ENV_FLAG_LATCH(walk, "OPENPETE_UI_RANK_WALK");
    return ui_game.rank_by_link && !walk;
}

/** @brief Orders two link records by primitive address.
 *
 * @param a A ui_link_t.
 * @param b A ui_link_t.
 * @return Negative, zero or positive as @p a's address is below, equal to or above @p b's. */
static int link_addr_cmp(const void* a, const void* b) {
    const uint32_t x = ((const ui_link_t*)a)->addr, y = ((const ui_link_t*)b)->addr;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/** @brief Orders two link records as the game draws them: table period, then slot descending, then link order.
 *
 * @param a A ui_link_t.
 * @param b A ui_link_t.
 * @return Negative when @p a draws first, positive when @p b does. */
static int link_draw_cmp(const void* a, const void* b) {
    const ui_link_t* x = (const ui_link_t*)a;
    const ui_link_t* y = (const ui_link_t*)b;
    if (x->period != y->period) return x->period < y->period ? -1 : 1;
    if (x->slot != y->slot) return x->slot > y->slot ? -1 : 1;
    return x->ord < y->ord ? -1 : (x->ord > y->ord ? 1 : 0);
}

/** @brief Builds the address-to-rank map of the scene's 2D primitives from their recorded links.
 *
 * Each known address takes its newest link in the ring. The linked ones are
 * sorted by link_draw_cmp and ranked 1.. in that order: table periods in
 * turn, and within one the order the game's stitch appends the slots
 * (highest first, the 2D entry last) with each slot's links first-in
 * first-out.
 * Reads no guest RAM. The map is left sorted by address for ot_rank_of; an
 * address with no link is left out and ranks 0.
 * @param known The scene's 2D primitive addresses, sorted ascending.
 * @param kn The count of known addresses. */
static void build_link_rank_map(const uint32_t* known, int kn) {
    static ui_link_t got[UI_OT_WALK_MAX];
    static ui_link_t sorted[UI_OT_WALK_MAX];
    int un = 0;
    for (int i = 0; i < kn && un < UI_OT_WALK_MAX; i++)      /* distinct addresses */
        if (!un || got[un - 1].addr != known[i]) { got[un].addr = known[i]; got[un].ord = UINT64_MAX; un++; }
    const uint64_t n = s_link_n < UI_LINK_RING ? s_link_n : UI_LINK_RING;
    for (uint64_t k = s_link_n - n; k < s_link_n; k++) {     /* oldest first: the newest link wins */
        const ui_link_t* l = &s_link[k % UI_LINK_RING];
        int lo = 0, hi = un - 1;
        while (lo <= hi) { int mid = (lo + hi) / 2;
            if (got[mid].addr == l->addr) { got[mid] = *l; break; }
            if (got[mid].addr < l->addr) lo = mid + 1; else hi = mid - 1; }
    }
    int sn = 0;
    for (int i = 0; i < un; i++) if (got[i].ord != UINT64_MAX) sorted[sn++] = got[i];
    qsort(sorted, (size_t)sn, sizeof sorted[0], link_draw_cmp);
    for (int i = 0; i < sn; i++) sorted[i].ord = (uint64_t)(i + 1);   /* the rank */
    qsort(sorted, (size_t)sn, sizeof sorted[0], link_addr_cmp);
    for (int i = 0; i < sn; i++) { s_walk_addr[i] = sorted[i].addr; s_walk_rank[i] = (uint32_t)sorted[i].ord; }
    s_walk_n = sn;
}

/** @brief Looks up a primitive's traversal rank in the ordering-table map without counting a hit.
 *
 * @param prim The primitive's guest address.
 * @return The 1-based rank, or 0 when the address is not in the map. */
static uint32_t ot_rank_of_quiet(uint32_t prim) {
    int lo = 0, hi = s_walk_n - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2;
        if (s_walk_addr[mid] == prim) return s_walk_rank[mid];
        if (s_walk_addr[mid] < prim) lo = mid + 1; else hi = mid - 1; }
    return 0u;
}

/** @brief Looks up a primitive's traversal rank in the ordering-table map and counts the hit in s_walk_hits.
 *
 * Passed to openpete_native_scene_rank_2d.
 * @param prim The primitive's guest address.
 * @return The 1-based rank, or 0 when the address is not in the map. */
static uint32_t ot_rank_of(uint32_t prim) {
    int lo = 0, hi = s_walk_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_walk_addr[mid] == prim) { s_walk_hits++; return s_walk_rank[mid]; }
        if (s_walk_addr[mid] < prim) lo = mid + 1; else hi = mid - 1;
    }
    return 0u;   /* not in the chain: keeps its feed position under the stable sort */
}

/** @brief Reports whether an opaque POLY_F4 spans the full 512-pixel width of the 2D layer.
 *
 * The four XY words at +0x08..+0x14 hold x in the low signed halfword. A
 * letterbox bar is authored across the full 0..512 width; a menu box's
 * backing covers part of it.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @return 1 when the x extent is at least UI_FRAME_W, else 0. */
static int f4_spans_frame_width(CPUState* cpu, uint32_t prim) {
    int lo = 0x7FFF, hi = -0x8000;
    for (uint32_t k = 0; k < 4u; k++) {
        int x = (int16_t)(cpu->read_word(prim + 0x08u + k * 4u) & 0xFFFFu);
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    return hi - lo >= UI_FRAME_W;
}

/** @brief Decodes one claimed 2D primitive by its claim kind and feeds it into the native scene.
 *
 * QK_FT4, QK_SPRT, QK_LINE_G2, QK_POLY_G and QK_LINE_F2 go to their
 * decoders; an SPRT takes the draw-mode word last mirrored from a QK_DR_MODE
 * claim. QK_FREEZE drives the native freeze backdrop at the FT4's tint and
 * feeds no quad. QK_DR_MODE mirrors the ABR and draw-mode word for the
 * claims after it. QK_F4_BAR feeds a frame-width quad as a letterbox bar on
 * the overlay layer and a narrower one, or a framebuffer grid border of any
 * width, as a menu-box backing on the panel layer, stamped with the
 * framebuffer cell key in its auxiliary word;
 * QK_F4_PANEL feeds a dark panel on the panel layer at the tracked ABR.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @param kind The claim kind (QK_*).
 * @param aux The claim's auxiliary word: for QK_F4_BAR the framebuffer cell
 *        key of a grid border (grid_elem_key), else 0. */
static void feed_one_claim(CPUState* cpu, uint32_t prim, int kind, uint32_t aux) {
    const uint32_t w1 = cpu->read_word(prim + 4);
    switch (kind) {
    case QK_FT4:    feed_ft4(cpu, prim, w1); break;
    case QK_FREEZE:
        /* Pause / inventory / flight-results green overlay: drive the native
         * freeze backdrop from our own last world frame and drop this FT4 (it
         * samples the emulated vram scratch). Tint = the FT4's setRGB0 (B,G,R
         * packed in w1), r=lo byte, g=mid, b=hi. */
        scene_begin_for_feed();
        {   /* the quad paints the frozen frame over what the buffer held */
            float x[4], y[4];
            for (int k = 0; k < 4; k++) {
                const uint32_t xy = cpu->read_word(prim + 0x08u + 8u * (uint32_t)k);
                x[k] = (float)(int16_t)xy; y[k] = (float)(int16_t)(xy >> 16);
            }
            openpete_native_scene_note_cover_quad(x, y);
        }
        openpete_native_scene_set_freeze_backdrop((int)(w1 & 0xFFu),
                              (int)((w1 >> 8) & 0xFFu),
                              (int)((w1 >> 16) & 0xFFu),
                              0);   /* static: no fade-to-black */
        s_fed_prims++;
        break;
    case QK_DR_MODE:                                             /* the GPU draw mode */
        s_last_abr = (int)((w1 >> 5) & 3u); s_last_tpage = w1 & 0x7FFu; break;
    case QK_F4_BAR: {
        /* A frame-width opaque quad is a cutscene letterbox bar: it draws over
         * everything and takes the widescreen widen. A narrower one backs a
         * menu box, which the game links FIRST so the box's tiles, frame and
         * text draw on top of it. The box's neighbours are textured tris on
         * the HUD layer, and the HUD draw paints its whole untextured range
         * after its whole textured one, so an untextured quad fed there covers
         * them whatever the ordering table says. The panel layer is drawn
         * before both and is where a 2D box's backing belongs. The quad keeps
         * blend class 0: the panel layer routes on the category alone, and
         * class 1 would take the intake's ABR 0 half alpha. */
        /* A border the framebuffer grid emitter linked carries its element
         * key (the claim's aux word, grid_elem_key), so the carry moves it
         * with the grid's cells between ticks. Such a border is the scaled
         * window's edge, not a letterbox bar, even on the first shrink step
         * where it spans x 0..512: it feeds the panel layer unwidened, inside
         * the 4:3 window, on every step. */
        const int full = !aux && f4_spans_frame_width(cpu, prim);
        openpete_native_scene_set_fb_cell(aux);
        if (full) feed_f4(cpu, prim, w1, OPENPETE_SCENE_OVERLAY, 0xFFu, 0, 1);
        else      feed_f4(cpu, prim, w1, OPENPETE_SCENE_HUD_PANEL, 0xFFu, 0, 0);
        openpete_native_scene_set_fb_cell(0u);
        break; }
    case QK_F4_PANEL: {
        /* The box links its own DR_MODE immediately before the quad, so the
         * ABR tracked at this point of the batch is the blend the GPU is in
         * when the quad draws. */
        semi_note(SEMI_F4, (unsigned)s_last_abr, 0);
        const ui_blend_t b = ui_blend(1, (unsigned)s_last_abr);
        feed_f4(cpu, prim, w1, OPENPETE_SCENE_HUD_PANEL, b.alpha, b.blend, 0);
        break; }
    case QK_SPRT:    feed_sprt_now(cpu, prim, s_last_tpage); break;
    case QK_LINE_G2: feed_line_g2(cpu, prim, w1); break;
    case QK_POLY_G:  feed_poly_g(cpu, prim, w1); break;
    case QK_LINE_F2: feed_line_f2(cpu, prim, w1); break;
    default: break;
    }
}

/** @brief Ranks every 2D triangle of the scene by the draw order of the primitive it was decoded from.
 *
 * The scene's 2D primitive addresses, the released batch's and the glyphs
 * fed at emission alike, are sorted and mapped to ranks once: by
 * build_link_rank_map on a link-rank game (rank_by_link_on), by
 * build_ot_rank_map otherwise; openpete_native_scene_rank_2d then stamps
 * them, and the hits are kept in s_ot_ranked. OPENPETE_OT_RING_PROBE prints
 * each unranked primitive, OPENPETE_UISPRITE_TRACE the map size and hits.
 * @param cpu The guest CPU state. */
static void rank_scene_2d(CPUState* cpu) {
    s_walk_hits = 0;
    { static uint32_t known[UI_CLAIM_MAX * 2];
      int kn = openpete_native_scene_collect_2d_prims(known, (int)(sizeof known / sizeof known[0]));
      for (int i = 1; i < kn; i++) {            /* sorted for the chain scoring */
          uint32_t a = known[i]; int j = i - 1;
          while (j >= 0 && known[j] > a) { known[j+1] = known[j]; j--; }
          known[j+1] = a; }
      if (rank_by_link_on()) build_link_rank_map(known, kn);
      else build_ot_rank_map(cpu, known, kn); }
    openpete_native_scene_rank_2d(ot_rank_of);
    s_ot_ranked = s_walk_hits;
    { static int pz2 = -1; ENV_FLAG_LATCH(pz2, "OPENPETE_OT_RING_PROBE");
      if (pz2) { static uint32_t kn2[UI_CLAIM_MAX * 2];
        int k2 = openpete_native_scene_collect_2d_prims(kn2, (int)(sizeof kn2 / sizeof kn2[0]));
        for (int i = 0; i < k2; i++) if (!ot_rank_of_quiet(kn2[i]))
            fprintf(stderr, "[otmiss f=%u] prim=%08X words=%08X %08X %08X\n", openpete_frame, kn2[i],
                    cpu->read_word(kn2[i]), cpu->read_word(kn2[i]+4), cpu->read_word(kn2[i]+8)); } }
    { static int tr2 = -1; ENV_FLAG_LATCH(tr2, "OPENPETE_UISPRITE_TRACE");
      if (tr2) fprintf(stderr, "[uirank f=%u] chain=%d ranked=%d by=%s\n",
                       openpete_frame, s_walk_n, s_walk_hits,
                       rank_by_link_on() ? "link" : "walk"); }
}

/** @brief Feeds the first n claimed primitives into the native scene and ranks the scene's 2D.
 *
 * Each released primitive is re-summed (a changed sum counts in s_mutated),
 * recorded for ui_sprite_ot_verify and fed through feed_one_claim, tagged
 * with its address. Every 2D triangle in the scene, the batch's and the
 * glyphs fed at emission, is then ranked by rank_scene_2d. The unreleased
 * tail moves to the front of the claim array. With no CPU, no n or the native scene off, the n claims
 * are dropped unfed, as they are while a render-path selection leaves RP_UI
 * out. Every caller releases the whole batch.
 * @param n The count to release, clamped to the claimed count. */
static void release_batch(unsigned n) {
    CPUState* cpu = s_cpu;
    if (n > s_claim_n) n = s_claim_n;
    if (!cpu || !n || !openpete_native_scene_enabled() || !render_path_enabled(RP_UI)) {
        s_claim_n -= n;
        return;
    }
    /* Which primitives are released is decided in claim order. What order
     * they are composed in is decided after the feed, by the table walk
     * below, over every 2D tri of the scene at once. */
    static int s_tr = -1;
    ENV_FLAG_LATCH(s_tr, "OPENPETE_UISPRITE_TRACE");
    for (unsigned i = 0; i < n; i++) {
        const uint32_t prim = s_claim[i].addr;
        openpete_native_scene_set_prim(prim);   /* ranked by the table walk below */
        const uint32_t sum = claim_sum(cpu, prim);
        if (sum != s_claim[i].sum) s_mutated++;
        { unsigned vi = s_verify_pending + i;
          if (vi < UI_CLAIM_MAX) { s_verify[vi].addr = prim; s_verify[vi].sum = sum;
                                   s_verify[vi].kind = s_claim[i].kind;
                                   s_verify[vi].xy_link = s_claim[i].xy_link;
                                   s_verify[vi].xy_rel  = cpu->read_word(prim + 0x08u); } }
        const uint32_t w1 = cpu->read_word(prim + 4);
        feed_one_claim(cpu, prim, s_claim[i].kind, s_claim[i].aux);
    }
    openpete_native_scene_set_prim(0u);
    rank_scene_2d(cpu);
    /* Carry the unreleased tail to the front for its own release. */
    s_claim_n -= n;
    for (unsigned i = 0; i < s_claim_n; i++) s_claim[i] = s_claim[i + n];
    s_verify_pending += n;
    if (s_tr)
        fprintf(stderr, "[uifeed f=%u built=%u live=%u] rel=%u kept=%u mutated=%u lost=%u late=%u cap=%u fed=%u\n",
                openpete_frame, s_claim_frame, openpete_native_scene_frame(), n, s_claim_n,
                s_mutated, s_claim_lost, s_claim_late, s_claim_at_capture, s_fed_prims);
    if (s_tr)
        fprintf(stderr, "[uispec f=%u] refused=%u\n", openpete_frame, s_claim_spec_refused);
}

/* Releases every claimed primitive still pending. */
void ui_sprite_ot_draw_begin(void) { release_batch(s_claim_n); }

/** @brief Releases the pending batch when the scene of the VBL that built it closes.
 *
 * The scene is about to be snapshotted and the next started, so this is
 * the last point at which the batch reaches the scene it belongs to. It
 * fires with a batch pending only when neither the canonical capture nor
 * the ordering-table walk released it first, such as on a screen where a
 * feeder of the next VBL (a menu's HUD mobys) runs before the walk. Each
 * such release counts in s_claim_late.
 * @param frame The VBL whose scene is closing. */
static void on_scene_frame_close(unsigned frame) {
    if (s_claim_n && frame == s_claim_frame) { s_claim_late++; ui_sprite_ot_draw_begin(); }
}

/** @brief Releases the whole pending batch at the canonical capture of the VBL that built it.
 *
 * By the capture every store the game makes into the batch's primitives has
 * run (the game fills a primitive right after linking it) and the image is
 * not composed yet, so the batch is final and still in its own scene. The
 * frame close and the ordering-table walk remain as later releases for a
 * VBL that composes no image. Each release here counts in
 * s_claim_at_capture. s_cap_frame becomes the released batch's build VBL,
 * or 0xFFFFFFFF when no batch was pending, from the claim count read before
 * the release zeroes it. s_presented_scene becomes the stamp of the live
 * scene, the one this capture takes. */
static void on_overlay_precapture(void) {
    post_capture_report();
    const unsigned built = s_claim_frame;
    const unsigned pending = s_claim_n;
    /* The whole batch goes at the capture. Holding the last-linked primitive
     * back to the frame close reads its words a frame later, which for 2D that
     * animates (the cutscene letterbox sliding in) is the next tick's position,
     * which puts the bar 2 PSX rows out on every tick of the slide. */
    if (s_claim_n) { s_claim_at_capture++; release_batch(s_claim_n); }
    s_cap_frame = pending ? built : 0xFFFFFFFFu;
    s_presented_scene = openpete_native_scene_frame();
}

/* Re-sums the primitives released since the last walk against their fed sums. */
void ui_sprite_ot_verify(void) {
    CPUState* cpu = s_cpu;
    unsigned n = s_verify_pending;
    s_verify_pending = 0;
    if (!cpu || !n) return;
    static int s_tr = -1;
    ENV_FLAG_LATCH(s_tr, "OPENPETE_UISPRITE_TRACE");
    { static int pf = -2; ENV_INT_LATCH(pf, "OPENPETE_XY3_PROBE", -1);
      if (pf >= 0 && (long)openpete_frame >= (long)pf - 3 && (long)openpete_frame <= (long)pf + 3)
        for (unsigned i = 0; i < n; i++) {
            uint32_t w = cpu->read_word(s_verify[i].addr + 0x08u);
            fprintf(stderr, "[xy3 f=%u] prim=%08X kind=%u link_y=%d cap_y=%d walk_y=%d\n",
                    openpete_frame, s_verify[i].addr, s_verify[i].kind,
                    (int)(int16_t)(s_verify[i].xy_link >> 16),
                    (int)(int16_t)(s_verify[i].xy_rel  >> 16),
                    (int)(int16_t)(w >> 16)); } }
    for (unsigned i = 0; i < n; i++) {
        s_verify_n++;
        if (claim_sum(cpu, s_verify[i].addr) != s_verify[i].sum) { s_verify_stale++;
            if (s_tr) fprintf(stderr, "[uistale f=%u] i=%u/%u kind=%u p=%08X\n",
                              openpete_frame, i, n, s_verify[i].kind, s_verify[i].addr); }
    }
}

static void feed_one_claim(CPUState* cpu, uint32_t prim, int kind, uint32_t aux);

/** @brief Records a 2D primitive's address, kind and link-time words in the batch fed at the ordering-table walk.
 *
 * Refused (counted in s_claim_spec_refused) unless the canonical capture is
 * open. A claim in a later frame first releases the earlier frame's
 * batch; the first claim of a batch opens the frame's scene. A claim past
 * UI_CLAIM_MAX is counted in s_claim_lost and dropped. The record keeps a
 * checksum of the primitive's words and its xy0 word as linked.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address.
 * @param kind A QK_* kind.
 * @param aux An extra word stored with the claim. */
static void claim_aux(CPUState* cpu, uint32_t prim, int kind, uint32_t aux) {
    /* A claim is a canonical latch: it records an address whose words are read
     * back at the batch release. A speculative tick's link is rolled back
     * with the rest of guest RAM, so a claim made there names a slot that holds
     * another tick's bytes by the time the batch is released, and the deferred
     * read decodes them as a primitive. The committed tick's link is the one
     * the release can read. */
    if (!spyro_native_canon_capture_open()) { s_claim_spec_refused++; return; }
    if (openpete_frame == s_cap_frame) s_post_claim++;
    /* A batch never spans frames: the tail the capture kept back belongs to the
     * frame that built it, and by the first claim of a later frame it is final,
     * so release it before starting the new batch. Without this the tail holds
     * the batch open and the next frame's primitives inherit its build frame. */
    if (s_claim_n && openpete_frame != s_claim_frame) release_batch(s_claim_n);
    if (!s_claim_n) {
        s_claim_frame = openpete_frame;
        /* Open this frame's scene now, where a 3D feeder of the same ordering
         * table opens it, so the batch has a scene of its own build frame to be
         * appended to at its release. On a screen with no 3D feeder (a menu, the
         * atlas) nothing else would open it, and the batch would land in the
         * walk's newer scene, one drawn tick after the world it belongs to. */
        openpete_native_scene_begin_frame(openpete_frame);
    }
    if (s_claim_n >= UI_CLAIM_MAX) { s_claim_lost++; return; }
    s_claim[s_claim_n].addr = prim;
    s_claim[s_claim_n].sum  = claim_sum(cpu, prim);
    s_claim[s_claim_n].seq  = 0;                /* the traversal walk ranks it at the release */
    s_claim[s_claim_n].aux  = aux;
    s_claim[s_claim_n].xy_link = cpu->read_word(prim + 0x08u);   /* xy0 as the game linked it */
    s_claim[s_claim_n].kind = (uint8_t)kind;
    s_claim_n++;
}
/// @brief Claims a primitive with a zero auxiliary word.
static void claim(CPUState* cpu, uint32_t prim, int kind) { claim_aux(cpu, prim, kind, 0u); }

/** @brief Returns the framebuffer cell key of the primitive the grid emitter links at an emission ordinal.
 *
 * The grid emitter links the same primitives in the same order on every
 * tick of a shrink or grow (its borders first, then its cells), so the
 * emitter's address and the ordinal name one element of the grid from tick
 * to tick while its screen box changes. The bit 0 is forced so a key is
 * never 0.
 * @param ord The emission ordinal within one call of the emitter.
 * @return The key. */
static uint32_t grid_elem_key(unsigned ord) {
    uint32_t h = 2166136261u;
    h ^= ui_game.fb_grid_emitter; h *= 16777619u;
    h ^= (uint32_t)ord;           h *= 16777619u;
    return h | 1u;
}

/** @brief Opens the scene an emission feed appends to before its primitive address is set.
 *
 * Opening a scene clears the sticky primitive address, so a feed that opens
 * the VBL's scene inside its decoder would add its triangles with address 0
 * and the release could not rank them. Under OPENPETE_UI_RANK_WALK the
 * scene is left to the decoder, as the DrawOTag-walk rank always did. */
static void prim_scene_open(void) {
    if (rank_by_link_on()) scene_begin_for_feed();
}

/* Feeds one glyph SPRT at emission, tagged with its address for the release's ranking. */
void ui_sprite_feed_sprt(CPUState* cpu, uint32_t prim, unsigned tpage) {
    if (!spyro_native_canon_capture_open()) { s_claim_spec_refused++; return; }
    if (openpete_frame == s_cap_frame) s_post_sprt++;
    if (!openpete_native_scene_enabled() || !render_path_enabled(RP_UI)) return;
    if (!s_cpu) s_cpu = cpu;
    prim_scene_open();
    openpete_native_scene_set_prim(prim);   /* ranked at the release */
    feed_sprt_now(cpu, prim, tpage);
    openpete_native_scene_set_prim(0u);
}

/* Feeds one glyph POLY_FT4 at emission through the chokepoint's FT4 decoder. */
void ui_sprite_feed_ft4(CPUState* cpu, uint32_t prim) {
    if (openpete_frame == s_cap_frame) s_post_sprt++;
    if (!openpete_native_scene_enabled() || !render_path_enabled(RP_UI)) return;
    if (!s_cpu) s_cpu = cpu;
    prim_scene_open();
    openpete_native_scene_set_prim(prim);   /* ranked at the release */
    feed_ft4(cpu, prim, cpu->read_word(prim + 4));
    openpete_native_scene_set_prim(0u);
}

/** @brief Record a 2D primitive's polybuf range as RP_UI while a render-path selection is active.
 *
 * The range is the packet's tag word length, (len + 1) words, so the OT walk
 * keeps the primitive when RP_UI is selected; a zero length records nothing.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address. */
static void ui_path_record(CPUState* cpu, uint32_t prim) {
    if (!render_paths_active()) return;
    const unsigned len = (cpu->read_word(prim) >> 24) & 0xFFu;
    if (len) render_path_record(RP_UI, prim, prim + (len + 1u) * 4u);
}

/** @brief Classifies the 2D primitive being linked and claims it by kind for the batch release.
 *
 * In order: freeze backdrop FT4 (also widens the freeze band), freeze mask
 * line (counted, not claimed), FT4, draw mode, opaque F4 bar (keyed by its
 * emission ordinal inside the framebuffer grid emitter), semi-transparent
 * F4 panel (not inside the fade emitter), SPRT, LINE_G2, POLY_G, LINE_F2;
 * anything else is counted as unclaimed. Reads guest RAM only.
 * @param cpu The guest CPU state.
 * @param prim The primitive's guest address. */
static void claim_linked(CPUState* cpu, uint32_t prim) {
    const unsigned grid_ord = s_grid_ord++;   /* meaningful inside the grid emitter only */
    /* Claimed here, fed at the batch release: the primitive's words are
     * not final yet (see the dispatch header). */
    if (is_freeze_backdrop(cpu, prim)) { freeze_band_note(cpu, prim); claim(cpu, prim, QK_FREEZE); }
    else if (is_freeze_mask_line(cpu, prim)) s_freeze_mask_dropped++;   /* see is_freeze_mask_line */
    else if (is_ft4(cpu, prim))        claim(cpu, prim, QK_FT4);
    else if (is_dr_mode(cpu, prim))    claim(cpu, prim, QK_DR_MODE);   /* the GPU draw-mode's ABR */
    else if (is_f4_opaque(cpu, prim))                                   /* letterbox bars, */
        claim_aux(cpu, prim, QK_F4_BAR, s_in_grid ? grid_elem_key(grid_ord) : 0u); /* grid borders */
    else if (is_f4_semitrans(cpu, prim) && !s_in_fade)
                                       claim(cpu, prim, QK_F4_PANEL);  /* HUD shaded box / menu panel */
    else if (is_sprt(cpu, prim))       claim(cpu, prim, QK_SPRT);      /* HUD icons and digits */
    else if (is_line_g2(cpu, prim))    claim(cpu, prim, QK_LINE_G2);   /* UI separator lines */
    else if (is_poly_g(cpu, prim))     claim(cpu, prim, QK_POLY_G);    /* gouraud health-bar fills */
    else if (is_line_f2(cpu, prim))    claim(cpu, prim, QK_LINE_F2);   /* health-bar borders */
    else if (!s_in_fade) unclaimed_note(cpu, prim);   /* no decoder for this encoding: linked, not fed
                                                      * (a prim linked inside the fade emitter is the
                                                      * fade's own F4, drawn from its arguments) */
}

/** @brief Hooks the game's 2D ordering-table insert: claims the primitive being linked, then runs the real insert.
 *
 * Inert (returns 0) while the native scene is off, apart from recording the
 * primitive's range under a render-path selection, and on re-entry from its
 * own dispatch. The first call registers the frame-close and overlay
 * precapture callbacks and names the UI atlas for the texture pack. The
 * primitive at a0 is claimed by claim_linked, and its link is recorded for
 * the link rank (ui_sprite_link_note) at slot a1 for the slotted insert and
 * at the 2D entry otherwise. The real
 * insert then runs unchanged, so the reference link is intact. Reads guest
 * RAM only; writes none.
 * @param cpu The guest CPU state; a0 is the primitive pointer.
 * @param addr The hooked entry address.
 * @return 0 when the ordinary dispatch runs the insert, 1 having run it. */
static int on_ot_insert(CPUState* cpu, uint32_t addr) {
    int feed = openpete_native_scene_enabled();
    if (!feed) {                                /* inert: normal dispatch runs it */
        ui_path_record(cpu, cpu->a0);
        return 0;
    }
    if (!s_cpu) {                               /* one-time: the CPU the flush reads through */
        s_cpu = cpu;
        openpete_native_scene_register_frame_close(on_scene_frame_close);
        openpete_native_scene_register_overlay_precapture(on_overlay_precapture);
    }
    /* Name the UI texture upload (logo + PRESS START + banner glyphs, one
     * VRAM block) so a pack file of that name replaces the whole block. The
     * first 2D primitive is linked after the block is uploaded, so the
     * annotation binds to the atlas content; the level texture pages that
     * later reuse the same VRAM block hash differently and stay unnamed. */
    static int s_annotated = 0;
    if (!s_annotated && ui_game.atlas_w) {
        s_annotated = 1;
        texture_pack_annotate(ui_game.atlas_x, ui_game.atlas_y,
                              ui_game.atlas_w, ui_game.atlas_h, ui_game.atlas_name);
    }
    static int s_in = 0;
    if (s_in) return 0;                         /* re-entry from our own call -> real body */
    s_in = 1;

    uint32_t prim = cpu->a0;                     /* a0 = prim pointer being linked */
    ui_path_record(cpu, prim);
    /* The slotted insert appends to slot a1, the head insert to the 2D entry. */
    ui_sprite_link_note(prim, addr == ui_game.ot_insert_slot ? (int)cpu->a1 : -1);
    if (openpete_frame != s_unclaimed_frame) { unclaimed_flush_tick(); s_unclaimed_frame = openpete_frame; }

    claim_linked(cpu, prim);

    { OP_PROF_ACCUM_SCOPE("ui_sprite.srend");
      openpete_recomp_module_get()->dispatch(cpu, addr); }  /* the real OT insert (ref unchanged) */

    /* OPENPETE_UISPRITE_CENSUS: the per-class engagement counters, once per
     * 100 VBLs. `mutated` is how many claimed primitives the game rewrote
     * between their claim and the OT walk, the measurement the deferral exists
     * for; `lost` is claims dropped on batch overflow. */
    { static int cen = -1;
      ENV_FLAG_LATCH(cen, "OPENPETE_UISPRITE_CENSUS");
      static unsigned last = 0xFFFFFFFFu;
      if (cen && openpete_frame / 100u != last) { last = openpete_frame / 100u;
          fprintf(stderr, "[census f=%u UI-SPRITE] fed=%u sprt=%u mutated=%u lost=%u"
                          " verified=%u stale=%u abr=%d"
                          " ft4=%u bar=%u panel=%u line=%u sprt_n=%u fade=%u freeze_mask=%u\n",
                  openpete_frame, s_fed_prims, s_fed_sprts, s_mutated, s_claim_lost,
                  s_verify_n, s_verify_stale, s_last_abr,
                  s_cls_n[UICLS_FT4], s_cls_n[UICLS_BAR], s_cls_n[UICLS_PANEL],
                  s_cls_n[UICLS_LINE], s_cls_n[UICLS_SPRT], s_cls_n[UICLS_FADE],
                  s_freeze_mask_dropped); } }
    bbox_report();
    s_in = 0;
    return 1;
}

/** @brief Hooks a slotted insert that claims nothing and records its link for the link rank.
 *
 * The primitive (a0) and slot (a1) are recorded through ui_sprite_link_note
 * while the native scene is on; the ordinary dispatch then runs the insert.
 * Reads the argument registers only; writes no guest RAM.
 * @param cpu The guest CPU state; a0 is the primitive, a1 the slot.
 * @param addr The hooked entry address; unused.
 * @return 0, so the ordinary dispatch runs the insert. */
static int on_ot_rank_link(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (openpete_native_scene_enabled()) ui_sprite_link_note(cpu->a0, (int)cpu->a1);
    return 0;
}

/** @brief Hooks the game's ordering-table stitch and starts a new table period for the link rank.
 *
 * The links recorded after it belong to the next table (ui_sprite_link_note).
 * Reads no guest state; the ordinary dispatch then runs the stitch.
 * @param cpu The guest CPU state; unused.
 * @param addr The hooked entry address; unused.
 * @return 0, so the ordinary dispatch runs the stitch. */
static int on_ot_stitch(CPUState* cpu, uint32_t addr) {
    (void)cpu; (void)addr;
    s_link_period++;
    return 0;
}

/* ---- full-screen fade overlay (ui_game.fade_emitter) --------------------- */
/** @brief Hooks the game's full-screen fade emitter and drives the native fade overlay from its arguments.
 *
 * The game tints the whole screen on every transition through one emitter
 * taking (mode, r, g, b) in a0-a3, which links one semi-transparent
 * full-screen POLY_F4 whose ABR is the fade direction: mode 2 (B-F) fades to
 * black, mode 1 (B+F) to white. PsyCross renders that F4 as a half-screen
 * diagonal triangle, so the native overlay is driven from the arguments
 * (openpete_native_scene_set_fade) rather than from the F4 and its
 * preceding DR_MODE. The fade clears each VBL, so it is set whenever the
 * emitter runs, unless a render-path selection leaves RP_UI out. The real
 * emitter then runs unchanged with s_in_fade set, so
 * the chokepoint does not claim its F4 as a dark panel. Reads the
 * arguments only; writes no PSX RAM.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the native scene is off or on re-entry (the ordinary
 *         dispatch runs the emitter), 1 having run it. */
static int on_fade_emitter(CPUState* cpu, uint32_t addr) {
    if (!openpete_native_scene_enabled()) return 0;   /* inert: normal dispatch runs it */
    if (openpete_frame == s_cap_frame) s_post_fade++;
    static int s_in = 0;
    if (s_in) return 0;                                /* re-entry from our own call -> real body */
    s_in = 1;
    openpete_native_scene_begin_frame(openpete_frame); /* idempotent within a frame */
    if (render_path_enabled(RP_UI)) {                  /* the fade is the 2D layer's */
        openpete_native_scene_set_fade((int)cpu->a0, (int)cpu->a1, (int)cpu->a2, (int)cpu->a3);
        if ((int)cpu->a1 | (int)cpu->a2 | (int)cpu->a3)
            bbox_note(UICLS_FADE, 0.0f, 0.0f, 512.0f, 240.0f);
    }
    /* The real fade emit links its 0x2A F4 through the chokepoint; flag it so
     * the chokepoint hook treats that F4 as the fade (already handled) and NOT
     * a dark panel (also a 0x2A F4). */
    s_in_fade = 1;
    { OP_PROF_ACCUM_SCOPE("ui_sprite.srend");
      openpete_recomp_module_get()->dispatch(cpu, addr); }  /* the real fade emit (ref unchanged) */
    s_in_fade = 0;
    s_in = 0;
    return 1;
}

/** @brief Hooks the framebuffer grid emitter and numbers the primitives it links.
 *
 * The real emitter runs unchanged with s_in_grid set and the emission
 * ordinal s_grid_ord restarted, so the chokepoint keys each opaque F4 the
 * call links by its ordinal (grid_elem_key). Reads no guest RAM; writes none.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the native scene is off or on re-entry (the ordinary
 *         dispatch runs the emitter), 1 having run it. */
static int on_fb_grid_emitter(CPUState* cpu, uint32_t addr) {
    if (!openpete_native_scene_enabled() || s_in_grid) return 0;
    s_in_grid = 1;
    s_grid_ord = 0;
    openpete_recomp_module_get()->dispatch(cpu, addr);   /* the real emitter (ref unchanged) */
    s_in_grid = 0;
    return 1;
}

/* The hook addresses are the descriptor's, bound at boot: a static
 * initializer cannot read another TU's const. A game whose descriptor names
 * neither address registers no hook and the module is inert. */
static feature_dispatch_entry_t ui_sprite_dispatch[] = {
    { 0u, on_ot_insert },
    { 0u, on_ot_insert },
    { 0u, on_fade_emitter },
    { 0u, on_fb_grid_emitter },
    { 0u, on_ot_rank_link },
    { 0u, on_ot_stitch },
    { 0u, NULL },
};

/* Retained-framebuffer freeze backdrop (see ui_game_t::freeze_retained).
 *
 * The menu screens that keep the last rendered frame write DRAWENV.isbg = 0 on
 * both draw buffers and draw no world; every other screen leaves isbg at 1 and
 * clears to its own background colour. Reading the byte reproduces the game's
 * own discriminator rather than guessing from what the scene holds: the same
 * DRAWENV the clear-colour commit reads decides whether the frame is cleared at
 * all, so the two are read at the same point of the present.
 *
 * Nothing here writes PSX RAM: the backdrop is display state only. */
static unsigned s_freeze_retain_n = 0;

unsigned ui_freeze_retain_count(void) { return s_freeze_retain_n; }

void ui_freeze_retain_commit(void) {
    if (!ui_game.freeze_retained) return;
    uint8_t env[4];
    if (!cyclorama_native_present_env(env)) return;   /* no valid DB* yet */
    if (env[0]) return;                               /* isbg 1: the frame is cleared */
    /* The tint is unused in retained mode (blit.frag passes the snapshot
     * through) but the packed word's active bit lives in the same field, so a
     * full-scale colour is what the encoder expects. */
    openpete_native_scene_set_freeze_backdrop(0xFF, 0xFF, 0xFF, 0);
    s_freeze_retain_n++;
}

/// @brief Prints the retained-backdrop present count at exit.
static void ui_freeze_retain_census(void) {
    fprintf(stderr, "ui-freeze: retained-backdrop presents=%u\n", s_freeze_retain_n);
}

/** @brief Binds the module's dispatch table to the game's ui_game hook addresses and arms the retained freeze.
 *
 * When ui_game.freeze_retained is set, turns on the scene's retained
 * freeze mode and registers the exit census. The named ot_insert,
 * ot_insert_slot, fade_emitter, fb_grid_emitter, ot_rank_slot_insert and
 * (on a rank_by_link game) ot_stitch addresses are packed to the front of
 * the table and a zero address ends it.
 * @param opts The options; unused. */
static void ui_sprite_apply_options(const openpete_options_t* opts) {
    (void)opts;
    if (ui_game.freeze_retained) {
        openpete_native_scene_set_freeze_retained(1);
        atexit(ui_freeze_retain_census);
    }
    /* A zero addr terminates the table, so the named addresses are packed to
     * the front: a game that names only one still reaches it. */
    int n = 0;
    if (ui_game.ot_insert) {
        ui_sprite_dispatch[n].addr = ui_game.ot_insert;
        ui_sprite_dispatch[n].fn   = on_ot_insert; n++;
    }
    if (ui_game.ot_insert_slot) {   /* same body: a1 carries only the depth slot */
        ui_sprite_dispatch[n].addr = ui_game.ot_insert_slot;
        ui_sprite_dispatch[n].fn   = on_ot_insert; n++;
    }
    if (ui_game.fade_emitter) {
        ui_sprite_dispatch[n].addr = ui_game.fade_emitter;
        ui_sprite_dispatch[n].fn   = on_fade_emitter; n++;
    }
    if (ui_game.fb_grid_emitter) {
        ui_sprite_dispatch[n].addr = ui_game.fb_grid_emitter;
        ui_sprite_dispatch[n].fn   = on_fb_grid_emitter; n++;
    }
    if (ui_game.ot_rank_slot_insert) {
        ui_sprite_dispatch[n].addr = ui_game.ot_rank_slot_insert;
        ui_sprite_dispatch[n].fn   = on_ot_rank_link; n++;
    }
    if (ui_game.rank_by_link && ui_game.ot_stitch) {
        ui_sprite_dispatch[n].addr = ui_game.ot_stitch;
        ui_sprite_dispatch[n].fn   = on_ot_stitch; n++;
    }
    ui_sprite_dispatch[n].addr = 0u;
    ui_sprite_dispatch[n].fn   = NULL;
}

const feature_module_t ui_sprite_module = {
    .name           = "ui_sprite_native",
    .apply_options  = ui_sprite_apply_options,
    .dispatch_table = ui_sprite_dispatch,
};
