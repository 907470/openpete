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
// This module hooks func_800258F0, runs the real renderer once (gameplay writes —
// sector visibility — happen identically), brackets the polybuf to capture the
// .s prims as the A/B oracle, then does the NATIVE walk: per visible sector it
// decodes 11/11/10 verts → float RTPS projection, walks HP (16B) / LP (8B) faces
// into tris, and computes per-vertex RGB888 + DPCS depth-cue fog from the sector
// colour arrays (terrain_color). The result is fed to the native scene
// (untextured gouraud) and numerically A/B'd vs the captured .s prims (geometry +
// colour). Textures (m_LQTexturePointer) + ABE are the next increment.
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
#include "openpete_psx.h"  /* openpete_psx_to_host_ptr (sector colour host reads) */
#include "render_paths.h"  /* --render-paths isolation */
#include "spyro_native_reextract.h"  /* sub-tick re-extract entry */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"

extern void     func_800258F0(CPUState* cpu);  /* the real world renderer */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map / recompiled C / r_environment.s) -------- */

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

    /* Tiledef table dump (OPENPETE_TERRAIN_TABLEDUMP): for each tile, show both
     * halves of the LQ table and the HQ table — find where the bpp8 (tpage mode 1)
     * detail tiles actually live (medallion = tpage 0x008E). */
    if (getenv("OPENPETE_TERRAIN_TABLEDUMP")) {
        uint32_t lq = lqTex, hq = hqTex;
        int found8_lq0=0,found8_lq8=0,found8_hq0=0,found8_hq8=0;
        for (int tile = 0; tile < 128; tile++) {
            uint32_t lq0w1 = rd(cpu, lq + tile*16 + 4), lq8w1 = rd(cpu, lq + tile*16 + 12);
            uint32_t hq0w1 = rd(cpu, hq + tile*16 + 4), hq8w1 = rd(cpu, hq + tile*16 + 12);
            int lq0m=((lq0w1>>16)>>7)&3, lq8m=((lq8w1>>16)>>7)&3;
            int hq0m=((hq0w1>>16)>>7)&3, hq8m=((hq8w1>>16)>>7)&3;
            if(lq0m==1)found8_lq0++; if(lq8m==1)found8_lq8++;
            if(hq0m==1)found8_hq0++; if(hq8m==1)found8_hq8++;
            if (tile < 8) fprintf(stderr,
                "  tile[%d] LQ[+0]tpage=%04X LQ[+8]tpage=%04X HQ[+0]tpage=%04X HQ[+8]tpage=%04X\n",
                tile, lq0w1>>16, lq8w1>>16, hq0w1>>16, hq8w1>>16);
        }
        fprintf(stderr, "[tabledump] bpp8(mode1) counts: LQ[+0]=%d LQ[+8]=%d HQ[+0]=%d HQ[+8]=%d\n",
            found8_lq0, found8_lq8, found8_hq0, found8_hq8);
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

/* Projection params (file-static so the clip/emit path can re-project clipped
 * vertices). Set per frame in terrain_build. */
static double s_ofx, s_ofy, s_H;

/* Float RTPS mirror, TR=0: camera-space MAC = (M*V)>>12. SX/SY = OFX/OFY +
 * MAC1/2*H/MAC3 (proj_screen). The LP-world/16 vs HP-world/4 scale cancels in
 * the MAC1/MAC3 ratio, so V can be built in any consistent unit (world/16). */
static void project(const int M[3][3], double vx, double vy, double vz,
                    double* m1, double* m2, double* m3) {
    *m1 = (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    *m2 = (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    *m3 = (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
}
static void proj_screen(double m1, double m2, double m3, float* sx, float* sy) {
    double z = (m3 > 1e-9) ? m3 : 1e-9;
    *sx = (float)(s_ofx + m1 * s_H / z);
    *sy = (float)(s_ofy + m2 * s_H / z);
}

/* point-in-triangle (sign test) — used only by the OPENPETE_TERRAIN_SECTORDUMP
 * coverage-gap grid to attribute cyclorama-bleed pixels to a sector. */
static int pt_in_tri(float px, float py, float ax, float ay,
                     float bx, float by, float cx, float cy) {
    float d1 = (px-bx)*(ay-by) - (ax-bx)*(py-by);
    float d2 = (px-cx)*(by-cy) - (bx-cx)*(py-cy);
    float d3 = (px-ax)*(cy-ay) - (cx-ax)*(py-ay);
    int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

#define MAX_TVERTS 16384
static float s_tvx[MAX_TVERTS], s_tvy[MAX_TVERTS];
static double s_tvz[MAX_TVERTS];
static int s_tv_n;
static int s_walk_sectors;

/* Built triangles for the scene feed. */
typedef struct {
    float sx[3], sy[3]; double vz[3]; uint32_t rgb[3];
    float u[3], v[3];                              /* page-relative texel coords */
    int textured, tpx, tpy, clx, cly, bpp, blend;  /* PSX material (VRAM px) */
    int tier;                                      /* 1=LP, 0=HP (debug tiercolor) */
} ttri;
static int s_emit_noHP = 0;     /* tier of the sector currently being emitted */
/* Local terrain-triangle cap (before the scene feed). Under --no-lod every
 * foreground cell is forced through the 2x2 HQ subdivide, so terrain peaks at
 * ~29k tris (beast_makers) vs ~9k at stock LOD; 65536 is ~2x the measured worst
 * case (headroom for worst-camera-angle). See MAX_SCENE_TRIS in native_render.c. */
#define MAX_TTRIS 65536
static ttri s_tri[MAX_TTRIS]; static int s_tri_n;
static int s_dbg_faces, s_dbg_oob, s_dbg_nclip, s_dbg_behind, s_dbg_spike;  /* diagnostics */

/* OPENPETE_TERRAIN_SECTORDUMP: per-sector coverage record for the LP↔HP gap probe */
typedef struct {
    int idx, flags, noHP, nLpV, nLpF, nHpV, nHpF, nv, nf, tris, vfront;
    int ox, oy, oz;
    double m3min, m3max;
    float bx0, by0, bx1, by1;
} secrec;
static secrec s_sd[1024]; static int s_sd_n;
static int s_sectordump = -1;
static int s_sd_active = 0;
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

/* Captured prim corner colours = the .s DPCS-fogged output, used as the ORACLE
 * for the native-colour A/B (terrain_compare). Not fed to the scene. */
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

/* Native per-vertex terrain colour: RGB888 read from the sector colour arrays +
 * the per-vertex DPCS depth-cue fog (§9/§10, asm-derived from r_environment.s
 * 0x80026DF0-0x80026E6C). The HP colour block is TWO 4B-stride arrays
 * [near0..][far0..]; `idx` is the pre-×4 byte offset from face word1. The DPCS
 * blend is out = seed + IR0·(FC-seed) with seed=far ($s0[idx]), FC=near
 * ($t7[idx]) → out = lerp(far, near, f), f = clamp((fogRef - viewZ)/4096, 0, 1):
 * near (small viewZ) keeps the near colour, far recedes to the far colour. viewZ
 * is the .s fog SZ3, which equals our m3 DIRECTLY (scale 1) — empirically
 * validated (fogscale=1 → 87-92% colour-exact across scenes; ×4 → 25-67%). The
 * earlier "world/16 ×4" was wrong: the projection matrix already yields m3 in the
 * SZ3 scale, and the geometry A/B can't catch an absolute-m3 scale error because
 * SX = m1·H/m3 cancels it — fog is the first consumer of absolute m3. Colour words
 * are 0x00BBGGRR (R=byte0), matching the captured prim RGB. LP (no-HP 0x4000
 * sectors) is a single 4B colour, no fog. */
static double s_fogscale = -1.0;    /* viewZ = fogscale·m3; 1.0 (m3 is already SZ3), env-tunable */
int s_fogdbg = 0;                   /* OPENPETE_TERRAIN_FOGDBG: dump fog samples */
static uint32_t terrain_color(CPUState* cpu, int noHP, uint32_t lpBase,
                              uint32_t nearBase, uint32_t farBase,
                              int idx, double m3, int fogRef) {
    if (noHP) return rd(cpu, lpBase + (uint32_t)idx) & 0xFFFFFFu;
    uint32_t nearC = rd(cpu, nearBase + (uint32_t)idx) & 0xFFFFFFu;
    uint32_t farC  = rd(cpu, farBase  + (uint32_t)idx) & 0xFFFFFFu;
    double f = ((double)fogRef - m3 * s_fogscale) / 4096.0;
    if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
    int nr = nearC & 0xFF, ng = (nearC >> 8) & 0xFF, nb = (nearC >> 16) & 0xFF;
    int fr = farC  & 0xFF, fg = (farC  >> 8) & 0xFF, fb = (farC  >> 16) & 0xFF;
    extern int s_fogdbg; static int s_fogdbg_n;
    if (s_fogdbg && m3 > 600.0 && s_fogdbg_n < 24) {
        fprintf(stderr, "[fogdbg] m3=%.0f f=%.3f fogRef=%d  near=%06X far=%06X  idx=%d\n",
                m3, f, fogRef, nearC, farC, idx);
        s_fogdbg_n++;
    }
    int r = (int)((double)fr + f * (nr - fr) + 0.5);
    int g = (int)((double)fg + f * (ng - fg) + 0.5);
    int b = (int)((double)fb + f * (nb - fb) + 0.5);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

/* Per-sector local projected vertex (indexed by face vertex index). Carries the
 * camera-space (m1,m2,m3) for near-plane clipping + the projected screen sx,sy. */
typedef struct { float sx, sy; double m1, m2, m3, vz; int ok; } lvert;

/* A face corner: native colour + page-relative texel UV. */
typedef struct { uint32_t rgb; float u, v; } lcorner;
/* A face's texture material (decoded from the Tiledef; VRAM-pixel units). */
typedef struct { int textured, tpx, tpy, clx, cly, bpp, blend; } tmat;

/* Decode a terrain face's texture material + its 4 corner UVs from the
 * m_LQTexturePointer Tiledef table (§8). texByte (int8 of HP face word2):
 *   0xFF (-1)  → untextured (flat gouraud)
 *   >= 0       → textured opaque  (tile = texByte)
 *   < 0, != -1 → textured ABE     (tile = texByte & 0x7F; blend from tpage ABR)
 * Tile = lqTable + tile*16, near/max-detail half [0..7] (skip the +8 far LOD):
 *   word0 = {u8 u0, u8 v0, u16 clut}; word1 = {u8 u1, u8 v1, u16 tpage}.
 * A fixed 32×32 tile. Output u/v[] are the 4 SPATIAL corners in [TL,TR,BL,BR]
 * order: TL=(u0,v0), TR=(u1,v1), BL=(u0,v0+0x1F), BR=(u0+0x1F,v0+0x1F) — matching
 * the .s GT4 prim emit (0x800270AC). NOTE the .s reorders the last two SXY/UV
 * slots (slot2=i3, slot3=i2; stores 0x80026DD0-DC) — so the caller maps vertex
 * i0→TL, i1→TR, i3→BL, i2→BR, and uses the GPU diagonal i1-i3 (affine-faithful).
 *
 * hiDetail selects the LOD HALF of the 16-byte Tiledef entry: each entry stores
 * TWO 8-byte tiles — [0..7] = FAR/low-detail (typically bpp4), [8..15] = NEAR/
 * high-detail (typically bpp8). The .s picks the half per-quad by depth at
 * 0x80026F70 (see terrain_build); under --no-lod the saturated m_LodDistance
 * forces hi-detail everywhere. (The CORRECTED layout: the +8 bump is the NEAR/HQ
 * tile, not far — empirically the bpp8 medallion/detail textures live in [8..15].) */
/* Decode one 8-byte Tiledef entry at byte address `e` → material + 4 spatial
 * corner UVs [TL,TR,BL,BR]. Shared by the per-quad LQ path (terrain_texmat) and
 * the near/HQ subdivide path (entries inside the 168-byte HQ cell record). `m`
 * must be zeroed by the caller. */
static void decode_tiledef(CPUState* cpu, uint32_t e, int abe, tmat* m, float u[4], float v[4]) {
    uint32_t w0 = rd(cpu, e + 0), w1 = rd(cpu, e + 4);
    int u0 = (int)(w0 & 0xFF), v0 = (int)((w0 >> 8) & 0xFF), clut = (int)((w0 >> 16) & 0xFFFF);
    int u1 = (int)(w1 & 0xFF), v1 = (int)((w1 >> 8) & 0xFF), tpage = (int)((w1 >> 16) & 0xFFFF);
    u[0]=(float)u0;               v[0]=(float)v0;                  /* TL */
    u[1]=(float)u1;               v[1]=(float)v1;                  /* TR */
    u[2]=(float)u0;               v[2]=(float)((v0 + 0x1F) & 0xFF);/* BL */
    u[3]=(float)((u0+0x1F)&0xFF); v[3]=(float)((v0 + 0x1F) & 0xFF);/* BR */
    m->textured = 1;
    m->tpx = (tpage & 0xF) * 64; m->tpy = ((tpage >> 4) & 1) * 256;
    int mode = (tpage >> 7) & 3; m->bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
    m->clx = (clut & 0x3F) * 16; m->cly = (clut >> 6) & 0x1FF;
    /* blend: opaque unless ABE; then the tpage ABR mode (0..3) → scene blend
     * 1..4 ((B+F)/2, +, −, B+F/4). */
    m->blend = abe ? (((tpage >> 5) & 3) + 1) : 0;
}

static void terrain_texmat(CPUState* cpu, uint32_t table, int texByte, int hiDetail,
                           tmat* m, float u[4], float v[4]) {
    memset(m, 0, sizeof(*m)); m->bpp = 16;
    if (texByte == -1) { m->textured = 0; return; }      /* 0xFF: untextured */
    uint32_t e = table + (uint32_t)(texByte & 0x7F) * 16u + (hiDetail ? 8u : 0u);  /* LOD half */
    decode_tiledef(cpu, e, (texByte < 0), m, u, v);      /* high bit = semi-transparent */
}

/* Native colour-verts (per emitted tri corner) for the colour A/B. */
static float s_cvx[MAX_TTRIS * 3], s_cvy[MAX_TTRIS * 3];
static double s_cvz[MAX_TTRIS * 3];               /* m3 (world/16 view-Z) for fog regime */
static uint32_t s_cvrgb[MAX_TTRIS * 3]; static int s_cv_n;
static int s_tex_faces, s_tex_tris;               /* diagnostics: textured-face count */
static int s_dbg_clipped;                          /* faces that hit the near plane */

/* A clip-space corner: camera-space pos (m1,m2,m3) + colour + UV. Near-plane
 * clipping (§near-plane) interpolates all of these along an edge. */
typedef struct { double m1, m2, m3; uint32_t rgb; float u, v; } cvert;

/* Near plane (camera-space m3, world/16 view-Z). Vertices closer than this are
 * clipped so float H/m3 never blows up and behind-camera (m3<=0) verts don't
 * project to a wild position. Env-tunable. */
static double s_nearz = -1.0;
/* near/HQ subdivide pass enable (Route 2; OPENPETE_TERRAIN_NEARHQ, default ON). */
static int s_nearhq = -1;

static cvert cv_lerp(cvert a, cvert b) {     /* crossing point at m3 == s_nearz */
    double t = (s_nearz - a.m3) / (b.m3 - a.m3);
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    cvert r;
    r.m1 = a.m1 + t * (b.m1 - a.m1);
    r.m2 = a.m2 + t * (b.m2 - a.m2);
    r.m3 = s_nearz;
    r.u  = a.u + (float)t * (b.u - a.u);
    r.v  = a.v + (float)t * (b.v - a.v);
    int ar=a.rgb&0xFF, ag=(a.rgb>>8)&0xFF, ab=(a.rgb>>16)&0xFF;
    int br=b.rgb&0xFF, bg=(b.rgb>>8)&0xFF, bb=(b.rgb>>16)&0xFF;
    int rr=(int)(ar+t*(br-ar)+0.5), rg=(int)(ag+t*(bg-ag)+0.5), rb=(int)(ab+t*(bb-ab)+0.5);
    r.rgb = (uint32_t)rr | ((uint32_t)rg<<8) | ((uint32_t)rb<<16);
    return r;
}

/* Push one (already near-clipped) camera-space triangle: project to screen + the
 * scene-feed ttri (+ the colour-A/B verts). */
static void push_tri(cvert a, cvert b, cvert c, const tmat* m) {
    if (s_tri_n >= MAX_TTRIS) return;
    cvert cc[3] = { a, b, c };
    ttri* t = &s_tri[s_tri_n++];
    for (int k = 0; k < 3; k++) {
        proj_screen(cc[k].m1, cc[k].m2, cc[k].m3, &t->sx[k], &t->sy[k]);
        t->vz[k] = cc[k].m3; t->rgb[k] = cc[k].rgb; t->u[k] = cc[k].u; t->v[k] = cc[k].v;
        if (s_cv_n < MAX_TTRIS * 3) {
            s_cvx[s_cv_n]=t->sx[k]; s_cvy[s_cv_n]=t->sy[k]; s_cvz[s_cv_n]=cc[k].m3; s_cvrgb[s_cv_n]=cc[k].rgb; s_cv_n++;
        }
    }
    t->textured=m->textured; t->tpx=m->tpx; t->tpy=m->tpy;
    t->clx=m->clx; t->cly=m->cly; t->bpp=m->bpp; t->blend=m->blend;
    t->tier = s_emit_noHP;
    if (m->textured) s_tex_tris++;
}

/* Near-plane clip (Sutherland-Hodgman against the single plane m3 >= s_nearz) of
 * a camera-space triangle given as 3 cverts, then push. 0 clipped → 1 tri; 1 →
 * quad (2 tris); 2 → 1 tri; 3 → dropped. */
static void clip_push_tri(cvert a, cvert b, cvert c, const tmat* m) {
    cvert in[3] = { a, b, c };
    cvert out[4]; int nout = 0; int clipped = 0;
    for (int i = 0; i < 3; i++) {
        cvert cur = in[i], nxt = in[(i + 1) % 3];
        int curIn = cur.m3 >= s_nearz, nxtIn = nxt.m3 >= s_nearz;
        if (curIn) out[nout++] = cur;
        if (curIn != nxtIn && nout < 4) { out[nout++] = cv_lerp(cur, nxt); clipped = 1; }
    }
    if (nout < 3) { s_dbg_behind++; return; }     /* fully behind the near plane */
    if (clipped) s_dbg_clipped++;
    push_tri(out[0], out[1], out[2], m);          /* fan the clipped polygon */
    if (nout == 4) push_tri(out[0], out[2], out[3], m);
}

/* Emit a triangle (sv[] index form) with near-plane clipping. Replaces the old
 * behind-drop + spike cull: near/straddling faces are clipped & rendered. */
static void emit_tri(const lvert* sv, int a, int b, int c,
                     lcorner ka, lcorner kb, lcorner kc, const tmat* m) {
    cvert A = { sv[a].m1, sv[a].m2, sv[a].m3, ka.rgb, ka.u, ka.v };
    cvert B = { sv[b].m1, sv[b].m2, sv[b].m3, kb.rgb, kb.u, kb.v };
    cvert C = { sv[c].m1, sv[c].m2, sv[c].m3, kc.rgb, kc.u, kc.v };
    clip_push_tri(A, B, C, m);
}

/* Camera-space midpoint of two cverts: position averaged, colour per-channel
 * averaged (UVs filled later by the caller). Mirrors the .s near-pass's
 * `(a+b)>>1` grid build (§near-HQ). */
static cvert cv_mid(cvert a, cvert b) {
    cvert r;
    r.m1 = (a.m1 + b.m1) * 0.5; r.m2 = (a.m2 + b.m2) * 0.5; r.m3 = (a.m3 + b.m3) * 0.5;
    r.u = 0; r.v = 0;
    int ar=a.rgb&0xFF, ag=(a.rgb>>8)&0xFF, ab=(a.rgb>>16)&0xFF;
    int br=b.rgb&0xFF, bg=(b.rgb>>8)&0xFF, bb=(b.rgb>>16)&0xFF;
    r.rgb = (uint32_t)((ar+br)>>1) | ((uint32_t)((ag+bg)>>1)<<8) | ((uint32_t)((ab+bb)>>1)<<16);
    return r;
}

/* Emit a textured camera-space quad (corners TL,TR,BL,BR) with near-clipping.
 * UVs come from a decoded Tiledef (u/v in [TL,TR,BL,BR]); GPU diagonal TR-BL
 * (triangulation (TL,TR,BL)+(TR,BL,BR)) — same as the main loop. */
static void emit_quad_cv(cvert tl, cvert tr, cvert bl, cvert br,
                         const tmat* m, const float u[4], const float v[4]) {
    tl.u=u[0]; tl.v=v[0]; tr.u=u[1]; tr.v=v[1]; bl.u=u[2]; bl.v=v[2]; br.u=u[3]; br.v=v[3];
    clip_push_tri(tl, tr, bl, m);
    clip_push_tri(tr, bl, br, m);
}

/* Near/HQ subdivide pass — the func_800276A0 simple-emit path (.L80027D7C),
 * verified against r_environment.s. A near foreground cell is split 2×2 into a
 * 3×3 grid (corners + edge midpoints + centre, midpoints in camera space via
 * `(a+b)>>1`) and emitted as 4 sub-quads textured from the per-cell HQ Tiledef
 * record (`m_HQTexturePointer + (faceWord2 & 0x7F)*168`): sub-quad i (i=0..3 =
 * TL,TR,BL,BR quadrant) uses the 8-byte Tiledef at `hqBase + i*8` (the .s does
 * `t6 += 8` per sub-quad), with the 4 spatial corner UVs (u0,v0)/(u1,v1)/
 * (u0,v0+0x1F)/(u0+0x1F,v0+0x1F) = [TL,TR,BL,BR] (the .s t1/t2/t1+0x1F00/
 * t1+0x1F1F). These are the bpp8 hi-detail tiles (the central-platform
 * medallion). `abe` = faceWord2 & 0x80 (the .s `s0 & 0x80` semi-transparent
 * gate). Colour is bilinear (no DPCS fog on near cells); the corner cverts carry
 * the original quad's raw NEAR colours. (The minority per-cell detail-bit finer
 * subdivision — the D_8006CF98 pattern path — is NOT replicated here; the 2×2 is
 * the dominant geometry and fixes the medallion's affine warp.) */
static void terrain_near_emit(CPUState* cpu, uint32_t hqBase, int abe,
                              cvert TL, cvert TR, cvert BL, cvert BR) {
    cvert mT = cv_mid(TL, TR), mB = cv_mid(BL, BR);
    cvert mL = cv_mid(TL, BL), mR = cv_mid(TR, BR);
    cvert C  = cv_mid(mT, mB);                 /* centre = mean of 4 corners */
    cvert q[4][4] = {                          /* {TL,TR,BL,BR} per sub-quad */
        { TL, mT, mL, C  },                    /* SQ0: top-left quadrant   → entry 0 */
        { mT, TR, C,  mR },                    /* SQ1: top-right quadrant  → entry 1 */
        { mL, C,  BL, mB },                    /* SQ2: bottom-left quadrant→ entry 2 */
        { C,  mR, mB, BR },                    /* SQ3: bottom-right        → entry 3 */
    };
    for (int i = 0; i < 4; i++) {
        tmat mat; float u[4], v[4]; memset(&mat, 0, sizeof(mat)); mat.bpp = 16;
        /* sub-quad i uses HQ Tiledef entry (i+1), NOT i: the .s simple-emit loop
         * (func_800276A0 .L80027D88) does `t6 += 8` in the branch DELAY SLOT before
         * the first Tiledef read, so descriptor[0]=TL → offset 8, …, descriptor[3]
         * =BR → offset 32 (entries 1..4; entry 0 is the detail-path base, unused by
         * the 2×2). Reading entries 0..3 shifted every sub-tile by one → "4 textures
         * in 1 with a wrong offset" (delay-slot trap, cf. read-recompiled-not-asm). */
        uint32_t e = hqBase + (uint32_t)(i + 1) * 8u;
        decode_tiledef(cpu, e, abe, &mat, u, v);
        /* HQ sub-quad UV ORIENTATION (rotation/flip), mirroring func_800276A0
         * block_80027EA8: the HQ Tiledef carries an orientation index in
         * `word1 >> 25`; when nonzero it selects an 8-byte entry in the offset
         * table D_8006D058 (`base + ori`) of per-corner packed (du | dv<<8) deltas
         * added to the base UVs — TL/BL/BR relative to (u0,v0) [word0], TR relative
         * to (u1,v1) [word1] — then masked to the u/v bytes (carry between u,v is
         * faithful). Entry 0/64 are the identity transform (≡ decode_tiledef's
         * default), so cells already drawn correctly are untouched. Without this,
         * rotated/flipped HQ tiles render unrotated (~0.4% of near sub-quads in the
         * attract demo: e.g. Artisans tiles 6/23 ori=104, the right-column sub-quads
         * of a cell). The 16px (0x0F) table half is unused in practice. */
        uint32_t ori = rd(cpu, e + 4) >> 25;
        if (ori) {
            uint32_t w0 = rd(cpu, e), w1 = rd(cpu, e + 4);
            uint32_t te = 0x8006D058u + ori;     /* D_8006D058: per-orientation UV-offset table */
            uint32_t ew0 = rd(cpu, te), ew1 = rd(cpu, te + 4);
            uint32_t tl = ((w0 & 0xFFFF) + (ew0 & 0xFFFF)) & 0xFFFF;  /* TL rel (u0,v0) */
            uint32_t tr = ((w1 & 0xFFFF) + (ew0 >> 16))    & 0xFFFF;  /* TR rel (u1,v1) */
            uint32_t bl = ((w0 & 0xFFFF) + (ew1 & 0xFFFF)) & 0xFFFF;  /* BL rel (u0,v0) */
            uint32_t br = ((w0 & 0xFFFF) + (ew1 >> 16))    & 0xFFFF;  /* BR rel (u0,v0) */
            u[0]=(float)(tl & 0xFF); v[0]=(float)((tl >> 8) & 0xFF);
            u[1]=(float)(tr & 0xFF); v[1]=(float)((tr >> 8) & 0xFF);
            u[2]=(float)(bl & 0xFF); v[2]=(float)((bl >> 8) & 0xFF);
            u[3]=(float)(br & 0xFF); v[3]=(float)((br >> 8) & 0xFF);
        }
        emit_quad_cv(q[i][0], q[i][1], q[i][2], q[i][3], &mat, u, v);
    }
}

/* ---- deferred near-cell queue (Route 2: the .s's own near classification) --
 *
 * func_800258F0's HP face walk classifies each HP face (the classifier at
 * r_environment.s 0x80026CFC): a NEAR foreground face is deferred into one of two
 * per-frame queues in g_SonyImage scratch, which func_800276A0 then drains +
 * subdivides:
 *   +0x2000 (tier 2): depthSum < 0x2000 && !(word3 & 0x80) && no corner SZ3
 *                     < 0x140 && !(word3 & 0x40)         (.L80026D3C, sb 2)
 *   +0x1E00 (tier 4): depthSum < 0x2000 && !(word3 & 0x80) && a corner SZ3
 *                     < 0x140 && depthSum != 0 && !(word3 & 0x40)  (.L80026D70, sb 4)
 * (depthSum = Σ of the 4 corner SZ3; faces with m_LodDistance - depthSum <= 0 are
 * dropped entirely — too far for HP.) Both queues are 0-terminated by
 * func_800276A0's entry; each entry is: 0 = end, POSITIVE = a header
 * (sectorPtr>>2, sets up the drain's vertex/colour bases), NEGATIVE = a cell
 * (faceRecPtr | clipBit, low 2 bits are flags). func_800276A0 drains +0x2000 then
 * +0x1E00 (.L8002857C re-points $ra at +0x1E00), so BOTH are rendered.
 *
 * We replicate the .s's classification EXACTLY by reading the filled queues from
 * RAM after func_800258F0 returns (the hook runs it first), collecting the set of
 * deferred face-record pointers. Re-deriving the classifier in the main loop was
 * Route 1, which over-deferred (the artist flags 0x40/0x80 are unset this scene,
 * so depthSum<0x2000 alone caught ~701/839 cells vs the .s's ~tens). Reading the
 * queue sidesteps that — it IS the game's verdict. The face record IS the same
 * 16B HP face record the main loop walks (faceRecPtr = faces + f*16), and the HQ
 * index is its word2 & 0x7F, so a deferred face just routes through
 * terrain_near_emit instead of the flat LQ emit. */
#define MAX_DEFER 4096
static uint32_t s_defer[MAX_DEFER]; static int s_defer_n;
static int s_defer_hdr2, s_defer_cell2, s_defer_hdr1e, s_defer_cell1e;
static int defer_cmp(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}
static void terrain_read_defer(CPUState* cpu) {
    s_defer_n = 0;
    s_defer_hdr2 = s_defer_cell2 = s_defer_hdr1e = s_defer_cell1e = 0;
    const uint32_t qbase[2] = { G_SCRATCH + 0x2000u, G_SCRATCH + 0x1E00u };
    for (int q = 0; q < 2; q++) {
        uint32_t p = qbase[q];
        for (int guard = 0; guard < MAX_DEFER * 2; guard++) {
            uint32_t e = rd(cpu, p); p += 4;
            if (e == 0) break;
            if ((int32_t)e > 0) { if (q) s_defer_hdr1e++; else s_defer_hdr2++; continue; }
            if (s_defer_n < MAX_DEFER) s_defer[s_defer_n++] = e & ~3u;  /* cell: faceRecPtr */
            if (q) s_defer_cell1e++; else s_defer_cell2++;
        }
    }
    if (s_defer_n > 1) qsort(s_defer, (size_t)s_defer_n, sizeof(s_defer[0]), defer_cmp);
}
static int is_deferred(uint32_t faceRecPtr) {
    int lo = 0, hi = s_defer_n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (s_defer[m] < faceRecPtr) lo = m + 1;
        else if (s_defer[m] > faceRecPtr) hi = m - 1;
        else return 1;
    }
    return 0;
}

/* Near-cell classifier — replicates func_800258F0's HP-walk defer test (the
 * classifier at r_environment.s 0x80026CFC) from GAMESTATE, so it works in ALL
 * modes. (The deferred QUEUE the .s fills is per-frame .s OUTPUT — the plan's
 * core principle forbids consuming that; and --no-lod's m_LodDistance saturation
 * perturbs the .s's scratch-buffer reuse so the queue is garbage there anyway. So
 * the queue is kept ONLY as a verify-path oracle to calibrate this against.)
 *
 * A face is deferred to the near subdivide pass when its 4 corner depths mark it
 * foreground:
 *   depthSum < 0x2000 && !(word3 & 0x80) && (minCorner < 0x140 || !(word3 & 0x40))
 * Depths are the GTE SZ3 (world/4). Our m3 is world/16, so SZ3 = HQSCALE·m3
 * (HQSCALE=4: HP renders world/4, we project world/16). The .s's leading
 * "m_LodDistance - depthSum <= 0 → skip" gate never fires for a near face
 * (depthSum < 0x2000 << m_LodDistance), so it is omitted — that omission is what
 * makes this --no-lod-robust (no m_LodDistance dependency). depthSum > 0 stands in
 * for the tier-4 depthSum != 0 guard and drops behind-camera faces. */
static double s_hqscale = -1.0;   /* SZ3 = s_hqscale * m3 (env OPENPETE_TERRAIN_HQSCALE) */
static int face_is_near(double z0, double z1, double z2, double z3, uint32_t word3) {
    if (s_hqscale < 0.0) {
        const char* e = getenv("OPENPETE_TERRAIN_HQSCALE"); s_hqscale = (e && *e) ? atof(e) : 4.0;
    }
    double s = s_hqscale;
    double sz0 = z0*s, sz1 = z1*s, sz2 = z2*s, sz3 = z3*s;
    double sum = sz0 + sz1 + sz2 + sz3;
    if (sum <= 0.0) return 0;                          /* behind camera */
    if (word3 & 0x80) return 0;                        /* face hard-excluded from HQ subdivide */
    /* --no-lod: pin the bpp8 HQ near-subdivide ON for every eligible face,
     * regardless of distance, so the coarse bpp4 LQ far tier is never shown. The
     * canonical game switches LQ→HQ only inside the 0x2000 foreground band (the
     * `sum < 0x2000` gate + per-corner tier checks below); under --no-lod we drop
     * all the distance-derived gates and keep only the two that aren't distance:
     * the behind-camera guard above and the word3 & 0x80 hard-exclusion (those
     * faces never carry a valid HQ Tiledef). Quads only — near TRIANGLES still
     * fall to the GT3 LQ emit (the existing detail-descriptor carve-out, see the
     * near-dispatch site). Display-only; reads no gameplay state. */
    if (openpete_no_lod) return 1;
    if (sum >= 8192.0) return 0;                       /* not foreground (0x2000) */
    double mn = sz0; if (sz1<mn) mn=sz1; if (sz2<mn) mn=sz2; if (sz3<mn) mn=sz3;
    if (mn < 320.0) return 1;                          /* a corner SZ3 < 0x140 → tier 4 */
    return !(word3 & 0x40);                            /* else tier 2 unless flag 0x40 */
}
static int s_near_q_agree, s_near_c_only;   /* classifier vs queue cross-check (verify) */
static int s_caldbg = -1, s_cal_qn;         /* calibration probe (OPENPETE_TERRAIN_CALDBG) */
static int s_nearq = -2;                     /* OPENPETE_TERRAIN_NEARQ: use queue oracle (debug) */
static double s_cal_qsum_max, s_cal_qmin_max, s_cal_nqsum_min;

/* ---- sector decode cache (terrain Tier 1, mirrors the moby faceblock cache) --
 * A sector's geometry is STATIC world-space level data: packed vertex words,
 * face words (indices/colour offsets/flags) and each HP face's LQ Tiledef never
 * change while the level is loaded — yet the build re-read and re-unpacked all
 * of it through cpu->read_word per sector PER PRESENT. Decode once per sector
 * (key: sector vaddr + a 5-word header signature, so a level load re-decodes;
 * 2-way probe — the 1-way moby map thrashed on ~80 hot entries) and iterate
 * the decoded arrays; only projection, NCLIP, near-classification, fog and the
 * near/HQ subdivide stay per-present. Colour arrays additionally resolve to
 * host pointers once per sector pass. Display-only; behaviour is byte-identical
 * to the inline parse (verified by the headless same-frame screenshot diff). */
typedef struct { int16_t x, y, z; } tsvert_t;      /* unpacked local vertex */
typedef struct {
    uint8_t  i0, i1, i2, i3, quad, flip;
    uint16_t c0, c1, c2, c3;                       /* colour byte-offsets */
    uint8_t  fogbias, trirot, hq_idx, abe;         /* HP: w3&1, w2>>8&3, w2&0x7F, w2&0x80 */
    uint32_t w3;                                   /* HP word3 (face_is_near gates) */
    tmat mat; float tu[4], tv[4];                  /* LQ-tier texmat (HP; LP untextured) */
} tsface_t;
typedef struct {
    uint32_t s, sig[5];
    int nLpV, nLpF, nHpV, nHpF, flags;
    int originX, originY, originZ;
    uint32_t lpColBase, nearBase, farBase, lpFaces, hpFaces;
    tsvert_t* lpV; tsface_t* lpF;
    tsvert_t* hpV; tsface_t* hpF;
} tscache_t;
#define TSC_SLOTS 512
static tscache_t s_tsc[TSC_SLOTS];
static long s_tsc_hit, s_tsc_miss;
static void tsc_census(void) {   /* OPENPETE_TERRAIN_CACHE_DBG: ~1/sec hit/miss */
    static int on = -1, n;
    if (on < 0) { const char* e = getenv("OPENPETE_TERRAIN_CACHE_DBG"); on = (e && *e && *e != '0') ? 1 : 0; }
    if (on && ++n >= 120) {
        fprintf(stderr, "[ter-cache] hit=%ld miss=%ld\n", s_tsc_hit, s_tsc_miss);
        s_tsc_hit = s_tsc_miss = 0; n = 0;
    }
}

static void tsc_decode_verts(CPUState* cpu, uint32_t verts, int nv, tsvert_t* out) {
    for (int k = 0; k < nv; k++) {
        uint32_t v = rd(cpu, verts + (uint32_t)k * 4);
        out[k].x = (int16_t)((v >> 21) & 0x7FF);
        out[k].y = (int16_t)((v >> 10) & 0x7FF);
        out[k].z = (int16_t)(v & 0x3FF);
    }
}
static void tsc_decode_faces(CPUState* cpu, uint32_t faces, int nf, int faceStride,
                             uint32_t lqTable, uint32_t hqTable, int texTblMode,
                             int hiDetail, tsface_t* out) {
    for (int f = 0; f < nf; f++) {
        tsface_t* F = &out[f];
        uint32_t w0 = rd(cpu, faces + (uint32_t)f * faceStride);
        uint32_t w1 = rd(cpu, faces + (uint32_t)f * faceStride + 4);
        memset(F, 0, sizeof(*F));
        if (faceStride == 16) {                    /* HP: byte indices; word2 tex; word3 flags */
            F->i0 = (uint8_t)((w0 >> 24) & 0xFF); F->i1 = (uint8_t)((w0 >> 16) & 0xFF);
            F->i2 = (uint8_t)((w0 >> 8) & 0xFF);  F->i3 = (uint8_t)(w0 & 0xFF);
            uint32_t w2 = rd(cpu, faces + (uint32_t)f * faceStride + 8);
            uint32_t w3 = rd(cpu, faces + (uint32_t)f * faceStride + 12);
            F->flip = (w3 & 0x2) != 0; F->fogbias = (w3 & 0x1) != 0; F->w3 = w3;
            F->c0 = (uint16_t)((w1 >> 22) & 0x3FC); F->c1 = (uint16_t)((w1 >> 14) & 0x3FC);
            F->c2 = (uint16_t)((w1 >> 6) & 0x3FC);  F->c3 = (uint16_t)((w1 << 2) & 0x3FC);
            F->hq_idx = (uint8_t)(w2 & 0x7F); F->abe = (w2 & 0x80) != 0;
            F->trirot = (uint8_t)((w2 >> 8) & 3u);
            int tb = (int)(int8_t)(w2 & 0xFF);
            uint32_t tbl = (texTblMode == 2) ? hqTable : lqTable;
            terrain_texmat(cpu, tbl, tb, hiDetail, &F->mat, F->tu, F->tv);
        } else {                                   /* LP: 6-bit indices; untextured */
            F->i0 = (uint8_t)(((w0 >> 23) & 0x1F8) >> 3); F->i1 = (uint8_t)(((w0 >> 17) & 0x1F8) >> 3);
            F->i2 = (uint8_t)(((w0 >> 11) & 0x1F8) >> 3); F->i3 = (uint8_t)(((w0 >> 5) & 0x1F8) >> 3);
            F->c0 = (uint16_t)((w1 >> 24) & 0xFC); F->c1 = (uint16_t)((w1 >> 18) & 0xFC);
            F->c2 = (uint16_t)((w1 >> 12) & 0xFC); F->c3 = (uint16_t)((w1 >> 6) & 0xFC);
            F->mat.bpp = 16;                       /* untextured flat gouraud */
        }
        F->quad = (F->i2 != F->i3);
    }
}

static const tscache_t* tsc_get(CPUState* cpu, uint32_t s,
                                uint32_t lqTable, uint32_t hqTable,
                                int texTblMode, int hiDetail) {
    uint32_t g[5] = { rd(cpu, s + 0x04), rd(cpu, s + 0x08), rd(cpu, s + 0x0C),
                      rd(cpu, s + 0x10), rd(cpu, s + 0x14) };
    uint32_t h = (((s >> 2) * 2654435761u) >> 14) & (TSC_SLOTS - 1);
    tscache_t* C = &s_tsc[h];
    if (!(C->s == s && C->sig[0]==g[0] && C->sig[1]==g[1] && C->sig[2]==g[2]
          && C->sig[3]==g[3] && C->sig[4]==g[4])) {
        tscache_t* C2 = &s_tsc[h ^ 1u];
        if (C2->s == s && C2->sig[0]==g[0] && C2->sig[1]==g[1] && C2->sig[2]==g[2]
            && C2->sig[3]==g[3] && C2->sig[4]==g[4]) { s_tsc_hit++; return C2; }
        s_tsc_miss++;
        if (C->s) { if (!C2->s) C = C2; else if (s_tsc_miss & 1) C = C2; }
        free(C->lpV); free(C->lpF); free(C->hpV); free(C->hpF);
        memset(C, 0, sizeof(*C));
        C->s = s; memcpy(C->sig, g, sizeof(g));
        C->flags = (int)(g[0] & 0xE000);
        C->originX = (int)(g[1] >> 16); C->originY = (int)(g[1] & 0xFFFF);
        C->originZ = (int)(g[2] >> 16);
        C->nLpV = g[3] & 0xFF; C->nLpF = (g[3] >> 16) & 0xFF;
        int nLpC = (g[3] >> 8) & 0xFF;
        C->nHpV = g[4] & 0xFF; C->nHpF = (g[4] >> 16) & 0xFF;
        int nHpC = (g[4] >> 8) & 0xFF, hpVertsOff = (int)((g[4] >> 24) & 0xFF);
        if (C->nLpV > 256) C->nLpV = 256;
        if (C->nHpV > 256) C->nHpV = 256;
        uint32_t lpVerts = s + 0x1C;
        C->lpFaces   = s + 0x1C + (uint32_t)((g[3] & 0xFF) + nLpC) * 4;
        C->lpColBase = s + 0x1C + (uint32_t)(g[3] & 0xFF) * 4;
        uint32_t hpVerts = s + 0x1C + (uint32_t)hpVertsOff * 4;
        C->hpFaces   = hpVerts + (uint32_t)((g[4] & 0xFF) + nHpC * 2) * 4;
        C->nearBase  = hpVerts + (uint32_t)(g[4] & 0xFF) * 4;
        C->farBase   = C->nearBase + (uint32_t)nHpC * 4;
        if (C->nLpV > 0) { C->lpV = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)C->nLpV);
                           if (C->lpV) tsc_decode_verts(cpu, lpVerts, C->nLpV, C->lpV); }
        if (C->nLpF > 0) { C->lpF = (tsface_t*)malloc(sizeof(tsface_t) * (size_t)C->nLpF);
                           if (C->lpF) tsc_decode_faces(cpu, C->lpFaces, C->nLpF, 8,
                                                        lqTable, hqTable, texTblMode, hiDetail, C->lpF); }
        if (C->nHpV > 0) { C->hpV = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)C->nHpV);
                           if (C->hpV) tsc_decode_verts(cpu, hpVerts, C->nHpV, C->hpV); }
        if (C->nHpF > 0) { C->hpF = (tsface_t*)malloc(sizeof(tsface_t) * (size_t)C->nHpF);
                           if (C->hpF) tsc_decode_faces(cpu, C->hpFaces, C->nHpF, 16,
                                                        lqTable, hqTable, texTblMode, hiDetail, C->hpF); }
    } else s_tsc_hit++;
    return C;
}

/* Host-pointer terrain colour (fog lerp identical to terrain_color; the debug
 * fog print stays on the cpu-path variant only). */
static inline uint32_t t_le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t terrain_color_h(int noHP, const uint8_t* lp, const uint8_t* nearp,
                                const uint8_t* farp, int idx, double m3, int fogRef) {
    if (noHP) return t_le32(lp + idx) & 0xFFFFFFu;
    uint32_t nearC = t_le32(nearp + idx) & 0xFFFFFFu;
    uint32_t farC  = t_le32(farp  + idx) & 0xFFFFFFu;
    double f = ((double)fogRef - m3 * s_fogscale) / 4096.0;
    if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
    int nr = nearC & 0xFF, ng = (nearC >> 8) & 0xFF, nb = (nearC >> 16) & 0xFF;
    int fr = farC  & 0xFF, fg = (farC  >> 8) & 0xFF, fb = (farC  >> 16) & 0xFF;
    int r = (int)((double)fr + f * (nr - fr) + 0.5);
    int g = (int)((double)fg + f * (ng - fg) + 0.5);
    int b = (int)((double)fb + f * (nb - fb) + 0.5);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

/* Walk every VISIBLE sector (D_800771C8[i]==0xFF — the .s's own canonical
 * visible set, isolating decode+projection+faces from the cull), project its HP
 * vertices (LP for 0x4000 "no-HP" sectors), then walk the HP faces (16B) / LP
 * faces (8B) into triangles. Camera-relative GTE-vector per r_environment.s
 * 0x80026238-0x800262C0 (matrix = g_Camera+0x00; world axes = origin+field). */
/* Camera-INDEPENDENT per-level ABR census (OPENPETE_TERRAIN_ABRSCAN). Walks EVERY
 * sector and EVERY HP face (no visibility/frustum gate, no projection) and tallies
 * the semi-transparent (ABE, face word2 bit 0x80) terrain faces by ABR blend mode
 * (from the LQ Tiledef tpage): 1=alpha (B+F)/2, 2=additive, 3=subtract, 4=mode3.
 * This answers "which levels CONTAIN mode-0 alpha terrain" across the whole game
 * via a --skip-to-level sweep, with no dependence on where the camera looked — the
 * gap that a playthrough (or my demo+spawn census) can leave. A back-to-front
 * transparency sort can only ever matter where alpha(mode 1) terrain exists AND
 * overlaps in view; this scan finds the necessary condition exhaustively. Prints
 * once per level (first call at openpete_frame>=1800, i.e. loaded). Display-only. */
static void terrain_abr_scan(CPUState* cpu) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_TERRAIN_ABRSCAN"); s_on = (e && *e && *e != '0') ? 1 : 0; }
    if (!s_on) return;
    static int s_done = 0;
    if (s_done || openpete_frame < 1800u) return;
    s_done = 1;

    uint32_t sectorPtr = rd(cpu, G_ENVIRONMENT + ENV_SECTORPTR);
    uint32_t sectorCnt = rd(cpu, G_ENVIRONMENT + ENV_SECTORCNT);
    uint32_t lqTable   = rd(cpu, G_ENVIRONMENT + ENV_LQTEX);
    long abe[5] = {0,0,0,0,0};   /* index by mat.blend (0..4); ABE faces only */
    long hpFaces = 0, abeFaces = 0, secWalked = 0;

    for (uint32_t i = 0; i < sectorCnt; i++) {       /* ALL sectors — no visibility gate */
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        secWalked++;
        uint32_t w1  = rd(cpu, s + 0x04);
        uint32_t lpw = rd(cpu, s + 0x10), hpw = rd(cpu, s + 0x14);
        if (w1 & 0x4000) continue;                   /* no-HP (LP-only) sector: untextured */
        int nLpV = lpw & 0xFF, nLpC = (lpw >> 8) & 0xFF, nLpF = (lpw >> 16) & 0xFF;
        int nHpV = hpw & 0xFF, nHpC = (hpw >> 8) & 0xFF, nHpF = (hpw >> 16) & 0xFF;
        int hpVertsOff = (int)((hpw >> 24) & 0xFF);
        (void)nLpV; (void)nLpC; (void)nLpF;
        uint32_t verts = s + 0x1C + (uint32_t)hpVertsOff * 4;
        uint32_t faces = verts + (uint32_t)(nHpV + nHpC * 2) * 4;
        for (int f = 0; f < nHpF; f++) {
            uint32_t w2 = rd(cpu, faces + (uint32_t)f * 16u + 8);
            hpFaces++;
            if (!(w2 & 0x80)) continue;              /* opaque (or 0xFF untextured) */
            int tb = (int)(int8_t)(w2 & 0xFF);
            tmat mat; float tu[4], tv[4]; memset(&mat, 0, sizeof(mat));
            terrain_texmat(cpu, lqTable, tb, 0, &mat, tu, tv);
            int b = mat.blend; if (b < 0 || b > 4) b = 0;
            abeFaces++; abe[b]++;
        }
    }
    fprintf(stderr,
        "ABR-SCAN frame=%u sectors=%ld hpFaces=%ld abeFaces=%ld  "
        "ABE-by-mode[alpha=%ld add=%ld sub=%ld mode3=%ld]\n",
        openpete_frame, secWalked, hpFaces, abeFaces, abe[1], abe[2], abe[3], abe[4]);
}

/* OPENPETE_TERRAIN_VISDBG: falsify the widescreen candidate set's correctness
 * invariant. Prints ONCE (once loaded): the .s's canonical 4:3 visible count
 * (D_800771C8==0xFF) vs the occlusion candidate set, and how many visible
 * sectors are MISSING from the candidate set. The candidate set must be a
 * SUPERSET of the 4:3 visible set — if missing>0 the occlusion-group walk is
 * wrong (offset/format) and widescreen would gap even at 4:3. Display-only. */
static int s_visdbg = -1;
static void terrain_visdbg(CPUState* cpu, uint32_t sectorCnt, int group,
                           uint32_t occCnt, const uint16_t* cand, int cn, int ws) {
    static int s_done = 0;
    if (s_done || openpete_frame < 1800u) return;
    s_done = 1;
    static char inset[1024]; memset(inset, 0, sizeof(inset));
    for (int j = 0; j < cn; j++) if (cand[j] < 1024) inset[cand[j]] = 1;
    int vis43 = 0, missing = 0;
    for (uint32_t i = 0; i < sectorCnt && i < 1024; i++) {
        uint8_t vis = (uint8_t)(rd(cpu, (D_VISIBILITY + i) & ~3u) >> ((i & 3) * 8));
        if (vis == 0xFF) { vis43++; if (!inset[i]) missing++; }
    }
    fprintf(stderr,
        "[terrain-vis] frame=%u group=%d occCnt=%u candidates=%d  vis4:3=%d "
        "missingFromCandidates=%d  (sectors=%u ws=%d)\n",
        openpete_frame, group, occCnt, cn, vis43, missing, sectorCnt, ws);
}

/* Build the set of sector indices to render this frame.
 *
 * Native 4:3: use the .s's own canonical visibility verdict D_800771C8
 * (== occlusion sublist ∩ the .s 4:3 frustum cull) directly — pixel-identical
 * to the .s visible set, zero risk.
 *
 * WIDESCREEN: the .s verdict is too narrow — its 4:3 frustum cull dropped the
 * side sectors the wider frustum now reveals (the terrain sibling of the
 * cyclorama wide-AR gap, FIXED 2026-06-20). We re-derive the candidate set
 * host-side from the SAME static, aspect/direction-INDEPENDENT occlusion data
 * the .s starts from: g_Camera.m_OcclusionGroup selects a baked PVS sublist in
 * g_Environment.m_OcclusionGroups (a conservative, direction-independent
 * superset for the camera's region), or ALL sectors when the group is invalid
 * (mirrors func_8002B9CC's group selection). The near-plane clip + the
 * full-window GPU scissor + the Z-buffer then confine and depth-sort the set.
 *
 * DISPLAY-ONLY: reads g_Camera/g_Environment read-only, never writes
 * D_800771C8 / m_WasDrawn (the canonical render→gameplay channel) — oracle-safe
 * by construction. The candidate set is a SUPERSET of the 4:3 visible set
 * (verified by OPENPETE_TERRAIN_VISDBG: every D_800771C8==0xFF sector is in the
 * sublist), so widescreen only ADDS side sectors, never drops a visible one. A
 * cheap per-sector wide-frustum prune of this candidate set (for the 480fps /
 * --no-lod passes) is a follow-up; see the plan.
 *
 * `group` is the occlusion-group ARGUMENT the .s renderer (func_800258F0)
 * received this frame (captured from cpu->a0 at the hook, BEFORE the real call
 * clobbers it) — already clamped by func_8002B9CC to a valid index or -1. It is
 * the exact input the .s used to build D_800771C8, so it's the ground truth
 * even for the in-level/overlay callers that bypass func_8002B9CC (which a
 * re-derivation from g_Camera.m_OcclusionGroup would miss). */
static int terrain_vislist(CPUState* cpu, uint32_t sectorCnt, uint16_t* out, int32_t group) {
    if (sectorCnt > 1024) sectorCnt = 1024;
    int ws = openpete_native_widescreen_factor() > 1.0001f;
    if (s_visdbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_VISDBG"); s_visdbg = (e && *e && *e != '0'); }

    uint32_t occCnt    = rd(cpu, G_ENVIRONMENT + ENV_OCCGROUPCNT);
    uint32_t occGroups = rd(cpu, G_ENVIRONMENT + ENV_OCCGROUPS);
    uint32_t list = (group >= 0 && (uint32_t)group < occCnt && occGroups)
                    ? rd(cpu, occGroups + (uint32_t)group * 4) : 0;

    if (ws) {
        int n = 0;
        if (list) {
            for (uint32_t k = 0; k < 2048 && n < 1024; k++) {
                uint8_t idx = cpu->read_byte(list + k);
                if (idx == 0xFF) break;
                if (idx < sectorCnt) out[n++] = idx;
            }
        } else {
            for (uint32_t i = 0; i < sectorCnt && n < 1024; i++) out[n++] = (uint16_t)i;
        }
        if (s_visdbg) terrain_visdbg(cpu, sectorCnt, group, occCnt, out, n, ws);
        return n;
    }

    /* native 4:3 — the canonical .s verdict */
    int n = 0;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint8_t vis = (uint8_t)(rd(cpu, (D_VISIBILITY + i) & ~3u) >> ((i & 3) * 8));
        if (vis == 0xFF) out[n++] = (uint16_t)i;
    }
    if (s_visdbg) {   /* validate the invariant without altering the 4:3 render set */
        static uint16_t cand[1024]; int cn = 0;
        if (list) {
            for (uint32_t k = 0; k < 2048 && cn < 1024; k++) {
                uint8_t idx = cpu->read_byte(list + k);
                if (idx == 0xFF) break;
                if (idx < sectorCnt) cand[cn++] = idx;
            }
        } else for (uint32_t i = 0; i < sectorCnt && cn < 1024; i++) cand[cn++] = (uint16_t)i;
        terrain_visdbg(cpu, sectorCnt, group, occCnt, cand, cn, ws);
    }
    return n;
}

static void terrain_build(CPUState* cpu, int32_t occGroup) {
    terrain_abr_scan(cpu);
    tsc_census();
    s_tv_n = 0; s_tri_n = 0; s_cv_n = 0; s_walk_sectors = 0;
    s_dbg_faces = s_dbg_oob = s_dbg_nclip = s_dbg_behind = s_dbg_spike = s_dbg_clipped = 0;
    s_near_q_agree = s_near_c_only = 0;
    if (s_sectordump < 0) { const char* e = getenv("OPENPETE_TERRAIN_SECTORDUMP"); s_sectordump = (e && *e && *e != '0'); }
    s_sd_n = 0;
    /* Fire the sector dump once, on the target frame (OPENPETE_TERRAIN_VERIFY_FRAME). */
    { static int s_sd_fired = 0; static unsigned s_sd_frame = 0xFFFFFFFFu;
      if (s_sd_frame == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_TERRAIN_VERIFY_FRAME");
                                       s_sd_frame = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
      s_sd_active = (s_sectordump > 0 && !s_sd_fired && openpete_frame >= s_sd_frame);
      if (s_sd_active) s_sd_fired = 1; }
    if (s_caldbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_CALDBG"); s_caldbg = (e && *e && *e != '0'); }
    if (s_nearq == -2) { const char* e = getenv("OPENPETE_TERRAIN_NEARQ"); s_nearq = (e && *e && *e != '0') ? 1 : -1; }
    s_cal_qn = 0; s_cal_qsum_max = 0; s_cal_qmin_max = 0; s_cal_nqsum_min = 1e18;
    /* The render decision uses face_is_near (gamestate); the deferred QUEUE is read
     * ONLY as a verify oracle / for the NEARQ debug path — never in the shipping
     * feed path (the plan forbids consuming .s per-frame output for rendering). */
    int wantOracle = s_caldbg || s_nearq > 0 ||
                     (getenv("OPENPETE_TERRAIN_VERIFY") && *getenv("OPENPETE_TERRAIN_VERIFY") != '0');
    terrain_dbg_init();
    if (s_fogscale < 0.0) {  /* viewZ = m3 (m3 already in SZ3 scale); env-tunable */
        const char* e = getenv("OPENPETE_TERRAIN_FOGSCALE");
        s_fogscale = (e && *e) ? atof(e) : 1.0;
    }
    if (s_nearz < 0.0) {     /* near plane in world/16 view-Z (env-tunable) */
        const char* e = getenv("OPENPETE_TERRAIN_NEARZ");
        s_nearz = (e && *e) ? atof(e) : 1.0;
    }
    { const char* e = getenv("OPENPETE_TERRAIN_FOGDBG"); s_fogdbg = (e && *e && *e != '0'); }
    int M[3][3]; read_matrix(cpu, G_CAMERA + 0x00, M);   /* m_ProjectionMatrix */
    s_ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    s_ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    s_H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
    /* Camera position in world/16 units at FRACTIONAL precision (÷16.0, not >>4).
     * The .s projects terrain in world/16 to fit the GTE's 16-bit range, so it
     * truncates the camera to 16-unit steps — fine at the 30 Hz lockstep, but under
     * sub-tick interpolation it makes the whole terrain frame jump in 16-world-unit
     * chunks while mobys/Spyro (full-precision camera) move smoothly = the terrain
     * jitter, worst under rotation (distant terrain sweeps fast). The native path
     * projects in float, so carrying the low 4 bits costs nothing and tracks the
     * smooth camera. Scale is unchanged (still /16), so depth / near-clip / fog are
     * untouched; only the camera reference gains sub-16 precision. */
    /* Sub-tick floor fix: when a re-extract bracket is active, use the FRACTIONAL
     * interpolated camera position (un-rounded) so terrain — which is purely
     * camera-driven — is TRULY continuous, not just stepped at the 1-world-unit
     * lerp32 grid. Falls back to the (floored) RAM at the 30 Hz lockstep. */
    double camx, camy, camz;
    { double cf[3];
      if (openpete_native_interp_pos(G_CAMERA, cf)) {
          camx = cf[0] / 16.0; camy = cf[1] / 16.0; camz = cf[2] / 16.0;
      } else {
          camx = (double)(int32_t)rd(cpu, G_CAMERA + 0x28) / 16.0;
          camy = (double)(int32_t)rd(cpu, G_CAMERA + 0x2C) / 16.0;
          camz = (double)(int32_t)rd(cpu, G_CAMERA + 0x30) / 16.0;
      } }
    int fogRefBase = (int32_t)rd(cpu, G_ENVIRONMENT + ENV_LODDIST) >> 2;  /* §10 */
    uint32_t lqTable = rd(cpu, G_ENVIRONMENT + ENV_LQTEX);  /* HP-walk per-quad Tiledef (§8) */
    uint32_t hqTable = rd(cpu, G_ENVIRONMENT + ENV_HQTEX);  /* near/detail Tiledef (bpp8 tiles) */
    /* OPENPETE_TERRAIN_TEXTBL: 0=auto(near→HQ,far→LQ) 1=force LQ 2=force HQ (debug) */
    int texTblMode = 0; { const char* e=getenv("OPENPETE_TERRAIN_TEXTBL"); if(e&&*e) texTblMode=atoi(e); }
    if (s_nearhq < 0) {      /* near/HQ subdivide pass enable (Route 2, default ON) */
        const char* e = getenv("OPENPETE_TERRAIN_NEARHQ");
        s_nearhq = (e && *e) ? (*e != '0') : 1;
    }
    s_tex_faces = s_tex_tris = 0;
    if (wantOracle) terrain_read_defer(cpu);   /* oracle only (verify/debug) — see above */
    else s_defer_n = 0;

    static lvert sv[256];
    uint32_t sectorPtr = rd(cpu, G_ENVIRONMENT + ENV_SECTORPTR);
    uint32_t sectorCnt = rd(cpu, G_ENVIRONMENT + ENV_SECTORCNT);
    /* Visible-sector index set: canonical 4:3 verdict (D_800771C8) at native
     * aspect; occlusion-group candidate set in widescreen (terrain_vislist). */
    static uint16_t visidx[1024];
    int nvis = terrain_vislist(cpu, sectorCnt, visidx, occGroup);
    for (int vi = 0; vi < nvis; vi++) {
        uint32_t i = visidx[vi];
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        /* Tier-1 sector decode cache: static level data unpacked once. */
        static int s_hidet = -2;
        if (s_hidet == -2) { const char* e = getenv("OPENPETE_TERRAIN_HIDETAIL"); s_hidet = e ? atoi(e) : -1; }
        const tscache_t* SC = tsc_get(cpu, s, lqTable, hqTable, texTblMode,
                                      s_hidet >= 0 ? s_hidet : 0);
        int flags   = SC->flags;
        int originX = SC->originX, originY = SC->originY, originZ = SC->originZ;
        double s4 = originX - camx, s5 = camy - originY, s6 = camz - originZ;

        int nLpV = SC->nLpV, nLpF = SC->nLpF;
        int nHpV = SC->nHpV, nHpF = SC->nHpF;

        /* Tier selection, distance-INDEPENDENT (no pop-in). HP is drawn wherever
         * a sector HAS HP geometry (the fine layer, all distances). LP is drawn
         * ONLY where there is no HP (the 0x4000 backdrop hills) OR where the
         * sector FORCES it (0x8000, whose HP is a sparse near-detail overlay —
         * stone-hill sector 79 = 3 HP vs 12 LP faces; HP-only there left the
         * LP↔HP hole). Those LP sets are a small, fixed handful, so the LP that
         * rides behind HP (3% LP_DEPTH_BIAS) only fills genuine gaps and isn't a
         * broad overlay.
         *   NB: drawing LP behind HP for EVERY sector (an earlier attempt to also
         *   close the artisans distant-water T-junction seam) made the low-poly
         *   LP poke THROUGH the HP on non-flat sectors at close range — the
         *   coarse mesh bulges in front of the HP detail and the depth bias can't
         *   hold it back. So LP stays restricted to the no-HP / force-LP sets;
         *   the distant-water seam is left for a poke-free fix (stencil hole-fill).
         * (Flags, confirmed vs the binning pass: 0x4000=no-HP [old noHP],
         * 0x2000=no-LP, 0x8000=force-LP.) */
        int drawHP = !(flags & 0x4000) && nHpF > 0;
        int drawLP = !(flags & 0x2000) && nLpF > 0 && ((flags & 0x8000) || !drawHP);

        s_walk_sectors++;

        /* per-sector accounting for OPENPETE_TERRAIN_SECTORDUMP (spans both passes) */
        int sd_triBefore = s_tri_n;
        double sd_m3min = 1e18, sd_m3max = -1e18;
        float sd_bx0 = 1e18f, sd_by0 = 1e18f, sd_bx1 = -1e18f, sd_by1 = -1e18f;
        int sd_vfront = 0, sd_nv = 0, sd_nf = 0;

      for (int pass = 0; pass < 2; pass++) {
        int passLP = (pass == 0);                 /* LP first → drawn behind HP */
        if (passLP && !drawLP) continue;
        if (!passLP && !drawHP) continue;

        uint32_t faces; int nv, nf, faceStride;
        const tsvert_t* cverts; const tsface_t* cfaces;
        uint32_t lpColBase = 0, nearBase = 0, farBase = 0;   /* §9/§10 colour arrays */
        if (passLP) {
            cverts = SC->lpV; cfaces = SC->lpF; nv = nLpV; nf = nLpF;
            faces = SC->lpFaces; faceStride = 8;
            lpColBase = SC->lpColBase;                     /* LP colours (4B, no fog) */
        } else {
            cverts = SC->hpV; cfaces = SC->hpF; nv = nHpV; nf = nHpF;
            faces = SC->hpFaces; faceStride = 16;
            nearBase = SC->nearBase;                       /* HP near colours (4B) */
            farBase  = SC->farBase;                        /* HP far  colours (4B) */
        }
        if (!cverts || !cfaces) continue;                  /* alloc failure: skip pass */
        /* Colour bases → host pointers (fallback: the cpu read path). */
        const uint8_t* h_lp   = passLP ? (const uint8_t*)openpete_psx_to_host_ptr(lpColBase) : NULL;
        const uint8_t* h_near = passLP ? NULL : (const uint8_t*)openpete_psx_to_host_ptr(nearBase);
        const uint8_t* h_far  = passLP ? NULL : (const uint8_t*)openpete_psx_to_host_ptr(farBase);
        int host_col = passLP ? (h_lp != NULL) : (h_near != NULL && h_far != NULL);
        s_emit_noHP = passLP;     /* tag emitted tris with this pass's tier */
        sd_nv += nv; sd_nf += nf;

        for (int k = 0; k < nv; k++) {
            int Xf = cverts[k].x, Yf = cverts[k].y, Zf = cverts[k].z;
            double m1, m2, m3;
            project(M, s5 - Yf, s6 - Zf, Xf + s4, &m1, &m2, &m3);
            sv[k].m1 = m1; sv[k].m2 = m2; sv[k].m3 = m3; sv[k].vz = m3; sv[k].ok = (m3 > 0.0);
            proj_screen(m1, m2, m3, &sv[k].sx, &sv[k].sy);
            if (m3 > 0.0) {
                sd_vfront++;
                if (m3 < sd_m3min) sd_m3min = m3;
                if (m3 > sd_m3max) sd_m3max = m3;
                if (sv[k].sx < sd_bx0) sd_bx0 = sv[k].sx;
                if (sv[k].sy < sd_by0) sd_by0 = sv[k].sy;
                if (sv[k].sx > sd_bx1) sd_bx1 = sv[k].sx;
                if (sv[k].sy > sd_by1) sd_by1 = sv[k].sy;
            }
            if (m3 > 0.0 && s_tv_n < MAX_TVERTS) {       /* feed the geometry A/B */
                s_tvx[s_tv_n] = sv[k].sx; s_tvy[s_tv_n] = sv[k].sy; s_tvz[s_tv_n] = m3; s_tv_n++;
            }
        }

        for (int f = 0; f < nf && s_tri_n < MAX_TTRIS - 4; f++) {  /* clipped quad → ≤4 tris */
            /* Face decode from the sector cache (indices/flags/colour offsets/
             * texmat all static — see tsc_decode_faces; §7/§8 layout comments
             * live there now). Per-present work starts at the projection use. */
            const tsface_t* F = &cfaces[f];
            int i0 = F->i0, i1 = F->i1, i2 = F->i2, i3 = F->i3;
            int flip = F->flip;
            int fogRef = fogRefBase + (F->fogbias ? 0x2000 : 0);
            int c0 = F->c0, c1 = F->c1, c2 = F->c2, c3 = F->c3;
            uint32_t hqW3 = F->w3;
            const tmat* mat = &F->mat;
            const float* tu = F->tu; const float* tv = F->tv;
            if (mat->textured) s_tex_faces++;
            if (i0 >= nv || i1 >= nv || i2 >= nv || i3 >= nv) { s_dbg_oob++; continue; }
            int quad = F->quad;
            s_dbg_faces++;
            /* NCLIP back-face sign (2D cross product == GTE MAC0; flip negates it,
             * word3 & 0x2). front = nclip > 0. */
            double nclip = (sv[i1].sx - sv[i0].sx) * (sv[i2].sy - sv[i0].sy)
                         - (sv[i2].sx - sv[i0].sx) * (sv[i1].sy - sv[i0].sy);
            if (flip) nclip = -nclip;
            int front = (nclip > 0.0);
            /* calibration probe (OPENPETE_TERRAIN_CALDBG): find the Σm3 + minm3
             * boundary that separates the .s's deferred cells (queue oracle) from
             * the rest, to calibrate face_is_near's scale/threshold. */
            if (faceStride == 16 && s_caldbg && front) {  /* front-facing only */
                double rs = sv[i0].m3+sv[i1].m3+sv[i2].m3+sv[i3].m3;
                double rmn = sv[i0].m3; if(sv[i1].m3<rmn)rmn=sv[i1].m3;
                if(sv[i2].m3<rmn)rmn=sv[i2].m3; if(sv[i3].m3<rmn)rmn=sv[i3].m3;
                if (is_deferred(faces + (uint32_t)f * 16u)) {
                    s_cal_qn++; if (rs > s_cal_qsum_max) s_cal_qsum_max = rs;
                    if (rmn > s_cal_qmin_max) s_cal_qmin_max = rmn;
                } else if (rs > 0 && rs < 4096.0) {
                    if (rs < s_cal_nqsum_min) s_cal_nqsum_min = rs;
                }
            }
            /* Near/HQ subdivide dispatch (Route 2): if this face classifies as a
             * near foreground cell (face_is_near, replicating the .s defer test from
             * gamestate), subdivide it 2×2 and texture from the per-cell HQ record
             * (word2 & 0x7F → m_HQTexturePointer + idx*168; word2 & 0x80 → ABE)
             * instead of the flat LQ emit — exactly the cells func_800276A0
             * subdivides. Corner→vertex: TL=i0,TR=i1,BL=i3,BR=i2 (the .s grid order).
             * Near colours are the raw HP near-colour array (no DPCS fog on near
             * cells — func_800276A0 reads s5[idx] directly). */
            /* ONLY quads route to the 2×2 near subdivide. Near TRIANGLES (i2==i3)
             * are emitted by the .s via its detail-descriptor path (func_800276A0
             * block_80027C74, GT3 sub-tris) — NOT the simple 2×2 quad path
             * (block_80027D88). terrain_near_emit only replicates the 2×2 quad path,
             * so forcing a triangle through it (BL==BR → degenerate sub-quads)
             * produced a protruding mis-textured "flap" at close range (the
             * Artisans bridge corner, tile-9 GT3 orientation 2 — the same face the
             * far-path triRot fix targets). Instead let near triangles fall through
             * to the standard GT3 emit below, which already applies the 2-bit UV
             * orientation (triRot). Geometry + orientation match; the only gap vs
             * the .s is LQ-vs-HQ texture on that single tri (the HQ detail-path
             * sub-tessellation is the existing carve-out). */
            int near = (faceStride == 16 && s_nearhq && hqTable && front && quad &&
                        face_is_near(sv[i0].m3, sv[i1].m3, sv[i2].m3, sv[i3].m3, hqW3));
            if (s_nearq >= 0)   /* debug: use the queue oracle directly (exact 37) */
                near = (faceStride == 16 && s_nearhq && hqTable && quad && is_deferred(faces + (uint32_t)f*16u));
            if (near) {
                if (is_deferred(faces + (uint32_t)f * 16u)) s_near_q_agree++; else s_near_c_only++;
                int abe = F->abe;
                uint32_t r0, r1, r2, r3;
                if (host_col) {
                    r0 = t_le32(h_near + c0) & 0xFFFFFFu; r1 = t_le32(h_near + c1) & 0xFFFFFFu;
                    r2 = t_le32(h_near + c2) & 0xFFFFFFu; r3 = t_le32(h_near + c3) & 0xFFFFFFu;
                } else {
                    r0 = rd(cpu, nearBase + (uint32_t)c0) & 0xFFFFFFu;
                    r1 = rd(cpu, nearBase + (uint32_t)c1) & 0xFFFFFFu;
                    r2 = rd(cpu, nearBase + (uint32_t)c2) & 0xFFFFFFu;
                    r3 = rd(cpu, nearBase + (uint32_t)c3) & 0xFFFFFFu;
                }
                cvert TL = { sv[i0].m1, sv[i0].m2, sv[i0].m3, r0, 0, 0 };
                cvert TR = { sv[i1].m1, sv[i1].m2, sv[i1].m3, r1, 0, 0 };
                cvert BL = { sv[i3].m1, sv[i3].m2, sv[i3].m3, r3, 0, 0 };
                cvert BR = { sv[i2].m1, sv[i2].m2, sv[i2].m3, r2, 0, 0 };
                terrain_near_emit(cpu, hqTable + (uint32_t)F->hq_idx * 168u, abe,
                                  TL, TR, BL, BR);
                s_dbg_spike++;   /* near/HQ cells subdivided */
                continue;
            }
            /* NCLIP back-face cull (off by default — the Z-buffer hides back-faces;
             * OPENPETE_TERRAIN_CULL=1 enables). front = nclip > 0, computed above. */
            if (s_cull && !front) { s_dbg_nclip++; continue; }
            /* Native per-corner colour: RGB888 + DPCS fog (HP) or flat (LP). The
             * colour index is per face corner (word1); the fog viewZ is the
             * corner's vertex m3. (§9/§10 — retires the NN scaffold.) */
            uint32_t rc0, rc1, rc2, rc3;
            if (host_col) {
                rc0 = terrain_color_h(passLP, h_lp, h_near, h_far, c0, sv[i0].vz, fogRef);
                rc1 = terrain_color_h(passLP, h_lp, h_near, h_far, c1, sv[i1].vz, fogRef);
                rc2 = terrain_color_h(passLP, h_lp, h_near, h_far, c2, sv[i2].vz, fogRef);
                rc3 = quad ? terrain_color_h(passLP, h_lp, h_near, h_far, c3, sv[i3].vz, fogRef) : 0;
            } else {
                rc0 = terrain_color(cpu, passLP, lpColBase, nearBase, farBase, c0, sv[i0].vz, fogRef);
                rc1 = terrain_color(cpu, passLP, lpColBase, nearBase, farBase, c1, sv[i1].vz, fogRef);
                rc2 = terrain_color(cpu, passLP, lpColBase, nearBase, farBase, c2, sv[i2].vz, fogRef);
                rc3 = quad ? terrain_color(cpu, passLP, lpColBase, nearBase, farBase, c3, sv[i3].vz, fogRef) : 0;
            }
            /* UV corners. decode_tiledef gives tu/tv[0..3] = [TL,TR,BL,BR]. A QUAD
             * (GT4) uses them directly via the .s slot reorder (i0→TL, i1→TR,
             * i3→BL, i2→BR). A TRIANGLE (GT3) carries a 2-bit UV ORIENTATION in
             * bits 8-9 of the texture word (w2): the .s GT3 emit (func_800258F0
             * .L80027534 — `t4 & 0x300`) selects one of 4 corner rotations. Native
             * applied only orientation 0, leaving rotated tiles 90°/180°-wrong (the
             * Artisans bridge-junction "flap" — a tile-9 GT3 with orientation 2). */
            lcorner k0 = { rc0, tu[0], tv[0] };                  /* i0 = TL */
            lcorner k1 = { rc1, tu[1], tv[1] };                  /* i1 = TR */
            lcorner k3 = { rc3, tu[2], tv[2] };                  /* i3 = BL */
            lcorner k2 = { rc2, quad ? tu[3] : tu[2], quad ? tv[3] : tv[2] };  /* i2 = BR/BL */
            /* GPU triangulation (affine-faithful): a quad is the prim strip
             * slots (0,1,2)+(1,2,3) = (i0,i1,i3)+(i1,i3,i2) — diagonal i1-i3
             * (NOT a FAN i0-i2: affine texture/colour warp is diagonal-dependent,
             * the cause of the "slanted textures"). A tri is just (i0,i1,i2). */
            if (quad && faceStride == 8) {
                /* LP (untextured G4) quad: the .s LP loop (r_environment .L80026390)
                 * NCLIPs tri(i0,i1,i2) then tri(i3,i1,i2) — a plain GPU strip with
                 * diagonal i1-i2 and NO i2/i3 slot swap. (HP's GT4 swaps i2/i3 below;
                 * applying that swap to LP's scanline-ordered corners bowties the quad
                 * → overlapping tris + gouraud-colour bleed = the missing distant-
                 * terrain polys + brown artifacts.) Untextured, so corner UVs unused. */
                emit_tri(sv, i0, i1, i2, k0, k1, k2, mat);
                emit_tri(sv, i1, i2, i3, k1, k2, k3, mat);
            } else if (quad) {
                emit_tri(sv, i0, i1, i3, k0, k1, k3, mat);
                emit_tri(sv, i1, i3, i2, k1, k3, k2, mat);
            } else {
                /* GT3 UV orientation: corner indices into [TL,TR,BL,BR]=[0,1,2,3]
                 * for slots (i0,i1,i2). rot 0 = {TL,TR,BL} (the .s .L80027588),
                 * 1 = {TR,BR,TL}, 2 = {BR,BL,TR}, 3 = {BL,TL,BR} (.L800275A4/C4/DC). */
                static const int triRot[4][3] = {{0,1,2},{1,3,0},{3,2,1},{2,0,3}};
                int rot = (faceStride == 16) ? (int)F->trirot : 0;
                const int* tcn = triRot[rot];
                lcorner t0 = { rc0, tu[tcn[0]], tv[tcn[0]] };
                lcorner t1 = { rc1, tu[tcn[1]], tv[tcn[1]] };
                lcorner t2 = { rc2, tu[tcn[2]], tv[tcn[2]] };
                emit_tri(sv, i0, i1, i2, t0, t1, t2, mat);
            }
        }
      }  /* pass (LP / HP) */
        if (s_sd_active && s_sd_n < 1024) {
            secrec* r = &s_sd[s_sd_n++];
            r->idx = (int)i; r->flags = flags;
            r->noHP = (drawLP ? 1 : 0) | (drawHP ? 2 : 0);   /* 1=LP 2=HP 3=BOTH */
            r->nLpV = nLpV; r->nLpF = nLpF; r->nHpV = nHpV; r->nHpF = nHpF;
            r->nv = sd_nv; r->nf = sd_nf; r->tris = s_tri_n - sd_triBefore; r->vfront = sd_vfront;
            r->ox = originX; r->oy = originY; r->oz = originZ;
            r->m3min = sd_m3min; r->m3max = sd_m3max;
            r->bx0 = sd_bx0; r->by0 = sd_by0; r->bx1 = sd_bx1; r->by1 = sd_by1;
        }
    }

    /* ---- LP↔HP gap probe (OPENPETE_TERRAIN_SECTORDUMP) -------------------- */
    if (s_sd_active && s_sd_n > 0) {
        fprintf(stderr, "\n[sectordump] frame=%u  visible=%d  nativeTris=%d  capturedPrims=%d\n",
                openpete_frame, s_sd_n, s_tri_n, s_gte_n);
        fprintf(stderr, "  %-4s %-5s %-3s %5s %5s %5s %5s %5s %6s  %-10s  %-9s  %-21s\n",
                "vi", "flags", "nHP", "nLpV", "nLpF", "nHpV", "nHpF", "tris", "vfront",
                "m3[min..max]", "tier", "screen-bbox");
        for (int j = 0; j < s_sd_n; j++) {
            secrec* r = &s_sd[j];
            const char* tlab = r->noHP == 3 ? "BOTH" : r->noHP == 1 ? "LP" : r->noHP == 2 ? "HP" : "-";
            fprintf(stderr,
                "  %-4d %04X  %-3d %5d %5d %5d %5d %5d %6d  %5.0f..%-5.0f  %-9s  %4.0f,%-4.0f %4.0f,%-4.0f\n",
                r->idx, r->flags, r->noHP, r->nLpV, r->nLpF, r->nHpV, r->nHpF, r->tris, r->vfront,
                r->m3min, r->m3max, tlab,
                r->bx0, r->by0, r->bx1, r->by1);
        }
        /* coverage-gap grid: cells covered by captured (PSX) prims but NOT by any
         * native tri = the cyclorama-bleed band. Grid over the 4:3 screen. */
        enum { GW = 64, GH = 36 };
        static uint8_t cap_cov[GW*GH], nat_cov[GW*GH];
        memset(cap_cov, 0, sizeof(cap_cov)); memset(nat_cov, 0, sizeof(nat_cov));
        const float SW = 512.0f, SH = 256.0f;   /* PSX 4:3 framebuffer extent */
        /* mark native tris — SKIP near-clip giant slivers (any vertex far
         * off-screen). Those don't rasterize across the visible band on the GPU
         * but a raw point-in-tri test would falsely mark them as coverage and
         * hide the real hole. */
        int skipped_giant = 0;
        for (int t = 0; t < s_tri_n; t++) {
            ttri* T = &s_tri[t];
            float mx = 0;
            for (int k = 0; k < 3; k++) {
                float ax = T->sx[k] < 0 ? -T->sx[k] : T->sx[k];
                float ay = T->sy[k] < 0 ? -T->sy[k] : T->sy[k];
                if (ax > mx) mx = ax; if (ay > mx) mx = ay;
            }
            if (mx > 1500.0f) { skipped_giant++; continue; }
            for (int gy = 0; gy < GH; gy++) for (int gx = 0; gx < GW; gx++) {
                float px = (gx + 0.5f) * SW / GW, py = (gy + 0.5f) * SH / GH;
                if (pt_in_tri(px, py, T->sx[0],T->sy[0], T->sx[1],T->sy[1], T->sx[2],T->sy[2]))
                    nat_cov[gy*GW+gx] = 1;
            }
        }
        fprintf(stderr, "  (skipped %d giant/near-clip tris from native coverage)\n", skipped_giant);
        /* mark captured prims (quad → two tris) */
        for (int p = 0; p < s_gte_n; p++) {
            prim_t* g = &s_gte[p];
            for (int gy = 0; gy < GH; gy++) for (int gx = 0; gx < GW; gx++) {
                float px = (gx + 0.5f) * SW / GW, py = (gy + 0.5f) * SH / GH;
                int hit = pt_in_tri(px,py, g->x[0],g->y[0], g->x[1],g->y[1], g->x[2],g->y[2]);
                if (!hit && g->nv == 4)
                    hit = pt_in_tri(px,py, g->x[1],g->y[1], g->x[2],g->y[2], g->x[3],g->y[3]);
                if (hit) cap_cov[gy*GW+gx] = 1;
            }
        }
        int gap = 0;
        fprintf(stderr, "  gap map ('#'=PSX-only/missing, '.'=native-ok, ' '=neither, 'o'=native-extra):\n");
        for (int gy = 0; gy < GH; gy++) {
            fprintf(stderr, "  ");
            for (int gx = 0; gx < GW; gx++) {
                int c = cap_cov[gy*GW+gx], n = nat_cov[gy*GW+gx];
                char ch = (c && !n) ? '#' : (c && n) ? '.' : (!c && n) ? 'o' : ' ';
                if (c && !n) gap++;
                fputc(ch, stderr);
            }
            fputc('\n', stderr);
        }
        fprintf(stderr, "  gap cells (PSX-covered, native-missing): %d / %d\n", gap, GW*GH);
    }
}

/* Feed the built tris to the native scene. Textured faces sample the PSX VRAM
 * mirror via the Tiledef material (modulated by the per-vertex DPCS-fogged
 * colour); untextured faces are gouraud. Colour = native RGB888 + DPCS fog. */
static int s_notex = -1;   /* OPENPETE_TERRAIN_NOTEX: force untextured (debug) */
static int s_tiercolor = -1;  /* OPENPETE_TERRAIN_TIERCOLOR: LP=red, HP=blue */
static int s_tieronly = -2;   /* OPENPETE_TERRAIN_TIERONLY: 1=LP only, 2=HP only */
static void terrain_feed_native(void) {
    if (s_notex < 0) { const char* e = getenv("OPENPETE_TERRAIN_NOTEX"); s_notex = (e && *e && *e != '0'); }
    if (s_tiercolor < 0) { const char* e = getenv("OPENPETE_TERRAIN_TIERCOLOR"); s_tiercolor = (e && *e && *e != '0'); }
    if (s_tieronly == -2) { const char* e = getenv("OPENPETE_TERRAIN_TIERONLY"); s_tieronly = e ? atoi(e) : 0; }
    { static int s_cap = -1, s_peak = 0;
      if (s_cap < 0) { const char* e = getenv("OPENPETE_SCENE_CAPDBG"); s_cap = (e && *e && *e != '0'); }
      if (s_cap && s_tri_n > s_peak) { s_peak = s_tri_n;
          fprintf(stderr, "[terraincap] NEW PEAK terrain_tris=%d / MAX_TTRIS=%d\n", s_tri_n, MAX_TTRIS); } }
    for (int i = 0; i < s_tri_n; i++) {
        ttri* t = &s_tri[i];
        if (s_tieronly == 1 && !t->tier) continue;   /* LP only */
        if (s_tieronly == 2 &&  t->tier) continue;   /* HP only */
        openpete_scene_tri tri; memset(&tri, 0, sizeof(tri));
        int tex = (!s_flat && !s_notex && !s_tiercolor) ? t->textured : 0;
        tri.textured = tex; tri.blend = tex ? t->blend : 0;
        tri.tpage_x = t->tpx; tri.tpage_y = t->tpy;
        tri.clut_x = t->clx; tri.clut_y = t->cly; tri.bpp = t->bpp;
        for (int k = 0; k < 3; k++) {
            tri.v[k].sx = t->sx[k]; tri.v[k].sy = t->sy[k];
            /* Terrain view-Z is in world/16 units (cam>>4, verts /16); the
             * actors feed FULL-scale view-Z. depth_from_viewz expects ONE shared
             * scale, so ×16 to match — else terrain's depth is ~16× too small
             * (always nearest) and draws on top of every actor.
             * LP_DEPTH_BIAS: the coarse LP layer is pushed slightly BEHIND the
             * HP layer (PSX draws LP first, HP overwrites it) so where a sector
             * carries both tiers the fine HP mesh wins the depth test and the LP
             * shows only in the seam gaps HP doesn't reach. Harmless for pure-LP
             * far sectors (still far in front of the cyclorama). */
            float vzf = (float)t->vz[k] * 16.0f;
            if (t->tier) vzf *= 1.03f;   /* tier==1 → LP */
            tri.v[k].z  = openpete_native_depth_from_viewz(vzf);
            tri.v[k].u = t->u[k]; tri.v[k].v = t->v[k];
            /* Textured: modulate the sampled texel by the fogged colour (the
             * pipeline does tex*colour/128, so feed the full RGB). Untextured:
             * the gouraud colour; s_flat = a debug coverage colour;
             * s_tiercolor = LP red / HP blue. */
            tri.v[k].rgba = s_tiercolor ? (t->tier ? 0xFF0000FFu : 0xFFFF0000u)
                          : s_flat       ? 0xFF20C040u
                          : (0xFF000000u | (t->rgb[k] & 0xFFFFFFu));
        }
        openpete_native_scene_add_tri(&tri, OPENPETE_SCENE_WORLD, 0);
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
    fprintf(stderr, "[terrain-faces] walked=%d oob=%d fullyBehind=%d nearClipped=%d nclipCulled=%d emitted=%d  texturedFaces=%d texTris=%d hqCells=%d\n",
        s_dbg_faces, s_dbg_oob, s_dbg_behind, s_dbg_clipped, s_dbg_nclip, s_tri_n, s_tex_faces, s_tex_tris, s_dbg_spike);
    fprintf(stderr, "[terrain-defer] queue(oracle): +0x2000{hdr=%d cell=%d} +0x1E00{hdr=%d cell=%d} total=%d  |  classifier: near=%d (×4 subquads → %d bpp8 prims)  agreeWithQueue=%d classifierOnly=%d queueOnly=%d\n",
        s_defer_hdr2, s_defer_cell2, s_defer_hdr1e, s_defer_cell1e, s_defer_n,
        s_dbg_spike, s_dbg_spike * 4, s_near_q_agree, s_near_c_only, s_defer_n - s_near_q_agree);
    if (s_caldbg)
        fprintf(stderr, "[terrain-cal] queueCells=%d  max(Σm3 over queue)=%.0f  max(minM3 over queue)=%.0f  min(Σm3 over non-queue HP, <4096)=%.0f\n",
            s_cal_qn, s_cal_qsum_max, s_cal_qmin_max, s_cal_nqsum_min);

    /* Colour A/B: NN-match each native colour-vert (post-fog) to the nearest
     * captured prim corner by SCREEN position; where the geometry match is tight
     * (<=2px) the captured prim's RGB is the .s DPCS oracle for that corner, so
     * compare. (Shared verts carry per-corner colours, so a few NN mismatches are
     * expected — read the median + exact-% as the discriminator, not the worst.) */
    build_captured_colors();
    if (s_cc_n && s_cv_n) {
        long matched = 0, exact = 0, near4 = 0; double esum = 0; int eworst = 0;
        /* Near-regime subset: f saturates to 1 (no fog) when 4·m3 <= fogRef-0x1000;
         * with fogRef>=0x2000 that's m3 < 0x400. There the .s also renders HP
         * unfogged → must match near-exactly. This isolates the decode (array
         * base/byte order) from the LP/HP tier confound + fog rounding. */
        long nmatched = 0, nexact = 0, nnear4 = 0; double nesum = 0; int nworst = 0;
        for (int i = 0; i < s_cv_n; i++) {
            float best = 1e18f; uint32_t crgb = 0;
            for (int j = 0; j < s_cc_n; j++) {
                float dx = s_cvx[i] - s_ccx[j], dy = s_cvy[i] - s_ccy[j], d = dx*dx + dy*dy;
                if (d < best) { best = d; crgb = s_ccrgb[j]; }
            }
            if (best > 4.0f) continue;                  /* geometry too loose to trust */
            uint32_t n = s_cvrgb[i];
            int dr = abs((int)(n & 0xFF)        - (int)(crgb & 0xFF));
            int dg = abs((int)((n >> 8) & 0xFF) - (int)((crgb >> 8) & 0xFF));
            int db = abs((int)((n >> 16) & 0xFF)- (int)((crgb >> 16) & 0xFF));
            int de = dr + dg + db;
            matched++; esum += de; if (de > eworst) eworst = de;
            if (de == 0) exact++;
            if (dr <= 4 && dg <= 4 && db <= 4) near4++;
            if (s_cvz[i] < 1024.0) {                     /* near (unfogged) regime */
                nmatched++; nesum += de; if (de > nworst) nworst = de;
                if (de == 0) nexact++;
                if (dr <= 4 && dg <= 4 && db <= 4) nnear4++;
            }
            if (s_fogdbg && s_cvz[i] > 1024.0 && de > 30) {  /* fogged, large-err samples */
                static int pr; if (pr < 20) { pr++;
                    fprintf(stderr, "[colcmp] m3=%.0f native=%06X psx=%06X  dRGB=(%d,%d,%d)\n",
                            s_cvz[i], n, crgb, dr, dg, db); }
            }
        }
        if (matched)
            fprintf(stderr,
                "[terrain-color] matched=%ld  meanΔ(r+g+b)=%.2f exact=%.0f%% within±4=%.0f%% worstΔ=%d\n",
                matched, esum / matched, 100.0 * exact / matched, 100.0 * near4 / matched, eworst);
        else
            fprintf(stderr, "[terrain-color] no tight geometry matches to score colour\n");
        if (nmatched)
            fprintf(stderr,
                "[terrain-color-near] matched=%ld  meanΔ=%.2f exact=%.0f%% within±4=%.0f%% worstΔ=%d (m3<1024, unfogged HP-in-both)\n",
                nmatched, nesum / nmatched, 100.0 * nexact / nmatched, 100.0 * nnear4 / nmatched, nworst);
    }

    /* UV/tile A/B (OPENPETE_TERRAIN_UVDBG): for each native textured tri, NN-match
     * to the nearest captured TEXTURED prim by screen centroid; where the centroid
     * match is tight, the captured prim's UVs/tpage/CLUT are the .s oracle — print
     * native vs captured so a tile-select / UV-mapping / CLUT-row divergence is
     * visible directly (geometry A/B can't see it). Optional box filter via
     * OPENPETE_TERRAIN_UVBOX="x0,y0,x1,y1" to focus on one feature (e.g. platform). */
    { const char* e = getenv("OPENPETE_TERRAIN_UVDBG");
      if (e && *e && *e != '0') {
        float bx0=-1e9f,by0=-1e9f,bx1=1e9f,by1=1e9f;
        const char* be = getenv("OPENPETE_TERRAIN_UVBOX");
        if (be && *be) sscanf(be, "%f,%f,%f,%f", &bx0, &by0, &bx1, &by1);
        int shown = 0, want = 40;
        long uvmatch=0, uvexact_tp=0, uvexact_cl=0, tight=0, tdis=0;
        double hi_min=1e18,hi_max=0,lo_min=1e18,lo_max=0,hi_sum=0,lo_sum=0; long hin=0,lon=0;
        for (int i = 0; i < s_tri_n; i++) {
            ttri* t = &s_tri[i];
            if (!t->textured) continue;
            float ncx=(t->sx[0]+t->sx[1]+t->sx[2])/3.f, ncy=(t->sy[0]+t->sy[1]+t->sy[2])/3.f;
            if (ncx<bx0||ncx>bx1||ncy<by0||ncy>by1) continue;
            /* nearest captured textured prim by centroid */
            int bj=-1; float bd=1e18f;
            for (int p=0;p<s_gte_n;p++){ prim_t* g=&s_gte[p]; if(!g->textured) continue;
                float gcx=0,gcy=0; for(int k=0;k<g->nv;k++){gcx+=g->x[k];gcy+=g->y[k];}
                gcx/=g->nv; gcy/=g->nv; float dx=ncx-gcx,dy=ncy-gcy,d=dx*dx+dy*dy;
                if(d<bd){bd=d;bj=p;} }
            if (bj<0) continue; bd=sqrtf(bd);
            if (bd > 6.0f) continue;                /* loose match — skip */
            prim_t* g=&s_gte[bj];
            /* decode captured material the same way terrain_texmat does */
            int gtpx=(g->tpage&0xF)*64, gtpy=((g->tpage>>4)&1)*256;
            int gclx=(g->clut&0x3F)*16, gcly=(g->clut>>6)&0x1FF;
            int gmode=(g->tpage>>7)&3, gbpp=(gmode==0)?4:(gmode==1)?8:16;
            uvmatch++;
            int tp_ok = (gtpx==t->tpx && gtpy==t->tpy && gbpp==t->bpp);
            int cl_ok = (gclx==t->clx && gcly==t->cly);
            if (tp_ok) uvexact_tp++;
            if (cl_ok) uvexact_cl++;
            /* Only print TIGHT matches (d<2.5px, trustworthy spatial pairing) that
             * actually DISAGREE — those are real per-tile decode divergences, not
             * NN noise from grabbing an adjacent checkerboard tile. */
            int printall = getenv("OPENPETE_TERRAIN_UVALL") != NULL;
            if (((printall && bd < 6.0f) || (bd < 2.5f && (!tp_ok || !cl_ok))) && shown < want) { shown++;
                fprintf(stderr,
                  "[uvdbg] c=(%.0f,%.0f) d=%.1f %s%s | NAT tp=(%d,%d) bpp=%d cl=(%d,%d) uv=[(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)]\n"
                  "                          | PSX tp=(%d,%d) bpp=%d cl=(%d,%d) uv=",
                  ncx, ncy, bd, tp_ok?"":"TP! ", cl_ok?"":"CL! ", t->tpx, t->tpy, t->bpp, t->clx, t->cly,
                  t->u[0],t->v[0],t->u[1],t->v[1],t->u[2],t->v[2],
                  gtpx, gtpy, gbpp, gclx, gcly);
                for (int k=0;k<g->nv;k++) fprintf(stderr,"(%d,%d)",g->u[k],g->v[k]);
                fprintf(stderr, " raw tpage=%04X clut=%04X\n", g->tpage, g->clut);
            }
            if (bd < 2.5f) { tight++; if (!tp_ok || !cl_ok) tdis++;
                /* empirical near/far fit: native mean view-Z (m3) vs the table the
                 * game chose (bpp8 = HQ/near, bpp4 = LQ/far). */
                double mvz = (t->vz[0]+t->vz[1]+t->vz[2])/3.0;
                if (gbpp==8){ if(mvz<hi_min)hi_min=mvz; if(mvz>hi_max)hi_max=mvz; hin++; hi_sum+=mvz; }
                else if (gbpp==4){ if(mvz<lo_min)lo_min=mvz; if(mvz>lo_max)lo_max=mvz; lon++; lo_sum+=mvz; }
            }
        }
        fprintf(stderr,"[nearfit] HQ/bpp8: n=%ld vz[%.0f..%.0f] mean=%.0f | LQ/bpp4: n=%ld vz[%.0f..%.0f] mean=%.0f\n",
            hin,hi_min,hi_max,hin?hi_sum/hin:0.0, lon,lo_min,lo_max,lon?lo_sum/lon:0.0);
        /* bpp histograms: native tris vs captured textured prims */
        { long nb[3]={0,0,0}, cb[3]={0,0,0};
          for(int i2=0;i2<s_tri_n;i2++){ if(!s_tri[i2].textured)continue;
              int b=s_tri[i2].bpp; nb[b==4?0:b==8?1:2]++; }
          for(int p=0;p<s_gte_n;p++){ prim_t* g=&s_gte[p]; if(!g->textured)continue;
              int m=(g->tpage>>7)&3,b=(m==0)?4:(m==1)?8:16; cb[b==4?0:b==8?1:2]++; }
          fprintf(stderr,"[uvdbg-bpphist] NAT 4bpp=%ld 8bpp=%ld 16bpp=%ld | PSX 4bpp=%ld 8bpp=%ld 16bpp=%ld\n",
              nb[0],nb[1],nb[2],cb[0],cb[1],cb[2]); }
        fprintf(stderr, "[uvdbg-summary] matched=%ld  tpage/bpp-exact=%ld (%.0f%%)  clut-exact=%ld (%.0f%%)\n",
            uvmatch, uvexact_tp, uvmatch?100.0*uvexact_tp/uvmatch:0.0,
            uvexact_cl, uvmatch?100.0*uvexact_cl/uvmatch:0.0);
        fprintf(stderr, "[uvdbg-tight] tight(<2.5px)=%ld disagree=%ld (%.0f%%)\n",
            tight, tdis, tight?100.0*tdis/tight:0.0);
      }
    }

    /* Point probe (OPENPETE_TERRAIN_POINT="x,y;x,y;..."): for each screen point,
     * find the FRONTMOST native tri and the FRONTMOST .s prim covering it
     * (point-in-poly + nearest vz), print both tiles — exact, no NN looseness. */
    { const char* pe = getenv("OPENPETE_TERRAIN_POINT");
      if (pe && *pe) {
        const char* p = pe;
        while (*p) {
            float px, py; if (sscanf(p, "%f,%f", &px, &py) != 2) break;
            /* frontmost native tri (min vz) whose screen tri contains (px,py) */
            int bi=-1; double bvz=1e18;
            for (int i=0;i<s_tri_n;i++){ ttri* t=&s_tri[i];
                float ax=t->sx[0],ay=t->sy[0],bx=t->sx[1],by=t->sy[1],cx=t->sx[2],cy=t->sy[2];
                float d1=(px-bx)*(ay-by)-(ax-bx)*(py-by);
                float d2=(px-cx)*(by-cy)-(bx-cx)*(py-cy);
                float d3=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
                int neg=(d1<0)||(d2<0)||(d3<0), pos=(d1>0)||(d2>0)||(d3>0);
                if(neg&&pos) continue;                       /* not inside */
                double vz=(t->vz[0]+t->vz[1]+t->vz[2])/3.0;
                if(vz<bvz){bvz=vz;bi=i;} }
            /* ALL .s prims covering the point (draw order; last = painter winner) */
            int gj=-1;
            fprintf(stderr,"          (%.0f,%.0f) PSX covering prims (draw order):\n",px,py);
            for (int q=0;q<s_gte_n;q++){ prim_t* g=&s_gte[q];
                int inside=0;
                int tris[2][3]={{0,1,2},{0,2,3}}; int nt=(g->nv==4)?2:1;
                for(int tt=0;tt<nt&&!inside;tt++){ int*ix=tris[tt];
                    float ax=g->x[ix[0]],ay=g->y[ix[0]],bx=g->x[ix[1]],by=g->y[ix[1]],cx=g->x[ix[2]],cy=g->y[ix[2]];
                    float d1=(px-bx)*(ay-by)-(ax-bx)*(py-by);
                    float d2=(px-cx)*(by-cy)-(bx-cx)*(py-cy);
                    float d3=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
                    int neg=(d1<0)||(d2<0)||(d3<0), pos=(d1>0)||(d2>0)||(d3>0);
                    if(!(neg&&pos)) inside=1; }
                if(inside){ int gtpx=(g->tpage&0xF)*64, gtpy=((g->tpage>>4)&1)*256;
                    int gclx=(g->clut&0x3F)*16, gcly=(g->clut>>6)&0x1FF;
                    fprintf(stderr,"      prim#%d tp=(%d,%d) cl=(%d,%d) nv=%d xy=",q,gtpx,gtpy,gclx,gcly,g->nv);
                    for(int k=0;k<g->nv;k++) fprintf(stderr,"(%.0f,%.0f)",g->x[k],g->y[k]);
                    fprintf(stderr," uv=");
                    for(int k=0;k<g->nv;k++) fprintf(stderr,"(%d,%d)",g->u[k],g->v[k]);
                    fprintf(stderr,"\n"); gj=q; } }
            /* list ALL native tris covering the point (depth order) */
            fprintf(stderr,"[point] (%.0f,%.0f) NAT covering tris:\n",px,py);
            for (int i=0;i<s_tri_n;i++){ ttri* t=&s_tri[i];
                float ax=t->sx[0],ay=t->sy[0],bx=t->sx[1],by=t->sy[1],cx=t->sx[2],cy=t->sy[2];
                float d1=(px-bx)*(ay-by)-(ax-bx)*(py-by);
                float d2=(px-cx)*(by-cy)-(bx-cx)*(py-cy);
                float d3=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
                int neg=(d1<0)||(d2<0)||(d3<0), pos=(d1>0)||(d2>0)||(d3>0);
                if(neg&&pos) continue;
                double vz=(t->vz[0]+t->vz[1]+t->vz[2])/3.0;
                fprintf(stderr,"    tri#%d tp=(%d,%d) cl=(%d,%d) vz=%.0f xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) uv=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                    i,t->tpx,t->tpy,t->clx,t->cly,vz,
                    t->sx[0],t->sy[0],t->sx[1],t->sy[1],t->sx[2],t->sy[2],
                    t->u[0],t->v[0],t->u[1],t->v[1],t->u[2],t->v[2]); }
            (void)bi;(void)bvz;
            if(gj>=0){ prim_t* g=&s_gte[gj];
                int gtpx=(g->tpage&0xF)*64, gtpy=((g->tpage>>4)&1)*256;
                int gclx=(g->clut&0x3F)*16, gcly=(g->clut>>6)&0x1FF;
                int gm=(g->tpage>>7)&3, gbpp=(gm==0)?4:(gm==1)?8:16;
                fprintf(stderr,"          (%.0f,%.0f) PSX prim#%d tex=%d tp=(%d,%d) bpp=%d cl=(%d,%d) raw tpage=%04X clut=%04X uv=",
                    px,py,gj,g->textured,gtpx,gtpy,gbpp,gclx,gcly,g->tpage,g->clut);
                for(int k=0;k<g->nv;k++) fprintf(stderr,"(%d,%d)",g->u[k],g->v[k]);
                fprintf(stderr,"\n");
            } else fprintf(stderr,"          (%.0f,%.0f) PSX none\n",px,py);
            const char* sc=strchr(p,';'); if(!sc) break; p=sc+1;
        }
      }
    }

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
/* Occlusion group from the last real render call — reused by the sub-tick
 * re-extract (which has no a0). -1 = walk-all (valid index or -1, as the .s). */
static int32_t s_terrain_saved_occGroup = -1;

static int on_800258F0_terrain(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = terrain_verify_enabled();
    int feed   = openpete_native_render_enabled();
    if (!verify && !feed && !render_paths_active()) return 0;  /* inert: normal dispatch */
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    /* Capture the occlusion-group arg BEFORE the real call clobbers a0. This is
     * the exact input the .s uses to build D_800771C8 — the ground truth for the
     * widescreen candidate set (terrain_vislist). */
    int32_t occGroup = (int32_t)cpu->a0;
    s_terrain_saved_occGroup = occGroup;   /* for sub-tick re-extract */

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_800258F0(cpu);                            /* run the real renderer (verdicts intact) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_TERRAIN, startp, endp);

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_TERRAIN)) {
        terrain_build(cpu, occGroup);               /* native walk + tris */
        openpete_native_scene_begin_frame(openpete_frame);  /* frame-keyed (composes) */
        terrain_feed_native();                      /* push tris to the scene */
    }

    if (verify && openpete_frame >= terrain_verify_frame()) {
        static int s_reports = 0;
        if (s_reports < 8) {
            terrain_dump(cpu);
            terrain_build(cpu, occGroup); /* native float decode + projection + faces */
            terrain_compare();          /* geometry A/B vs captured prims */
            s_reports++;
        }
    }
    s_in = 0;
    return 1;
}

/* Sub-tick re-extract (high-FPS spine): reproduce the terrain scene from the
 * current gamestate + the interpolated g_Camera the orchestrator has written
 * — extract-only (no func_800258F0 call, no verdict writes). Terrain is static
 * world geometry, so an interpolated camera alone makes it move smoothly. */
void terrain_native_reextract(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;
    if (!render_path_enabled(RP_TERRAIN)) return;
    /* Reproduce terrain ONLY if the canonical frame drew it. During a level
     * transition the game stops calling func_800258F0 (no world to render), but
     * s_terrain_saved_occGroup + the level data still in RAM are stale; replaying
     * them paints the old level's geometry over the cyclorama. The canon-drawn
     * mask is clear for RP_TERRAIN on those frames, so we skip. */
    if (!render_path_canon_drawn(RP_TERRAIN)) return;
    terrain_build(cpu, s_terrain_saved_occGroup);
    terrain_feed_native();
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
