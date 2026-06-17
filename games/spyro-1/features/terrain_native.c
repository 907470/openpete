// terrain_native.c — native world/terrain renderer (Phase 4).
//
// Format/contract map: the format notes.
//
// The world renderer is func_800258F0 (-> func_800276A0 fallthrough), dispatched
// per frame by func_8002B9CC (PrepareEnvironmentRendering). It is camera-relative
// (GTE rotation = g_Camera+0x14, TR=0), projects each visible sector's 11/11/10
// packed vertices via RTPS, frustum-culls sectors (writing the D_800771C8
// visibility VERDICT), walks LP (8B) / HP (16B) face lists, and emits fogged
// GT4/GT3 (+ flat G3) prims for the whole environment.
//
// This module currently provides the CAPTURE + DUMP harness (the Phase-4 walk's
// step 1): it hooks func_8002B9CC, runs the real renderer once (gameplay writes —
// sector visibility — happen identically), brackets the polybuf to capture the
// .s prims as ground truth, live-decodes g_Environment + visible sector headers
// to confirm the format map against real data, and records RP_TERRAIN so
// --render-paths can isolate terrain. The native gamestate walk + scene feed are
// the next increment (a TODO stub below).
//
// Determinism: display-only. It never writes PSX RAM (it READS gamestate + the
// captured polybuf). func_8002B9CC/func_800258F0 keep running canonical-4:3 as
// the verdict producer; the native path will read g_Camera/g_Environment
// read-only and cull a wide frustum host-side. See the doc §0/§6.
//
// Gated by OPENPETE_TERRAIN_VERIFY (dump/A/B) or a native renderer mode (feed).

#include "feature_registry.h"
#include "psx_runtime.h"   /* full CPUState (read_word, gte_ctrl, ...) */
#include "openpete_native_render.h"
#include "render_paths.h"  /* --render-paths isolation */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void     func_800258F0(CPUState* cpu);  /* the real world renderer */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map / recompiled C / r_environment.s) -------- */
#define G_ENVIRONMENT  0x800785A8u   /* Environment struct (see offsets below)   */
#define G_CAMERA       0x80076DD0u   /* m_ProjectionMatrix +0x00, m_Position +0x28 */
#define D_VISIBILITY   0x800771C8u   /* per-sector visibility VERDICT (1 byte/sector) */
#define POLYBUF_CURSOR 0x800757B0u   /* shared polybuf write cursor (holds a ptr) */
#define D_SONY         0x8006FCF4u   /* g_SonyImage scratch; HP queue +0x1C00, LP +0x2000 */

/* Environment field offsets (environment.h:60-73) */
#define ENV_SECTORPTR   0x00u
#define ENV_SECTORCNT   0x04u
#define ENV_OCCGROUPS   0x08u
#define ENV_OCCGROUPCNT 0x0Cu
#define ENV_LQTEX       0x18u
#define ENV_HQTEX       0x1Cu
#define ENV_TEXCNT      0x20u
#define ENV_LODDIST     0x24u
#define ENV_CULLDIST    0x28u

/* ---- captured prims (ground truth) --------------------------------------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1]>>24) */
    uint8_t  len;           /* OT tag length (word[0]>>24)      */
    int      nv;            /* 3 or 4                           */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
} prim_t;

/* Terrain emits the whole world — store a bounded sample for dumps, but the
 * histogram counts every prim. */
#define MAX_PRIMS 8192
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
static int    s_code_hist[256];
static int    s_prim_total;

/* Walk the polybuf [start,end) by OT tag length (stride=((w0>>24)+1)*4); decode
 * each prim by its code byte (word[1]>>24) with the general PSX polygon decode
 * (bit4 gouraud / bit3 quad / bit2 textured). Same scheme moby/Spyro/flame use. */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_gte_n = 0;
    s_prim_total = 0;
    memset(s_code_hist, 0, sizeof(s_code_hist));
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < (MAX_PRIMS * 8)) {
        uint32_t w0  = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFF;
        if (len == 0 || len > 24) break;          /* not a tag / corrupt — stop */
        uint32_t stride = (len + 1) * 4;
        uint8_t  code = (uint8_t)(cpu->read_word(p + 4) >> 24);
        s_code_hist[code]++;
        s_prim_total++;
        if (s_gte_n < MAX_PRIMS && (code & 0xE0) == 0x20) {
            uint32_t raw[20];
            int nraw = (int)(stride / 4); if (nraw > 20) nraw = 20;
            for (int i = 0; i < nraw; i++) raw[i] = cpu->read_word(p + (uint32_t)i * 4);
            prim_t* t = &s_gte[s_gte_n];
            memset(t, 0, sizeof(*t));
            t->code = code; t->len = (uint8_t)len;
            int gouraud = (code & 0x10) != 0;
            int quad    = (code & 0x08) != 0;
            int tex     = (code & 0x04) != 0;
            t->gouraud = gouraud; t->textured = tex; t->quad = quad;
            t->nv = quad ? 4 : 3;
            uint32_t rgb0 = raw[1] & 0xFFFFFF;
            int c = 2;
            for (int k = 0; k < t->nv; k++) {
                t->rgb[k] = (gouraud && k > 0) ? (raw[c++] & 0xFFFFFF) : rgb0;
                uint32_t xy = raw[c++];
                t->x[k] = (float)(int16_t)(xy & 0xFFFF);
                t->y[k] = (float)(int16_t)(xy >> 16);
                if (tex) {
                    uint32_t uv = raw[c++];
                    t->u[k] = (int)(uv & 0xFF); t->v[k] = (int)((uv >> 8) & 0xFF);
                    if (k == 0)      t->clut  = (int)(uv >> 16);
                    else if (k == 1) t->tpage = (int)(uv >> 16);
                }
            }
            s_gte_n++;
        }
        p += stride;
    }
}

/* ---- live gamestate decode (validates the format map vs real data) -------- */

static uint32_t rd(CPUState* cpu, uint32_t a) { return cpu->read_word(a); }

static void terrain_dump(CPUState* cpu) {
    uint32_t sectorPtr = rd(cpu, G_ENVIRONMENT + ENV_SECTORPTR);
    uint32_t sectorCnt = rd(cpu, G_ENVIRONMENT + ENV_SECTORCNT);
    uint32_t occCnt    = rd(cpu, G_ENVIRONMENT + ENV_OCCGROUPCNT);
    uint32_t texCnt    = rd(cpu, G_ENVIRONMENT + ENV_TEXCNT);
    uint32_t lqTex     = rd(cpu, G_ENVIRONMENT + ENV_LQTEX);
    uint32_t hqTex     = rd(cpu, G_ENVIRONMENT + ENV_HQTEX);
    int lodDist        = (int)rd(cpu, G_ENVIRONMENT + ENV_LODDIST);
    int cullDist       = (int)rd(cpu, G_ENVIRONMENT + ENV_CULLDIST);
    int camX = (int)rd(cpu, G_CAMERA + 0x28);
    int camY = (int)rd(cpu, G_CAMERA + 0x2C);
    int camZ = (int)rd(cpu, G_CAMERA + 0x30);

    fprintf(stderr,
        "[terrain] frame=%u  sectors=%u occGroups=%u texCnt=%u  LQtex=%08X HQtex=%08X\n"
        "          lodDist=%X cullDist=%X  cam=(%d,%d,%d) cam/16=(%d,%d,%d)\n"
        "          prims: total=%d decoded=%d  codes:",
        openpete_frame, sectorCnt, occCnt, texCnt, lqTex, hqTex,
        lodDist, cullDist, camX, camY, camZ, camX >> 4, camY >> 4, camZ >> 4,
        s_prim_total, s_gte_n);
    for (int i = 0; i < 256; i++)
        if (s_code_hist[i]) fprintf(stderr, " %02X:%d", i, s_code_hist[i]);
    fprintf(stderr, "\n");

    /* Decode the first few VISIBLE sectors (D_800771C8[i]==0xFF) to confirm the
     * header count-prefix + the 11/11/10 HP vertex format against live data. */
    int shown = 0;
    for (uint32_t i = 0; i < sectorCnt && shown < 3; i++) {
        uint8_t vis = (uint8_t)(rd(cpu, (D_VISIBILITY + i) & ~3u) >> ((i & 3) * 8));
        if (vis != 0xFF) continue;
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t w0 = rd(cpu, s + 0x00), w1 = rd(cpu, s + 0x04);
        uint32_t og = rd(cpu, s + 0x08);     /* originX<<16 | originY */
        uint32_t oz = rd(cpu, s + 0x0C);     /* originZ<<16 | flistId */
        uint32_t lpw = rd(cpu, s + 0x10), hpw = rd(cpu, s + 0x14);
        int centreX = (int)(w0 >> 16), centreY = (int)(w0 & 0xFFFF), centreZ = (int)(w1 >> 16);
        int radius  = (int)(w1 & 0x1FFF), flags = (int)(w1 & 0xE000);
        int originX = (int)(og >> 16), originY = (int)(og & 0xFFFF), originZ = (int)(oz >> 16);
        int nLpV = lpw & 0xFF, nLpC = (lpw >> 8) & 0xFF, nLpF = (lpw >> 16) & 0xFF;
        int nHpV = hpw & 0xFF, nHpC = (hpw >> 8) & 0xFF, nHpF = (hpw >> 16) & 0xFF;
        int hpVertsOff = (int)((hpw >> 24) & 0xFF);   /* ×4 = byte offset to HP verts */
        fprintf(stderr,
            "  sector[%u] @%08X  centre=(%d,%d,%d) r=%d flags=%04X  origin=(%d,%d,%d)\n"
            "            LP{v=%d c=%d f=%d}  HP{v=%d c=%d f=%d hpVertsOff=%d(*4=%d)}\n",
            i, s, centreX, centreY, centreZ, radius, flags, originX, originY, originZ,
            nLpV, nLpC, nLpF, nHpV, nHpC, nHpF, hpVertsOff, hpVertsOff * 4);
        /* Decode the first 2 HP verts: 11/11/10, world/16 = origin_field + offset. */
        uint32_t hpVerts = s + 0x1C + (uint32_t)(hpVertsOff * 4);
        for (int k = 0; k < 2 && k < nHpV; k++) {
            uint32_t v = rd(cpu, hpVerts + (uint32_t)k * 4);
            int vx = (int)((v >> 21) & 0x7FF), vy = (int)((v >> 10) & 0x7FF), vz = (int)(v & 0x3FF);
            fprintf(stderr,
                "            hpVert[%d]=%08X  off=(%d,%d,%d)  world/16=(%d,%d,%d)\n",
                k, v, vx, vy, vz, originX + vx, originY - vy, originZ - vz);
        }
        shown++;
    }

    /* A few decoded sample prims (SXY / RGB / UV / tpage). */
    int samples = s_gte_n < 4 ? s_gte_n : 4;
    for (int p = 0; p < samples; p++) {
        prim_t* t = &s_gte[p];
        fprintf(stderr, "  prim[%d] code=%02X nv=%d tex=%d gour=%d  v0=(%.0f,%.0f) rgb0=%06X",
                p, t->code, t->nv, t->textured, t->gouraud, t->x[0], t->y[0], t->rgb[0]);
        if (t->textured) fprintf(stderr, "  uv0=(%d,%d) clut=%04X tpage=%04X", t->u[0], t->v[0], t->clut, t->tpage);
        fprintf(stderr, "\n");
    }
}

/* ---- native gamestate walk (float) + geometry A/B ------------------------- */

/* Camera rotation matrix g_Camera+0x14 (5 words, standard ctc2 packing). */
static void read_matrix(CPUState* cpu, uint32_t p, int M[3][3]) {
    uint32_t w0 = rd(cpu, p + 0), w1 = rd(cpu, p + 4), w2 = rd(cpu, p + 8);
    uint32_t w3 = rd(cpu, p + 12), w4 = rd(cpu, p + 16);
    M[0][0] = (int16_t)(w0 & 0xFFFF); M[0][1] = (int16_t)(w0 >> 16); M[0][2] = (int16_t)(w1 & 0xFFFF);
    M[1][0] = (int16_t)(w1 >> 16);    M[1][1] = (int16_t)(w2 & 0xFFFF); M[1][2] = (int16_t)(w2 >> 16);
    M[2][0] = (int16_t)(w3 & 0xFFFF); M[2][1] = (int16_t)(w3 >> 16);  M[2][2] = (int16_t)(w4 & 0xFFFF);
}

/* Float RTPS mirror, TR=0: MAC = (M*V)>>12 ; SX = OFX + MAC1*H/MAC3. The
 * LP-world/16 vs HP-world/4 scale cancels in the MAC1/MAC3 ratio, so V can be
 * built in any consistent unit (we use world/16). */
static double project(const int M[3][3], int vx, int vy, int vz,
                      double ofx, double ofy, double H, double* sx, double* sy) {
    double m1 = (M[0][0]*(double)vx + M[0][1]*(double)vy + M[0][2]*(double)vz) / 4096.0;
    double m2 = (M[1][0]*(double)vx + M[1][1]*(double)vy + M[1][2]*(double)vz) / 4096.0;
    double m3 = (M[2][0]*(double)vx + M[2][1]*(double)vy + M[2][2]*(double)vz) / 4096.0;
    double z = (m3 != 0.0) ? m3 : 1.0;
    *sx = ofx + m1 * H / z;
    *sy = ofy + m2 * H / z;
    return m3;
}

#define MAX_TVERTS 16384
static float s_tvx[MAX_TVERTS], s_tvy[MAX_TVERTS];
static double s_tvz[MAX_TVERTS];
static int s_tv_n;
static int s_walk_sectors;

/* Built triangles for the scene feed. */
typedef struct { float sx[3], sy[3]; double vz[3]; uint32_t rgb[3]; } ttri;
#define MAX_TTRIS 24576
static ttri s_tri[MAX_TTRIS]; static int s_tri_n;
static int s_dbg_faces, s_dbg_oob, s_dbg_nclip, s_dbg_behind, s_dbg_spike;  /* diagnostics */
static int s_cull = -1, s_flat = -1;              /* debug toggles (env) */
static void terrain_dbg_init(void) {
    /* Backface cull is OFF by default: a native Z-buffer hides occluded
     * backfaces, so emit-all gives full coverage. The .s NCLIP-culls for GPU
     * perf; replicating it dropped ~half the faces (front faces too, on the
     * sign/flip convention) → gaps. Re-enable for debug / once the NCLIP sign+
     * flip is nailed as a perf optimization. */
    if (s_cull < 0) { const char* e = getenv("OPENPETE_TERRAIN_CULL"); s_cull = (e && *e && *e != '0'); }
    if (s_flat < 0) { const char* e = getenv("OPENPETE_TERRAIN_FLAT"); s_flat = (e && *e && *e != '0'); }
}

/* Scaffold colour: NN each native vertex to the nearest captured prim vertex's
 * RGB. Temporary (the .s prim colour is already DPCS-fogged) — to be replaced by
 * native RGB888 + DPCS fog (the two colour tables $t7/$s0 + DPCS, §10) next
 * increment. Tracked in the Native-derivation backlog. */
static float s_ccx[MAX_PRIMS * 4], s_ccy[MAX_PRIMS * 4];
static uint32_t s_ccrgb[MAX_PRIMS * 4]; static int s_cc_n;
static void build_captured_colors(void) {
    s_cc_n = 0;
    for (int p = 0; p < s_gte_n && s_cc_n < MAX_PRIMS * 4 - 4; p++) {
        prim_t* t = &s_gte[p];
        for (int k = 0; k < t->nv; k++) {
            s_ccx[s_cc_n] = t->x[k]; s_ccy[s_cc_n] = t->y[k]; s_ccrgb[s_cc_n] = t->rgb[k];
            s_cc_n++;
        }
    }
}
static uint32_t nn_color(float sx, float sy) {
    float best = 1e18f; uint32_t rgb = 0x808080;
    for (int j = 0; j < s_cc_n; j++) {
        float dx = sx - s_ccx[j], dy = sy - s_ccy[j], d = dx * dx + dy * dy;
        if (d < best) { best = d; rgb = s_ccrgb[j]; }
    }
    return rgb;
}

/* Per-sector local projected vertex (indexed by face vertex index). */
typedef struct { float sx, sy; double vz; int ok; } lvert;

/* Emit one triangle (scaffold colour + near-plane spike cull). */
static void emit_tri(const lvert* sv, int a, int b, int c) {
    if (s_tri_n >= MAX_TTRIS) return;
    /* Near-plane spike cull: a vertex just in front of the camera makes float
     * H/Z explode → a tri spiking across the screen (the GTE clamps these). */
    if (fabsf(sv[a].sx) > 768.f || fabsf(sv[a].sy) > 640.f ||
        fabsf(sv[b].sx) > 768.f || fabsf(sv[b].sy) > 640.f ||
        fabsf(sv[c].sx) > 768.f || fabsf(sv[c].sy) > 640.f) { s_dbg_spike++; return; }
    ttri* t = &s_tri[s_tri_n++];
    t->sx[0]=sv[a].sx; t->sy[0]=sv[a].sy; t->vz[0]=sv[a].vz;
    t->sx[1]=sv[b].sx; t->sy[1]=sv[b].sy; t->vz[1]=sv[b].vz;
    t->sx[2]=sv[c].sx; t->sy[2]=sv[c].sy; t->vz[2]=sv[c].vz;
    t->rgb[0]=nn_color(sv[a].sx,sv[a].sy);
    t->rgb[1]=nn_color(sv[b].sx,sv[b].sy);
    t->rgb[2]=nn_color(sv[c].sx,sv[c].sy);
}

/* Walk every VISIBLE sector (D_800771C8[i]==0xFF — the .s's own canonical
 * visible set, isolating decode+projection+faces from the cull), project its HP
 * vertices (LP for 0x4000 "no-HP" sectors), then walk the HP faces (16B) / LP
 * faces (8B) into triangles. Camera-relative GTE-vector per r_environment.s
 * 0x80026238-0x800262C0 (matrix = g_Camera+0x00; world axes = origin+field). */
static void terrain_build(CPUState* cpu) {
    s_tv_n = 0; s_tri_n = 0; s_walk_sectors = 0;
    s_dbg_faces = s_dbg_oob = s_dbg_nclip = s_dbg_behind = s_dbg_spike = 0;
    terrain_dbg_init();
    build_captured_colors();
    int M[3][3]; read_matrix(cpu, G_CAMERA + 0x00, M);   /* m_ProjectionMatrix */
    double ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    double ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    double H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
    int camx = (int32_t)rd(cpu, G_CAMERA + 0x28) >> 4;
    int camy = (int32_t)rd(cpu, G_CAMERA + 0x2C) >> 4;
    int camz = (int32_t)rd(cpu, G_CAMERA + 0x30) >> 4;

    static lvert sv[256];
    uint32_t sectorPtr = rd(cpu, G_ENVIRONMENT + ENV_SECTORPTR);
    uint32_t sectorCnt = rd(cpu, G_ENVIRONMENT + ENV_SECTORCNT);
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint8_t vis = (uint8_t)(rd(cpu, (D_VISIBILITY + i) & ~3u) >> ((i & 3) * 8));
        if (vis != 0xFF) continue;
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t w1  = rd(cpu, s + 0x04);
        uint32_t og  = rd(cpu, s + 0x08), oz = rd(cpu, s + 0x0C);
        uint32_t lpw = rd(cpu, s + 0x10), hpw = rd(cpu, s + 0x14);
        int flags   = (int)(w1 & 0xE000);
        int originX = (int)(og >> 16), originY = (int)(og & 0xFFFF), originZ = (int)(oz >> 16);
        int s4 = originX - camx, s5 = camy - originY, s6 = camz - originZ;

        int noHP = (flags & 0x4000) != 0;
        int nLpV = lpw & 0xFF, nLpC = (lpw >> 8) & 0xFF, nLpF = (lpw >> 16) & 0xFF;
        int nHpV = hpw & 0xFF, nHpC = (hpw >> 8) & 0xFF, nHpF = (hpw >> 16) & 0xFF;
        int hpVertsOff = (int)((hpw >> 24) & 0xFF);

        uint32_t verts, faces; int nv, nf, faceStride;
        if (noHP) {
            verts = s + 0x1C;                                       nv = nLpV;
            faces = s + 0x1C + (uint32_t)(nLpV + nLpC) * 4;         nf = nLpF; faceStride = 8;
        } else {
            verts = s + 0x1C + (uint32_t)hpVertsOff * 4;            nv = nHpV;
            faces = verts + (uint32_t)(nHpV + nHpC * 2) * 4;        nf = nHpF; faceStride = 16;
        }
        if (nv > 256) nv = 256;
        s_walk_sectors++;

        for (int k = 0; k < nv; k++) {
            uint32_t v = rd(cpu, verts + (uint32_t)k * 4);
            int Xf = (int)((v >> 21) & 0x7FF), Yf = (int)((v >> 10) & 0x7FF), Zf = (int)(v & 0x3FF);
            double sx, sy;
            double m3 = project(M, s5 - Yf, s6 - Zf, Xf + s4, ofx, ofy, H, &sx, &sy);
            sv[k].sx = (float)sx; sv[k].sy = (float)sy; sv[k].vz = m3; sv[k].ok = (m3 > 0.0);
            if (m3 > 0.0 && s_tv_n < MAX_TVERTS) {       /* feed the geometry A/B */
                s_tvx[s_tv_n] = (float)sx; s_tvy[s_tv_n] = (float)sy; s_tvz[s_tv_n] = m3; s_tv_n++;
            }
        }

        for (int f = 0; f < nf && s_tri_n < MAX_TTRIS - 2; f++) {
            uint32_t w0 = rd(cpu, faces + (uint32_t)f * faceStride);
            int i0, i1, i2, i3, flip;
            if (faceStride == 16) {            /* HP: byte-packed indices, word2 flags */
                i0 = (w0 >> 24) & 0xFF; i1 = (w0 >> 16) & 0xFF;
                i2 = (w0 >> 8) & 0xFF;  i3 = w0 & 0xFF;
                uint32_t w2 = rd(cpu, faces + (uint32_t)f * faceStride + 8);
                flip = (w2 & 0x2) != 0;
            } else {                            /* LP: 6-bit indices in word0 */
                i0 = (int)((w0 >> 23) & 0x1F8) >> 3; i1 = (int)((w0 >> 17) & 0x1F8) >> 3;
                i2 = (int)((w0 >> 11) & 0x1F8) >> 3; i3 = (int)((w0 >> 5) & 0x1F8) >> 3;
                flip = 0;
            }
            if (i0 >= nv || i1 >= nv || i2 >= nv || i3 >= nv) { s_dbg_oob++; continue; }
            if (!sv[i0].ok || !sv[i1].ok || !sv[i2].ok) { s_dbg_behind++; continue; }
            int quad = (i2 != i3);
            /* NCLIP back-face cull (2D cross product) on the quad as a whole,
             * honoring the flip bit. Gate ONCE — then emit BOTH tris with
             * consistent winding (i0,i1,i2)+(i1,i2,i3). Per-tri NCLIP dropped
             * the 2nd tri of every front-facing quad (reversed winding) → holes
             * everywhere (the "missing tris" symptom). */
            s_dbg_faces++;
            double nclip = (sv[i1].sx - sv[i0].sx) * (sv[i2].sy - sv[i0].sy)
                         - (sv[i2].sx - sv[i0].sx) * (sv[i1].sy - sv[i0].sy);
            if (flip) nclip = -nclip;
            if (s_cull && nclip <= 0.0) { s_dbg_nclip++; continue; }
            /* Quad triangulation = FAN from i0 (diagonal i0-i2): (i0,i1,i2) +
             * (i0,i2,i3). The strip split (i0,i1,i2)+(i1,i2,i3) shares EDGE
             * i1-i2 which is a quad edge, not a diagonal, for this winding — it
             * left a triangular gap inside every quad (the "missing tris"). */
            emit_tri(sv, i0, i1, i2);
            if (quad && sv[i3].ok) emit_tri(sv, i0, i2, i3);
        }
    }
}

/* Feed the built tris to the native scene (untextured gouraud, opaque). Scaffold
 * colour (above); textures + DPCS fog + ABE are the next increment. */
static void terrain_feed_native(void) {
    for (int i = 0; i < s_tri_n; i++) {
        ttri* t = &s_tri[i];
        openpete_scene_tri tri; memset(&tri, 0, sizeof(tri));
        tri.textured = 0; tri.blend = 0;
        for (int k = 0; k < 3; k++) {
            tri.v[k].sx = t->sx[k]; tri.v[k].sy = t->sy[k];
            /* Terrain view-Z is in world/16 units (cam>>4, verts /16); the
             * actors feed FULL-scale view-Z. depth_from_viewz expects ONE shared
             * scale, so ×16 to match — else terrain's depth is ~16× too small
             * (always nearest) and draws on top of every actor. */
            tri.v[k].z  = openpete_native_depth_from_viewz((float)t->vz[k] * 16.0f);
            tri.v[k].rgba = s_flat ? 0xFF20C040u : (0xFF000000u | (t->rgb[k] & 0xFFFFFFu));
        }
        openpete_native_scene_add_tri(&tri);
    }
}

/* Geometry A/B: NN-match each native projected vertex to the nearest captured
 * prim vertex (order/topology independent — the .s emits LP for far sectors, so
 * exact ordering won't line up; NN measures the decode+projection). */
static void terrain_compare(void) {
    /* Gather captured prim verts. */
    static float cx[MAX_PRIMS * 4], cy[MAX_PRIMS * 4]; int cn = 0;
    for (int p = 0; p < s_gte_n && cn < MAX_PRIMS * 4 - 4; p++) {
        prim_t* t = &s_gte[p];
        for (int k = 0; k < t->nv; k++) { cx[cn] = t->x[k]; cy[cn] = t->y[k]; cn++; }
    }
    if (!cn || !s_tv_n) {
        fprintf(stderr, "[terrain-ab] native=%d captured=%d (no compare)\n", s_tv_n, cn);
        return;
    }
    static float nn[MAX_TVERTS]; int nnn = 0;
    static float nn_on[MAX_TVERTS]; int nnon = 0;   /* on-screen subset */
    double sum = 0; float worst = 0; int within1 = 0, within2 = 0;
    for (int i = 0; i < s_tv_n; i++) {
        float best = 1e9f;
        for (int j = 0; j < cn; j++) {
            float dx = s_tvx[i] - cx[j], dy = s_tvy[i] - cy[j];
            float d = dx * dx + dy * dy;
            if (d < best) best = d;
        }
        best = (float)sqrt((double)best);
        sum += best; if (best > worst) worst = best;
        if (best <= 1.0f) within1++;
        if (best <= 2.0f) within2++;
        nn[nnn++] = best;
        if (s_tvx[i] >= -16.f && s_tvx[i] <= 528.f && s_tvy[i] >= -16.f && s_tvy[i] <= 272.f)
            nn_on[nnon++] = best;
    }
    /* insertion sort (small N, once per dump) for percentiles */
    for (int a = 1; a < nnn; a++) { float v = nn[a]; int b = a - 1;
        while (b >= 0 && nn[b] > v) { nn[b+1] = nn[b]; b--; } nn[b+1] = v; }
    float med = nn[nnn/2], p90 = nn[(nnn*9)/10];
    /* on-screen median */
    for (int a = 1; a < nnon; a++) { float v = nn_on[a]; int b = a - 1;
        while (b >= 0 && nn_on[b] > v) { nn_on[b+1] = nn_on[b]; b--; } nn_on[b+1] = v; }
    float med_on = nnon ? nn_on[nnon/2] : 0.f;
    fprintf(stderr,
        "[terrain-ab] sectors=%d nativeV=%d tris=%d capturedV=%d  mean=%.2f median=%.2f p90=%.1f worst=%.1f"
        "  <=1px=%.0f%% <=2px=%.0f%%  onscreen(n=%d med=%.2f)\n",
        s_walk_sectors, s_tv_n, s_tri_n, cn, sum / s_tv_n, med, p90, worst,
        100.0 * within1 / s_tv_n, 100.0 * within2 / s_tv_n, nnon, med_on);
    fprintf(stderr, "[terrain-faces] walked=%d oob=%d behind=%d nclipCulled=%d spikeCulled=%d emitted=%d\n",
        s_dbg_faces, s_dbg_oob, s_dbg_behind, s_dbg_nclip, s_dbg_spike, s_tri_n);
}

/* ---- gating --------------------------------------------------------------- */
static int terrain_verify_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TERRAIN_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static unsigned terrain_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_TERRAIN_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

/* Hook the world renderer func_800258F0 directly (NOT the func_8002B9CC wrapper):
 * level/overlay draw paths call func_800258F0 directly, bypassing func_8002B9CC,
 * so a wrapper hook misses in-level frames. Widescreen also hooks 0x800258F0 but
 * falls through (returns 0) at native aspect (disp_w_factor==1), and is
 * registered before us, so our hook is reached at native/headless aspect. (In
 * widescreen mode widescreen runs the renderer + returns 1 first; the native
 * capture/feed runs at native aspect so that's fine — widescreen deletes at
 * Phase 6 anyway.) */
static int on_800258F0_terrain(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = terrain_verify_enabled();
    int feed   = openpete_native_render_enabled();
    if (!verify && !feed && !render_paths_active()) return 0;  /* inert: normal dispatch */
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_800258F0(cpu);                            /* run the real renderer (verdicts intact) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_TERRAIN, startp, endp);

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_TERRAIN)) {
        terrain_build(cpu);                         /* native walk + tris */
        openpete_native_scene_begin_frame(openpete_frame);  /* frame-keyed (composes) */
        terrain_feed_native();                      /* push tris to the scene */
    }

    if (verify && openpete_frame >= terrain_verify_frame()) {
        static int s_reports = 0;
        if (s_reports < 8) {
            terrain_dump(cpu);
            terrain_build(cpu);         /* native float decode + projection + faces */
            terrain_compare();          /* geometry A/B vs captured prims */
            s_reports++;
        }
    }
    s_in = 0;
    return 1;
}

static const feature_dispatch_entry_t terrain_dispatch[] = {
    { 0x800258F0u, on_800258F0_terrain },
    { 0u, NULL },
};

const feature_module_t terrain_native_module = {
    .name           = "terrain_native",
    .apply_options  = NULL,
    .dispatch_table = terrain_dispatch,
};
