// games/spyro-1/features/portal_native.c — native re-derivation of the PORTAL
// skybox previews (the destination-level sky shown inside homeworld portal
// arches), plus its falsify-before-ship capture/dump harness.
//
// ┌─ HIGH-FPS sub-tick spine: SUPPORTED (portal_native_reextract, 2026-06-22) ───┐
// │ Portal previews now ride the high-FPS sub-tick spine like every other        │
// │ feeder. The two original blockers are closed by a CAPTURE-AND-REPLAY design: │
// │ (1) on each canonical RenderPortals the leaf hooks record every drawn        │
// │ portal's decision (owning portal, flat/dome kind, resolved colour, fog,      │
// │ clip rect) into s_pcap[]; portal_native_reextract (in the spine's feeder     │
// │ list, after cyclorama) replays them on every intermediate present, so the    │
// │ previews are re-added each sub-tick instead of vanishing. (2) The dome       │
// │ rotation matrix is no longer read from the live GTE on the reextract (it is  │
// │ stale on the host fiber). Instead the canonical hook captures BOTH the dome  │
// │ matrix (dm_canon) and the camera matrix it was paired with (cam_canon =      │
// │ g_Camera+0x00, the proj matrix the spine interpolates); the reextract rotates│
// │ the dome by the camera's sub-tick rotation INCREMENT: ΔR = camM_interp ·     │
// │ cam_canonᵀ, dome = ΔR · dm_canon. This is exact for a pure yaw or pure pitch │
// │ change (dome and camera share the camera orientation, so the same world-space│
// │ ΔR rotates both) and only second-order off for combined rotation — a FROZEN  │
// │ portal-vs-camera delta was first-order wrong in the full-circle portal yaw   │
// │ under camera pitch and skewed the dome's horizon. Clip rect + outline depth  │
// │ come from project_world, which already reads the interpolated camera live. See│
// │ portal_native_reextract() below + the per-feeder table in                    │
// │ notes/plans/openpete-native-renderer.md, "Portal cycloramas" row.         │
// └─────────────────────────────────────────────────────────────────────────────┘
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
#include "spyro_native_reextract.h"   /* portal_native_reextract export */

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

/* Resolve the flat-fill colour a leaf carries in a0 (see feed_portal_quad): the
 * far/medium leaves pass a pointer whose [0] IS the colour (high bytes set); the
 * close DOME leaf passes the skybox struct whose colour is at +0x18. Returns the
 * raw 0x00BBGGRR (pre-fog); 0 if a0 isn't a RAM pointer. Shared by the canonical
 * feed and the sub-tick capture so both resolve the colour identically. */
static uint32_t portal_flat_raw_rgb(CPUState* cpu, uint32_t a0) {
    if (a0 < 0x80010000u || a0 >= 0x80200000u) return 0;
    uint32_t w0 = cpu->read_word(a0) & 0xFFFFFF;
    return (w0 >= 0x10000u) ? w0 : (cpu->read_word(a0 + 0x18) & 0xFFFFFF);
}

/* Capture the close-dome rotation matrix AND the camera matrix it was paired with,
 * so the sub-tick reextract can rotate the dome by the camera's sub-tick rotation
 * INCREMENT (the live GTE matrix is stale on the host fiber — see the banner).
 *   dm_canon = the matrix the leaf left in the GTE (gte_ctrl[0..4]): the camera
 *              euler ⊗ the portal's animated spin (D_80075858 yaw, D_800758FC pitch).
 *   cam_canon = g_Camera+0x00 (the proj matrix the spine interpolates) at this frame.
 * The reextract forms ΔR = camM_interp · cam_canonᵀ (the camera's world-space rotation
 * since capture) and applies dome = ΔR · dm_canon. This is EXACT for a pure yaw or
 * pure pitch change (the dome and the camera share the camera orientation, so the
 * same world-space ΔR rotates both) and only second-order off for combined rotation
 * — far better than reapplying a FROZEN portal-vs-camera delta, which is first-order
 * wrong in the (large, full-circle) portal yaw whenever the camera pitches and
 * skewed the dome's horizon. Both are Q12 (orthonormal). */
static void read_gte_rotmatrix(CPUState* cpu, int M[3][3]) {
    uint32_t w0 = cpu->gte_ctrl[0], w1 = cpu->gte_ctrl[1], w2 = cpu->gte_ctrl[2];
    uint32_t w3 = cpu->gte_ctrl[3], w4 = cpu->gte_ctrl[4];
    M[0][0] = (int16_t)(w0 & 0xFFFF); M[0][1] = (int16_t)(w0 >> 16); M[0][2] = (int16_t)(w1 & 0xFFFF);
    M[1][0] = (int16_t)(w1 >> 16);    M[1][1] = (int16_t)(w2 & 0xFFFF); M[1][2] = (int16_t)(w2 >> 16);
    M[2][0] = (int16_t)(w3 & 0xFFFF); M[2][1] = (int16_t)(w3 >> 16);  M[2][2] = (int16_t)(w4 & 0xFFFF);
}
static void capture_dome_matrices(CPUState* cpu, int dm_canon[3][3], int cam_canon[3][3]) {
    read_gte_rotmatrix(cpu, dm_canon);
    read_matrix(cpu, G_CAMERA + 0x00, cam_canon);
}

/* ---- sub-tick capture-and-replay ------------------------------------------
 * The canonical RenderPortals leaf hooks record each DRAWN portal's high-level
 * decision here (the transient clip rect / colour / GTE matrix are live only
 * during the leaf call); portal_native_reextract replays them on every
 * intermediate present, re-projecting the geometry against the interpolated
 * camera. One entry per leaf call = one flat quad (+ optional dome). */
typedef struct {
    uint32_t portal;        /* owning Portal* (outline re-projection)            */
    uint32_t flat_a0;       /* the leaf's a0 (skybox-link check; may be stale)    */
    uint32_t flat_rgb;      /* resolved raw flat colour (a0 is stale on replay)   */
    uint32_t skybox;        /* skybox ptr for the dome walk (is_dome only)        */
    int      is_dome;       /* leaf emitted a gradient dome (kind != FLAT)        */
    double   backstop_bias; /* flat depth push-back (dome backstop vs flat preview)*/
    uint32_t fog_rgb;       /* blended-medium DPCS fog target (0 if none)         */
    double   fog_t;         /* blended-medium fog lerp coeff (0 if none)          */
    int      clip[4];       /* captured D_CLIPRECT y0,y1,x0,x1 (>>16) base rect   */
    int      dm_canon[3][3];  /* captured dome rotation (gte_ctrl), Q12; is_dome only */
    int      cam_canon[3][3]; /* g_Camera+0x00 at capture (for the ΔR sub-tick rotation) */
} portal_cap_t;
#define PORTAL_CAP_MAX 16
static portal_cap_t s_pcap[PORTAL_CAP_MAX];
static int          s_pcap_n = 0;     /* entries captured this canonical frame    */
static int          s_pcap_valid = 0; /* a canonical RenderPortals has populated it*/
static unsigned     s_pcap_frame = 0; /* openpete_frame of the populating RenderPortals */
/* Per-canonical-present "RenderPortals ran" latch (staleness gate, see
 * portal_native_reextract). s_pcap_ran is set in on_80050BE0 while the canonical
 * frame renders; portal_native_commit (chained onto the host present commit) latches
 * it into s_pcap_canon_drawn and resets it once per canonical present, BEFORE the
 * sub-tick reextract reads it. When RenderPortals stops (load transition) the latch
 * goes false next present and the reextract stops replaying the stale capture. */
static int          s_pcap_ran = 0;
static int          s_pcap_canon_drawn = 0;

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

/* The portal cyclorama (the gouraud dome AND the flat far/medium preview) sits on
 * the portal OPENING PLANE. The original confines it to the arch by OT painter's
 * order — RenderPortals paints the sky OVER everything seen THROUGH the opening and
 * UNDER the arch stone — so a depth-buffer port must reproduce that with depth, and
 * two things at the opening plane wrongly win against the sky:
 *   1. INNER FRAME — many arches carry a thin decorative band at the opening plane;
 *      it z-fights the sky and the sliver flickers through the rim as the camera moves.
 *   2. THROUGH-ARCH GROUND/SILL — when close to a portal, the floor/sill seen through
 *      the LOWER opening (below the sky dome's horizon, where only the flat backstop
 *      fills) is nearer than the backstop and punches through ("ground disocclusion").
 * Both are fixed by pulling the WHOLE portal sky (dome faces, flat preview, AND the
 * backstop — see feed_portal_quad) toward the camera by this multiplicative view-Z
 * bias, so it wins against coincident/through-arch geometry while staying behind the
 * MUCH-nearer outer arch stone (which must still occlude it into the arch shape) and
 * the near-side floor. Multiplicative (not a constant offset) so the margin tracks
 * distance — depth_from_viewz compresses far depths, and a view-Z scale widens its
 * step there. Value: 0.90 is the gentlest that covers the sill (the disocclusion
 * persists at 0.95, clears at 0.90; A/B-matched to PsyCross on portal-ground-
 * disocclusion f2444); verified no regression of the sharp-angle base connection
 * (magic-crafters f2945), the close z-fight tiers (cyclorama-portal f1908), or far
 * portals (f1750). Tunable: larger risks poking through the stone. */
#define PORTAL_PLANE_FRONT_BIAS 0.90
static float portal_plane_depth_from_viewz(double vz) {
    return openpete_native_depth_from_viewz((float)(vz * PORTAL_PLANE_FRONT_BIAS));
}

/* Native depth from the portal plane at a screen point (perspective-correct via
 * the inv-vz fit), pulled forward by PORTAL_PLANE_FRONT_BIAS to win the inner-frame
 * z-fight. */
static float plane_depth(double a, double b, double c, double x, double y) {
    double inv = a * x + b * y + c;
    double vz = (inv > 1e-9) ? 1.0 / inv : 1e9;
    return portal_plane_depth_from_viewz(vz);
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
/* rgb_override == PORTAL_RGB_RESOLVE means "resolve the colour from a0" (the
 * canonical leaf path, where a0 is live); any other value is used directly (the
 * sub-tick reextract path, where a0 may be a stale stack/transient pointer so the
 * captured raw colour is supplied instead). portal_hint != 0 likewise supplies the
 * owning portal directly (captured) instead of re-matching a possibly-stale a0. */
#define PORTAL_RGB_RESOLVE 0xFFFFFFFFu
static int feed_portal_quad(CPUState* cpu, uint32_t a0, uint32_t portal_hint,
                            uint32_t rgb_override, double backstop_bias,
                            uint32_t fog_rgb, double fog_t) {
    if (rgb_override == PORTAL_RGB_RESOLVE &&
        (a0 < 0x80010000u || a0 >= 0x80200000u)) return 0;   /* canonical: a0 must be a RAM pointer */
    /* The flat-fill colour. Far/medium leaves (func_8004FEA0/80050240) pass a
     * pointer whose [0] IS the colour (stack fog colour, &D_800757D4 blend, or
     * the close-pass skybox+0x10 bg colour) — all have high bytes set. The close
     * DOME leaf (func_8004F4BC) passes the skybox struct itself, whose [0] is the
     * small sector COUNT, not a colour; there the m_BackgroundColor lives at
     * skybox+0x18. portal_flat_raw_rgb discriminates by magnitude. On the reextract
     * a0/D_800757D4 are stale, so the captured raw colour is passed in directly. */
    uint32_t rgb = (rgb_override != PORTAL_RGB_RESOLVE)
                       ? (rgb_override & 0xFFFFFF)
                       : portal_flat_raw_rgb(cpu, a0);
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
    uint32_t p = portal_hint ? portal_hint
                             : owning_portal_ptr(cpu, a0, (x0 + x1) * 0.5, (y0 + y1) * 0.5);
    /* Only a skybox-linked flat is a dome backstop (close FEA0 a0=skybox+0x10, or
     * the dome's own a0=skybox) — push it behind the dome. Far/medium previews
     * (a0 = stack/global colour) are the whole sky and stay on the plane. */
    double back_bias = 1.0;
    if (p) { uint32_t sb = cpu->read_word(p + 0x00); if (sb == a0 || sb == a0 - 0x10) back_bias = backstop_bias; }
    /* Pull the WHOLE fill forward by the front bias: the far/medium FLAT preview
     * (back_bias 1.0) so it clears the inner frame, AND the close/medium backstop
     * (back_bias>1) so the lower opening (below the dome's horizon, where only the
     * backstop fills) clears the through-arch ground/sill. The backstop keeps its
     * relative push-back (back_bias) so it still sits just behind the dome where
     * they overlap. */
    double depth_bias = back_bias * PORTAL_PLANE_FRONT_BIAS;
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
                t.v[k].z = openpete_native_depth_from_viewz((float)(v->vz * depth_bias));
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
    float depth = openpete_native_depth_from_viewz((float)(vz * depth_bias));
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
    float depth = portal_plane_depth_from_viewz(vz);   /* rect-fallback depth, front-biased */

    /* Tilted-plane window: outline polygon + its perspective-correct inv-vz depth
     * field, so each dome pixel takes the portal-plane depth (the stone frame
     * occludes it into the arch shape even at a sharp angle, f2945). Falls back to
     * the rect clip + constant depth when an outline point is behind the camera. */
    uint32_t p = owning_portal_ptr(cpu, skybox, cx, cy);
    ppt poly[16]; int pn = p ? portal_outline_screen(cpu, p, poly, 16) : 0;
    double pa = 0, pb = 0, pc = 0;
    int use_plane = (pn >= 3) && fit_invz(poly, pn, &pa, &pb, &pc);

    /* Widescreen un-clamp: the .s clip rect D_8007AA00 is the GPU scissor, clamped
     * to the 4:3 framebuffer [0,512]. For a portal at the screen edge the outline
     * (and the stone arch, which widens through scene_to_clip) extends past 512,
     * but the dome — clipped to the clamped rect — gets cut at the 4:3 boundary,
     * leaving the widened arch's outer part with no sky. Extend the rect to the
     * projected-outline bbox (still a RECT clip, so depth occlusion still shapes
     * the arch — no coverage gaps; UNION so a non-clamped 4:3 rect is unchanged →
     * no 4:3 regression, and at 4:3 any spill past 512 is GPU-scissored anyway). */
    if (pn >= 3) {
        float oxlo=poly[0].x, oxhi=poly[0].x, oylo=poly[0].y, oyhi=poly[0].y;
        for (int i = 1; i < pn; i++) {
            if (poly[i].x < oxlo) oxlo = poly[i].x; if (poly[i].x > oxhi) oxhi = poly[i].x;
            if (poly[i].y < oylo) oylo = poly[i].y; if (poly[i].y > oyhi) oyhi = poly[i].y;
        }
        if ((int)floorf(oxlo) < x0) x0 = (int)floorf(oxlo);
        if ((int)ceilf (oxhi) > x1) x1 = (int)ceilf (oxhi);
        if ((int)floorf(oylo) < y0) y0 = (int)floorf(oylo);
        if ((int)ceilf (oyhi) > y1) y1 = (int)ceilf (oyhi);
    }

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
    /* Widescreen bug probe: the CLAMPED .s scissor rect (above) vs the native
     * projected-outline bbox (which widens through scene_to_clip). If the outline
     * X extends past the rect's clamped edge, the dome/flat-fallback (clipped to
     * the rect) is cut at the 4:3 boundary while the stone arch widens → mismatch. */
    {
        uint32_t pp = owning_portal_ptr(cpu, a0, (rx0 + rx1) * 0.5, (ry0 + ry1) * 0.5);
        ppt poly[16]; int pn = pp ? portal_outline_screen(cpu, pp, poly, 16) : 0;
        if (pn >= 3) {
            float oxlo = poly[0].x, oxhi = poly[0].x, oylo = poly[0].y, oyhi = poly[0].y;
            for (int i = 1; i < pn; i++) {
                if (poly[i].x < oxlo) oxlo = poly[i].x; if (poly[i].x > oxhi) oxhi = poly[i].x;
                if (poly[i].y < oylo) oylo = poly[i].y; if (poly[i].y > oyhi) oyhi = poly[i].y;
            }
            float xlo, xhi; openpete_native_widescreen_x_bounds(&xlo, &xhi);
            fprintf(stderr, "    outline-bbox x[%.0f,%.0f] y[%.0f,%.0f] (rect x[%d,%d]) wsF=%.3f visX[%.0f,%.0f]\n",
                    oxlo, oxhi, oylo, oyhi, rx0, rx1,
                    openpete_native_widescreen_factor(), xlo, xhi);
        }
    }
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
/* Portals a leaf actually rendered this frame (the on-4:3 set), so the widescreen
 * off-4:3 reveal (feed_revealed_portals) doesn't double-draw them. Reset per frame
 * in on_80050BE0 before the real RenderPortals runs. */
static uint32_t s_fed_portal[8]; static int s_fed_n = 0;
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
        int is_skybox_a0 = (a0 >= 0x80010000u && a0 < 0x80200000u);
        feed_portal_quad(cpu, a0, 0, PORTAL_RGB_RESOLVE, backstop_bias, fog, fog_t);
        if (is_dome && is_skybox_a0)
            feed_portal_dome(cpu, a0, fog, fog_t);
        /* Record which portal this leaf rendered (the .s only renders on-4:3 portals)
         * so the widescreen off-4:3 reveal below doesn't double-draw it. Also the
         * sub-tick capture (replayed by portal_native_reextract every present). */
        {
            int ry0=(int32_t)cpu->read_word(D_CLIPRECT+0x0)>>16, ry1=(int32_t)cpu->read_word(D_CLIPRECT+0x4)>>16;
            int rx0=(int32_t)cpu->read_word(D_CLIPRECT+0x8)>>16, rx1=(int32_t)cpu->read_word(D_CLIPRECT+0xC)>>16;
            uint32_t fp = owning_portal_ptr(cpu, a0, (rx0+rx1)*0.5, (ry0+ry1)*0.5);
            if (fp && s_fed_n < 8) s_fed_portal[s_fed_n++] = fp;
            if (s_pcap_n < PORTAL_CAP_MAX) {
                portal_cap_t* c = &s_pcap[s_pcap_n++];
                c->portal        = fp;
                c->flat_a0       = a0;
                c->flat_rgb      = portal_flat_raw_rgb(cpu, a0);   /* raw, pre-fog */
                c->is_dome       = (is_dome && is_skybox_a0);
                c->skybox        = c->is_dome ? a0 : 0;
                c->backstop_bias = backstop_bias;
                c->fog_rgb       = fog;
                c->fog_t         = fog_t;
                c->clip[0] = ry0; c->clip[1] = ry1; c->clip[2] = rx0; c->clip[3] = rx1;
                if (c->is_dome) capture_dome_matrices(cpu, c->dm_canon, c->cam_canon);
            }
        }
    }
    s_leaf_in = 0;
    return 1;
}

static int on_8004FEA0(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "FEA0(close)",     func_8004FEA0, PORTAL_FLAT);    }
static int on_80050240(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "50240(blended)",  func_80050240, PORTAL_BLENDED); }
static int on_8004F4BC(CPUState* cpu, uint32_t a) { (void)a; return leaf_hook(cpu, "F4BC(secondary)", func_8004F4BC, PORTAL_DOME);    }

/* Widescreen off-4:3 portal reveal. The .s RenderPortals culls portals whose
 * centroid is off the 4:3 screen, so at wide AR a portal in the side margin shows
 * no destination-sky preview (the arch frames empty world). After the real
 * RenderPortals has fed the on-4:3 portals via the leaves, walk g_Portals and draw
 * a FLAT destination-sky preview for each portal that is off-4:3 but inside the
 * widened view and was NOT already fed by a leaf. Mirrors feed_portal_quad's
 * outline-polygon fill (tilted-plane per-vertex view-Z, front-biased) so the stone
 * arch occludes it into the opening shape and it occludes the world behind. Flat
 * only: a margin portal is to the SIDE, never the close one you walk into head-on
 * (that one is centred / on-4:3), so the gradient dome isn't needed here. Inert at
 * 4:3 (F=1). Display-only — reads g_Portals + camera, never writes PSX RAM. */
static void feed_revealed_portals(CPUState* cpu) {
    float F = openpete_native_widescreen_factor();
    if (F <= 1.0f) return;
    float xlo, xhi; openpete_native_widescreen_x_bounds(&xlo, &xhi);
    uint32_t n = cpu->read_word(G_PORTALCOUNT); if (n > 6) n = 6;
    int revealed = 0;
    openpete_native_scene_begin_frame(openpete_frame);   /* append; world/cyclorama began it */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4);
        if (p < 0x80010000u || p >= 0x80200000u) continue;
        int fed = 0;                                     /* skip a portal a leaf already drew (on-4:3) */
        for (int k = 0; k < s_fed_n; k++) if (s_fed_portal[k] == p) { fed = 1; break; }
        if (fed) continue;
        uint32_t skybox = cpu->read_word(p + 0x00);
        if (skybox < 0x80010000u || skybox >= 0x80200000u) continue;
        ppt poly[16];
        int pn = portal_outline_screen(cpu, p, poly, 16); /* >=3 iff every outline pt is in front */
        if (pn < 3) continue;
        double cxs = 0.0; for (int k = 0; k < pn; k++) cxs += poly[k].x; cxs /= pn;
        if (cxs >= 0.0 && cxs <= 512.0) continue;         /* on 4:3 — the .s renders it */
        if (cxs < (double)xlo || cxs > (double)xhi) continue;  /* outside the widened view */
        uint32_t rgb = cpu->read_word(skybox + 0x18) & 0xFFFFFF;   /* destination sky bg colour */
        uint32_t rgba = rgb | 0xFF000000u;
        for (int k = 1; k + 1 < pn; k++) {                /* fan-triangulate the outline polygon */
            openpete_scene_tri t;
            memset(&t, 0, sizeof t);
            t.textured = 0; t.blend = 0; t.category = OPENPETE_SCENE_WORLD;
            const int idx3[3] = { 0, k, k + 1 };
            for (int j = 0; j < 3; j++) {
                const ppt* v = &poly[idx3[j]];
                t.v[j].sx = v->x; t.v[j].sy = v->y;
                t.v[j].z = openpete_native_depth_from_viewz((float)(v->vz * PORTAL_PLANE_FRONT_BIAS));
                t.v[j].rgba = rgba;
            }
            openpete_native_scene_add_tri(&t);
        }
        revealed++;
        /* Capture this off-4:3 reveal for the sub-tick replay too (else it
         * flickers like the on-4:3 leaves did). Flat, portal-plane, no fog; the
         * base clip rect is the projected-outline bbox (RenderPortals set no
         * D_CLIPRECT for a culled portal). feed_portal_quad re-projects the
         * outline against the interpolated camera on replay. */
        if (s_pcap_n < PORTAL_CAP_MAX) {
            float oxlo=poly[0].x, oxhi=poly[0].x, oylo=poly[0].y, oyhi=poly[0].y;
            for (int k = 1; k < pn; k++) {
                if (poly[k].x < oxlo) oxlo = poly[k].x; if (poly[k].x > oxhi) oxhi = poly[k].x;
                if (poly[k].y < oylo) oylo = poly[k].y; if (poly[k].y > oyhi) oyhi = poly[k].y;
            }
            portal_cap_t* c = &s_pcap[s_pcap_n++];
            c->portal        = p;
            c->flat_a0       = 0;
            c->flat_rgb      = rgb;
            c->is_dome       = 0;
            c->skybox        = 0;
            c->backstop_bias = 1.0;
            c->fog_rgb       = 0;
            c->fog_t         = 0.0;
            c->clip[0] = (int)floorf(oylo); c->clip[1] = (int)ceilf(oyhi);
            c->clip[2] = (int)floorf(oxlo); c->clip[3] = (int)ceilf(oxhi);
        }
    }
    static int s_logged = 0;
    if (!s_logged && revealed > 0) {
        s_logged = 1;
        fprintf(stderr, "[portal-reveal] frame=%u F=%.3f drew %d off-4:3 portal preview(s) "
                "the .s culled\n", openpete_frame, F, revealed);
    }
}

static int s_rp_in = 0;
static int on_80050BE0(CPUState* cpu, uint32_t a) {
    (void)a;
    int feed = openpete_native_render_enabled();
    if (!portal_dump_enabled() && !feed && !render_paths_active()) return 0;
    if (s_rp_in) return 0;
    s_rp_in = 1;
    s_fed_n = 0;                              /* reset the per-frame on-4:3 fed-portal dedup set */
    s_pcap_n = 0;                             /* reset the per-frame sub-tick capture set */
    static int s_reports = 0;
    if (s_want_dump_this_frame() && s_reports < 8) { dump_portal_table(cpu); s_reports++; }
    func_80050BE0(cpu);                       /* real RenderPortals: on-4:3 portals fed via leaves */
    if (feed) feed_revealed_portals(cpu);     /* widescreen: off-4:3 portals the .s culled */
    /* The leaves + reveal have populated s_pcap[] this canonical frame; mark it
     * usable so portal_native_reextract replays it on the intermediate presents.
     * (s_pcap_n may be 0 in a portal-less level — the reextract then no-ops.) */
    if (feed) { s_pcap_valid = 1; s_pcap_frame = openpete_frame; s_pcap_ran = 1; }
    s_rp_in = 0;
    return 1;
}

/* Chained host-present commit: latch "RenderPortals ran this canonical present"
 * (s_pcap_ran, set above during the game render) into s_pcap_canon_drawn and reset
 * it, then call the previously-registered commit (render_paths'). Runs once per
 * canonical present, after the game render, before the sub-tick reextract — so the
 * reextract's staleness gate sees this present's verdict, not last present's. */
static openpete_native_commit_fn s_prev_commit = NULL;
static void portal_native_commit(void) {
    s_pcap_canon_drawn = s_pcap_ran;
    s_pcap_ran = 0;
    if (s_prev_commit) s_prev_commit();
}

/* Chain our commit onto whatever's already registered (render_paths registers
 * earlier in the feature list, so get_commit returns its fn here). */
static void portal_native_apply_options(const openpete_options_t* opts) {
    (void)opts;
    s_prev_commit = openpete_native_get_commit();
    openpete_native_register_commit(portal_native_commit);
}

/* ---- sub-tick re-extract (high-FPS spine) --------------------------------
 * Replays the captured per-portal decisions on each intermediate present,
 * re-projecting against the spine-interpolated camera. Brackets the two
 * transient inputs the feeders read from RAM/GTE (the per-portal clip rect in
 * D_8007AA00 and, for a close dome, the GTE rotation matrix) with values
 * recomposed from the capture, runs the same feed_portal_quad/feed_portal_dome
 * the canonical path uses, then restores them. Display-only: never perturbs
 * gameplay RAM, and never runs headless (the spine doesn't run headless). */
void portal_native_reextract(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;
    if (s_pcap_n == 0) return;
    /* Don't replay a STALE capture. During a level-load transition (e.g. warping
     * THROUGH a portal — "ENTERING STONE HILL…") the normal draw loop stops, so
     * canonical RenderPortals (on_80050BE0) no longer runs and s_pcap freezes at
     * the last homeworld frame (a screen-filling close portal). Replaying it paints
     * a flat 4:3 backdrop-colour quad over everything on every intermediate present.
     * (The main cyclorama IS still drawn during the transition by a non-RenderPortals
     * path, so render_path_canon_drawn(RP_CYCLORAMA) is NOT a valid proxy here —
     * proven by instrument: the stale replay kept firing with RP_CYCLORAMA true.)
     * s_pcap_canon_drawn is latched per canonical present (portal_native_commit)
     * from whether on_80050BE0 actually ran this present, so it goes false the
     * moment RenderPortals stops — exactly the staleness signal we need. */
    if (!s_pcap_canon_drawn) return;
    openpete_native_scene_begin_frame(openpete_frame);   /* append; cyclorama began it */

    /* The camera proj matrix the spine has interpolated this present (live). */
    int camI[3][3];
    read_matrix(cpu, G_CAMERA + 0x00, camI);

    /* Save the transient inputs we overwrite; restore before returning so the
     * game fiber resumes on canonical CPU/RAM state. */
    uint32_t save_clip[4], save_gte[5];
    for (int i = 0; i < 4; i++) save_clip[i] = cpu->read_word(D_CLIPRECT + i * 4);
    for (int i = 0; i < 5; i++) save_gte[i]  = cpu->gte_ctrl[i];

    for (int n = 0; n < s_pcap_n; n++) {
        const portal_cap_t* c = &s_pcap[n];
        /* Write the captured base clip rect (the feeders union it with the live
         * projected-outline bbox, so the scissor still rides the camera). */
        cpu->write_word(D_CLIPRECT + 0x0, (uint32_t)c->clip[0] << 16);
        cpu->write_word(D_CLIPRECT + 0x4, (uint32_t)c->clip[1] << 16);
        cpu->write_word(D_CLIPRECT + 0x8, (uint32_t)c->clip[2] << 16);
        cpu->write_word(D_CLIPRECT + 0xC, (uint32_t)c->clip[3] << 16);

        if (c->is_dome) {
            /* Rotate the captured dome by the camera's sub-tick rotation INCREMENT:
             * ΔR = camM_interp · cam_canonᵀ (world-space rotation since capture), then
             * dome = ΔR · dm_canon. Exact for pure yaw/pitch, second-order for combined
             * — vs the old frozen-delta recompose, which skewed the dome's horizon
             * (first-order wrong in the full-circle portal yaw under camera pitch). */
            double dR[3][3];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    double s = 0.0;
                    for (int k = 0; k < 3; k++) s += (double)camI[i][k] * (double)c->cam_canon[j][k];
                    dR[i][j] = s / (4096.0 * 4096.0);   /* camM_interp · cam_canonᵀ (real) */
                }
            int dm[3][3];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    double s = 0.0;
                    for (int k = 0; k < 3; k++) s += dR[i][k] * (double)c->dm_canon[k][j];
                    long v = (long)(s >= 0.0 ? s + 0.5 : s - 0.5);
                    if (v >  32767) v =  32767; else if (v < -32768) v = -32768;
                    dm[i][j] = (int)v;
                }
            cpu->gte_ctrl[0] = (uint32_t)(uint16_t)dm[0][0] | ((uint32_t)(uint16_t)dm[0][1] << 16);
            cpu->gte_ctrl[1] = (uint32_t)(uint16_t)dm[0][2] | ((uint32_t)(uint16_t)dm[1][0] << 16);
            cpu->gte_ctrl[2] = (uint32_t)(uint16_t)dm[1][1] | ((uint32_t)(uint16_t)dm[1][2] << 16);
            cpu->gte_ctrl[3] = (uint32_t)(uint16_t)dm[2][0] | ((uint32_t)(uint16_t)dm[2][1] << 16);
            cpu->gte_ctrl[4] = (uint32_t)(uint16_t)dm[2][2];
        }

        /* Self-check (OPENPETE_PORTAL_REEXTRACT_DBG): per-cap replay trace. For a
         * dome, a valid recomposed matrix is a rotation — each row magnitude ≈ 4096
         * (Q12); a packing/algebra bug shows wild magnitudes. Also reports the
         * captured clip rect + capture-frame so a STALE replay (canonical
         * RenderPortals stopped firing during a load transition while s_pcap kept
         * its last value) is visible as a frozen s_pcap_frame. */
        static int s_dbg = -1;
        if (s_dbg < 0) { const char* e = getenv("OPENPETE_PORTAL_REEXTRACT_DBG"); s_dbg = (e && *e && *e != '0'); }
        if (s_dbg && (openpete_frame % 20u) == 0) {
            double rowmag = 0.0;
            if (c->is_dome) {
                int dm00 = (int16_t)(cpu->gte_ctrl[0] & 0xFFFF), dm01 = (int16_t)(cpu->gte_ctrl[0] >> 16);
                int dm02 = (int16_t)(cpu->gte_ctrl[1] & 0xFFFF);
                rowmag = sqrt((double)dm00*dm00 + (double)dm01*dm01 + (double)dm02*dm02);
            }
            fprintf(stderr, "[portal-reextract f=%u capframe=%u] cap %d/%d %s p=%08X rgb=%06X clip y[%d,%d] x[%d,%d]%s\n",
                    openpete_frame, s_pcap_frame, n, s_pcap_n,
                    c->is_dome ? "DOME" : "flat", c->portal, c->flat_rgb,
                    c->clip[0], c->clip[1], c->clip[2], c->clip[3],
                    c->is_dome ? "" : "");
            if (c->is_dome) fprintf(stderr, "    |dome row0|=%.0f (≈4096?)\n", rowmag);
        }

        /* Same emit the canonical leaf used, driven by the captured decision. */
        feed_portal_quad(cpu, c->flat_a0, c->portal, c->flat_rgb,
                         c->backstop_bias, c->fog_rgb, c->fog_t);
        if (c->is_dome)
            feed_portal_dome(cpu, c->skybox, c->fog_rgb, c->fog_t);
    }

    for (int i = 0; i < 4; i++) cpu->write_word(D_CLIPRECT + i * 4, save_clip[i]);
    for (int i = 0; i < 5; i++) cpu->gte_ctrl[i] = save_gte[i];
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
    .apply_options  = portal_native_apply_options,
    .dispatch_table = portal_dispatch,
};
