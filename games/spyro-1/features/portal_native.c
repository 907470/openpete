// games/spyro-1/features/portal_native.c — native re-derivation of the PORTAL
// skybox previews (the destination-level sky shown inside homeworld portal
// arches), plus its falsify-before-ship capture/dump harness.
//
// Background. The main level sky is func_8004EBA8 (handled by
// cyclorama_native.c). The PORTAL previews are a separate pipeline driven by
// func_80050BD0 (RenderPortals): for each visible portal it projects the
// portal's own destination skybox dome with an animated rotation matrix, sets
// a per-portal screen CLIP RECT (D_8007AA00..0C), and emits the dome prims via
// one of three handwritten leaf renderers —
//   func_8004FEA0  close range (< 0x3001), full colour
//   func_80050240  medium range, distance colour-blended (D_800757D4 target)
//   func_8004F4BC  secondary pass
// — then finally calls func_8004EBA8 for the main sky. The native feeder only
// hooked the main sky, so portal previews were missing (the arch showed the
// hill behind it instead of the destination dome).
//
// This module re-derives the portal dome in float from gamestate (the chosen
// approach: faithful to the renderer's "3D-from-gamestate, never replay GPU
// packets" design, and widescreen-ready). Phase 1 (this commit) is the
// capture/dump harness: hook the leaf renderers + RenderPortals, snapshot the
// inputs (skybox struct, the two matrices, clip rect, depth) and the GTE prims
// each leaf actually emitted to the polybuf (ground truth), so the float
// extraction can be measured against real output. Gated by OPENPETE_PORTAL_DUMP
// (zero overhead off). Determinism: never writes PSX RAM; runs the real leaf
// renderer exactly once (behaviour-identical fall-through) and only READS after.
//
// Format notes (cyclorama.h warns m_Skybox is "strange storage" — so trust the
// dump, not the field names): see the format notes.

#include "feature_registry.h"
#include "psx_runtime.h"
#include "openpete_native_render.h"
#include "render_paths.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void     func_8004FEA0(CPUState* cpu);  /* portal sky: close, full colour */
extern void     func_80050240(CPUState* cpu);  /* portal sky: medium, blended    */
extern void     func_8004F4BC(CPUState* cpu);  /* portal sky: secondary pass     */
extern void     func_80050BE0(CPUState* cpu);  /* RenderPortals (the driver)     */
extern unsigned openpete_frame;

#define G_CAMERA         0x80076DD0u   /* g_Camera: view matrix @+0x00, pos @+0x28/2C/30 */
#define G_PORTALS        0x80078640u   /* Portal* g_Portals[6]   */
#define G_PORTALCOUNT    0x800758BCu   /* int     g_PortalCount  */
#define POLYBUF_CURSOR   0x800757B0u   /* D_800757B0: polybuf write cursor */
#define D_PORTAL_YAW     0x80075858u   /* portal rotation yaw   (animated) */
#define D_PORTAL_PITCH   0x800758FCu   /* portal rotation pitch (animated) */
#define D_PORTAL_DEPTH   0x80075934u   /* D_80075934: portal OT depth */
#define D_CLIPRECT       0x8007AA00u   /* minY,maxY,minX,maxX each <<16 (per doc) */
#define D_BLEND_TARGET   0x800757D4u   /* D_800757D4: GTE far-colour (FC), the fog target (0x00BBGGRR) */
#define D_FOG_FACTOR     0x8007575Cu   /* D_8007575C: GTE IR0, the DPCS depth-cue lerp coeff (0x1000 = 1.0) */

/* DPCS depth-cue blend: lerp an 0x00BBGGRR colour toward the fog target by t in
 * [0,1] (the blended-medium portal's cross-dissolve, func_80050240 — t=IR0/4096,
 * fog=D_800757D4). t<=0 returns the colour unchanged (the un-fogged dome/flat). */
static uint32_t blend_to_fog(uint32_t rgb, uint32_t fog, double t) {
    if (t <= 0.0) return rgb & 0xFFFFFF;
    if (t > 1.0) t = 1.0;
    int r0 =  rgb        & 0xFF, g0 = (rgb  >> 8) & 0xFF, b0 = (rgb  >> 16) & 0xFF;
    int rf =  fog        & 0xFF, gf = (fog  >> 8) & 0xFF, bf = (fog  >> 16) & 0xFF;
    int r = r0 + (int)((rf - r0) * t + 0.5);
    int g = g0 + (int)((gf - g0) * t + 0.5);
    int b = b0 + (int)((bf - b0) * t + 0.5);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

/* ---- dump gating ---------------------------------------------------- */
static int portal_dump_enabled(void) {
    static int s = -1;
    if (s < 0) { const char* v = getenv("OPENPETE_PORTAL_DUMP"); s = (v && v[0] && v[0] != '0') ? 1 : 0; }
    return s;
}
static unsigned portal_dump_frame(void) {
    static unsigned f = 0; static int init = 0;
    if (!init) { const char* v = getenv("OPENPETE_PORTAL_FRAME"); f = v ? (unsigned)atoi(v) : 1750u; init = 1; }
    return f;
}

static void   read_matrix(CPUState* cpu, uint32_t p, int M[3][3]);
static double project_world(CPUState* cpu, int wx, int wy, int wz, double* out_sx, double* out_sy);

typedef struct { float x, y, r, g, b; } pgv;   /* gouraud screen vertex (clipping) */
typedef struct { float x, y; double vz; } ppt; /* projected portal-outline point  */
/* Owning portal of a leaf: skybox-pointer match (dome/close), else screen-centre
 * match (far/medium flat). Returns the Portal* or 0. */
static uint32_t owning_portal_ptr(CPUState* cpu, uint32_t a0, double cx, double cy);
/* Project a portal's outline (m_Points) to screen with per-point view-Z. Returns
 * the count (>=3) iff every point is in front of the camera, else 0 (the portal
 * is a tilted planar window — its sky is depth-interpolated across this polygon
 * instead of drawn flat, so the stone frame occludes it correctly at an angle). */
static int    portal_outline_screen(CPUState* cpu, uint32_t p, ppt* poly, int maxn);

static void dump_portal_table(CPUState* cpu) {
    uint32_t n = cpu->read_word(G_PORTALCOUNT);
    fprintf(stderr, "[portal-dump f=%u] RenderPortals: g_PortalCount=%u yaw=%d pitch=%d\n",
            openpete_frame, n, (int32_t)cpu->read_word(D_PORTAL_YAW),
            (int32_t)cpu->read_word(D_PORTAL_PITCH));
    if (n > 6) n = 6;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4);
        if (!p) { fprintf(stderr, "  portal[%u] = NULL\n", i); continue; }
        uint32_t skybox = cpu->read_word(p + 0x00);
        int levelId     = (int32_t)cpu->read_word(p + 0x1C);
        /* Centroid of the outline points (p+0x20 + 12*k), projected as the game
         * does — confirms which portals are on-screen + their view-Z. */
        double sx, sy, vz = project_world(cpu, (int32_t)cpu->read_word(p + 0x20),
                                          (int32_t)cpu->read_word(p + 0x24),
                                          (int32_t)cpu->read_word(p + 0x28), &sx, &sy);
        fprintf(stderr, "  portal[%u]@%08X skybox=%08X levelId=%d bg=%06X -> screen(%.0f,%.0f) viewZ=%.0f\n",
                i, p, skybox, levelId, cpu->read_word(skybox + 0x18) & 0xFFFFFF, sx, sy, vz);
    }
}

static void read_matrix(CPUState* cpu, uint32_t p, int M[3][3]) {
    uint32_t w0 = cpu->read_word(p), w1 = cpu->read_word(p + 4);
    uint32_t w2 = cpu->read_word(p + 8), w3 = cpu->read_word(p + 12);
    uint32_t w4 = cpu->read_word(p + 16);
    M[0][0] = (int16_t)(w0 & 0xFFFF); M[0][1] = (int16_t)(w0 >> 16); M[0][2] = (int16_t)(w1 & 0xFFFF);
    M[1][0] = (int16_t)(w1 >> 16);    M[1][1] = (int16_t)(w2 & 0xFFFF); M[1][2] = (int16_t)(w2 >> 16);
    M[2][0] = (int16_t)(w3 & 0xFFFF); M[2][1] = (int16_t)(w3 >> 16);  M[2][2] = (int16_t)(w4 & 0xFFFF);
}

/* Float-project a world point exactly as func_80017B48 (the routine that
 * projects the portal outline → clip rect). It uses the g_Camera+0x00 rotation
 * (TR=0) with a PERMUTED/NEGATED vertex feed: GTE V = (-(wy-cy), -(wz-cz),
 * (wx-cx)). The per-point distance >>shift it applies cancels in the MAC1/MAC3
 * ratio, so float needs no shift. Returns the full-world view-space Z (MAC3);
 * fills screen sx/sy. OFX/OFY/H from the live GTE control regs. */
static double project_world(CPUState* cpu, int wx, int wy, int wz,
                            double* out_sx, double* out_sy) {
    int M[3][3];
    read_matrix(cpu, G_CAMERA + 0x00, M);
    int cx = (int32_t)cpu->read_word(G_CAMERA + 0x28);
    int cy = (int32_t)cpu->read_word(G_CAMERA + 0x2C);
    int cz = (int32_t)cpu->read_word(G_CAMERA + 0x30);
    double vx = -((double)wy - cy);    /* GTE VX = -(world.y - cam.y) */
    double vy = -((double)wz - cz);    /* GTE VY = -(world.z - cam.z) */
    double vz =  ((double)wx - cx);    /* GTE VZ =  (world.x - cam.x) */
    double mac1 = (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    double mac2 = (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    double mac3 = (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
    double ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    double ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    double H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
    double z = (mac3 > 1.0) ? mac3 : 1.0;
    if (out_sx) *out_sx = ofx + mac1 * H / z;
    if (out_sy) *out_sy = ofy + mac2 * H / z;
    return mac3;
}

/* The depth a portal's sky should sit at: the FARTHEST of its outline points'
 * view-Z. The stone ring's pointed top curves slightly back from the centre, so
 * a centre-depth sky overdraws the top of the frame (f1845); placing it behind
 * the whole ring lets the stone occlude it into the arch shape, while it stays
 * far nearer than the through-hole background so it still shows in the opening.
 * Used by BOTH the dome path and the flat far/medium quad. -1 if all behind. */
static double portal_outline_maxvz(CPUState* cpu, uint32_t p) {
    int npts = (int32_t)cpu->read_word(p + 0x04);
    if (npts < 1 || npts > 16) npts = 5;
    double maxvz = -1.0;
    for (int k = 0; k < npts; k++) {
        int wx = (int32_t)cpu->read_word(p + 0x20 + k*12);
        int wy = (int32_t)cpu->read_word(p + 0x24 + k*12);
        int wz = (int32_t)cpu->read_word(p + 0x28 + k*12);
        double sx, sy, vz = project_world(cpu, wx, wy, wz, &sx, &sy);
        if (vz > maxvz) maxvz = vz;
    }
    return maxvz;
}

/* Owning portal of a leaf. The close/dome leaves pass the owning portal's skybox
 * directly (func_8004F4BC: a0=m_Skybox; the close func_8004FEA0: a0=m_Skybox+0x10)
 * → EXACT match (the screen-centre match mis-binds a large, screen-clamped portal
 * whose clip-rect centre drifts onto another portal — the f2075 "portal vanished").
 * Far/medium flat leaves carry a stack/global colour ptr with no skybox link →
 * fall back to the portal whose centroid projects nearest the clip-rect centre. */
static uint32_t owning_portal_ptr(CPUState* cpu, uint32_t a0, double cx, double cy) {
    uint32_t n = cpu->read_word(G_PORTALCOUNT); if (n > 6) n = 6;
    for (uint32_t i = 0; i < n; i++) {                  /* skybox-pointer match */
        uint32_t p = cpu->read_word(G_PORTALS + i * 4); if (!p) continue;
        uint32_t sb = cpu->read_word(p + 0x00);
        if (sb && (sb == a0 || sb == a0 - 0x10)) return p;
    }
    double best_d = 1e30; uint32_t best_p = 0;          /* screen-centre match */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4); if (!p) continue;
        int npts = (int32_t)cpu->read_word(p + 0x04); if (npts < 1 || npts > 16) npts = 5;
        double ax = 0, ay = 0, az = 0;
        for (int k = 0; k < npts; k++) {
            ax += (int32_t)cpu->read_word(p + 0x20 + k*12);
            ay += (int32_t)cpu->read_word(p + 0x24 + k*12);
            az += (int32_t)cpu->read_word(p + 0x28 + k*12);
        }
        ax /= npts; ay /= npts; az /= npts;
        double sx, sy, vz = project_world(cpu, (int)ax, (int)ay, (int)az, &sx, &sy);
        if (vz <= 1.0) continue;
        double dx = sx - cx, dy = sy - cy, d = dx*dx + dy*dy;
        if (d < best_d) { best_d = d; best_p = p; }
    }
    return best_p;
}

/* Single fallback view-Z (farthest outline point) for when the tilted-plane path
 * can't be used (an outline point is behind the camera). */
static double portal_viewz(CPUState* cpu, uint32_t a0, double cx, double cy) {
    uint32_t p = owning_portal_ptr(cpu, a0, cx, cy);
    return p ? portal_outline_maxvz(cpu, p) : -1.0;
}

/* Project the portal outline (m_Points) to screen, per-point view-Z. Returns the
 * count iff >=3 points and ALL are in front (so the polygon + plane are valid). */
static int portal_outline_screen(CPUState* cpu, uint32_t p, ppt* poly, int maxn) {
    int npts = (int32_t)cpu->read_word(p + 0x04);
    if (npts < 3 || npts > maxn) return 0;
    for (int k = 0; k < npts; k++) {
        int wx = (int32_t)cpu->read_word(p + 0x20 + k*12);
        int wy = (int32_t)cpu->read_word(p + 0x24 + k*12);
        int wz = (int32_t)cpu->read_word(p + 0x28 + k*12);
        double sx, sy, vz = project_world(cpu, wx, wy, wz, &sx, &sy);
        if (vz <= 1.0) return 0;                         /* a point is behind the camera */
        poly[k].x = (float)sx; poly[k].y = (float)sy; poly[k].vz = vz;
    }
    return npts;
}

/* Fit inv-vz = a*x + b*y + c over the (planar) outline — 1/w is linear in screen
 * space for a plane, so this is the portal window's depth field. 0 if degenerate. */
static int fit_invz(const ppt* poly, int n, double* a, double* b, double* c) {
    int i0 = 0, i1 = n / 3, i2 = (2 * n) / 3;
    if (i1 == i0) i1 = 1; if (i2 == i1) i2 = n - 1; if (i2 == i0) return 0;
    double x0 = poly[i0].x, y0 = poly[i0].y, w0 = 1.0 / poly[i0].vz;
    double x1 = poly[i1].x, y1 = poly[i1].y, w1 = 1.0 / poly[i1].vz;
    double x2 = poly[i2].x, y2 = poly[i2].y, w2 = 1.0 / poly[i2].vz;
    double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (det > -1e-6 && det < 1e-6) return 0;            /* collinear */
    *a = ((w1 - w0) * (y2 - y0) - (w2 - w0) * (y1 - y0)) / det;
    *b = ((x1 - x0) * (w2 - w0) - (x2 - x0) * (w1 - w0)) / det;
    *c = w0 - *a * x0 - *b * y0;
    return 1;
}

/* Native depth from the portal plane at a screen point (perspective-correct via
 * the inv-vz fit). */
static float plane_depth(double a, double b, double c, double x, double y) {
    double inv = a * x + b * y + c;
    double vz = (inv > 1e-9) ? 1.0 / inv : 1e9;
    return openpete_native_depth_from_viewz((float)vz);
}

/* Far-path portal preview: the destination dome renders as a single flat colour
 * (a0[0]) filling the arch clip rect. Emit it as one opaque quad at the portal's
 * world-Z depth, so it sorts OVER the far terrain but UNDER the near arch stone
 * (which the terrain/moby feeders draw on top, leaving colour only in the hole).
 * Close-range portals (a0 not a RAM pointer -> the per-vertex dome path) are left
 * to the next increment. Returns 1 if a quad was emitted.
 *
 * For a CLOSE portal the original draws TWO passes: a flat colour base (the close
 * func_8004FEA0, a0=skybox+0x10) THEN the gouraud dome over it (func_8004F4BC) —
 * painter's order in the OT. With a depth buffer the flat base would land on the
 * same portal plane as the dome and z-fight (worse: the two interpolate depth
 * differently across the interior — this quad linearly per fan-tri, the dome
 * perspective-correct per pixel — so the bg colour shimmers through, f1910). So a
 * SKYBOX-LINKED flat (a0 == a portal's m_Skybox, or m_Skybox+0x10) is the dome's
 * BACKSTOP: push it behind the dome by backstop_bias (>1 scales the fill's view-Z
 * so depth_from_viewz sinks it farther). The dome then wins wherever it has
 * coverage; the backstop only shows through the dome's culled/near-rejected edge
 * gaps (still far nearer than the through-hole terrain, so it never reveals the hill
 * behind). A far/medium flat (a0 = a stack/global colour ptr, NOT skybox-linked) IS
 * the whole preview — it stays on the portal plane (bias 1.0, backstop_bias ignored). */
static int feed_portal_quad(CPUState* cpu, uint32_t a0, double backstop_bias,
                            uint32_t fog_rgb, double fog_t) {
    if (a0 < 0x80010000u || a0 >= 0x80200000u) return 0;   /* not a RAM pointer */
    /* The flat-fill colour. Far/medium leaves (func_8004FEA0/80050240) pass a
     * pointer whose [0] IS the colour (stack fog colour, &D_800757D4 blend, or
     * the close-pass skybox+0x10 bg colour) — all have high bytes set. The close
     * DOME leaf (func_8004F4BC) passes the skybox struct itself, whose [0] is the
     * small sector COUNT, not a colour; there the m_BackgroundColor lives at
     * skybox+0x18. Discriminate by magnitude so one path handles both.
     * (This fills the arch with the destination sky's base colour; the full
     * per-vertex gradient dome is the next increment — see CARVE-OUT.) */
    uint32_t w0 = cpu->read_word(a0) & 0xFFFFFF;
    uint32_t rgb = (w0 >= 0x10000u) ? w0 : (cpu->read_word(a0 + 0x18) & 0xFFFFFF);
    rgb = blend_to_fog(rgb, fog_rgb, fog_t);     /* blended-medium: fade the base toward the fog colour */
    uint32_t rgba = rgb | 0xFF000000u;

    int y0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x0) >> 16;
    int y1 = (int32_t)cpu->read_word(D_CLIPRECT + 0x4) >> 16;
    int x0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x8) >> 16;
    int x1 = (int32_t)cpu->read_word(D_CLIPRECT + 0xC) >> 16;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }      /* rect is min/max either order */
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) return 0;                /* empty/degenerate */

    /* The portal opening is a tilted planar window. Fill its actual outline
     * polygon (projected m_Points) with per-vertex depth = each point's view-Z,
     * so at a sharp angle the near (bottom) edge keeps its near depth and the
     * floor occludes it exactly at the frame, instead of a single flat depth
     * that makes the floor eat the near part (f2945). */
    uint32_t p = owning_portal_ptr(cpu, a0, (x0 + x1) * 0.5, (y0 + y1) * 0.5);
    /* Only a skybox-linked flat is a dome backstop (close FEA0 a0=skybox+0x10, or
     * the dome's own a0=skybox) — push it behind the dome. Far/medium previews
     * (a0 = stack/global colour) are the whole sky and stay on the plane. */
    double back_bias = 1.0;
    if (p) { uint32_t sb = cpu->read_word(p + 0x00); if (sb == a0 || sb == a0 - 0x10) back_bias = backstop_bias; }
    ppt poly[16];
    int pn = p ? portal_outline_screen(cpu, p, poly, 16) : 0;
    if (pn >= 3) {
        for (int i = 1; i + 1 < pn; i++) {             /* fan-triangulate the outline */
            openpete_scene_tri t;
            memset(&t, 0, sizeof t);
            t.textured = 0; t.blend = 0; t.category = OPENPETE_SCENE_WORLD;
            const int idx3[3] = { 0, i, i + 1 };
            for (int k = 0; k < 3; k++) {
                const ppt* v = &poly[idx3[k]];
                t.v[k].sx = v->x; t.v[k].sy = v->y;
                t.v[k].z = openpete_native_depth_from_viewz((float)(v->vz * back_bias));
                t.v[k].rgba = rgba;
            }
            openpete_native_scene_add_tri(&t);
        }
        return 1;
    }

    /* Fallback (an outline point behind the camera / no match): the axis-aligned
     * clip rect at the farthest-outline constant depth. */
    double vz = portal_viewz(cpu, a0, (x0 + x1) * 0.5, (y0 + y1) * 0.5);
    if (vz <= 1.0) vz = 16384.0;
    float depth = openpete_native_depth_from_viewz((float)(vz * back_bias));
    float cxx[4] = { (float)x0, (float)x1, (float)x1, (float)x0 };
    float cyy[4] = { (float)y0, (float)y0, (float)y1, (float)y1 };
    int idx[2][3] = { {0,1,2}, {0,2,3} };
    for (int ti = 0; ti < 2; ti++) {
        openpete_scene_tri t;
        memset(&t, 0, sizeof t);
        t.textured = 0; t.blend = 0; t.category = OPENPETE_SCENE_WORLD;
        for (int k = 0; k < 3; k++) {
            t.v[k].sx = cxx[idx[ti][k]]; t.v[k].sy = cyy[idx[ti][k]];
            t.v[k].z = depth; t.v[k].u = 0.0f; t.v[k].v = 0.0f; t.v[k].rgba = rgba;
        }
        openpete_native_scene_add_tri(&t);
    }
    return 1;
}

/* ---- close-range portal: the full per-vertex gradient DOME --------------- */
/* When the camera is near a portal, func_8004F4BC re-projects the destination
 * skybox dome (gouraud, with clouds/horizon) instead of a flat colour. It's the
 * same cyclorama dome FORMAT as the main sky (cull pt + centre header + packed
 * 11/11/10 verts + F3/G3 faces), so this mirrors cyclorama_native's cyc_extract:
 * matrix from the GTE the leaf left (post-call, TR=0), sectors from the skybox
 * (count@+0x00, sector-array ptr@+0x04), gouraud faces Sutherland-Hodgman-clipped
 * to the arch rect at the portal depth. */
/* Clip a convex polygon against one axis-aligned half-plane (axis 0=x,1=y;
 * keepGE: keep the >=val side). out holds up to n+1 verts. Interpolates colour. */
static int clip_half(const pgv* in, int n, pgv* out, int axis, float val, int keepGE) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const pgv* a = &in[i];
        const pgv* b = &in[(i + 1) % n];
        float av = axis ? a->y : a->x, bv = axis ? b->y : b->x;
        int ain = keepGE ? (av >= val) : (av <= val);
        int bin = keepGE ? (bv >= val) : (bv <= val);
        if (ain) out[m++] = *a;
        if (ain != bin) {
            float t = (bv != av) ? (val - av) / (bv - av) : 0.0f;
            pgv c;
            c.x = a->x + t * (b->x - a->x);  c.y = a->y + t * (b->y - a->y);
            c.r = a->r + t * (b->r - a->r);  c.g = a->g + t * (b->g - a->g);  c.b = a->b + t * (b->b - a->b);
            out[m++] = c;
        }
    }
    return m;
}

/* Clip a gouraud face to the portal CLIP RECT and fan-emit it. This is what the
 * dome leaves actually do: func_80050BD0 sets D_80077EA0[0]=0, so func_8005060C
 * skips outline clipping — the dome is confined ONLY by the GPU scissor rect
 * (D_8007AA00), and the arch SHAPE comes from the stone/floor occluding the rect
 * spill via depth, not from clipping to the opening polygon. Depth is the portal
 * PLANE (use_plane: inv-vz fit) so that occlusion is correct at any angle (f2945);
 * `depth` is the constant fallback when the plane fit is unavailable. */
static void emit_clipped_gouraud(pgv v0, pgv v1, pgv v2, int x0, int y0, int x1, int y1,
                                 float depth, int use_plane, double pa, double pb, double pc) {
    pgv a[10], b[10];
    a[0] = v0; a[1] = v1; a[2] = v2;
    int n = 3;
    n = clip_half(a, n, b, 0, (float)x0, 1); if (n < 3) return;
    n = clip_half(b, n, a, 0, (float)x1, 0); if (n < 3) return;
    n = clip_half(a, n, b, 1, (float)y0, 1); if (n < 3) return;
    n = clip_half(b, n, a, 1, (float)y1, 0); if (n < 3) return;
    for (int i = 1; i + 1 < n; i++) {                       /* fan-triangulate */
        openpete_scene_tri t;
        memset(&t, 0, sizeof t);
        t.textured = 0; t.blend = 0; t.category = OPENPETE_SCENE_WORLD;
        const pgv* tv[3] = { &a[0], &a[i], &a[i + 1] };
        for (int k = 0; k < 3; k++) {
            uint32_t r = (uint32_t)(tv[k]->r + 0.5f), g = (uint32_t)(tv[k]->g + 0.5f), bl = (uint32_t)(tv[k]->b + 0.5f);
            if (r > 255) r = 255; if (g > 255) g = 255; if (bl > 255) bl = 255;
            t.v[k].sx = tv[k]->x; t.v[k].sy = tv[k]->y;
            t.v[k].z = use_plane ? plane_depth(pa, pb, pc, tv[k]->x, tv[k]->y) : depth;
            t.v[k].rgba = r | (g << 8) | (bl << 16) | 0xFF000000u;   /* low byte=R, matches cyc */
        }
        openpete_native_scene_add_tri(&t);
    }
}

/* A collected dome face awaiting depth-sorted emission. The dome is drawn at the
 * single portal-plane depth so the arch occludes it, but the medium leaf's matrix
 * compresses the dome's Y (~0.6x) and folds it onto itself in screen space; with
 * every face at the same depth the fold has no z-order, so the back rim overdraws
 * the front into a jagged mess. The PSX sorts the faces in its OT; we mirror that by
 * sorting far→near and emitting in that order — with LESS_OR_EQUAL the last (nearest)
 * face wins per pixel, so the near rim correctly paints over the far rim. */
typedef struct { pgv a, b, c; double key; } dome_face;
static int dome_face_cmp(const void* x, const void* y) {
    double kx = ((const dome_face*)x)->key, ky = ((const dome_face*)y)->key;
    return (kx < ky) ? 1 : (kx > ky) ? -1 : 0;   /* DESCENDING view-Z: far first */
}

static int feed_portal_dome(CPUState* cpu, uint32_t skybox, uint32_t fog_rgb, double fog_t) {
    int y0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x0) >> 16, y1 = (int32_t)cpu->read_word(D_CLIPRECT + 0x4) >> 16;
    int x0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x8) >> 16, x1 = (int32_t)cpu->read_word(D_CLIPRECT + 0xC) >> 16;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) return 0;

    /* dome rotation matrix the leaf left in the GTE (post-call), TR=0 (sky) */
    uint32_t w0 = cpu->gte_ctrl[0], w1 = cpu->gte_ctrl[1], w2 = cpu->gte_ctrl[2], w3 = cpu->gte_ctrl[3], w4 = cpu->gte_ctrl[4];
    int M[3][3];
    M[0][0] = (int16_t)(w0 & 0xFFFF); M[0][1] = (int16_t)(w0 >> 16); M[0][2] = (int16_t)(w1 & 0xFFFF);
    M[1][0] = (int16_t)(w1 >> 16);    M[1][1] = (int16_t)(w2 & 0xFFFF); M[1][2] = (int16_t)(w2 >> 16);
    M[2][0] = (int16_t)(w3 & 0xFFFF); M[2][1] = (int16_t)(w3 >> 16);  M[2][2] = (int16_t)(w4 & 0xFFFF);
    double ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    double ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    double H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);

    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    double vz = portal_viewz(cpu, skybox, cx, cy);
    if (vz <= 1.0) vz = 16384.0;
    float depth = openpete_native_depth_from_viewz((float)vz);   /* rect-fallback depth */

    /* Tilted-plane window: outline polygon + its perspective-correct inv-vz depth
     * field, so each dome pixel takes the portal-plane depth (the stone frame
     * occludes it into the arch shape even at a sharp angle, f2945). Falls back to
     * the rect clip + constant depth when an outline point is behind the camera. */
    uint32_t p = owning_portal_ptr(cpu, skybox, cx, cy);
    ppt poly[16]; int pn = p ? portal_outline_screen(cpu, p, poly, 16) : 0;
    double pa = 0, pb = 0, pc = 0;
    int use_plane = (pn >= 3) && fit_invz(poly, pn, &pa, &pb, &pc);

    uint32_t cnt = cpu->read_word(skybox + 0x00), arr = cpu->read_word(skybox + 0x04);
    if (cnt > 1024) cnt = 1024;
    static float  px[4096], py[4096];
    static double pz[4096];
    static uint32_t col[4096];
    static dome_face faces[8192]; int nf = 0;   /* collected for far→near depth sort */
    for (uint32_t si = 0; si < cnt; si++) {
        uint32_t S = cpu->read_word(arr + si * 4);
        if (!S) continue;
        /* phase-1 cull: project the sector's representative point, skip if behind
         * the cull radius (same test as the main cyclorama). */
        uint32_t c0 = cpu->read_word(S + 0x00), c1 = cpu->read_word(S + 0x04);
        int cvx = (int16_t)(c0 & 0xFFFF), cvy = (int16_t)(c0 >> 16), cvz = (int16_t)(c1 >> 16), radius = (int16_t)(c1 & 0xFFFF);
        double cm3 = (M[2][0]*(double)cvx + M[2][1]*(double)cvy + M[2][2]*(double)cvz) / 4096.0;
        if (cm3 <= (double)radius) continue;

        uint32_t h8 = cpu->read_word(S + 0x08), hC = cpu->read_word(S + 0x0C), hF = cpu->read_word(S + 0x10);
        int centerX = (int16_t)(h8 & 0xFFFF), centerY = (int16_t)(h8 >> 16), centerZ = (int16_t)(hC >> 16);
        uint32_t vc = hC & 0xFFFF, fc = hF & 0x1FFF, foff = hF >> 14;
        if (vc > 4096) vc = 4096;
        uint32_t vertBase = S + 0x18, colorBase = vertBase + vc * 4, faceBase = colorBase + foff;
        for (uint32_t i = 0; i < vc; i++) {
            uint32_t w = cpu->read_word(vertBase + i * 4);
            int vx = centerX - (int)((w >> 10) & 0x7FF), vy = centerY - (int)(w & 0x3FF), vz2 = centerZ + (int)(w >> 21);
            double m1 = (M[0][0]*(double)vx + M[0][1]*vy + M[0][2]*vz2) / 4096.0;
            double m2 = (M[1][0]*(double)vx + M[1][1]*vy + M[1][2]*vz2) / 4096.0;
            double m3 = (M[2][0]*(double)vx + M[2][1]*vy + M[2][2]*vz2) / 4096.0;
            pz[i] = m3; double z = (m3 > 1.0) ? m3 : 1.0;
            px[i] = (float)(ofx + m1 * H / z); py[i] = (float)(ofy + m2 * H / z);
        }
        uint32_t colorCount = foff / 4; if (colorCount > 4096) colorCount = 4096;
        for (uint32_t j = 0; j < colorCount; j++) col[j] = cpu->read_word(colorBase + j * 4) & 0xFFFFFF;
        for (uint32_t f = 0; f < fc; f++) {
            uint32_t fw0 = cpu->read_word(faceBase + f * 8), fw1 = cpu->read_word(faceBase + f * 8 + 4);
            uint32_t i0 = (fw0 >> 22) & 0x3FF, i1 = (fw0 >> 12) & 0xFF, i2 = (fw0 >> 2) & 0xFF;
            uint32_t k0 = (fw1 >> 22) & 0x3FF, k1 = (fw1 >> 12) & 0xFF, k2 = (fw1 >> 2) & 0xFF;
            if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
            if (pz[i0] <= 1.0 || pz[i1] <= 1.0 || pz[i2] <= 1.0) continue;   /* near-plane reject */
            uint32_t cc0 = (k0 < colorCount) ? col[k0] : 0, cc1 = (k1 < colorCount) ? col[k1] : 0, cc2 = (k2 < colorCount) ? col[k2] : 0;
            /* blended-medium dome (func_80050240): DPCS depth-cue fades each vertex
             * colour toward the fog target (fog_t=0 for the un-fogged F4BC dome). */
            cc0 = blend_to_fog(cc0, fog_rgb, fog_t);
            cc1 = blend_to_fog(cc1, fog_rgb, fog_t);
            cc2 = blend_to_fog(cc2, fog_rgb, fog_t);
            if (nf >= (int)(sizeof faces / sizeof faces[0])) continue;
            dome_face* df = &faces[nf++];
            df->a.x = px[i0]; df->a.y = py[i0]; df->a.r = cc0 & 0xFF; df->a.g = (cc0 >> 8) & 0xFF; df->a.b = (cc0 >> 16) & 0xFF;
            df->b.x = px[i1]; df->b.y = py[i1]; df->b.r = cc1 & 0xFF; df->b.g = (cc1 >> 8) & 0xFF; df->b.b = (cc1 >> 16) & 0xFF;
            df->c.x = px[i2]; df->c.y = py[i2]; df->c.r = cc2 & 0xFF; df->c.g = (cc2 >> 8) & 0xFF; df->c.b = (cc2 >> 16) & 0xFF;
            df->key = (pz[i0] + pz[i1] + pz[i2]) / 3.0;   /* face view-Z for the far→near sort */
        }
    }
    /* depth-sort the whole dome far→near, then emit (LESS_OR_EQUAL → last wins). */
    qsort(faces, nf, sizeof faces[0], dome_face_cmp);
    for (int i = 0; i < nf; i++)
        emit_clipped_gouraud(faces[i].a, faces[i].b, faces[i].c, x0, y0, x1, y1, depth, use_plane, pa, pb, pc);
    return 1;
}

/* Dump one leaf-renderer call: the per-portal clip rect, the colour the leaf
 * carries (a0[0] for the far/medium flat paths), the matched view-Z / depth the
 * feed assigns, and the prims it emitted to the polybuf in [start,end). */
static void dump_leaf(CPUState* cpu, const char* which, uint32_t a0, uint32_t a1,
                      uint32_t a2, uint32_t start, uint32_t end) {
    int ry0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x0) >> 16, ry1 = (int32_t)cpu->read_word(D_CLIPRECT + 0x4) >> 16;
    int rx0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x8) >> 16, rx1 = (int32_t)cpu->read_word(D_CLIPRECT + 0xC) >> 16;
    uint32_t col = (a0 >= 0x80010000u && a0 < 0x80200000u) ? (cpu->read_word(a0) & 0xFFFFFF) : 0xFFFFFFFFu;
    double vz = portal_viewz(cpu, a0, (rx0 + rx1) * 0.5, (ry0 + ry1) * 0.5);
    uint32_t fog = cpu->read_word(D_BLEND_TARGET) & 0xFFFFFF, ir0 = cpu->read_word(D_FOG_FACTOR) & 0xFFFF;
    fprintf(stderr, "[portal-dump f=%u] %s a0=%08X colour=%06X clip x[%d,%d] y[%d,%d] "
                    "OTdepth=%d matchedViewZ=%.0f nativeZ=%.3f  prims=%u bytes  fogFC=%06X IR0=%u(t=%.3f)\n",
            openpete_frame, which, a0, col, rx0, rx1, ry0, ry1,
            (int32_t)cpu->read_word(D_PORTAL_DEPTH), vz,
            (double)openpete_native_depth_from_viewz((float)(vz > 1.0 ? vz : 16384.0)),
            (end > start) ? (end - start) : 0, fog, ir0, (double)ir0 / 4096.0);
    /* For the dome leaf (F4BC, a0=skybox), capture everything the float walk
     * needs: the GTE transform it projected with (post-call), the skybox sector
     * table, sector0's cyclorama-format header, and the first emitted prims. */
    if (((which[0] == 'F' && which[1] == '4') || (which[0] == '5')) && a0 >= 0x80010000u && a0 < 0x80200000u) {
        fprintf(stderr, "    GTE post: M=%08X %08X %08X %08X %08X TR=%d,%d,%d OFX=%d OFY=%d H=%u\n",
                cpu->gte_ctrl[0], cpu->gte_ctrl[1], cpu->gte_ctrl[2], cpu->gte_ctrl[3], cpu->gte_ctrl[4],
                (int32_t)cpu->gte_ctrl[5], (int32_t)cpu->gte_ctrl[6], (int32_t)cpu->gte_ctrl[7],
                (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16, cpu->gte_ctrl[26] & 0xFFFF);
        uint32_t cnt = cpu->read_word(a0 + 0x00), arr = cpu->read_word(a0 + 0x04);
        uint32_t s0 = cpu->read_word(arr);
        fprintf(stderr, "    skybox cnt=%u sectorArr=%08X sec0=%08X  sec0 hdr:", cnt, arr, s0);
        for (int o = 0; o <= 0x14; o += 4) fprintf(stderr, " [%02X]=%08X", o, cpu->read_word(s0 + o));
        fprintf(stderr, "\n    sec0 verts@+18:");
        for (int i = 0; i < 4; i++) fprintf(stderr, " %08X", cpu->read_word(s0 + 0x18 + i*4));
        fprintf(stderr, "\n    prims:");
        for (int i = 0; i < 10 && start + i*4 < end; i++) fprintf(stderr, " %08X", cpu->read_word(start + i*4));
        fprintf(stderr, "\n");
    }
    (void)a1; (void)a2;
}

/* ---- dispatch hooks ------------------------------------------------- */
static int s_want_dump_this_frame(void) {
    return portal_dump_enabled() && openpete_frame >= portal_dump_frame();
}

/* Which native render a leaf maps to. The same destination skybox renders three
 * ways across a portal's approach: FLAT colour when far, the DPCS-fogged dome at
 * medium (the cross-dissolve), the full gouraud dome when close. */
#define PORTAL_FLAT     0   /* func_8004FEA0: flat colour (far preview / close backstop) */
#define PORTAL_DOME     1   /* func_8004F4BC: full gouraud dome, no fog                  */
#define PORTAL_BLENDED  2   /* func_80050240: dome with DPCS fog toward D_800757D4       */

/* Generic leaf hook: run the real renderer, capturing the polybuf range, then
 * dump. A re-entry guard keeps a nested call (shouldn't happen for leaves)
 * from re-dumping. */
static int s_leaf_in = 0;
static int leaf_hook(CPUState* cpu, const char* which, void (*real)(CPUState*), int kind) {
    int feed = openpete_native_render_enabled();   /* --renderer vk|ab|diff live */
    if (!portal_dump_enabled() && !feed && !render_paths_active()) return 0; /* inert */
    if (s_leaf_in) return 0;
    s_leaf_in = 1;
    uint32_t a0 = cpu->a0, a1 = cpu->a1, a2 = cpu->a2;
    uint32_t start = cpu->read_word(POLYBUF_CURSOR);
    real(cpu);
    uint32_t end = cpu->read_word(POLYBUF_CURSOR);
    static int s_reports = 0;
    if (s_want_dump_this_frame() && s_reports < 512) { dump_leaf(cpu, which, a0, a1, a2, start, end); s_reports++; }
    if (feed) {
        openpete_native_scene_begin_frame(openpete_frame);  /* append; cyclorama/world already began it */
        int is_dome = (kind != PORTAL_FLAT);
        /* The blended-medium leaf (func_80050240) is the cross-dissolve: it draws
         * the SAME dome as F4BC but runs DPCS depth-cue, fading every colour toward
         * the GTE far-colour FC (D_800757D4) by IR0/4096 (D_8007575C). Far = IR0≈4096
         * (faded fully to the flat colour, the FEA0 preview); close = IR0≈0 (full
         * dome). Apply the same fade to its backstop fill so the gaps match. */
        uint32_t fog = 0; double fog_t = 0.0;
        if (kind == PORTAL_BLENDED) {
            fog   = cpu->read_word(D_BLEND_TARGET) & 0xFFFFFF;
            fog_t = (double)(cpu->read_word(D_FOG_FACTOR) & 0xFFFF) / 4096.0;
            if (fog_t > 1.0) fog_t = 1.0;
        }
        /* Far/medium FLAT leaves: one quad = the whole preview, on the portal plane
         * (a non-skybox a0 stays at bias 1.0). The close FEA0 flat (a0=skybox+0x10)
         * is the dome's authored BACKSTOP — sunk just behind the dome (1.0625). The
         * DOME leaves walk the gouraud dome on the plane AND lay a backstop: F4BC at
         * 1.125 (so it can't z-fight the paired FEA0 backstop at 1.0625), the medium
         * leaf at 1.0625 (no FEA0 at medium). Depths front→back: dome (×1.0) <
         * backstop (×1.0625) [< F4BC safety ×1.125]. All far nearer than the
         * through-hole terrain. (Fixes the bg z-fight at f1910.) */
        double backstop_bias = (kind == PORTAL_DOME) ? 1.125 : 1.0625;
        feed_portal_quad(cpu, a0, backstop_bias, fog, fog_t);
        if (is_dome && a0 >= 0x80010000u && a0 < 0x80200000u)
            feed_portal_dome(cpu, a0, fog, fog_t);
    }
    s_leaf_in = 0;
    return 1;
}

static int on_8004FEA0(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "FEA0(close)",     func_8004FEA0, PORTAL_FLAT);    }
static int on_80050240(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "50240(blended)",  func_80050240, PORTAL_BLENDED); }
static int on_8004F4BC(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "F4BC(secondary)", func_8004F4BC, PORTAL_DOME);    }

static int s_rp_in = 0;
static int on_80050BE0(CPUState* cpu, uint32_t a) {
    (void)a;
    if (!portal_dump_enabled()) return 0;
    if (s_rp_in) return 0;
    s_rp_in = 1;
    static int s_reports = 0;
    if (s_want_dump_this_frame() && s_reports < 8) { dump_portal_table(cpu); s_reports++; }
    func_80050BE0(cpu);   /* run the real RenderPortals (leaves dump inside) */
    s_rp_in = 0;
    return 1;
}

static const feature_dispatch_entry_t portal_dispatch[] = {
    { 0x80050BE0u, on_80050BE0 },
    { 0x8004FEA0u, on_8004FEA0 },
    { 0x80050240u, on_80050240 },
    { 0x8004F4BCu, on_8004F4BC },
    { 0u, NULL },
};

const feature_module_t portal_native_module = {
    .name           = "portal_native",
    .apply_options  = NULL,
    .dispatch_table = portal_dispatch,
};
