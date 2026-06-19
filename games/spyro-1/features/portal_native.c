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
#define D_BLEND_TARGET   0x800757D4u   /* D_800757D4: distance colour-lerp target */

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

/* Identify the portal a leaf call belongs to by projecting every portal centre
 * and choosing the one whose screen position is nearest the clip-rect centre.
 * Returns the portal's full-world view-Z (for the native depth) or -1 on miss. */
static double owning_portal_viewz(CPUState* cpu, double rect_cx, double rect_cy) {
    uint32_t n = cpu->read_word(G_PORTALCOUNT);
    if (n > 6) n = 6;
    double best_d = 1e30, best_vz = -1.0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4);
        if (!p) continue;
        int npts = (int32_t)cpu->read_word(p + 0x04);
        if (npts < 1 || npts > 16) npts = 5;
        double ax = 0, ay = 0, az = 0;                 /* centroid of the outline points */
        for (int k = 0; k < npts; k++) {
            ax += (int32_t)cpu->read_word(p + 0x20 + k*12);
            ay += (int32_t)cpu->read_word(p + 0x24 + k*12);
            az += (int32_t)cpu->read_word(p + 0x28 + k*12);
        }
        ax /= npts; ay /= npts; az /= npts;
        double sx, sy;
        double vz = project_world(cpu, (int)ax, (int)ay, (int)az, &sx, &sy);
        if (vz <= 1.0) continue;                       /* behind camera */
        double dx = sx - rect_cx, dy = sy - rect_cy, d = dx*dx + dy*dy;
        if (d < best_d) { best_d = d; best_vz = vz; }
    }
    return best_vz;
}

/* View-Z for the portal a leaf is rendering. The close/dome leaves pass the
 * owning portal's skybox directly (func_8004F4BC: a0=m_Skybox; the close
 * func_8004FEA0: a0=m_Skybox+0x10), so match by that pointer for an EXACT
 * portal→depth — the screen-centre match (owning_portal_viewz) mis-binds a large,
 * screen-clamped portal whose clip-rect centre drifts onto a different portal
 * (the f2075 "portal vanished": its near dome was handed a far portal's view-Z
 * and got Z-occluded). Far/medium flat leaves carry a stack/global colour ptr
 * with no skybox link → fall back to the screen-centre match. */
static double portal_viewz(CPUState* cpu, uint32_t a0, double rect_cx, double rect_cy) {
    uint32_t n = cpu->read_word(G_PORTALCOUNT); if (n > 6) n = 6;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4); if (!p) continue;
        uint32_t sb = cpu->read_word(p + 0x00);
        if (!sb || (sb != a0 && sb != a0 - 0x10)) continue;
        int npts = (int32_t)cpu->read_word(p + 0x04); if (npts < 1 || npts > 16) npts = 5;
        /* Return the FARTHEST outline point's view-Z, not the centroid's: the
         * stone arch ring's pointed top curves slightly back from the portal
         * centre, so a centre-depth sky overdraws the top of the frame (f1845).
         * Placing the sky behind the whole ring lets the stone occlude it into
         * the arch shape — and it stays far nearer than the through-hole
         * background, so the sky still shows in the opening. */
        double maxvz = -1.0;
        for (int k = 0; k < npts; k++) {
            int wx = (int32_t)cpu->read_word(p + 0x20 + k*12);
            int wy = (int32_t)cpu->read_word(p + 0x24 + k*12);
            int wz = (int32_t)cpu->read_word(p + 0x28 + k*12);
            double sx, sy, vz = project_world(cpu, wx, wy, wz, &sx, &sy);
            if (vz > maxvz) maxvz = vz;
        }
        if (maxvz > 1.0) return maxvz;
    }
    return owning_portal_viewz(cpu, rect_cx, rect_cy);   /* far/medium: no skybox link */
}

/* Far-path portal preview: the destination dome renders as a single flat colour
 * (a0[0]) filling the arch clip rect. Emit it as one opaque quad at the portal's
 * world-Z depth, so it sorts OVER the far terrain but UNDER the near arch stone
 * (which the terrain/moby feeders draw on top, leaving colour only in the hole).
 * Close-range portals (a0 not a RAM pointer -> the per-vertex dome path) are left
 * to the next increment. Returns 1 if a quad was emitted. */
static int feed_portal_quad(CPUState* cpu, uint32_t a0) {
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

    int y0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x0) >> 16;
    int y1 = (int32_t)cpu->read_word(D_CLIPRECT + 0x4) >> 16;
    int x0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x8) >> 16;
    int x1 = (int32_t)cpu->read_word(D_CLIPRECT + 0xC) >> 16;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }      /* rect is min/max either order */
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) return 0;                /* empty/degenerate */

    /* Depth from the owning portal's true view-Z (project its outline centroid
     * exactly as func_80017B48 does, match to this clip rect). Mapped through
     * the shared depth_from_viewz so the quad sorts against terrain: far portals
     * land behind the foreground hills, the near arch stays visible and is
     * occluded by its own (nearer) stone ring, giving the arch shape. */
    double vz = portal_viewz(cpu, a0, (x0 + x1) * 0.5, (y0 + y1) * 0.5);
    if (vz <= 1.0) vz = 16384.0;                       /* fallback mid-depth */
    float depth = openpete_native_depth_from_viewz((float)vz);

    uint32_t rgba = rgb | 0xFF000000u;
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
typedef struct { float x, y, r, g, b; } pgv;   /* gouraud screen vertex for clipping */

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

static void emit_clipped_gouraud(pgv v0, pgv v1, pgv v2, int x0, int y0, int x1, int y1, float depth) {
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
            t.v[k].sx = tv[k]->x; t.v[k].sy = tv[k]->y; t.v[k].z = depth;
            t.v[k].rgba = r | (g << 8) | (bl << 16) | 0xFF000000u;   /* low byte=R, matches cyc */
        }
        openpete_native_scene_add_tri(&t);
    }
}

static int feed_portal_dome(CPUState* cpu, uint32_t skybox) {
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

    double vz = portal_viewz(cpu, skybox, (x0 + x1) * 0.5, (y0 + y1) * 0.5);
    if (vz <= 1.0) vz = 16384.0;
    float depth = openpete_native_depth_from_viewz((float)vz);

    uint32_t cnt = cpu->read_word(skybox + 0x00), arr = cpu->read_word(skybox + 0x04);
    if (cnt > 1024) cnt = 1024;
    static float  px[4096], py[4096];
    static double pz[4096];
    static uint32_t col[4096];
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
            pgv a, b, c;
            a.x = px[i0]; a.y = py[i0]; a.r = cc0 & 0xFF; a.g = (cc0 >> 8) & 0xFF; a.b = (cc0 >> 16) & 0xFF;
            b.x = px[i1]; b.y = py[i1]; b.r = cc1 & 0xFF; b.g = (cc1 >> 8) & 0xFF; b.b = (cc1 >> 16) & 0xFF;
            c.x = px[i2]; c.y = py[i2]; c.r = cc2 & 0xFF; c.g = (cc2 >> 8) & 0xFF; c.b = (cc2 >> 16) & 0xFF;
            emit_clipped_gouraud(a, b, c, x0, y0, x1, y1, depth);
        }
    }
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
    fprintf(stderr, "[portal-dump f=%u] %s a0=%08X colour=%06X clip x[%d,%d] y[%d,%d] "
                    "OTdepth=%d matchedViewZ=%.0f nativeZ=%.3f  prims=%u bytes\n",
            openpete_frame, which, a0, col, rx0, rx1, ry0, ry1,
            (int32_t)cpu->read_word(D_PORTAL_DEPTH), vz,
            (double)openpete_native_depth_from_viewz((float)(vz > 1.0 ? vz : 16384.0)),
            (end > start) ? (end - start) : 0);
    /* For the dome leaf (F4BC, a0=skybox), capture everything the float walk
     * needs: the GTE transform it projected with (post-call), the skybox sector
     * table, sector0's cyclorama-format header, and the first emitted prims. */
    if (which[0] == 'F' && which[1] == '4' && a0 >= 0x80010000u && a0 < 0x80200000u) {
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

/* Generic leaf hook: run the real renderer, capturing the polybuf range, then
 * dump. A re-entry guard keeps a nested call (shouldn't happen for leaves)
 * from re-dumping. */
static int s_leaf_in = 0;
static int leaf_hook(CPUState* cpu, const char* which, void (*real)(CPUState*), int is_dome) {
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
        /* Far/medium + the close base-fill: one flat quad of the destination
         * sky's base colour, clipped to the arch rect (feed_portal_quad picks the
         * colour vs skybox+0x18). The DOME leaf (func_8004F4BC, a0=skybox) ALSO
         * walks the gouraud gradient dome on top — clouds/horizon when near. */
        feed_portal_quad(cpu, a0);
        if (is_dome && a0 >= 0x80010000u && a0 < 0x80200000u)
            feed_portal_dome(cpu, a0);
    }
    s_leaf_in = 0;
    return 1;
}

static int on_8004FEA0(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "FEA0(close)",     func_8004FEA0, 0); }
static int on_80050240(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "50240(blended)",  func_80050240, 0); }
static int on_8004F4BC(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "F4BC(secondary)", func_8004F4BC, 1); }

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
