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
#include "openpete_interp.h"         /* openpete_interp_present_now (scroll-anim alpha) */
#include "openpete_interp_objs.h"    /* per-descriptor scroll snapshots */
#include "native_gte.h"    /* shared GTE float-mirror math (W2.3) */
#include "features/no_lod.h"  /* openpete_no_lod + openpete_fog_scale (--fog-scale) */

#include "portal_native.h" /* portal-guard gate + the sky-plane provider (OT-conformance clamp) */
#include "texture_pack.h"           /* identity resolve for the record dumper */
#include "openpete_mods.h"          /* untextured-band material resolve (S8c) */
#include "openpete_native_vram.h"   /* VRAM model reads for the record dumper */

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

/* g_EnvironmentAnimations (environment.h) sits 0x48 bytes BEFORE g_Environment
 * (0x80078560; the generated env-anim func_8002A6FC indexes it as t9+0x10..).
 * Each list is {count, ptr-to-descriptor-pointer-array}. */
#define G_ENVANIM           (G_ENVIRONMENT - 0x48u)
#define ENVANIM_SCROLL_CNT  0x08u   /* m_ScrollingTextureCount / +4 ptr */
#define ENVANIM_LPPOLY_CNT  0x10u   /* m_LowPolyAnimationCount / +4 ptr */
#define ENVANIM_HPPOLY_CNT  0x20u   /* m_HighPolyAnimationCount / +4 ptr */

/* Sub-tick UV-scroll offsets (see tsc_scroll_refresh). The scrolling-texture
 * animation writes WHOLE TEXELS into the tiledef V bytes once per canonical
 * tick, so waterfalls step at 29.97 Hz while the world around them interpolates.
 * These tables carry the fractional V offset — in texels, relative to the
 * integer decode the game committed — for the tiles that scrolled this tick.
 * They are filled only on sub-tick re-extract builds and zeroed on the canonical
 * path, so the 30 Hz render stays byte-exact. */
static uint8_t s_tanim_act[128];      /* tile carries a fractional offset this build */
static float   s_tanim_dvlq[128];     /* LQ per-quad tiledef  (game writes scroll>>2) */
static float   s_tanim_dvhq[128][2];  /* HQ record entries 1-2 / 3-4 (scroll>>1) */
static int     s_tanim_have;          /* any tile active — skips the per-face test */
static int     s_tanim_en;            /* OPENPETE_TERRAIN_TANIM latch (re-extract only) */

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

/* Projection params (file-static so the clip/emit path can re-project clipped
 * vertices). Set per frame in terrain_build. */
static double s_ofx, s_ofy, s_H;

/* Float RTPS mirror, TR=0: camera-space MAC = (M*V)>>12 = ngte_mac (shared,
 * W2.3). SX/SY = OFX/OFY + MAC1/2*H/MAC3 (proj_screen). The LP-world/16 vs
 * HP-world/4 scale cancels in the MAC1/MAC3 ratio, so V can be built in any
 * consistent unit (world/16). */

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
    int fog_delta;                                 /* CLUT ladder row (see tmat) */
    int16_t texid;                                 /* Tiledef/record id, -1 untex (S9) */
    int tier;                                      /* 1=LP, 0=HP (debug tiercolor) */
    uint8_t guard;                                 /* .s-deferred face (portal-guard dup) */
    uint8_t fill;                                  /* LP hole-fill backing (W2.6-c) -> WORLD_FILL */
    int16_t sec, face;                             /* source identity (fardbg attribution) */
    uint8_t env;                                   /* env-anim tile member (provenance tag) */
    float otkey;                                   /* parent face's .s OT key (full-world mean
                                                    * vz of the 4 corners; portal conformance) */
} ttri;
static int s_emit_noHP = 0;     /* tier of the sector currently being emitted */
static int s_emit_guard = 0;    /* current face is .s-DEFERRED (near band) -> portal-guard dup */
static int s_emit_fill = 0;     /* current pass is the LP hole-fill backing (W2.6-c) */
static int s_emit_sec = -1, s_emit_face = -1;  /* fardbg identity of the current face */
static int s_emit_env = 0;      /* current face is in the env-anim rewrite set */
static float s_emit_otkey = 0.0f;  /* current face's .s OT key (portal conformance) */
/* Local terrain-triangle cap (before the scene feed). Under --no-lod every
 * foreground cell is forced through the 2x2 HQ subdivide, so terrain peaks at
 * ~29k tris (beast_makers) vs ~9k at stock LOD; 65536 is ~2x the measured worst
 * case (headroom for worst-camera-angle). See MAX_SCENE_TRIS in native_render.c. */
#define MAX_TTRIS 65536
static ttri s_tri[MAX_TTRIS]; static int s_tri_n;

static int s_dbg_faces, s_dbg_oob, s_dbg_nclip, s_dbg_behind, s_dbg_spike;  /* diagnostics */
static int s_fardbg = -1, s_fardbg_done = 0, s_fardbg_active = 0;  /* OPENPETE_TERRAIN_FARDBG */
static int s_facemap = -1, s_facemap_done = 0, s_facemap_active = 0;  /* OPENPETE_TERRAIN_FACEMAP */

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

/* Per-VBL env resolve for the policy toggles: the dev overlay's setenv is
 * their live channel, but the walks read them per face/sector — too hot for
 * getenv. Each site keeps a parse cache and re-resolves once per VBL. */
typedef struct { int v; unsigned f; int have; } tn_envvbl_t;
static int tn_env_vbl(tn_envvbl_t* c, const char* name, int def, int boolean) {
    extern unsigned openpete_frame;
    if (!c->have || c->f != openpete_frame) {
        const char* e = getenv(name);
        c->v = (e && *e) ? (boolean ? (*e != '0') : atoi(e)) : def;
        c->f = openpete_frame;
        c->have = 1;
    }
    return c->v;
}

static void terrain_dbg_init(void) {
    /* NCLIP back-face cull, default ON for the HP tier (bit0). A Z-buffer
     * hides *occluded* back-faces but not back-faces that sit NEARER than the
     * front geometry: back-to-back wall pairs (a room interior vs the
     * cliff/waterfall outside it) interpenetrate within a few world units, so
     * emit-all let the far surface's back-face win depth and punch through
     * (magic_crafters waterfall room, 2026-07-09). The .s NCLIP verdict — the
     * exact two-half quad rule, see the emit-site comment — is the canonical
     * visibility rule; the old "dropped ~half the faces" failure was the
     * single-triple mirror missing the second-half acceptance.
     * OPENPETE_TERRAIN_CULL is a bitmask: bit0 = HP cull (default), bit1 = LP
     * cull (EXPERIMENTAL — mixed winding in visible LP sets made both signs
     * regress at default cameras; needs the LP walk's own two-NCLIP rule read
     * from the .s before enabling), bit2 = invert LP sign (probe). "0"
     * disables everything. */
    static tn_envvbl_t cc, fc;
    s_cull = tn_env_vbl(&cc, "OPENPETE_TERRAIN_CULL", 1, 0);
    s_flat = tn_env_vbl(&fc, "OPENPETE_TERRAIN_FLAT", 0, 1);
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
 * the per-vertex DPCS depth-cue fog. The HP colour block is TWO 4B-stride
 * arrays [near0..][far0..]; `idx` is the pre-×4 byte offset from face word1.
 *
 * The .s decode (generated C block_80026DF0-80026E6C), PROVEN against the
 * title-vista captured prims (W2.3 fog A/B, 2026-07-03: implied-ir0 regression
 * over 546 matched fog-band verts fits residual 0, MAD 26):
 *   ir0 = fogRef - SZ,  SZ = 4·m3 (native V is world/16; the .s HP walk feeds
 *                                  world/4, so the .s SZ3 = 4× our m3);
 *   ir0 >= 4096 -> NEAR colour raw (block_80026FB4);
 *   ir0 <= 0    -> FAR colour raw (block_80026FAC; the .s DOES fully fog);
 *   else GTE DPCS (cmd 0x10): seed RGBC = FAR word ($s0[idx]), far-colour regs
 *   = NEAR word 12-bit-expanded ($t7[idx]) -> out = far + ir0·(near-far) with
 *   the GTE's truncating >>12 >>4 per channel (ngte_dpcs_ch).
 * The old claim "scale 1 validated, ×4 25-67%" is RETRACTED (it predates the
 * exact decode; the float+0.5 lerp at scale 1 pins f=1 across the fog band).
 *
 * SHIPPED DEFAULT since the fog arc (2026-07-05): the EXACT decode. The old
 * "exact worsens the vista (9.0→13.0)" park was a two-error cancellation:
 * PSX terrain fog is TWO channels — the DPCS vertex blend here AND a
 * per-face CLUT-ROW ladder of pre-baked progressively-fogged palettes
 * (see the .s texture-fog select at the face loop) — and native had
 * neither; the under-fogged lerp partially compensated for the missing
 * palette fog. With both channels implemented the exact decode is strictly
 * closer (identity join: 100% within ±4 at EVERY band; the lerp is Δ67-112
 * off in deep fog). OPENPETE_TERRAIN_FOGDPCS=0 restores the lerp (A/B);
 * OPENPETE_TERRAIN_LOD_HANDOFF=1 is the old .s-handoff experiment.
 * Colour words are 0x00BBGGRR (R=byte0), matching the captured prim RGB. LP
 * faces are NEVER fogged in the .s (raw working palette at all distances —
 * identity-join-proven); noHP=1 keeps that. */
static double s_fogscale = -1.0;    /* SZ = fogscale·m3; shipped default 1.0 (see above) */
static int s_fogdpcs = -1;          /* OPENPETE_TERRAIN_FOGDPCS: GTE-exact DPCS blend */
static int s_lod_handoff = -1;      /* OPENPETE_TERRAIN_LOD_HANDOFF (experiment) */
static int s_gapfill_en = -1;       /* OPENPETE_TERRAIN_GAPFILL (near-LP-gap fill, default on) */
static int s_tier_faithful = -1;    /* OPENPETE_TERRAIN_LOD_FAITHFUL: .s sector binning + exact fog (W2.6-c) */
static int s_clutfog = -1;          /* OPENPETE_TERRAIN_CLUTFOG: .s per-face CLUT-row fog ladder (default ON) */
int s_fogdbg = 0;                   /* OPENPETE_TERRAIN_FOGDBG: dump fog samples */
extern int s_fogdbg;
static uint32_t terrain_fog_blend(uint32_t nearC, uint32_t farC, double m3, int fogRef);
static uint32_t terrain_fog_blend_inner(uint32_t nearC, uint32_t farC, double m3, int fogRef);
static uint32_t terrain_fog_blend(uint32_t nearC, uint32_t farC, double m3, int fogRef) {
    uint32_t out = terrain_fog_blend_inner(nearC, farC, m3, fogRef);
    static int s_fs_n;
    if (s_fogdbg && s_fs_n < 6000) {
        fprintf(stderr, "[fogsample] m3=%.1f fogRef=%d near=%06X far=%06X out=%06X\n",
                m3, fogRef, nearC, farC, out);
        s_fs_n++;
    }
    return out;
}
static uint32_t terrain_fog_blend_inner(uint32_t nearC, uint32_t farC, double m3, int fogRef) {
    /* Pack fog shift (texture_pack_fog_shift_set): a full-coverage re-graded
     * pack needs the DPCS vertex-fog TARGET re-graded with it — the authored
     * FAR colours are the stock ambience, and modulating dark pack texels
     * toward them re-brightens the distance the pack darkened. Near colours
     * (lighting) stay stock; only the fog destination moves. 0x00BBGGRR. */
    {
        float fs[3], fo[3];
        if (texture_pack_fog_shift_get(fs, fo)) {
            int r = (int)(fs[0] * (float)(farC & 0xFF)         + fo[0] + 0.5f);
            int g = (int)(fs[1] * (float)((farC >> 8) & 0xFF)  + fo[1] + 0.5f);
            int b = (int)(fs[2] * (float)((farC >> 16) & 0xFF) + fo[2] + 0.5f);
            if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            farC = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
        }
    }
    if (s_fogdpcs) {                      /* GTE-exact path (proven; not yet default) */
        int ir0 = fogRef - (int)(m3 * s_fogscale);
        if (ir0 >= 4096) return nearC;
        if (ir0 <= 0)    return farC;
        int r = ngte_dpcs_ch((int)(farC & 0xFF),          (int)((nearC & 0xFF) << 4),         ir0);
        int g = ngte_dpcs_ch((int)((farC >> 8) & 0xFF),   (int)(((nearC >> 8) & 0xFF) << 4),  ir0);
        int b = ngte_dpcs_ch((int)((farC >> 16) & 0xFF),  (int)(((nearC >> 16) & 0xFF) << 4), ir0);
        return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
    }
    /* Shipped default: the float+0.5 lerp at fogscale 1 — under-fogs vs the .s
     * (f pins to 1 across most of the fog band) but its look co-evolved with
     * the tier coverage below; see the header comment for why the exact decode
     * is gated until the LP/HP handoff work lands. */
    double f = ((double)fogRef - m3 * s_fogscale) / 4096.0;
    if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
    int nr = (int)(nearC & 0xFF), ng = (int)((nearC >> 8) & 0xFF), nb = (int)((nearC >> 16) & 0xFF);
    int fr = (int)(farC & 0xFF),  fg = (int)((farC >> 8) & 0xFF),  fb = (int)((farC >> 16) & 0xFF);
    int r = (int)((double)fr + f * (nr - fr) + 0.5);
    int g = (int)((double)fg + f * (ng - fg) + 0.5);
    int b = (int)((double)fb + f * (nb - fb) + 0.5);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}
static uint32_t terrain_color(CPUState* cpu, int noHP, uint32_t lpBase,
                              uint32_t nearBase, uint32_t farBase,
                              int idx, double m3, int fogRef) {
    if (noHP) return rd(cpu, lpBase + (uint32_t)idx) & 0xFFFFFFu;
    uint32_t nearC = rd(cpu, nearBase + (uint32_t)idx) & 0xFFFFFFu;
    uint32_t farC  = rd(cpu, farBase  + (uint32_t)idx) & 0xFFFFFFu;
    return terrain_fog_blend(nearC, farC, m3, fogRef);
}

/* Per-sector local projected vertex (indexed by face vertex index). Carries the
 * camera-space (m1,m2,m3) for near-plane clipping + the projected screen sx,sy. */
typedef struct { float sx, sy; double m1, m2, m3, vz; int ok; } lvert;

/* A face corner: native colour + page-relative texel UV. */
typedef struct { uint32_t rgb; float u, v; } lcorner;
/* A face's texture material (decoded from the Tiledef; VRAM-pixel units).
 * clut_raw keeps the undecoded CLUT halfword: the .s HP walk ADDS a per-face
 * fog row offset to it per frame (Δ<<6 → cly += Δ) before decoding. */
/* fog_delta is the ladder row offset applied to clut_raw (0 = unfogged base
 * row). Carried alongside the decoded CLUT so consumers can recover the base
 * palette the texture was authored against: the texture-replacement registry
 * keys identity on the BASE row, so one pack file covers a texture at every
 * fog distance instead of the ladder fanning it into 15 near-duplicates. */
typedef struct { int textured, tpx, tpy, clx, cly, bpp, blend, clut_raw, fog_delta;
                 int texid; /* Tiledef/record id (word2 & 0x7F), -1 = untextured */ } tmat;

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
    m->clut_raw = clut;
    m->fog_delta = 0;              /* base row; the HP walk ladders it per face */
    /* blend: opaque unless ABE; then the tpage ABR mode (0..3) → scene blend
     * 1..4 ((B+F)/2, +, −, B+F/4). */
    m->blend = abe ? (((tpage >> 5) & 3) + 1) : 0;
}

static void terrain_texmat(CPUState* cpu, uint32_t table, int texByte, int hiDetail,
                           tmat* m, float u[4], float v[4]) {
    memset(m, 0, sizeof(*m)); m->bpp = 16; m->texid = -1;
    if (texByte == -1) { m->textured = 0; return; }      /* 0xFF: untextured */
    uint32_t e = table + (uint32_t)(texByte & 0x7F) * 16u + (hiDetail ? 8u : 0u);  /* LOD half */
    decode_tiledef(cpu, e, (texByte < 0), m, u, v);      /* high bit = semi-transparent */
    m->texid = texByte & 0x7F;                           /* texture-selector key (S9) */
}

/* Native colour-verts (per emitted tri corner) for the colour A/B. */
static float s_cvx[MAX_TTRIS * 3], s_cvy[MAX_TTRIS * 3];
static double s_cvz[MAX_TTRIS * 3];               /* m3 (world/16 view-Z) for fog regime */
static uint32_t s_cvrgb[MAX_TTRIS * 3]; static int s_cv_n;
static int s_tex_faces, s_tex_tris;               /* diagnostics: textured-face count */
static int s_dbg_clipped;                          /* faces that hit the near plane */

/* A clip-space corner: camera-space pos (m1,m2,m3) + colour + UV. Near-plane
 * clipping (§near-plane) interpolates all of these along an edge. */
/* osx/osy/oflag: optional SCREEN-space override consumed by push_tri instead
 * of proj_screen. The near/HQ subdivide sets it on its boundary midpoints
 * under the integer vertex snap: a midpoint projected from camera space lands
 * sub-pixel OFF the parent's projected edge (perspective), and the snap
 * amplifies that into a visible T-junction crack against the unsubdivided
 * neighbour sharing that edge. The override places the midpoint at the EXACT
 * screen midpoint of the SNAPPED parent corners (integer or exact-.5 — the
 * renderer's snap preserves exact halves), which is collinear with the
 * neighbour's snapped edge: watertight by construction. */
typedef struct { double m1, m2, m3; uint32_t rgb; float u, v;
                 float osx, osy; uint8_t oflag; } cvert;

/* Near plane (camera-space m3, world/16 view-Z). Vertices closer than this are
 * clipped so float H/m3 never blows up and behind-camera (m3<=0) verts don't
 * project to a wild position. Env-tunable. */
static double s_nearz = -1.0;
/* near/HQ subdivide pass enable (Route 2; OPENPETE_TERRAIN_NEARHQ, default ON). */
static int s_nearhq = -1;

static cvert cv_lerp(cvert a, cvert b, double zc) {  /* crossing point at m3 == zc */
    double t = (zc - a.m3) / (b.m3 - a.m3);
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    cvert r;
    r.osx = r.osy = 0.0f; r.oflag = 0;
    r.m1 = a.m1 + t * (b.m1 - a.m1);
    r.m2 = a.m2 + t * (b.m2 - a.m2);
    r.m3 = zc;
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
    int fsnap = openpete_native_vertex_snap_active();
    for (int k = 0; k < 3; k++) {
        if (cc[k].oflag) { t->sx[k] = cc[k].osx; t->sy[k] = cc[k].osy; }
        else {
            proj_screen(cc[k].m1, cc[k].m2, cc[k].m3, &t->sx[k], &t->sy[k]);
            /* Integer snap is applied HERE, feeder-side (the renderer skips
             * this feed — presnap contract): the near/HQ subdivide needs its
             * boundary midpoints at exact positions ON the snapped parent
             * edges (oflag above), which no per-vertex renderer snap could
             * preserve. Same floor as the renderer's (GTE MAC0>>16). */
            if (fsnap) { t->sx[k] = floorf(t->sx[k]); t->sy[k] = floorf(t->sy[k]); }
        }
        t->vz[k] = cc[k].m3; t->rgb[k] = cc[k].rgb; t->u[k] = cc[k].u; t->v[k] = cc[k].v;
        if (s_cv_n < MAX_TTRIS * 3) {
            s_cvx[s_cv_n]=t->sx[k]; s_cvy[s_cv_n]=t->sy[k]; s_cvz[s_cv_n]=cc[k].m3; s_cvrgb[s_cv_n]=cc[k].rgb; s_cv_n++;
        }
    }
    t->textured=m->textured; t->tpx=m->tpx; t->tpy=m->tpy;
    t->clx=m->clx; t->cly=m->cly; t->bpp=m->bpp; t->blend=m->blend;
    t->fog_delta=m->fog_delta;
    t->texid=(int16_t)(m->textured ? m->texid : -1);
    t->tier = s_emit_noHP;
    t->guard = (uint8_t)s_emit_guard;
    t->fill = (uint8_t)s_emit_fill;
    t->sec = (int16_t)s_emit_sec; t->face = (int16_t)s_emit_face;
    t->env = (uint8_t)s_emit_env;
    t->otkey = s_emit_otkey;
    if (m->textured) s_tex_tris++;
}

/* Max distance (PSX px) a near-clip CROSSING vertex may project from the screen
 * centre. A face straddling the eye plane clipped at s_nearz (16 world units)
 * puts its crossing verts essentially AT the eye — they project 20k+ px
 * offscreen, and rasterizers (llvmpipe AND hardware) drop the resulting sliver
 * even though its on-screen wedge is large (peace_keepers ridge tri s113/f10:
 * the terrain popped out whenever the camera crossed its plane). Everything
 * beyond this bound is far outside any visible frame (±~600 PSX px at 32:9),
 * so re-clipping until crossing verts land inside it trims only invisible
 * geometry: the wedge's bounding EDGES are the projections of the same 3D
 * lines regardless of where the cut lands along them. The doubling loop also
 * bounds the accepted radius from BELOW (~0.4×LIMIT, the crossing slides
 * ~half the distance per doubling), so the closing cut can never enter the
 * frame: 0.4×2048 = 819 px > the widest supported visible half-extent
 * (32:9 ≈ 683 PSX px). llvmpipe still under-rasters slivers this long (a
 * software-raster artifact — hardware draws them exactly); don't judge the
 * wedge's full extent on a WSLg screenshot. */
#define CLIP_PROJ_LIMIT 2048.0f

/* Near-plane clip (Sutherland-Hodgman against the plane m3 >= zc) of a
 * camera-space triangle given as 3 cverts, then push. 0 clipped → 1 tri; 1 →
 * quad (2 tris); 2 → 1 tri; 3 → dropped. Starts at zc = s_nearz; when a
 * crossing vertex still projects beyond CLIP_PROJ_LIMIT the whole face is
 * re-clipped at a doubled zc (crossing projections shrink ~1/zc, so this
 * terminates fast; faces that end fully behind the raised plane were entirely
 * inside the invisible band). Faces that never clip are pushed on round 0 —
 * identical to the old single-plane behaviour. */
static void clip_push_tri(cvert a, cvert b, cvert c, const tmat* m) {
    cvert in[3] = { a, b, c };
    double zc = s_nearz;
    for (int round = 0; ; round++) {
        cvert out[4]; int nout = 0; int clipped = 0;
        for (int i = 0; i < 3; i++) {
            cvert cur = in[i], nxt = in[(i + 1) % 3];
            int curIn = cur.m3 >= zc, nxtIn = nxt.m3 >= zc;
            if (curIn) out[nout++] = cur;
            if (curIn != nxtIn && nout < 4) { out[nout++] = cv_lerp(cur, nxt, zc); clipped = 1; }
        }
        if (nout < 3) { s_dbg_behind++; return; } /* fully behind the near plane */
        if (clipped && round < 16) {
            int sane = 1;
            for (int i = 0; i < nout && sane; i++) {
                if (out[i].m3 != zc) continue;    /* only crossing verts move with zc */
                float sx, sy;
                proj_screen(out[i].m1, out[i].m2, out[i].m3, &sx, &sy);
                if (fabsf(sx - (float)s_ofx) > CLIP_PROJ_LIMIT ||
                    fabsf(sy - (float)s_ofy) > CLIP_PROJ_LIMIT) sane = 0;
            }
            if (!sane) { zc *= 2.0; continue; }
        }
        if (clipped) s_dbg_clipped++;
        push_tri(out[0], out[1], out[2], m);            /* fan the clipped polygon */
        if (nout == 4) push_tri(out[0], out[2], out[3], m);
        return;
    }
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
    r.osx = r.osy = 0.0f; r.oflag = 0;
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
    /* Portal OT-conformance key: near-subdivided (deferred) faces are drained
     * into the OT as 4 SEPARATE sub-quad prims, EACH bucketed by its own
     * corner sum — the .s sky-vs-floor boundary at a portal base is at
     * SUB-QUAD granularity (og f2440: the walkway's near sub-quads paint over
     * the preview, its far ones are covered). Override the parent face's key
     * with this sub-quad's own. */
    s_emit_otkey = (float)((tl.m3 + tr.m3 + bl.m3 + br.m3) * 4.0);
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
/* HQ sub-quad UV ORIENTATION (rotation/flip), mirroring func_800276A0
 * block_80027EA8: the HQ Tiledef carries an orientation index in `word1 >> 25`;
 * when nonzero it selects an 8-byte entry in the offset table D_8006D058
 * (`base + ori`) of per-corner packed (du | dv<<8) deltas added to the base
 * UVs — TL/BL/BR relative to (u0,v0) [word0], TR relative to (u1,v1) [word1] —
 * then masked to the u/v bytes (carry between u,v is faithful). Entry 0/64 are
 * the identity transform (≡ decode_tiledef's default), so cells already drawn
 * correctly are untouched. Without this, rotated/flipped HQ tiles render
 * unrotated (~0.4% of near sub-quads in the attract demo: e.g. Artisans tiles
 * 6/23 ori=104, the right-column sub-quads of a cell). The 16px (0x0F) table
 * half is unused in practice. Returns the ori index (0 = identity, u/v kept). */
static int tiledef_apply_ori(CPUState* cpu, uint32_t e, float u[4], float v[4]) {
    uint32_t ori = rd(cpu, e + 4) >> 25;
    if (!ori) return 0;
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
    return (int)ori;
}

static void terrain_near_emit(CPUState* cpu, uint32_t hqBase, int texid, int abe,
                              cvert TL, cvert TR, cvert BL, cvert BR) {
    cvert mT = cv_mid(TL, TR), mB = cv_mid(BL, BR);
    cvert mL = cv_mid(TL, BL), mR = cv_mid(TR, BR);
    cvert C  = cv_mid(mT, mB);                 /* centre = mean of 4 corners */
    /* Integer vertex snap: pin each boundary midpoint to the SNAPPED parent
     * edge so the sub-quads stay watertight against the neighbour sharing
     * that edge (see cvert.oflag) — but at the PERSPECTIVE-CORRECT position
     * along the edge (the true camera-space midpoint's projection, dropped
     * onto the snapped segment), NOT the screen midpoint: the quadrant HQ
     * tiles are attached to these vertices, and a screen-parametric midpoint
     * re-warps near textures exactly the way the subdivide exists to
     * prevent.
     *
     * The near-plane guard is per-EDGE, not per-family: the neighbouring
     * cell sharing an edge — often another subdivided family whose OWN far
     * corners dip inside the near plane — must reach the identical verdict
     * for the SHARED midpoint, and an edge's safety is a property of its
     * two shared endpoints alone. (A family-wide bail parted two adjacent
     * families' copies of the same midpoint — one on-segment, one floored
     * fresh in push_tri — and drew a hairline across the near ground at the
     * subdivide-ring distance.) An unsafe edge gets no override: a clipping
     * child rebuilds its crossings (cv_lerp zeroes oflag) and the segment
     * projection would lie. */
    if (openpete_native_vertex_snap_active()) {
        cvert* P[4] = { &TL, &TR, &BL, &BR };
        cvert* M[4] = { &mT, &mB, &mL, &mR };
        const int E[4][2] = { {0,1}, {2,3}, {0,2}, {1,3} };  /* corner pairs */
        int safe[4];
        float px[4], py[4];
        for (int i = 0; i < 4; i++) {
            safe[i] = P[i]->m3 >= s_nearz;
            if (!safe[i]) continue;            /* mirrored projection: unusable */
            proj_screen(P[i]->m1, P[i]->m2, P[i]->m3, &px[i], &py[i]);
            px[i] = floorf(px[i]); py[i] = floorf(py[i]);
            P[i]->osx = px[i]; P[i]->osy = py[i]; P[i]->oflag = 1;
        }
        for (int i = 0; i < 4; i++) {
            if (!safe[E[i][0]] || !safe[E[i][1]]) continue;
            float mx, my;
            proj_screen(M[i]->m1, M[i]->m2, M[i]->m3, &mx, &my);
            float ax = px[E[i][0]], ay = py[E[i][0]];
            float dx = px[E[i][1]] - ax, dy = py[E[i][1]] - ay;
            float L2 = dx*dx + dy*dy;
            float tt = L2 > 0.0f ? ((mx-ax)*dx + (my-ay)*dy) / L2 : 0.5f;
            if (tt < 0.0f) tt = 0.0f; else if (tt > 1.0f) tt = 1.0f;
            M[i]->osx = ax + tt*dx; M[i]->osy = ay + tt*dy; M[i]->oflag = 1;
        }
        /* Centre: interior (shared by all 4 children only) — snapped true
         * projection keeps it wobble-consistent with the rest of the mesh. */
        if (safe[0] && safe[1] && safe[2] && safe[3]) {
            float cx2, cy2; proj_screen(C.m1, C.m2, C.m3, &cx2, &cy2);
            C.osx = floorf(cx2); C.osy = floorf(cy2); C.oflag = 1;
        }
    }
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
        mat.texid = texid;                /* HQ quadrants share the record's id */
        tiledef_apply_ori(cpu, e, u, v);  /* rotated/flipped HQ tiles (word1>>25) */
        /* Sub-tick UV scroll. Applied AFTER the orientation on purpose: the
         * offset is a pure V translation in texel space and apply_ori never
         * moves V into U (its per-corner deltas only carry within the u/v byte
         * pair), so scrolling here is identical to scrolling the base entry —
         * a rotated tile scrolls along its rotated axis, exactly as the game's
         * whole-texel writes do. Sub-quads 0-1 read HQ entries 1-2 and 2-3 read
         * entries 3-4; the two groups sit half a window apart in the scroll
         * ring, so they carry separately-wrapped offsets. */
        if (s_tanim_have && texid >= 0 && texid < 128 && s_tanim_act[texid]) {
            float dv = s_tanim_dvhq[texid][i >> 1];
            for (int k = 0; k < 4; k++) v[k] += dv;
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
    /* Per-corner SATURATION at 0 mirrors the GTE: SZ3 is an unsigned clamped
     * register, so a corner behind the camera contributes 0 to the .s's
     * depthSum — the face still classifies NEAR (it is the closest thing on
     * screen). Native's raw float m3 goes NEGATIVE there and used to drag the
     * sum ≤ 0 → "behind camera" → the closest ground/wall cells fell back to
     * the LQ tier exactly when the camera pressed against them. */
    double sz0 = z0 > 0.0 ? z0*s : 0.0, sz1 = z1 > 0.0 ? z1*s : 0.0;
    double sz2 = z2 > 0.0 ? z2*s : 0.0, sz3 = z3 > 0.0 ? z3*s : 0.0;
    double sum = sz0 + sz1 + sz2 + sz3;
    if (sum <= 0.0) return 0;                          /* FULLY behind camera */
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
    uint8_t  gapfill;                              /* LP only: HP-coverage-gap filler (see tsc_classify_gapfill) */
    uint8_t  floorf;                               /* LP only: up-facing floor by winding (n_z < 0, |n_z| > 0.8) */
    int8_t   tb;                                   /* HP texByte (int8 of w2; -1 = untextured) */
    uint32_t w3;                                   /* HP word3 (face_is_near gates) */
    uint32_t tgen;                                 /* s_tgen[tile] at texmat decode (anim liveness) */
    tmat mat; float tu[4], tv[4];                  /* tiledef half +0 (far/LQ; LP untextured) */
    tmat matN; float tuN[4], tvN[4];               /* tiledef half +8 (near tile, .s t0<=0 branch) */
} tsface_t;

/* Env texture animation liveness (the artisans waterfall fix): the game's
 * env-anim (func_8002A6FC) animates textures by rewriting the LQ tiledef
 * TABLE ENTRIES in place each keyframe advance — loop 1 copies whole 16-byte
 * entries from an animation bank (texture-descriptor swap), loop 2 rewrites
 * the V bytes of BOTH halves (+1/+5/+9/+13, UV scroll — waterfalls/water),
 * and both also rewrite the sibling 168-byte HQ record. The .s HP walk reads
 * the entry live per face per frame (block_8002709C), so PSX animates; the
 * sector cache snapshot froze it (near/HQ subdivide reads the HQ record live,
 * which is why close-up terrain still animated). Mirror = one generation
 * counter per tile, bumped when the 16-byte entry content changes (checked
 * once per present); cached faces re-decode their texmat on gen mismatch.
 * Sector verts/colours are NOT anim-rewritten storage (colour anim goes
 * through the live-read colour arrays; verified by OPENPETE_TERRAIN_ANIMDBG
 * in artisans: only lq/hq tile 23 churns, secV/secC never). */
static uint32_t s_tgen[128];
static uint32_t s_tgen_epoch = 1;     /* bumped when ANY tile gen bumps */
static uint32_t s_lqsnap[128][4];
static uint32_t s_lqsnap_base;
static void tsc_tile_gens(CPUState* cpu, uint32_t lqTable, uint32_t texCnt) {
    if (!lqTable) return;
    if (texCnt > 128) texCnt = 128;
    int rebase = (lqTable != s_lqsnap_base);
    for (uint32_t t = 0; t < texCnt; t++) {
        uint32_t w[4]; int diff = rebase;
        for (int k = 0; k < 4; k++) {
            w[k] = rd(cpu, lqTable + t * 16u + (uint32_t)k * 4u);
            diff |= (w[k] != s_lqsnap[t][k]);
        }
        if (diff) { memcpy(s_lqsnap[t], w, sizeof(w)); s_tgen[t]++; s_tgen_epoch++; }
    }
    s_lqsnap_base = lqTable;
}
typedef struct {
    uint32_t s, sig[5];
    uint32_t animEpoch;                      /* s_tgen_epoch at last texmat refresh */
    uint32_t lastUse;                        /* s_tsc_stamp at last hit — LRU eviction key */
    int nLpV, nLpF, nHpV, nHpF, flags;
    int nGapfill;                            /* LP faces classified HP-coverage-gap fillers */
    int originX, originY, originZ;
    int centreX, centreY, centreZ, radius;   /* header bounding sphere (w0/w1) — the .s LP/HP binning inputs */
    uint32_t lpColBase, nearBase, farBase, lpFaces, hpFaces;
    tsvert_t* lpV; tsface_t* lpF;
    tsvert_t* hpV; tsface_t* hpF;
    /* Poly-anim sectors only (lazy): the previous canonical tick's decoded verts
     * + the sub-tick fractional lerp banks (x,y,z triples). vrollPres/vfracPres
     * stamp which terrain_build filled them (dedupes sectors named by BOTH the
     * LP and HP descriptor lists, and invalidates lerp banks the moment a
     * sector leaves the anim registry — a stale stamp never matches). */
    tsvert_t* lpVprev; tsvert_t* hpVprev;
    float* lpVf; float* hpVf;
    uint32_t vrollPres, vfracPres;
    int polyanim;   /* named by an env poly-anim descriptor (vert banks animate) */
} tscache_t;
#define TSC_SLOTS 512
static tscache_t s_tsc[TSC_SLOTS];
static uint32_t s_tsc_stamp;                 /* bumped once per terrain_build call */
static long s_tsc_hit, s_tsc_miss;
static long s_tocc_culled;   /* sectors occlusion-culled this window (census) */
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
            F->tb = (int8_t)tb;
            F->tgen = (tb == -1) ? 0 : s_tgen[tb & 0x7F];
            terrain_texmat(cpu, tbl, tb, hiDetail, &F->mat, F->tu, F->tv);
            /* Both tiledef halves are cached; the walk picks per FRAME by the
             * .s distance rule (t0 = key−fogRef+4096; t0<=0 → the +8 half).
             * Animated tiles re-decode when their gen bumps (tsc_refresh_anim). */
            terrain_texmat(cpu, tbl, tb, 1, &F->matN, F->tuN, F->tvN);
        } else {                                   /* LP: 6-bit indices; untextured */
            F->i0 = (uint8_t)(((w0 >> 23) & 0x1F8) >> 3); F->i1 = (uint8_t)(((w0 >> 17) & 0x1F8) >> 3);
            F->i2 = (uint8_t)(((w0 >> 11) & 0x1F8) >> 3); F->i3 = (uint8_t)(((w0 >> 5) & 0x1F8) >> 3);
            F->c0 = (uint16_t)((w1 >> 24) & 0xFC); F->c1 = (uint16_t)((w1 >> 18) & 0xFC);
            F->c2 = (uint16_t)((w1 >> 12) & 0xFC); F->c3 = (uint16_t)((w1 >> 6) & 0xFC);
            F->tb = -1;
            F->mat.bpp = 16;                       /* untextured flat gouraud */
        }
        F->quad = (F->i2 != F->i3);
    }
}

/* ---- LP HP-coverage-gap classifier (the near-LP-gap fix) --------------------
 * The game bins terrain HP-near / LP-far, so an lpFarOnly LP face with no
 * corner past the fog line is dropped (the .s never draws a near HP-sector's
 * LP).  Where the sector's HP mesh is SPARSE — authored without polys over
 * part of the LP footprint (unreachable at gameplay cameras) — that drop
 * leaves a bare hole to the cyclorama at detached/free cameras.
 *
 * Which LP faces may back-fill near is decidable from GEOMETRY, offline-
 * validated on artisans (2026-07-09): sample each LP face against the same sector's HP mesh.
 * LP faces over intentional openings (portal arches, windows) are ~fully
 * REDUNDANT with the HP surface around the opening (cov = 1.00 — the LP arch
 * hugs the HP walls), while true coverage-gap faces are LP-EXCLUSIVE
 * (cov 0.0–0.8, median 150+ units from any HP poly).  The distribution is
 * strongly bimodal: 24 of 833 artisans LP faces classify as gap-fillers.
 * Misclassification is benign in both directions: a false gap-filler draws
 * at true LP depth behind the HP that covers it (the WORLD LP tier pull),
 * a missed one keeps the status-quo hole.
 *
 * Same-sector pooling only (neighbour HP changed no verdict in the offline
 * sweep).  Computed once per sector decode; vertex-anim sectors keep their
 * decode-pose verdict (a moving wall's classification drift is cosmetic). */
static double gp_dot(const double a[3], const double b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static double gp_pt_tri_d2_v(const double p[3], const double a[3], const double bv[3], const double cv[3]) {
    double ab[3] = { bv[0] - a[0], bv[1] - a[1], bv[2] - a[2] };
    double ac[3] = { cv[0] - a[0], cv[1] - a[1], cv[2] - a[2] };
    double ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    double d1 = gp_dot(ab, ap), d2 = gp_dot(ac, ap);
    double q[3];
    if (d1 <= 0.0 && d2 <= 0.0) { q[0]=a[0]; q[1]=a[1]; q[2]=a[2]; goto out; }
    {
        double bp[3] = { p[0]-bv[0], p[1]-bv[1], p[2]-bv[2] };
        double d3 = gp_dot(ab, bp), d4 = gp_dot(ac, bp);
        if (d3 >= 0.0 && d4 <= d3) { q[0]=bv[0]; q[1]=bv[1]; q[2]=bv[2]; goto out; }
        double vc = d1*d4 - d3*d2;
        if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
            double den = d1 - d3, t = den != 0.0 ? d1/den : 0.0;
            q[0]=a[0]+ab[0]*t; q[1]=a[1]+ab[1]*t; q[2]=a[2]+ab[2]*t; goto out;
        }
        double cp[3] = { p[0]-cv[0], p[1]-cv[1], p[2]-cv[2] };
        double d5 = gp_dot(ab, cp), d6 = gp_dot(ac, cp);
        if (d6 >= 0.0 && d5 <= d6) { q[0]=cv[0]; q[1]=cv[1]; q[2]=cv[2]; goto out; }
        double vb = d5*d2 - d1*d6;
        if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
            double den = d2 - d6, t = den != 0.0 ? d2/den : 0.0;
            q[0]=a[0]+ac[0]*t; q[1]=a[1]+ac[1]*t; q[2]=a[2]+ac[2]*t; goto out;
        }
        double va = d3*d6 - d5*d4;
        if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
            double den = (d4 - d3) + (d5 - d6), t = den != 0.0 ? (d4 - d3)/den : 0.0;
            q[0]=bv[0]+(cv[0]-bv[0])*t; q[1]=bv[1]+(cv[1]-bv[1])*t; q[2]=bv[2]+(cv[2]-bv[2])*t; goto out;
        }
        /* interior: distance along the plane normal */
        double n[3] = { ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0] };
        double nl = gp_dot(n, n);
        if (nl <= 0.0) { q[0]=a[0]; q[1]=a[1]; q[2]=a[2]; goto out; }
        double t = gp_dot(ap, n) / nl;
        q[0] = p[0] - n[0]*t; q[1] = p[1] - n[1]*t; q[2] = p[2] - n[2]*t;
    }
out:;
    double dx = p[0]-q[0], dy = p[1]-q[1], dz = p[2]-q[2];
    return dx*dx + dy*dy + dz*dz;
}
static double gp_pt_tri_d2(const double p[3], const tsvert_t* A, const tsvert_t* B, const tsvert_t* C) {
    double a[3] = { (double)A->x, (double)A->y, (double)A->z };
    double b[3] = { (double)B->x, (double)B->y, (double)B->z };
    double c[3] = { (double)C->x, (double)C->y, (double)C->z };
    return gp_pt_tri_d2_v(p, a, b, c);
}
/* Level-wide pool of LP triangles from LP-AUTHORITATIVE sectors (no HP faces,
 * or force-LP 0x8000) in WORLD space — the near-noHP re-admission test below
 * asks whether a wall gap face's unbacked span continues into territory where
 * the LP tier is the only geometry that exists (level-boundary cliffs) vs
 * deep inside HP-dense territory (interior shortcut skins).  Built lazily
 * once per level (keyed on the sector-table pointer). */
typedef struct { float ax,ay,az,bx,by,bz,cx,cy,cz; } gp_wtri;
static gp_wtri* s_nohp_tris; static int s_nohp_n; static uint32_t s_nohp_key;
#define GP_NOHP_MAX 8192
static void gp_build_nohp_pool(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    /* Stamp covers the pointer, count and the first sector's origin word so an
     * in-place level reload (same table address) still rebuilds. */
    uint32_t key = sectorPtr ^ (sectorCnt * 2654435761u);
    if (sectorPtr) { uint32_t s0 = rd(cpu, sectorPtr); if (s0) key ^= rd(cpu, s0 + 0x08); }
    if (key == s_nohp_key) return;
    s_nohp_key = key; s_nohp_n = 0;
    if (!s_nohp_tris) s_nohp_tris = (gp_wtri*)malloc(sizeof(gp_wtri) * GP_NOHP_MAX);
    if (!s_nohp_tris || !sectorPtr) return;
    if (sectorCnt > 1024) sectorCnt = 1024;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t g0 = rd(cpu, s + 0x04), g1 = rd(cpu, s + 0x08), g2 = rd(cpu, s + 0x0C);
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int nHpF = (int)((g4 >> 16) & 0xFF);
        int flags = (int)(g0 & 0xE000);
        if (nHpF != 0 && !(flags & 0x8000)) continue;      /* LP-authoritative only */
        int nLpV = (int)(g3 & 0xFF), nLpC = (int)((g3 >> 8) & 0xFF), nLpF = (int)((g3 >> 16) & 0xFF);
        if (nLpV <= 0 || nLpF <= 0 || nLpV > 256) continue;
        int ox = (int)(g1 >> 16), oy = (int)(g1 & 0xFFFF), oz = (int)(g2 >> 16);
        uint32_t verts = s + 0x1C;
        uint32_t faces = s + 0x1C + (uint32_t)(nLpV + nLpC) * 4;
        tsvert_t lv[256];
        tsc_decode_verts(cpu, verts, nLpV, lv);
        for (int f = 0; f < nLpF && s_nohp_n + 2 <= GP_NOHP_MAX; f++) {
            uint32_t w0 = rd(cpu, faces + (uint32_t)f * 8u);
            int i0 = (int)(((w0 >> 23) & 0x1F8) >> 3), i1 = (int)(((w0 >> 17) & 0x1F8) >> 3);
            int i2 = (int)(((w0 >> 11) & 0x1F8) >> 3), i3 = (int)(((w0 >> 5) & 0x1F8) >> 3);
            if (i0 >= nLpV || i1 >= nLpV || i2 >= nLpV || i3 >= nLpV) continue;
            gp_wtri* T = &s_nohp_tris[s_nohp_n++];
            T->ax = (float)(ox + lv[i0].x); T->ay = (float)(oy + lv[i0].y); T->az = (float)(oz + lv[i0].z);
            T->bx = (float)(ox + lv[i1].x); T->by = (float)(oy + lv[i1].y); T->bz = (float)(oz + lv[i1].z);
            T->cx = (float)(ox + lv[i2].x); T->cy = (float)(oy + lv[i2].y); T->cz = (float)(oz + lv[i2].z);
            if (i2 != i3) {   /* quad: second half (i0,i2,i3) */
                gp_wtri* U = &s_nohp_tris[s_nohp_n++];
                U->ax = T->ax; U->ay = T->ay; U->az = T->az;
                U->bx = T->cx; U->by = T->cy; U->bz = T->cz;
                U->cx = (float)(ox + lv[i3].x); U->cy = (float)(oy + lv[i3].y); U->cz = (float)(oz + lv[i3].z);
            }
        }
    }
}
static double gp_pool_d2(const double p[3]) {
    double best = 1e30;
    for (int k = 0; k < s_nohp_n; k++) {
        const gp_wtri* T = &s_nohp_tris[k];
        double A[3] = { T->ax, T->ay, T->az }, B[3] = { T->bx, T->by, T->bz }, Cc[3] = { T->cx, T->cy, T->cz };
        double d = gp_pt_tri_d2_v(p, A, B, Cc);
        if (d < best) best = d;
    }
    return best;
}

/* Level-wide pool of ALL terrain triangles (both tiers, every sector) in
 * WORLD space, owner-tagged so a face can exclude itself.  The mid-band
 * (cov in [0.9, 0.97)) VOID re-admission below asks whether an unbacked
 * sample sits over a TRUE WORLD VOID — nothing drawn there by anything,
 * so dropping the face exposes cyclorama (peace_keepers' authored one-cell
 * HP hole under a mostly-backed water quad) — or over/behind OTHER
 * geometry (tree_tops' branch-floor sliver overlapping the trunk: the LP
 * poking through the HP, found in testing 2026-07-09).  Same lazy per-level
 * stamp as the no-HP pool. */
typedef struct { float ax,ay,az,bx,by,bz,cx,cy,cz; uint32_t owner; uint16_t face; uint8_t hp; } gp_wtri2;
static gp_wtri2* s_all_tris; static int s_all_n; static uint32_t s_all_key;
#define GP_ALL_MAX 32768
static void gp_all_push(uint32_t owner, int f, int hp, int ox, int oy, int oz,
                        const tsvert_t* a, const tsvert_t* b, const tsvert_t* c) {
    gp_wtri2* T = &s_all_tris[s_all_n++];
    T->ax = (float)(ox + a->x); T->ay = (float)(oy + a->y); T->az = (float)(oz + a->z);
    T->bx = (float)(ox + b->x); T->by = (float)(oy + b->y); T->bz = (float)(oz + b->z);
    T->cx = (float)(ox + c->x); T->cy = (float)(oy + c->y); T->cz = (float)(oz + c->z);
    T->owner = owner; T->face = (uint16_t)f; T->hp = (uint8_t)hp;
}
static void gp_build_all_pool(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    uint32_t key = sectorPtr ^ (sectorCnt * 2654435761u) ^ 0x9E3779B9u;
    if (sectorPtr) { uint32_t s0 = rd(cpu, sectorPtr); if (s0) key ^= rd(cpu, s0 + 0x08); }
    if (key == s_all_key) return;
    s_all_key = key; s_all_n = 0;
    if (!s_all_tris) s_all_tris = (gp_wtri2*)malloc(sizeof(gp_wtri2) * GP_ALL_MAX);
    if (!s_all_tris || !sectorPtr) return;
    if (sectorCnt > 1024) sectorCnt = 1024;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t g1 = rd(cpu, s + 0x08), g2 = rd(cpu, s + 0x0C);
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int nLpV = (int)(g3 & 0xFF), nLpC = (int)((g3 >> 8) & 0xFF), nLpF = (int)((g3 >> 16) & 0xFF);
        int nHpV = (int)(g4 & 0xFF), nHpC = (int)((g4 >> 8) & 0xFF), nHpF = (int)((g4 >> 16) & 0xFF);
        int hpVertsOff = (int)((g4 >> 24) & 0xFF);
        int ox = (int)(g1 >> 16), oy = (int)(g1 & 0xFFFF), oz = (int)(g2 >> 16);
        tsvert_t tv[256];
        if (nLpV > 0 && nLpV <= 256 && nLpF > 0) {
            tsc_decode_verts(cpu, s + 0x1C, nLpV, tv);
            uint32_t faces = s + 0x1C + (uint32_t)(nLpV + nLpC) * 4;
            for (int f = 0; f < nLpF && s_all_n + 2 <= GP_ALL_MAX; f++) {
                uint32_t w0 = rd(cpu, faces + (uint32_t)f * 8u);
                int i0 = (int)(((w0 >> 23) & 0x1F8) >> 3), i1 = (int)(((w0 >> 17) & 0x1F8) >> 3);
                int i2 = (int)(((w0 >> 11) & 0x1F8) >> 3), i3 = (int)(((w0 >> 5) & 0x1F8) >> 3);
                if (i0 >= nLpV || i1 >= nLpV || i2 >= nLpV || i3 >= nLpV) continue;
                gp_all_push(s, f, 0, ox, oy, oz, &tv[i0], &tv[i1], &tv[i2]);
                if (i2 != i3) gp_all_push(s, f, 0, ox, oy, oz, &tv[i0], &tv[i2], &tv[i3]);
            }
        }
        if (nHpV > 0 && nHpV <= 256 && nHpF > 0) {
            uint32_t hpVerts = s + 0x1C + (uint32_t)hpVertsOff * 4;
            tsc_decode_verts(cpu, hpVerts, nHpV, tv);
            uint32_t faces = hpVerts + (uint32_t)(nHpV + nHpC * 2) * 4;
            for (int f = 0; f < nHpF && s_all_n + 2 <= GP_ALL_MAX; f++) {
                uint32_t w0 = rd(cpu, faces + (uint32_t)f * 16u);
                int i0 = (int)((w0 >> 24) & 0xFF), i1 = (int)((w0 >> 16) & 0xFF);
                int i2 = (int)((w0 >> 8) & 0xFF),  i3 = (int)(w0 & 0xFF);
                if (i0 >= nHpV || i1 >= nHpV || i2 >= nHpV || i3 >= nHpV) continue;
                gp_all_push(s, f, 1, ox, oy, oz, &tv[i0], &tv[i1], &tv[i2]);
                if (i2 != i3) gp_all_push(s, f, 1, ox, oy, oz, &tv[i0], &tv[i2], &tv[i3]);
            }
        }
    }
}
/* min dist^2 from p to the pool, excluding the LP face (ownerSec, face);
 * early-outs once at/below lim2. */
static double gp_all_d2_excl(const double p[3], uint32_t ownerSec, int face, double lim2) {
    double best = 1e30;
    for (int k = 0; k < s_all_n; k++) {
        const gp_wtri2* T = &s_all_tris[k];
        if (T->owner == ownerSec && !T->hp && T->face == face) continue;
        double A[3] = { T->ax, T->ay, T->az }, B[3] = { T->bx, T->by, T->bz }, Cc[3] = { T->cx, T->cy, T->cz };
        double d = gp_pt_tri_d2_v(p, A, B, Cc);
        if (d < best) { best = d; if (best <= lim2) break; }
    }
    return best;
}
/* Any pool triangle hit by the ray p + t*dir (unit dir), t in (eps, tmax],
 * excluding the LP face itself?  Moller-Trumbore. */
static int gp_all_ray_hit(const double p[3], const double dir[3], double tmax,
                          uint32_t ownerSec, int face) {
    for (int k = 0; k < s_all_n; k++) {
        const gp_wtri2* T = &s_all_tris[k];
        if (T->owner == ownerSec && !T->hp && T->face == face) continue;
        double e1[3] = { T->bx - T->ax, T->by - T->ay, T->bz - T->az };
        double e2[3] = { T->cx - T->ax, T->cy - T->ay, T->cz - T->az };
        double h[3] = { dir[1]*e2[2] - dir[2]*e2[1], dir[2]*e2[0] - dir[0]*e2[2], dir[0]*e2[1] - dir[1]*e2[0] };
        double det = e1[0]*h[0] + e1[1]*h[1] + e1[2]*h[2];
        if (det > -1e-9 && det < 1e-9) continue;
        double f2 = 1.0 / det;
        double sv[3] = { p[0] - T->ax, p[1] - T->ay, p[2] - T->az };
        double u = f2 * (sv[0]*h[0] + sv[1]*h[1] + sv[2]*h[2]);
        if (u < -1e-6 || u > 1.0 + 1e-6) continue;
        double q[3] = { sv[1]*e1[2] - sv[2]*e1[1], sv[2]*e1[0] - sv[0]*e1[2], sv[0]*e1[1] - sv[1]*e1[0] };
        double v = f2 * (dir[0]*q[0] + dir[1]*q[1] + dir[2]*q[2]);
        if (v < -1e-6 || u + v > 1.0 + 1e-6) continue;
        double t = f2 * (e2[0]*q[0] + e2[1]*q[1] + e2[2]*q[2]);
        if (t > 1e-6 && t <= tmax) return 1;
    }
    return 0;
}

/* Per-face gapfill kill switch (bisection probe): "s:f,s:f" list. Shared by
 * BOTH draw paths (the lpFarOnly beyond==0 exemption and the --no-lod
 * lpGapOnly gate). */
static int gp_gf_excluded(uint32_t i, int f) {
    static const char* s_excl = (const char*)-1;
    if (s_excl == (const char*)-1) s_excl = getenv("OPENPETE_TERRAIN_GAPFILL_EXCL");
    if (!s_excl || !*s_excl) return 0;
    char key[24]; snprintf(key, sizeof key, ",%u:%d,", i, f);
    char pad[512]; snprintf(pad, sizeof pad, ",%s,", s_excl);
    return strstr(pad, key) != NULL;
}

/* ---- authored-data art corrections (native display only) -----------------
 * Level-data authoring bugs corrected at the native colour fetch; emulated RAM
 * stays canonical, so the PSX/og legs keep the authentic data and gameplay
 * state is untouched. Keyed on exact (g_LevelId, sector, face).
 *
 * stone_hill s205 f4-f7: four quads of the water backdrop group carry the
 * editor-default vertex colour 7F7F7F (unpainted) while every sibling panel
 * is FF7F60 — the pre-fogged sky camouflage. On PSX the grey shows when
 * looking up from under the water; natively it also shows from above (the LP
 * either-half NCLIP acceptance draws the underside the .s culls). Recolour
 * to the sibling sky-blue. OPENPETE_TERRAIN_ARTFIX=0 restores authentic. */
typedef struct { int level, sec, face; uint32_t rgb; } lp_artfix_t;
static const lp_artfix_t k_lp_artfix[] = {
    { 11, 205, 4, 0xFF7F60u }, { 11, 205, 5, 0xFF7F60u },
    { 11, 205, 6, 0xFF7F60u }, { 11, 205, 7, 0xFF7F60u },
};
static int lp_artfix_lookup(CPUState* cpu, int sec, int face, uint32_t* rgb) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_TERRAIN_ARTFIX");
                    s_on = !(e && *e == '0'); }
    if (!s_on) return 0;
    static int s_lvl = -1, s_lvl_frame = -1;
    if (s_lvl_frame != (int)openpete_frame) {   /* one RAM read per VBL */
        s_lvl = (int)(int32_t)rd(cpu, 0x8007596C);   /* g_LevelId */
        s_lvl_frame = (int)openpete_frame;
    }
    for (size_t k = 0; k < sizeof(k_lp_artfix)/sizeof(k_lp_artfix[0]); k++)
        if (k_lp_artfix[k].level == s_lvl && k_lp_artfix[k].sec == sec
            && k_lp_artfix[k].face == face) { *rgb = k_lp_artfix[k].rgb; return 1; }
    return 0;
}

/* Per-LEVEL gapfill kill list: levels whose LP tier is NOT a coarse mirror
 * of the HP world but an authored ALTERNATE (blowhard/34: the LP has a
 * portal ARCH where the HP world has the exit vortex — LP-exclusive
 * structures classify as "gap fillers" and materialize as green arches on
 * the plateau, found in testing 2026-07-09).  The redundancy premise the whole
 * classifier rests on doesn't hold there; disable the feature wholesale.
 * OPENPETE_TERRAIN_GAPFILL_SKIP="34,..." overrides (comma list of
 * g_LevelId values; empty string = skip nothing). */
static int gp_level_skipped(CPUState* cpu) {
    static const char* s_skip = (const char*)-1;
    if (s_skip == (const char*)-1) {
        s_skip = getenv("OPENPETE_TERRAIN_GAPFILL_SKIP");
        if (!s_skip) s_skip = "34";
    }
    if (!*s_skip) return 0;
    int lvl = (int)(int32_t)rd(cpu, 0x8007596C);   /* g_LevelId */
    char key[16]; snprintf(key, sizeof key, ",%d,", lvl);
    char pad[128]; snprintf(pad, sizeof pad, ",%s,", s_skip);
    return strstr(pad, key) != NULL;
}

static void tsc_classify_gapfill(CPUState* cpu, tscache_t* C,
                                 uint32_t sectorPtr, uint32_t sectorCnt) {
    /* Thresholds are env-tunable without a rebuild (corpus-harness sweeps /
     * future per-level tuning): OPENPETE_TERRAIN_GAPFILL_TUNE=
     * "tol,cov,dgap,nz,nohp" (any suffix omittable; defaults
     * 96,0.9,192,0.8,1200). */
    static double s_tol = -1.0, s_cov, s_dgap, s_nz, s_nohp, s_covhi;
    if (s_tol < 0.0) {
        /* covhi 0.97 mid-band: peace_keepers' authored one-cell HP hole under
         * a 512x512 LP water quad measures cov = 27/30 = 0.900 EXACTLY and
         * was rejected on the knife's edge, leaving a cyclorama hole under
         * extended LOD (found in testing 2026-07-09).  Simply widening the cov
         * gate to 0.97 regressed tree_tops (a branch-floor LP sliver in the
         * same band POKES THROUGH the trunk HP — found in testing the same day),
         * so band faces get the stricter VOID test instead of the v5 rules:
         * admit only if some unbacked sample sits over a true world void.
         * The all-level bimodality sweep puts intentional-opening LP at
         * cov >= 0.97 and true gaps at <= 0.83; the band is the dead zone
         * between. */
        s_tol = 96.0; s_cov = 0.9; s_dgap = 192.0; s_nz = 0.8; s_nohp = 1200.0;
        s_covhi = 0.97;
        const char* e = getenv("OPENPETE_TERRAIN_GAPFILL_TUNE");
        if (e && *e) sscanf(e, "%lf,%lf,%lf,%lf,%lf,%lf", &s_tol, &s_cov, &s_dgap, &s_nz, &s_nohp, &s_covhi);
    }
    gp_build_nohp_pool(cpu, sectorPtr, sectorCnt);
    gp_build_all_pool(cpu, sectorPtr, sectorCnt);
    /* One-shot per-level tally of how the sector table splits by LOD class —
     * sizes the set where the two tiers can actually coexist. */
    if (getenv("OPENPETE_TERRAIN_TIERCENSUS")) {
        static uint32_t last_key;
        uint32_t ck = sectorPtr ^ (sectorCnt * 2654435761u);
        if (ck != last_key) {
            last_key = ck;
            int both = 0, forceLP = 0, noHP = 0, noLP = 0, hpOnly = 0, lpOnly = 0, tot = 0;
            long lpF_overlap = 0, lpF_tot = 0, hpF_tot = 0;
            uint32_t n = sectorCnt > 1024 ? 1024 : sectorCnt;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t s = rd(cpu, sectorPtr + i * 4); if (!s) continue;
                uint32_t g0 = rd(cpu, s + 0x04), g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
                int flags = (int)(g0 & 0xE000);
                int nLpF = (int)((g3 >> 16) & 0xFF), nHpF = (int)((g4 >> 16) & 0xFF);
                tot++; lpF_tot += nLpF; hpF_tot += nHpF;
                if (flags & 0x8000) { forceLP++; lpF_overlap += nLpF; }
                else if (flags & 0x4000) noHP++;
                else if (flags & 0x2000) noLP++;
                else if (nHpF && nLpF) { both++; lpF_overlap += nLpF; }
                else if (nHpF) hpOnly++;
                else if (nLpF) lpOnly++;
            }
            fprintf(stderr, "[tiercensus] sectors=%d forceLP(0x8000)=%d both(plain)=%d "
                            "noHP(0x4000)=%d noLP(0x2000)=%d hpOnly=%d lpOnly=%d "
                            "lpFaces=%ld hpFaces=%ld lpFacesInOverlap=%ld\n",
                    tot, forceLP, both, noHP, noLP, hpOnly, lpOnly,
                    lpF_tot, hpF_tot, lpF_overlap);
        }
    }
    const double TOL2 = s_tol * s_tol;     /* offline-validated: backed dmax ≤ ~82, gap dmed ≥ 150 */
    const double COV_MIN = s_cov;          /* gap-filler iff >10% of samples are unbacked... */
    const double GAP_D2  = s_dgap * s_dgap; /* ...AND the unbacked region is FAR from all HP
                                            * (median ≥ 2×TOL) OR the face is an UP-FACING
                                            * floor (n_z < 0, |n_z| > 0.8).  The distance test separates
                                            * true coverage voids (artisans holes: unbacked
                                            * dmed 218–512) from LP "shortcut skins" hugging
                                            * recessed HP detail (sunny_flight cave walls:
                                            * dmed 102–172), which punch through in front of
                                            * the wall — the ×1.03 depth pull only protects
                                            * LP≈HP surfaces (found in testing 2026-07-09,
                                            * sunny_flight freecam).  The horizontal exemption
                                            * re-admits the mid-distance FLOOR strips the two
                                            * bands share (artisans z=192 water-floor plain
                                            * edges, gnorc_cove z=1088 water plane: dmed
                                            * 116–181) — every observed punch-through skin is
                                            * a WALL, every observed mid-distance hole filler
                                            * is a floor. */
    const double NZ_MIN  = s_nz;
    if (!C->lpF || !C->lpV) return;
    if (!C->hpF || !C->hpV || C->nHpF <= 0) return;   /* no HP: LP draws anyway */
    for (int f = 0; f < C->nLpF; f++) {
        tsface_t* F = &C->lpF[f];
        F->gapfill = 0;
        if (F->i0 >= C->nLpV || F->i1 >= C->nLpV || F->i2 >= C->nLpV || F->i3 >= C->nLpV) continue;
        {   /* stamp the up-facing-floor bit for EVERY LP face (the near-floor
             * fill backstop reads it; same winding convention as the rules
             * below: floors have n_z < 0). */
            const tsvert_t* a = &C->lpV[F->i0];
            const tsvert_t* b = &C->lpV[F->i1];
            const tsvert_t* c = &C->lpV[F->i2];
            double e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
            double e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
            double nz = e1[0]*e2[1] - e1[1]*e2[0];
            double nx = e1[1]*e2[2] - e1[2]*e2[1];
            double ny = e1[2]*e2[0] - e1[0]*e2[2];
            double nl = nx*nx + ny*ny + nz*nz;
            F->floorf = (nl > 0.0 && nz < 0.0 && nz*nz >= NZ_MIN*NZ_MIN*nl);
        }
        if (gp_level_skipped(cpu)) continue;   /* alternate-LP level: no verdicts */
        int hit = 0, total = 0, nunb = 0;
        double unb[30];                    /* per-unbacked-sample min dist² (≤ 2 tris × 15) */
        double unbp[30][3];                /* their sector-local positions (near-noHP test) */
        for (int half = 0; half < (F->quad ? 2 : 1); half++) {
            /* same split as the offline sweep: (i0,i1,i2) + (i0,i2,i3) */
            const tsvert_t* a = &C->lpV[F->i0];
            const tsvert_t* b = &C->lpV[half ? F->i2 : F->i1];
            const tsvert_t* c = &C->lpV[half ? F->i3 : F->i2];
            for (int u = 0; u <= 4; u++)
                for (int v = 0; v + u <= 4; v++) {
                    int w = 4 - u - v;
                    double p[3] = { (a->x*u + b->x*v + c->x*w) / 4.0,
                                    (a->y*u + b->y*v + c->y*w) / 4.0,
                                    (a->z*u + b->z*v + c->z*w) / 4.0 };
                    total++;
                    double best = 1e30;
                    for (int hf = 0; hf < C->nHpF; hf++) {
                        const tsface_t* H = &C->hpF[hf];
                        if (H->i0 >= C->nHpV || H->i1 >= C->nHpV ||
                            H->i2 >= C->nHpV || H->i3 >= C->nHpV) continue;
                        double d = gp_pt_tri_d2(p, &C->hpV[H->i0], &C->hpV[H->i1], &C->hpV[H->i2]);
                        if (d < best) best = d;
                        if (H->quad) {
                            d = gp_pt_tri_d2(p, &C->hpV[H->i0], &C->hpV[H->i2], &C->hpV[H->i3]);
                            if (d < best) best = d;
                        }
                        if (best <= TOL2) break;
                    }
                    if (best <= TOL2) hit++;
                    else if (nunb < 30) {
                        unbp[nunb][0] = p[0]; unbp[nunb][1] = p[1]; unbp[nunb][2] = p[2];
                        unb[nunb++] = best;
                    }
                }
        }
        if (total > 0 && (double)hit < s_covhi * (double)total && nunb > 0) {
            /* median of the unbacked distances (insertion sort, ≤30);
             * keep the position array in the same order */
            for (int a2 = 1; a2 < nunb; a2++) {
                double v2 = unb[a2]; int b2 = a2 - 1;
                double p2[3] = { unbp[a2][0], unbp[a2][1], unbp[a2][2] };
                while (b2 >= 0 && unb[b2] > v2) {
                    unb[b2 + 1] = unb[b2];
                    unbp[b2 + 1][0] = unbp[b2][0]; unbp[b2 + 1][1] = unbp[b2][1]; unbp[b2 + 1][2] = unbp[b2][2];
                    b2--;
                }
                unb[b2 + 1] = v2;
                unbp[b2 + 1][0] = p2[0]; unbp[b2 + 1][1] = p2[1]; unbp[b2 + 1][2] = p2[2];
            }
            int far_gap = (unb[nunb / 2] >= GAP_D2);
            int floorH = 0, wallH = 0;
            double nrm[3] = { 0.0, 0.0, 0.0 };
            {   /* SIGNED n_z of the first triangle (z = the height axis; the
                 * game's face winding gives UP-facing floors n_z < 0, i.e.
                 * the winding normal points AWAY from the visible side).
                 * Only FLOORS earn the horizontal exemption — a DOWN-facing
                 * flat face in the sunny-skin dmed band is a coarse LP
                 * *ceiling* skin drawn below the recessed HP ceiling
                 * (magic_crafters z=1770 room lids, n_z=+1.00, dmed 103–128:
                 * found in testing 2026-07-09); every needed horizontal filler
                 * across artisans/gnorc (197 faces) is n_z ≤ −0.85. */
                const tsvert_t* a = &C->lpV[F->i0];
                const tsvert_t* b = &C->lpV[F->i1];
                const tsvert_t* c = &C->lpV[F->i2];
                double e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
                double e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
                double nx = e1[1]*e2[2] - e1[2]*e2[1];
                double ny = e1[2]*e2[0] - e1[0]*e2[2];
                double nz = e1[0]*e2[1] - e1[1]*e2[0];
                double nl = nx*nx + ny*ny + nz*nz;
                floorH = (nl > 0.0 && nz < 0.0 && nz*nz >= NZ_MIN*NZ_MIN*nl);
                wallH  = (nl > 0.0 && nz*nz < NZ_MIN*NZ_MIN*nl);
                if (nl > 0.0) {
                    double inv = 1.0 / sqrt(nl);
                    nrm[0] = nx*inv; nrm[1] = ny*inv; nrm[2] = nz*inv;
                }
            }
            int in_band = ((double)hit >= COV_MIN * (double)total);
            int voidok = 0;
            if (s_all_n > 0 && (in_band || floorH || far_gap)) {
                /* TWO-SIDED world-void test: some unbacked sample farther
                 * than TOL from ALL other geometry (both tiers, every
                 * sector, self-excluded) with BOTH normal directions clear
                 * within 256 — a needed fill is the world's last surface
                 * there and floats in genuinely OPEN space (peace_keepers
                 * s58 f#3: void sample at d_any=128, both rays clear).
                 * One-sided (behind-only) was not enough: a skin slicing
                 * INTO a solid has under-surface samples that are
                 * geometrically empty but visually inside the hill — the
                 * front ray (visible side, −n) hits the surface above
                 * (artisans s12 f#21 grass poke, found in testing 2026-07-09).
                 * tree_tops s47/s155/s180: samples touch other geometry
                 * (d_any=0) or have the trunk 31 behind → rejected. */
                double neg[3] = { -nrm[0], -nrm[1], -nrm[2] };
                for (int k = 0; k < nunb && !voidok; k++) {
                    double pw[3] = { unbp[k][0] + C->originX,
                                     unbp[k][1] + C->originY,
                                     unbp[k][2] + C->originZ };
                    if (gp_all_d2_excl(pw, C->s, f, TOL2) > TOL2 &&
                        !gp_all_ray_hit(pw, nrm, 256.0, C->s, f) &&
                        !gp_all_ray_hit(pw, neg, 256.0, C->s, f))
                        voidok = 1;
                }
            }
            if (!in_band) {
                int near_nohp = 0;
                /* The pool must be SUBSTANTIAL (≥32 tris) for "near noHP LP"
                 * to mean "LP-authoritative territory": levels with real
                 * boundary terrain have 178–310 pool tris (artisans 269,
                 * peace_keepers 310, cliff_town 178), while degenerate no-HP
                 * SLIVER sectors give 1–7 (haunted_towers 2, magic_crafters
                 * 2, tree_tops 1, gnorc 7) and made the proximity test
                 * meaningless — haunted's tunnel-interior LP walls (dmed
                 * 112–144, all four two-sided-void-rejected) were admitted
                 * off a 2-tri sliver and drew as dark slabs across the
                 * corridor (found in testing 2026-07-09). */
                if (!far_gap && wallH && s_nohp_n >= 32) {
                    /* Wall re-admission: the unbacked span continues into
                     * LP-AUTHORITATIVE territory (min sample distance to a no-HP /
                     * force-LP sector's LP ≤ s_nohp) — a level-boundary cliff whose
                     * LP is the only geometry there (artisans outer wall: 606–991),
                     * NOT an interior shortcut skin (sunny cave walls: ≥1382). */
                    double lim2 = s_nohp * s_nohp;
                    for (int k = 0; k < nunb && !near_nohp; k++) {
                        double pw[3] = { unbp[k][0] + C->originX,
                                         unbp[k][1] + C->originY,
                                         unbp[k][2] + C->originZ };
                        if (gp_pool_d2(pw) <= lim2) near_nohp = 1;
                    }
                }
                /* floorH alone is NOT sufficient (v8): the horizontal
                 * exemption's mid-distance floor strips must also pass the
                 * void test, or a coarse LP floor tri slicing through an HP
                 * mound gets admitted and pokes through the grass. */
                /* far_gap TOO is void-gated (v12): an LP-exclusive WALL tri
                 * with dmed 308 poked out of a jacques wall base — dmed≥192
                 * alone can't distinguish "deep void" from "deeply different
                 * alternate geometry"; the true artisans holes all carry void
                 * samples (offline audit: 6 kept / 16 flipped across 8
                 * levels; the artisans outer-wall cliffs stay admitted via
                 * near_nohp). */
                F->gapfill = (((far_gap || floorH) && voidok) || near_nohp);
            } else {
                /* MID-BAND [COV_MIN, covhi) — the bimodality dead zone; the
                 * v5 orientation rules are not trustworthy here.  Void test
                 * only. */
                F->gapfill = voidok;
            }
        }
        C->nGapfill += F->gapfill;
    }
}

/* A sector named by an env poly-anim descriptor animates its packed verts in
 * RAM — but the .s Phase-1 walk only re-packs the bank of the TIER it draws,
 * so the LP bank of a sector the .s always bins HP (near) can hold an
 * arbitrarily old pose (the Sunny-Flight wall's LP stays RAISED in RAM after
 * the puzzle lowers the HP). Static LP-vs-HP coverage classification is
 * meaningless for such sectors (it's a function of animation time — a stale
 * raised LP wall reads as "uncovered by HP" and gets admitted as a gap
 * filler), and native-only LP passes must never draw the stale bank. LP the
 * .s itself draws is safe: the walk re-packs LP verts before drawing them. */
static int tsc_sector_polyanim(CPUState* cpu, uint32_t s,
                               uint32_t sectorPtr, uint32_t sectorCnt) {
    for (int list = 0; list < 2; list++) {
        uint32_t off  = list ? ENVANIM_HPPOLY_CNT : ENVANIM_LPPOLY_CNT;
        uint32_t cnt  = rd(cpu, G_ENVANIM + off);
        uint32_t base = rd(cpu, G_ENVANIM + off + 4);
        if (!base || cnt == 0 || cnt > 1024) continue;
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t desc = rd(cpu, base + k * 4);
            if (!desc) continue;
            uint32_t idx = cpu->read_half(desc + 4);
            if (idx >= sectorCnt) continue;
            if (rd(cpu, sectorPtr + idx * 4) == s) return 1;
        }
    }
    return 0;
}

static const tscache_t* tsc_get(CPUState* cpu, uint32_t s,
                                uint32_t lqTable, uint32_t hqTable,
                                int texTblMode, int hiDetail,
                                uint32_t sectorPtr, uint32_t sectorCnt) {
    uint32_t g[5] = { rd(cpu, s + 0x04), rd(cpu, s + 0x08), rd(cpu, s + 0x0C),
                      rd(cpu, s + 0x10), rd(cpu, s + 0x14) };
    uint32_t h = (((s >> 2) * 2654435761u) >> 14) & (TSC_SLOTS - 1);
    /* 8-slot probe window + LRU eviction. The old 2-way (h, h^1) alternating
     * eviction thrashed: gnorc_cove freecam at the level's long axis pulls
     * ~170 sectors into one present, and any 3+ visible sectors sharing a slot
     * pair evicted each other EVERY present — each miss is a full re-decode +
     * gapfill classification (ms-scale for big sectors: s148 alone burned
     * ~5ms/present; 17-19 misses/present at the hot heading = the
     * direction-dependent perf cliff, 2026-07-10). The window makes thrash
     * need 9+ hot sectors in one 8-window; LRU (stamped per terrain_build
     * call) evicts a stale sector, never this present's working set. */
    tscache_t* C = NULL;
    for (uint32_t k = 0; k < 8u; k++) {
        tscache_t* W = &s_tsc[(h + k) & (TSC_SLOTS - 1)];
        if (W->s == s && W->sig[0]==g[0] && W->sig[1]==g[1] && W->sig[2]==g[2]
            && W->sig[3]==g[3] && W->sig[4]==g[4]) {
            s_tsc_hit++; W->lastUse = s_tsc_stamp; return W;
        }
    }
    {
        s_tsc_miss++;
        for (uint32_t k = 0; k < 8u; k++) {          /* empty slot first, else LRU */
            tscache_t* W = &s_tsc[(h + k) & (TSC_SLOTS - 1)];
            if (!W->s) { C = W; break; }
            if (!C || W->lastUse < C->lastUse) C = W;
        }
        free(C->lpV); free(C->lpF); free(C->hpV); free(C->hpF);
        free(C->lpVprev); free(C->hpVprev); free(C->lpVf); free(C->hpVf);
        memset(C, 0, sizeof(*C));
        C->s = s; memcpy(C->sig, g, sizeof(g));
        C->lastUse = s_tsc_stamp;
        C->flags = (int)(g[0] & 0xE000);
        {   /* Bounding sphere (header w0/w1) — the .s LP/HP binning inputs. w0 is
             * not in the signature (it never changes independently of w1..w4). */
            uint32_t w0 = rd(cpu, s + 0x00);
            C->centreX = (int)(w0 >> 16); C->centreY = (int)(w0 & 0xFFFF);
            C->centreZ = (int)(g[0] >> 16); C->radius = (int)(g[0] & 0x1FFF);
        }
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
        tsc_classify_gapfill(cpu, C, sectorPtr, sectorCnt);
        C->polyanim = tsc_sector_polyanim(cpu, s, sectorPtr, sectorCnt);
        if (C->polyanim && C->lpF) {   /* stale-bank guard: see tsc_sector_polyanim */
            for (int f = 0; f < C->nLpF; f++) C->lpF[f].gapfill = 0;
            C->nGapfill = 0;
        }
    }
    return C;
}

/* OPENPETE_TERRAIN_MESHDUMP="12,45,..." (or "all"): one-shot dump of sector
 * LP+HP meshes — world verts, face indices, raw face words, per-face gapfill
 * verdicts — for offline geometry analysis (near-LP-gap classifier). Walks
 * the WHOLE sector table (not just the vislist) once openpete_frame reaches
 * OPENPETE_TERRAIN_MESHDUMP_FRAME; dump at the analysis frame — the sector
 * table at first walk (~level load) is NOT the steady-state table. */
static void md_dump_sector(CPUState* cpu, uint32_t i, const tscache_t* SC) {
    fprintf(stderr, "[md-sec] s=%u flags=%04X origin=%d,%d,%d centre=%d,%d,%d rad=%d "
            "nLpV=%d nLpF=%d nHpV=%d nHpF=%d nGap=%d\n", i, SC->flags,
            SC->originX, SC->originY, SC->originZ, SC->centreX, SC->centreY, SC->centreZ,
            SC->radius, SC->nLpV, SC->nLpF, SC->nHpV, SC->nHpF, SC->nGapfill);
    for (int k = 0; k < SC->nLpV; k++)
        fprintf(stderr, "[md-v] s=%u t=lp k=%d p=%d,%d,%d\n", i, k,
                SC->originX + SC->lpV[k].x, SC->originY + SC->lpV[k].y, SC->originZ + SC->lpV[k].z);
    for (int k = 0; k < SC->nHpV; k++)
        fprintf(stderr, "[md-v] s=%u t=hp k=%d p=%d,%d,%d\n", i, k,
                SC->originX + SC->hpV[k].x, SC->originY + SC->hpV[k].y, SC->originZ + SC->hpV[k].z);
    for (int f2 = 0; f2 < SC->nLpF; f2++) {
        const tsface_t* F2 = &SC->lpF[f2];
        fprintf(stderr, "[md-f] s=%u t=lp f=%d i=%d,%d,%d,%d q=%d gf=%d w0=%08X w1=%08X\n",
                i, f2, F2->i0, F2->i1, F2->i2, F2->i3, F2->quad, F2->gapfill,
                rd(cpu, SC->lpFaces + (uint32_t)f2 * 8u),
                rd(cpu, SC->lpFaces + (uint32_t)f2 * 8u + 4u));
    }
    for (int f2 = 0; f2 < SC->nHpF; f2++) {
        const tsface_t* F2 = &SC->hpF[f2];
        fprintf(stderr, "[md-f] s=%u t=hp f=%d i=%d,%d,%d,%d q=%d w0=%08X w1=%08X w2=%08X w3=%08X\n",
                i, f2, F2->i0, F2->i1, F2->i2, F2->i3, F2->quad,
                rd(cpu, SC->hpFaces + (uint32_t)f2 * 16u),
                rd(cpu, SC->hpFaces + (uint32_t)f2 * 16u + 4u),
                rd(cpu, SC->hpFaces + (uint32_t)f2 * 16u + 8u),
                rd(cpu, SC->hpFaces + (uint32_t)f2 * 16u + 12u));
    }
}
static void md_dump_all(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt,
                        uint32_t lqTable, uint32_t hqTable, int texTblMode) {
    static const char* s_mdump = (const char*)-1;
    static int s_mdframe, s_mddone;
    if (s_mdump == (const char*)-1) {
        s_mdump = getenv("OPENPETE_TERRAIN_MESHDUMP");
        const char* e = getenv("OPENPETE_TERRAIN_MESHDUMP_FRAME");
        s_mdframe = (e && *e) ? atoi(e) : 0;
        if (s_mdump) fprintf(stderr, "[md-init] env=%s frame=%d\n", s_mdump, s_mdframe);
    }
    if (!s_mdump || !*s_mdump || s_mddone) return;
    if (openpete_frame < (unsigned)s_mdframe) return;
    s_mddone = 1;
    fprintf(stderr, "[md-fire] frame=%u sectorPtr=%08X sectorCnt=%u\n",
            openpete_frame, sectorPtr, sectorCnt);
    int all = (strcmp(s_mdump, "all") == 0);
    for (uint32_t i = 0; i < sectorCnt && i < 1024; i++) {
        if (!all) {
            char buf[16]; snprintf(buf, sizeof buf, ",%u,", i);
            char pad[512]; snprintf(pad, sizeof pad, ",%s,", s_mdump);
            if (!strstr(pad, buf)) continue;
        }
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        md_dump_sector(cpu, i, tsc_get(cpu, s, lqTable, hqTable, texTblMode, 0, sectorPtr, sectorCnt));
    }
}

/* Geometry (vertex) env-anim liveness — the artisans lowering-wall fix.
 * func_8002A6FC's poly-anim handlers (blocks 8002AA18 LP / 8002ABB8 HP) advance
 * a keyframe per descriptor and stamp it into sector+0x18/+0x1A; the .s Phase-1
 * walk then GTE-INTPL blends the descriptor's keyframe vertex banks and rewrites
 * the sector's packed vertex words IN PLACE (block_80025C08..80025D10) every
 * canonical tick while the animation runs. The cache signature only covers
 * header words +0x04..+0x14, so cached verts froze at whatever pose the sector
 * was first decoded in (user repro: moving-terrain.bk2, the Sunny Flight wall).
 * Mirror the game's own registry instead of hashing vertex RAM: any sector named
 * by an LP/HP poly-anim descriptor (u16 sector index at desc+4) gets its cached
 * verts re-decoded once per canonical tick (rolling the old decode into a prev
 * bank), and on sub-tick presents lerped prev→cur at the spine's alpha so the
 * geometry moves at present rate, not 29.97 Hz. A handful of descriptors per
 * level, ≤256 words per sector — negligible next to the walk itself. Colours
 * already read live; texmats are covered by tsc_tile_gens/tsc_refresh_anim. */
/* Sub-tick vert interp state (set around terrain_build by terrain_native_reextract;
 * zero on the canonical path so the 30 Hz render stays byte-exact int verts). */
static int    s_vanim_sub;     /* this build is a sub-tick re-extract (never roll banks) */
static int    s_vanim_on;      /* ...with a paired camera bracket (alpha valid) */
static double s_vanim_alpha;   /* the spine's 1-tick-lag present alpha (prev→cur) */
static uint32_t s_vanim_pres;  /* per-terrain_build counter — validity stamp for the banks */

/* Teleport guard: continuous env vert anim (the Sunny-Flight wall) moves a few
 * local units per tick; a keyframe-bank reset (loop restart, level event) jumps
 * hundreds. 64 local units/tick splits the two by an order of magnitude each
 * way — above it, snap to cur instead of sweeping the sector across the map. */
#define VANIM_JUMP_LOCAL 64
static int tsc_vanim_jump(const tsvert_t* p, const tsvert_t* c, int n) {
    for (int k = 0; k < n; k++) {
        if (abs((int)c[k].x - p[k].x) > VANIM_JUMP_LOCAL ||
            abs((int)c[k].y - p[k].y) > VANIM_JUMP_LOCAL ||
            abs((int)c[k].z - p[k].z) > VANIM_JUMP_LOCAL) return 1;
    }
    return 0;
}
static void tsc_vanim_lerp(const tsvert_t* p, const tsvert_t* c, int n,
                           double a, float* out) {
    for (int k = 0; k < n; k++) {
        out[k*3+0] = (float)(p[k].x + ((double)c[k].x - p[k].x) * a);
        out[k*3+1] = (float)(p[k].y + ((double)c[k].y - p[k].y) * a);
        out[k*3+2] = (float)(p[k].z + ((double)c[k].z - p[k].z) * a);
    }
}
static void tsc_vert_anim_refresh(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    if (!sectorPtr) return;
    static int s_vdbg = -1;
    if (s_vdbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_VANIMDBG"); s_vdbg = (e && *e && *e != '0'); }
    int dbg_desc = 0, dbg_cached = 0, dbg_prev = 0, dbg_filled = 0;
    for (int list = 0; list < 2; list++) {
        uint32_t off  = list ? ENVANIM_HPPOLY_CNT : ENVANIM_LPPOLY_CNT;
        uint32_t cnt  = rd(cpu, G_ENVANIM + off);
        uint32_t base = rd(cpu, G_ENVANIM + off + 4);
        if (!base || cnt == 0 || cnt > 1024) continue;
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t desc = rd(cpu, base + k * 4);
            if (!desc) continue;
            uint32_t idx = cpu->read_half(desc + 4);
            if (idx >= sectorCnt) continue;
            uint32_t s = rd(cpu, sectorPtr + idx * 4);
            if (!s) continue;
            dbg_desc++;
            uint32_t h = (((s >> 2) * 2654435761u) >> 14) & (TSC_SLOTS - 1);
            tscache_t* C = NULL;
            for (uint32_t w = 0; w < 8u; w++) {   /* same probe window as tsc_get */
                tscache_t* W = &s_tsc[(h + w) & (TSC_SLOTS - 1)];
                if (W->s == s) { C = W; break; }
            }
            if (!C) continue;
            dbg_cached++;
            if (C->lpVprev) dbg_prev++;
            int nLp = C->lpV ? C->nLpV : 0, nHp = C->hpV ? C->nHpV : 0;
            if (!s_vanim_sub) {
                /* Canonical build (once per tick, AFTER the .s ran its INTPL vert
                 * rewrite): roll prev = last tick's decode, re-decode cur from RAM.
                 * The roll pairs cur with the tick-N snapshot the interp spine
                 * committed, so the sub-tick lerp below rides the same 1-tick-lag
                 * timeline as the camera/mobys. */
                if (C->vrollPres == s_vanim_pres) continue;   /* named by both lists */
                C->vrollPres = s_vanim_pres;
                if (nLp) {
                    if (!C->lpVprev) C->lpVprev = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)nLp);
                    if (C->lpVprev) memcpy(C->lpVprev, C->lpV, sizeof(tsvert_t) * (size_t)nLp);
                    tsc_decode_verts(cpu, C->s + 0x1C, nLp, C->lpV);
                }
                if (nHp) {
                    if (!C->hpVprev) C->hpVprev = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)nHp);
                    if (C->hpVprev) memcpy(C->hpVprev, C->hpV, sizeof(tsvert_t) * (size_t)nHp);
                    tsc_decode_verts(cpu, C->s + 0x1C + ((C->sig[4] >> 24) & 0xFF) * 4,
                                     nHp, C->hpV);
                }
            } else if (s_vanim_on) {
                /* Sub-tick present: fill the fractional lerp banks prev→cur at the
                 * spine's alpha. Fractional floats on purpose — 1 LP local unit is
                 * 16 world units, so an int16 lerp would step the wall on the same
                 * 16-unit grid the fractional-camera fix removed. The walk consumes
                 * the banks only when vfracPres matches this build. */
                if (C->vfracPres == s_vanim_pres) continue;
                if ((nLp && C->lpVprev && tsc_vanim_jump(C->lpVprev, C->lpV, nLp)) ||
                    (nHp && C->hpVprev && tsc_vanim_jump(C->hpVprev, C->hpV, nHp)))
                    continue;                        /* teleport/keyframe reset: snap to cur */
                int filled = 0;
                if (nLp && C->lpVprev) {
                    if (!C->lpVf) C->lpVf = (float*)malloc(sizeof(float) * 3u * (size_t)nLp);
                    if (C->lpVf) { tsc_vanim_lerp(C->lpVprev, C->lpV, nLp, s_vanim_alpha, C->lpVf); filled = 1; }
                }
                if (nHp && C->hpVprev) {
                    if (!C->hpVf) C->hpVf = (float*)malloc(sizeof(float) * 3u * (size_t)nHp);
                    if (C->hpVf) { tsc_vanim_lerp(C->hpVprev, C->hpV, nHp, s_vanim_alpha, C->hpVf); filled = 1; }
                }
                if (filled) { C->vfracPres = s_vanim_pres; dbg_filled++; }
                if (filled && s_vdbg) {
                    /* first vert (either bank) that actually moved this tick */
                    for (int bank = 0; bank < 2; bank++) {
                        const tsvert_t* P = bank ? C->hpVprev : C->lpVprev;
                        const tsvert_t* V = bank ? C->hpV : C->lpV;
                        const float* F = bank ? C->hpVf : C->lpVf;
                        int n = bank ? nHp : nLp;
                        if (!P || !V || !F) continue;
                        for (int v = 0; v < n; v++) {
                            if (P[v].x != V[v].x || P[v].y != V[v].y || P[v].z != V[v].z) {
                                fprintf(stderr, "[vanim f=%u pres=%u] s=%u a=%.3f %sV[%d] (%d,%d,%d)->(%d,%d,%d) lerp=(%.2f,%.2f,%.2f)\n",
                                        openpete_frame, s_vanim_pres, idx, s_vanim_alpha,
                                        bank ? "hp" : "lp", v,
                                        P[v].x, P[v].y, P[v].z, V[v].x, V[v].y, V[v].z,
                                        F[v*3], F[v*3+1], F[v*3+2]);
                                bank = 2; break;
                            }
                        }
                    }
                }
            }
        }
    }
    if (s_vdbg && dbg_desc)
        fprintf(stderr, "[vanim-scan f=%u sub=%d on=%d desc=%d cached=%d prev=%d filled=%d]\n",
                openpete_frame, s_vanim_sub, s_vanim_on, dbg_desc, dbg_cached, dbg_prev, dbg_filled);
}

/* Texture (UV-scroll) env-anim sub-tick interpolation — the artisans waterfall
 * judder. func_8002A6FC's scrolling-texture list (g_EnvironmentAnimations
 * +0x08/+0x0C) keeps a scroller's ENTIRE animation state in a 7-bit accumulator
 * at desc+6: on a keyframe advance it adds the keyframe's signed int8 delta,
 * wraps to 0..127, and writes the accumulator into the tiledef V bytes as whole
 * texels — scroll>>2 into the LQ entry (+1/+5/+9/+13, i.e. v0/v1 of BOTH LOD
 * halves) and scroll>>1 into the 21-entry HQ record, whose sub-tile groups sit
 * at fixed +0x10/+0x20/+0x30 row offsets (mod 64). Those writes land once per
 * canonical tick, so tsc_tile_gens sees no change in between and the cached
 * texmats hold still while everything around them interpolates.
 *
 * Mirror the game's own registry (as tsc_vert_anim_refresh does for verts)
 * rather than diffing texture RAM. The prev/cur accumulator pair comes from the
 * shared per-object snapshot table (spyro_interp_capture_objs captures the
 * (tile, accumulator) pair at desc+4 once per canonical tick, keyed by the
 * descriptor address); here each descriptor is turned into a FRACTIONAL V offset
 * relative to the committed decode, which the two emit sites add to their corner
 * Vs. Rendered position is exactly lerp(prev,cur,alpha) in accumulator units —
 * the low bits the game's >>2 / >>1 truncated away are added back, which is why
 * an interpolating tile can sit up to 0.75 texel off the canonical whole-texel
 * position (sub-perceptual, and the direction the fractional-camera fix already
 * took). Everything else about the tiledef — CLUT, tpage, U, the HQ row offsets
 * — is left exactly as the game wrote it.
 *
 * The alpha is the SLOT'S OWN (from the pair's roll timestamps), not the camera
 * bracket's: a waterfall keeps flowing while the camera holds still, and the
 * camera ring dedupes a static camera down to no pair at all (freecam, standing
 * still) — the same reason the HUD icons and gem sparkles carry per-object
 * alphas. It also means a scroller that advances every N ticks sweeps smoothly
 * across its real advance interval rather than jumping once and holding.
 *
 * NOT handled here, deliberately: the tile-SWAP list (+0x00/+0x04) memcpys whole
 * 16-byte entries / 168-byte records between texture descriptors. That is a
 * discrete frame flip (supercharge ramps, portals), not a continuous scroll —
 * snapping it is faithful, and tsc_tile_gens/tsc_refresh_anim already do. */
/* A keyframe-table reset (loop restart, level event) moves the accumulator
 * arbitrarily; a real scroll steps by one keyframe's int8 delta — a few units.
 * 32 of the 128 accumulator units splits the two by an order of magnitude each
 * way: above it, snap to the committed decode rather than sweep the texture
 * across the tile. */
#define TSCROLL_JUMP 32
/* Interval ceiling. A scroller's keyframe duration is 6 bits of 1/60 s, so a
 * legitimate advance interval reaches ~31 ticks; sweeping across all of it is
 * the smoothest reading of the animation. The cap only bounds the pathological
 * case — a scroller that PAUSES (descriptor flag bit1) and resumes much later
 * would otherwise pair across the whole pause and crawl up from a stale pose. */
#define TSCROLL_PERIOD_MAX (8 * g_openpete_tick_period_ns)
/* Presents are generated slightly ahead of the tick stamp they belong to, so the
 * first present of a cycle legitimately lands just before alpha 0 (the spine's
 * neg-alpha floor). Extrapolate rather than clamp, or ~a quarter of all motion
 * arrives as a snap. */
#define TSCROLL_ALPHA_MIN (-0.35)
static void tsc_scroll_refresh(CPUState* cpu) {
    static int s_tdbg = -1;
    if (s_tdbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_TANIMDBG"); s_tdbg = (e && *e && *e != '0'); }
    s_tanim_have = 0;
    memset(s_tanim_act, 0, sizeof(s_tanim_act));
    int dbg_desc = 0, dbg_paired = 0;
    /* Canonical builds render the committed whole-texel decode, byte-exact. */
    if (!s_vanim_sub || !s_tanim_en) goto scan;
    {
    uint32_t cnt  = rd(cpu, G_ENVANIM + ENVANIM_SCROLL_CNT);
    uint32_t base = rd(cpu, G_ENVANIM + ENVANIM_SCROLL_CNT + 4);
    if (!base || cnt == 0 || cnt > 1024) goto scan;
    op_time_t now = openpete_interp_present_now();
    if (now <= 0) goto scan;
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t desc = rd(cpu, base + k * 4);
        if (!desc) continue;
        dbg_desc++;
        const openpete_interp_obj_t* o = openpete_interp_objs_find(desc);
        /* curr_ns == prev_ns means the snapshot clock stood still between the
         * two distinct accumulator states — there is no interval to sweep. It
         * happens under DETERMINISTIC capture with a frozen camera (the headless
         * freecam legs): the deterministic object stamp is the DIRECT ring's
         * tick_ns, and a static camera dedupes that ring so it never advances.
         * Windowed the stamp is the PLL virtual clock, which advances per tick
         * regardless of camera motion, so a still camera still scrolls. */
        if (!o || !o->have_prev || o->curr_ns <= o->prev_ns) continue;
        /* blob = the descriptor's u16 tile at +4 then u16 accumulator at +6 */
        uint16_t ctile, ptile, cur, prv;
        memcpy(&ctile, o->curr + 0, 2); memcpy(&cur, o->curr + 2, 2);
        memcpy(&ptile, o->prev + 0, 2); memcpy(&prv, o->prev + 2, 2);
        if (ctile >= 128 || ptile != ctile) continue;   /* identity: same tile both ends */
        cur &= 0x7Fu; prv &= 0x7Fu;
        /* Shortest path around the 7-bit ring: the accumulator wraps 127→0 mid
         * animation, and a raw difference there would read as a full-window jump. */
        int d = ((((int)cur - (int)prv) + 64) & 127) - 64;
        if (d == 0 || d > TSCROLL_JUMP || d < -TSCROLL_JUMP) continue;
        op_time_t per = o->curr_ns - o->prev_ns;
        if (per > TSCROLL_PERIOD_MAX) per = TSCROLL_PERIOD_MAX;
        double a = (double)(now - o->curr_ns) / (double)per;
        if (a < TSCROLL_ALPHA_MIN) a = TSCROLL_ALPHA_MIN; else if (a > 1.0) a = 1.0;
        /* Rendered accumulator = lerp(prev,cur,a) = cur + (a-1)*d. */
        double off = (a - 1.0) * (double)d;
        /* Land on the exact fractional position by adding back the low bits the
         * game's >>2 / >>1 truncated away. The window wrap keeps base+dv inside
         * the scroll ring: the sampler truncates toward zero (psx_sample.frag),
         * so a negative V would fetch the wrong row — and the content is
         * periodic over the window by construction, since the game's own 31→0
         * accumulator wrap would otherwise be a visible jump every cycle. */
        double dvq = off * 0.25 + (double)(cur & 3u) * 0.25;
        double baseQ = (double)(cur >> 2);
        if (baseQ + dvq < 0.0)        dvq += 32.0;
        else if (baseQ + dvq >= 32.0) dvq -= 32.0;
        s_tanim_dvlq[ctile] = (float)dvq;
        double dvh = off * 0.5 + (double)(cur & 1u) * 0.5;
        double baseA = (double)(cur >> 1);                  /* HQ entries 1-2 */
        double baseB = (double)(((cur >> 1) + 32u) & 63u);  /* HQ entries 3-4 (+0x20) */
        double dvA = dvh, dvB = dvh;
        if (baseA + dvA < 0.0)        dvA += 64.0;
        else if (baseA + dvA >= 64.0) dvA -= 64.0;
        if (baseB + dvB < 0.0)        dvB += 64.0;
        else if (baseB + dvB >= 64.0) dvB -= 64.0;
        s_tanim_dvhq[ctile][0] = (float)dvA;
        s_tanim_dvhq[ctile][1] = (float)dvB;
        s_tanim_act[ctile] = 1;
        s_tanim_have = 1;
        dbg_paired++;
        if (s_tdbg)
            fprintf(stderr, "[tanim f=%u pres=%u] tile=%u prev=%u cur=%u d=%d per=%.1fms a=%.3f dvlq=%.3f dvhq=%.3f/%.3f\n",
                    openpete_frame, s_vanim_pres, ctile, prv, cur, d,
                    (double)per / 1e6, a, dvq, dvA, dvB);
    }
    }
scan:
    if (s_tdbg)
        fprintf(stderr, "[tanim-scan f=%u pres=%u sub=%d en=%d] descs=%d paired=%d\n",
                openpete_frame, s_vanim_pres, s_vanim_sub, s_tanim_en, dbg_desc, dbg_paired);
}

/* Re-decode cached texmats whose tile content changed (env texture anim).
 * Epoch-gated: free when no tile changed since this sector's last refresh. */
static void tsc_refresh_anim(CPUState* cpu, tscache_t* C,
                             uint32_t lqTable, uint32_t hqTable,
                             int texTblMode, int hiDetail) {
    if (C->animEpoch == s_tgen_epoch) return;
    C->animEpoch = s_tgen_epoch;
    if (!C->hpF) return;                     /* LP faces are untextured */
    uint32_t tbl = (texTblMode == 2) ? hqTable : lqTable;
    for (int f = 0; f < C->nHpF; f++) {
        tsface_t* F = &C->hpF[f];
        if (F->tb == -1) continue;
        uint32_t g = s_tgen[F->tb & 0x7F];
        if (F->tgen == g) continue;
        F->tgen = g;
        terrain_texmat(cpu, tbl, F->tb, hiDetail, &F->mat, F->tu, F->tv);
        terrain_texmat(cpu, tbl, F->tb, 1, &F->matN, F->tuN, F->tvN);
    }
}

/* Host-pointer terrain colour (DPCS blend identical to terrain_color; the debug
 * fog print stays on the cpu-path variant only). */
static inline uint32_t t_le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t terrain_color_h(int noHP, const uint8_t* lp, const uint8_t* nearp,
                                const uint8_t* farp, int idx, double m3, int fogRef) {
    if (noHP) return t_le32(lp + idx) & 0xFFFFFFu;
    uint32_t nearC = t_le32(nearp + idx) & 0xFFFFFFu;
    uint32_t farC  = t_le32(farp  + idx) & 0xFFFFFFu;
    return terrain_fog_blend(nearC, farC, m3, fogRef);
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
    {   /* OPENPETE_TERRAIN_ALLVIS=1: walk ALL sectors (bypass D_VISIBILITY /
         * occlusion groups) — A/B probe for view-dependent sector culling. */
        static int s_allvis = -1;
        if (s_allvis < 0) { const char* e = getenv("OPENPETE_TERRAIN_ALLVIS"); s_allvis = (e && *e && *e != '0'); }
        if (s_allvis) {
            int n = 0;
            for (uint32_t i = 0; i < sectorCnt && n < 1024; i++) out[n++] = (uint16_t)i;
            return n;
        }
    }

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

/* OPENPETE_TERRAIN_ANIMDBG (keeper instrument): per-present RAM-change census
 * for env anim. Hashes each LQ tiledef entry (16B), each HQ record (168B), and
 * per sector: the +0x18 anim word, vert regions, colour regions. Prints indices
 * that changed since the previous present. Proved the waterfall freeze (only
 * lq/hq tile 23 churns in artisans; secV/secC never → verts safe to cache). */
/* ---- texture-record dumper (HD-pack matcher input) ------------------------ */

int stbi_write_png(const char *filename, int w, int h, int comp,
                   const void *data, int stride_in_bytes);   /* impl: screenshot.c TU */

/* Sample one 32x32 tile out of the native VRAM model (base-row CLUT) into an
 * RGBA8 canvas at (dx,dy). Texel decode + alpha encoding are identical to
 * texture_pack_dump_draw (0x0000 sentinel -> transparent, STP -> 128). */
static void texrec_blit32(const uint16_t* vr, const tmat* m, int u0, int v0,
                          unsigned char* dst, int dst_w, int dx, int dy) {
    const uint16_t* clut = vr + (size_t)m->cly * OPENPETE_VRAM_W + m->clx;
    for (int y = 0; y < 32; y++) {
        const uint16_t* row = vr + (size_t)(m->tpy + v0 + y) * OPENPETE_VRAM_W;
        for (int x = 0; x < 32; x++) {
            int uu = u0 + x;
            uint16_t p;
            if (m->bpp == 16)     p = row[m->tpx + uu];
            else if (m->bpp == 8) p = clut[(row[m->tpx + (uu >> 1)] >> (8 * (uu & 1))) & 0xFFu];
            else                  p = clut[(row[m->tpx + (uu >> 2)] >> (4 * (uu & 3))) & 0xFu];
            unsigned char* o = dst + ((size_t)(dy + y) * dst_w + (size_t)(dx + x)) * 4;
            unsigned r5 = p & 31u, g5 = (p >> 5) & 31u, b5 = (p >> 10) & 31u;
            o[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
            o[1] = (unsigned char)((g5 << 3) | (g5 >> 2));
            o[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
            o[3] = (p == 0) ? 0 : ((p & 0x8000u) ? 128 : 255);
        }
    }
}

/* Decode one record Tiledef entry, apply orientation, and reduce to the tile's
 * stored VRAM box origin (min corner over the effective UVs — a rotated tile
 * still occupies one 32x32 box; the ori deltas only permute which corner each
 * spatial vertex samples). Returns the ori index. */
static int texrec_entry(CPUState* cpu, uint32_t e, tmat* m, int* bu, int* bv) {
    float u[4], v[4];
    memset(m, 0, sizeof(*m)); m->bpp = 16;
    decode_tiledef(cpu, e, 0, m, u, v);
    int ori = tiledef_apply_ori(cpu, e, u, v);
    float mu = u[0], mv = v[0];
    for (int k = 1; k < 4; k++) {
        if (u[k] < mu) mu = u[k];
        if (v[k] < mv) mv = v[k];
    }
    *bu = (int)mu; *bv = (int)mv;
    return ori;
}

/* Emit one entry's JSON: page/uv material + the resolved pack identity (the
 * same (thash, box, phash) the replacement lookup keys on, so the offline
 * pack builder can name files without any manifest join). */
static void texrec_json(CPUState* cpu, FILE* f, const char* key, uint32_t e) {
    tmat m; int bu, bv;
    int ori = texrec_entry(cpu, e, &m, &bu, &bv);
    fprintf(f, ",\"%s\":{\"tpx\":%d,\"tpy\":%d,\"u\":%d,\"v\":%d,\"bpp\":%d,\"clut\":%d,\"ori\":%d",
            key, m.tpx, m.tpy, bu, bv, m.bpp, m.clut_raw, ori);
    texture_pack_id id;
    if (texture_pack_resolve(m.tpx, m.tpy, m.clx, m.cly, m.bpp,
                             bu, bv, bu + 31, bv + 31, &id))
        fprintf(f, ",\"id\":{\"thash\":\"%016llx\",\"phash\":\"%016llx\","
                   "\"bx\":%d,\"by\":%d,\"bw\":%d,\"bh\":%d}",
                (unsigned long long)id.thash, (unsigned long long)id.phash,
                id.box_x, id.box_y, id.box_w, id.box_h);
    else
        fprintf(f, ",\"id\":null");
    fprintf(f, "}");
}

/* OPENPETE_TEXPACK_RECORDS=<path>: dump the level's texture table as LOGICAL
 * textures for the HD-pack matcher. Per tile index N (LQ tile index == HQ record
 * index == a face's word2 & 0x7F), emit its LQ far/near Tiledef and the 4 HQ-record
 * quadrant Tiledefs (m_HQTexturePointer + N*168, entries (i+1)*8 = TL,TR,BL,BR),
 * each decoded to its VRAM page box + within-page (u,v) + bpp + clut + the
 * resolved replacement identity. With OPENPETE_TEXPACK_RECORDS_DIR also set,
 * each record's PIXELS are sampled straight from the native VRAM model — the
 * same texels the renderer binds, so no offline join can skew them — as
 * recNN-hq.png (64x64, the 4 quadrants composited), recNN-lq.png (32x32 near)
 * and recNN-lqfar.png (32x32 far). Re-dumps (overwriting) whenever the table
 * POINTERS change — every level load (titlescreen included) bump-allocates
 * fresh tables, so the last dump always describes the level currently loaded,
 * not whichever scene rendered first. */
static void terrain_texrecords(CPUState* cpu) {
    static int off = 0;
    static uint32_t sig = 0;
    if (off) return;
    const char* path = getenv("OPENPETE_TEXPACK_RECORDS");
    if (!path || !*path) { off = 1; return; }
    const char* dir = getenv("OPENPETE_TEXPACK_RECORDS_DIR");
    uint32_t lqTable = rd(cpu, G_ENVIRONMENT + ENV_LQTEX);
    uint32_t hqTable = rd(cpu, G_ENVIRONMENT + ENV_HQTEX);
    uint32_t texCnt  = rd(cpu, G_ENVIRONMENT + ENV_TEXCNT);
    if (!lqTable || !hqTable || !texCnt) return;   /* not loaded yet — retry next frame */
    uint32_t s = lqTable ^ (hqTable * 2654435761u) ^ (texCnt << 24);
    if (s == sig) return;                          /* this level already dumped */
    sig = s;
    if (texCnt > 128) texCnt = 128;
    FILE* f = fopen(path, "w");
    if (!f) { off = 1; return; }
    const uint16_t* vr = openpete_vram_data();
    fprintf(f, "{\"texCnt\":%u,\"tiles\":[", texCnt);
    for (uint32_t N = 0; N < texCnt; N++) {
        fprintf(f, "%s{\"n\":%u", N ? "," : "", N);
        texrec_json(cpu, f, "lq_far",  lqTable + N*16u);
        texrec_json(cpu, f, "lq_near", lqTable + N*16u + 8u);
        fprintf(f, ",\"hq\":[");
        for (int i = 0; i < 4; i++) {
            fprintf(f, "%s{\"q\":%d", i ? "," : "", i);
            texrec_json(cpu, f, "t", hqTable + N*168u + (uint32_t)(i+1)*8u);
            fprintf(f, "}");
        }
        fprintf(f, "]}");
        if (dir && *dir && vr) {
            static unsigned char px[64 * 64 * 4];
            char fn[640];
            tmat m; int bu, bv;
            /* 64x64 HQ composite: quadrant i -> (i&1)*32, (i>>1)*32. */
            for (int i = 0; i < 4; i++) {
                texrec_entry(cpu, hqTable + N*168u + (uint32_t)(i+1)*8u, &m, &bu, &bv);
                texrec_blit32(vr, &m, bu, bv, px, 64, (i & 1) * 32, (i >> 1) * 32);
            }
            snprintf(fn, sizeof fn, "%s/rec%02u-hq.png", dir, N);
            stbi_write_png(fn, 64, 64, 4, px, 64 * 4);
            /* LQ near + far 32x32 tiles. */
            texrec_entry(cpu, lqTable + N*16u + 8u, &m, &bu, &bv);
            texrec_blit32(vr, &m, bu, bv, px, 32, 0, 0);
            snprintf(fn, sizeof fn, "%s/rec%02u-lq.png", dir, N);
            stbi_write_png(fn, 32, 32, 4, px, 32 * 4);
            texrec_entry(cpu, lqTable + N*16u, &m, &bu, &bv);
            texrec_blit32(vr, &m, bu, bv, px, 32, 0, 0);
            snprintf(fn, sizeof fn, "%s/rec%02u-lqfar.png", dir, N);
            stbi_write_png(fn, 32, 32, 4, px, 32 * 4);
        }
    }
    fprintf(f, "]}\n");
    fclose(f);
    fprintf(stderr, "[texpack-records] wrote %u logical tiles -> %s%s%s\n",
            texCnt, path, (dir && *dir) ? ", pixels -> " : "",
            (dir && *dir) ? dir : "");
}

static void terrain_animdbg(CPUState* cpu) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_TERRAIN_ANIMDBG"); on = (e && *e && *e != '0') ? 1 : 0; }
    /* OPENPETE_TEXPACK_ANIMPROBE=<N>: raw Tiledef words of record N each
     * present — shows HOW env anim (func_8002A6FC) rewrites an animated tile
     * (UV scroll vs CLUT swap), which decides the shape of its replacement
     * file (a box covering the full swept range). */
    static int probeN = -2;
    if (probeN == -2) { const char* e = getenv("OPENPETE_TEXPACK_ANIMPROBE"); probeN = (e && *e) ? atoi(e) : -1; }
    if (probeN >= 0) {
        uint32_t lt = rd(cpu, G_ENVIRONMENT + ENV_LQTEX), ht = rd(cpu, G_ENVIRONMENT + ENV_HQTEX);
        if (lt && ht) {
            fprintf(stderr, "[animprobe f=%u] lq %08x %08x | %08x %08x | hq",
                    openpete_frame, rd(cpu, lt + (uint32_t)probeN*16u), rd(cpu, lt + (uint32_t)probeN*16u + 4),
                    rd(cpu, lt + (uint32_t)probeN*16u + 8), rd(cpu, lt + (uint32_t)probeN*16u + 12));
            for (int i = 0; i < 5; i++)
                fprintf(stderr, " %08x %08x", rd(cpu, ht + (uint32_t)probeN*168u + (uint32_t)i*8u),
                        rd(cpu, ht + (uint32_t)probeN*168u + (uint32_t)i*8u + 4));
            fprintf(stderr, "\n");
        }
    }
    if (!on) return;
    uint32_t lqTable   = rd(cpu, G_ENVIRONMENT + ENV_LQTEX);
    uint32_t hqTable   = rd(cpu, G_ENVIRONMENT + ENV_HQTEX);
    uint32_t texCnt    = rd(cpu, G_ENVIRONMENT + ENV_TEXCNT);
    uint32_t sectorPtr = rd(cpu, G_ENVIRONMENT + ENV_SECTORPTR);
    uint32_t sectorCnt = rd(cpu, G_ENVIRONMENT + ENV_SECTORCNT);
    if (texCnt > 128) texCnt = 128;
    if (sectorCnt > 512) sectorCnt = 512;
    static uint32_t lq[128], hq[128], s18[512], svh[512], sch[512];
    static int have = 0;
    char lqs[256] = "", hqs[256] = "", s18s[256] = "", svs[256] = "", scs[256] = "";
    for (uint32_t t = 0; t < texCnt; t++) {
        uint32_t h = 2166136261u;
        for (int w = 0; w < 4; w++) h = (h ^ rd(cpu, lqTable + t * 16u + (uint32_t)w * 4u)) * 16777619u;
        if (have && h != lq[t] && strlen(lqs) < 240) snprintf(lqs + strlen(lqs), 12, " %u", t);
        lq[t] = h;
        h = 2166136261u;
        for (int w = 0; w < 42; w++) h = (h ^ rd(cpu, hqTable + t * 168u + (uint32_t)w * 4u)) * 16777619u;
        if (have && h != hq[t] && strlen(hqs) < 240) snprintf(hqs + strlen(hqs), 12, " %u", t);
        hq[t] = h;
    }
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4u);
        if (!s) continue;
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int nLpV = g3 & 0xFF, nLpC = (g3 >> 8) & 0xFF;
        int nHpV = g4 & 0xFF, nHpC = (g4 >> 8) & 0xFF, hpOff = (int)((g4 >> 24) & 0xFF);
        uint32_t h = rd(cpu, s + 0x18);
        if (have && h != s18[i] && strlen(s18s) < 240) snprintf(s18s + strlen(s18s), 12, " %u", i);
        s18[i] = h;
        h = 2166136261u;                                   /* verts: LP + HP */
        for (int w = 0; w < nLpV; w++) h = (h ^ rd(cpu, s + 0x1C + (uint32_t)w * 4u)) * 16777619u;
        for (int w = 0; w < nHpV; w++) h = (h ^ rd(cpu, s + 0x1C + (uint32_t)(hpOff + w) * 4u)) * 16777619u;
        if (have && h != svh[i] && strlen(svs) < 240) snprintf(svs + strlen(svs), 12, " %u", i);
        svh[i] = h;
        h = 2166136261u;                                   /* colours: LP + HP near/far pairs */
        for (int w = 0; w < nLpC; w++) h = (h ^ rd(cpu, s + 0x1C + (uint32_t)(nLpV + w) * 4u)) * 16777619u;
        for (int w = 0; w < nHpC * 2; w++) h = (h ^ rd(cpu, s + 0x1C + (uint32_t)(hpOff + nHpV + w) * 4u)) * 16777619u;
        if (have && h != sch[i] && strlen(scs) < 240) snprintf(scs + strlen(scs), 12, " %u", i);
        sch[i] = h;
    }
    if (have && (lqs[0] || hqs[0] || s18s[0] || svs[0] || scs[0]))
        fprintf(stderr, "[anim-dbg] f=%u lq:%s | hq:%s | s18:%s | secV:%s | secC:%s\n",
                openpete_frame, lqs, hqs, s18s, svs, scs);
    have = 1;
}

static void terrain_build(CPUState* cpu, int32_t occGroup) {
    s_vanim_pres++;                /* new build → prior lerp/roll bank stamps go stale */
    terrain_texrecords(cpu);
    terrain_animdbg(cpu);
    terrain_abr_scan(cpu);
    tsc_census();
    {   /* OPENPETE_TERRAIN_OCCL_DBG: occlusion-cull census (~1/120 walks) */
        static int on = -1, n; static long prev_tris;
        if (on < 0) { const char* e = getenv("OPENPETE_TERRAIN_OCCL_DBG"); on = (e && *e && *e != '0'); }
        if (on && ++n >= 120) {
            fprintf(stderr, "[ter-occl f=%u] sectors-culled=%ld/window last-tris=%ld\n",
                    openpete_frame, s_tocc_culled, prev_tris);
            s_tocc_culled = 0; n = 0;
        }
        prev_tris = s_tri_n;
    }
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
    int wantOracle = openpete_portal_sky_recent() ||   /* portal guard reads the queue */
                     s_caldbg || s_nearq > 0 ||
                     (getenv("OPENPETE_TERRAIN_VERIFY") && *getenv("OPENPETE_TERRAIN_VERIFY") != '0');
    terrain_dbg_init();
    /* FAITHFUL LOD (W2.6-c, OPENPETE_TERRAIN_LOD_FAITHFUL): mirror the .s
     * sector binning — HP list = lodThr > centreZ − radius (HP ends at the LOD
     * distance), LP list = 0x8000 || lodThr < centreZ + radius + 0x100 (LP
     * substitutes beyond it) — AND default the exact DPCS fog on (the two are
     * one mechanism: the .s tier swap happens exactly at fog saturation,
     * ir0 = fogRef−4·m3 = 0 at m3 = lodThr, which is what makes the PSX
     * handoff invisible; the 2026-06 "pop-in" rejection of mirrored lists
     * happened under the under-fogged fogscale-1 default). Forced OFF under
     * --no-lod: HP-everywhere IS the no-lod intent, and there the exact fog
     * must stay off with it (fully-fogged HP without the .s tier coverage was
     * the W2.3 vista regression). */
    if (s_tier_faithful < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_LOD_FAITHFUL");
        s_tier_faithful = (e && *e) ? (*e != '0') : 0;
    }
    /* no-lod override applied per walk (not baked into the latch): the
     * draw-distance toggle can flip openpete_no_lod mid-level. */
    int tier_faithful = s_tier_faithful && !openpete_no_lod;
    if (s_fogscale < 0.0) {  /* viewZ = m3 (m3 already in SZ3 scale); env-tunable */
        const char* e = getenv("OPENPETE_TERRAIN_FOGSCALE");
        s_fogscale = (e && *e) ? atof(e) : 1.0;
    }
    if (s_fogdpcs < 0) {
        /* DEFAULT ON since the fog arc (2026-07-05): the exact decode is
         * proven per-corner at every band, and the old vista regression it
         * was blamed for was actually the MISSING CLUT-row fog (s_clutfog)
         * that the under-fogged lerp partially compensated. Ship both. */
        const char* e = getenv("OPENPETE_TERRAIN_FOGDPCS");
        s_fogdpcs = (e && *e) ? (*e != '0') : 1;
    }
    /* The whole fog model rides ONE user knob, --fog-scale (fog-distance
     * multiplier k; openpete_fog_scale: explicit --fog-scale wins, else 100
     * under --no-lod / 1 stock). Internally SZ = s_fogscale·m3 with
     * s_fogscale = 4/k: k=1 → the exact PSX curve (SZ = 4·m3); k=100 →
     * saturation ~100x farther, i.e. fog effectively off — extended LOD
     * already draws the far detail, so the default stops fogging it away.
     * (k=4 is the old no-lod default, numerically the old lerp curve
     * fogscale 1; still selectable via --fog-scale 4.) The exact math, the
     * palette ladder and the saturation handoff all ride the stretched curve.
     * Re-resolved EVERY walk, not latched at boot: the settings-menu
     * draw-distance toggle (openpete_no_lod_set) flips openpete_fog_scale
     * live, and the LP/HP fog-saturation handoff below rides s_fogscale —
     * the boot-latched value froze the handoff at the boot mode's distance,
     * so toggling no-lod ON mid-level kept substituting the untextured LP
     * tier at stock fog range until the camera walked each sector out of
     * the fully-fogged band (user report 2026-07-08: distant LP "snapping"
     * to HP on approach). OPENPETE_TERRAIN_FOGSCALE force-overrides (debug). */
    {
        static int s_fs_env = -1;
        if (s_fs_env < 0) s_fs_env = getenv("OPENPETE_TERRAIN_FOGSCALE") ? 1 : 0;
        if (s_fogdpcs && !s_fs_env)
            s_fogscale = 4.0 / (double)openpete_fog_scale;
    }
    if (s_clutfog < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_CLUTFOG");
        s_clutfog = (e && *e) ? (*e != '0') : 1;
    }
    /* .s LP/HP fog-saturation handoff — HP faces are dropped once FULLY beyond
     * fog saturation (ir0 <= 0 at every corner) and LP substitutes (far-only).
     * DEFAULT ON since the fog arc (2026-07-05): beyond saturation the .s
     * picture IS the LP tier (raw palette — identity-join-proven), and any HP
     * content native paints there is invented (the artisans far-water green
     * stripe once the CLUT ladder landed). The swap happens strictly inside
     * fully-fogged pixels, so there is no visible pop-in — which is why this
     * stays on under --no-lod too (no-lod removes MESH pop-in; fog saturation
     * is the one place a tier swap is invisible by construction). The W2.3
     * "recovered only ~0.9/4.0" verdict predates exact verts + the ladder.
     * OPENPETE_TERRAIN_LOD_HANDOFF=0 restores HP-everywhere (A/B). */
    {
        static tn_envvbl_t hc, gc;
        /* Rides the --fog-scale curve via s_fogscale (the per-face beyond
         * test below uses it), so under a stretched curve the handoff fires
         * only at the stretched saturation distance. */
        s_lod_handoff = tn_env_vbl(&hc, "OPENPETE_TERRAIN_LOD_HANDOFF", 1, 1);
        /* =0 disables the near-LP-gap fill (A/B) */
        s_gapfill_en  = tn_env_vbl(&gc, "OPENPETE_TERRAIN_GAPFILL", 1, 1);
    }
    /* Effective handoff for THIS walk. Under --no-lod the near/far LOD handoff
     * must NOT run: it keys off the RAW-RAM lodDist (no-lod saturates only the
     * .s read sites, not RAM — g_Environment.m_LodDistance stays 0x8000), so it
     * keeps dropping HP at the real fog-saturation band (m3 >= fogRef) and opens
     * the HP/LP seam the from-above freecam view exposes (an LP-only far sector
     * abutting an HP near sector, neither reaching the boundary). --lod extended
     * means HP everywhere it exists; the poke-free holefill bridges the residual
     * sector-boundary seam. Original LOD keeps the handoff verbatim — this local
     * equals s_lod_handoff there, so that path stays byte-for-byte unchanged.
     * (This overrides the "stays on under --no-lod" note above; the fully-fogged
     * tier-swap that note protects is verified against the green-stripe regression
     * at the fogged horizon under --no-lod, not just the seam view.) */
    int lod_handoff = s_lod_handoff && !openpete_no_lod;
    /* Cross-feeder global-consistency probe (rides the reveal-census env):
     * prints the nl/fog values THIS walk sees + the global's address, so a
     * per-module duplicate of openpete_no_lod / openpete_fog_scale shows up
     * as a value or address mismatch vs the [reveal-census] line. */
    {
        static int s_p = -1; static unsigned s_last = 0;
        if (s_p < 0) { const char* e = getenv("OPENPETE_MOBY_REVEAL_CENSUS"); s_p = (e && *e && *e != '0'); }
        if (s_p && openpete_frame >= s_last + 100u) {
            s_last = openpete_frame;
            fprintf(stderr, "[terrain-walk] f=%u nl=%d@%p fogscale=%.4f handoff=%d faithful=%d\n",
                    openpete_frame, openpete_no_lod, (void*)&openpete_no_lod,
                    s_fogscale, lod_handoff, tier_faithful);
        }
    }
    if (s_nearz < 0.0) {     /* near plane in world/16 view-Z (env-tunable) */
        const char* e = getenv("OPENPETE_TERRAIN_NEARZ");
        s_nearz = (e && *e) ? atof(e) : 1.0;
    }
    { const char* e = getenv("OPENPETE_TERRAIN_FOGDBG"); s_fogdbg = (e && *e && *e != '0'); }
    /* Far-band identity instrument (fog decode arc): one-shot full dump of
     * per-sector binning state + per-face corner records (screen xy, m3, raw
     * colour words per tier) for the offline identity JOIN against the
     * captured .s prims (which self-classify LP/HP by prim code 0x30/0x38 vs
     * 0x34/0x3C). Use with OPENPETE_TERRAIN_VERIFY (no feed) so the single
     * build is the canonical verify one. */
    if (s_fardbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_FARDBG"); s_fardbg = (e && *e) ? atoi(e) : 0; }
    /* value > 1 = fire at that openpete_frame (feed mode, pairs with a
     * --screenshot-on-frame of the same number); 1 = first build (verify mode) */
    s_fardbg_active = s_fardbg && !s_fardbg_done
                      && (s_fardbg == 1 || openpete_frame >= (unsigned)s_fardbg);
    /* OPENPETE_TERRAIN_FACEMAP=<frame>: one-shot per-LP-face screen-footprint
     * dump (fires on the first build at/after <frame> — no VBL-parity trap) for
     * offline hole→face attribution: rasterize the dumped footprints and
     * intersect with the {og covers ∧ native uncovered} pixel mask to derive
     * the fill-face list. Forces a dump-only LP walk for sectors whose LP pass
     * would not otherwise run, so DROPPED candidates are attributable too —
     * the pixels needing attribution are exactly the ones nothing drew. */
    if (s_facemap < 0) { const char* e = getenv("OPENPETE_TERRAIN_FACEMAP"); s_facemap = (e && *e) ? atoi(e) : 0; }
    s_facemap_active = s_facemap && !s_facemap_done && openpete_frame >= (unsigned)s_facemap;
    int M[3][3]; ngte_read_matrix(cpu, G_CAMERA + 0x00, M);   /* m_ProjectionMatrix */
    int Mbin[3][3]; ngte_read_matrix(cpu, G_CAMERA + 0x14, Mbin); /* .s sector-binning matrix (LP/HP LOD lists) */
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
    tsc_tile_gens(cpu, lqTable, rd(cpu, G_ENVIRONMENT + ENV_TEXCNT));  /* env-anim liveness */
    tsc_scroll_refresh(cpu);                                           /* env-anim UV scroll (sub-tick) */
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
    tsc_vert_anim_refresh(cpu, sectorPtr, sectorCnt);  /* geometry env-anim liveness */
    md_dump_all(cpu, sectorPtr, sectorCnt, lqTable, hqTable, texTblMode);  /* offline-analysis dump (env-gated) */
    /* Visible-sector index set: canonical 4:3 verdict (D_800771C8) at native
     * aspect; occlusion-group candidate set in widescreen (terrain_vislist). */
    static uint16_t visidx[1024];
    int nvis = terrain_vislist(cpu, sectorCnt, visidx, occGroup);
    s_tsc_stamp++;   /* LRU epoch: everything touched this walk outlives strangers */
    /* OPENPETE_TERRAIN_TIMERS=1: per-SECTOR walk-cost census (~1/s). Attributes
     * the terrain feeder's µs to individual sectors (accounted at the top of
     * the NEXT iteration so every exit path of the huge loop body is covered)
     * and names the top offenders — built for the gnorc_cove freecam
     * direction-dependent perf hunt (2026-07-10). Inert unless the env is set. */
    static int tt_on = -1;
    if (tt_on < 0) { const char* e = getenv("OPENPETE_TERRAIN_TIMERS"); tt_on = (e && *e && *e != '0'); }
    static uint32_t tt_us[1024], tt_tris[1024];      /* window-accumulated per sector */
    static uint64_t tt_win_us, tt_report_t;
    static uint32_t tt_presents, tt_nvis_sum, tt_nvis_max, tt_pres_max_us;
    uint64_t tt_prev = 0; uint32_t tt_prev_i = 0; int tt_prev_valid = 0, tt_prev_tris = 0;
    uint64_t tt_present_us = 0;
    for (int vi = 0; vi < nvis; vi++) {
        uint32_t i = visidx[vi];
        if (tt_on) {
            uint64_t tnow = openpete_phase_now_us();
            if (tt_prev_valid && tt_prev_i < 1024u) {
                uint32_t d = (uint32_t)(tnow - tt_prev);
                tt_us[tt_prev_i] += d; tt_present_us += d;
                tt_tris[tt_prev_i] += (uint32_t)(s_tri_n - tt_prev_tris);
            }
            tt_prev = tnow; tt_prev_i = i; tt_prev_tris = s_tri_n; tt_prev_valid = 1;
        }
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        /* Tier-1 sector decode cache: static level data unpacked once. */
        static int s_hidet = -2;
        if (s_hidet == -2) { const char* e = getenv("OPENPETE_TERRAIN_HIDETAIL"); s_hidet = e ? atoi(e) : -1; }
        const tscache_t* SC = tsc_get(cpu, s, lqTable, hqTable, texTblMode,
                                      s_hidet >= 0 ? s_hidet : 0,
                                      sectorPtr, sectorCnt);
        tsc_refresh_anim(cpu, (tscache_t*)SC, lqTable, hqTable, texTblMode,
                         s_hidet >= 0 ? s_hidet : 0);

        /* Sector occlusion cull — the extended-LOD win. A sector whose entire
         * screen bbox sits BEHIND nearer opaque terrain paints zero pixels: the
         * GPU depth-tests it away, but the CPU still paid to extract, sort and
         * upload it. Under --lod extended the game's distance cull is defeated,
         * so fully-occluded distant sectors (everything behind a wall) still get
         * walked — measured ~22k tris vs ~2.7k against a beast_makers wall, a
         * pixel-identical frame at ~172 vs ~453 fps (4:3; the ultrawide win is
         * larger). Reuse the moby occlusion grid (built from the WORLD-opaque
         * terrain depth pre-pass, frames-lagged like the moby query): cull a
         * sector only when EVERY grid cell over its (inflated) bbox is nearer
         * than the sector's NEAREST point — partial occlusion keeps it, so no
         * pop-in. openpete_native_occl_query returns 0 (keep) whenever the grid
         * is absent/stale/too-near, so this is a pure conservative skip.
         * Display-only: the canonical PSX render that writes gameplay channels
         * is a separate pass, untouched (and the native feed is inert headless,
         * so the oracle/determinism gates are unaffected). Bit-identical off vs
         * on, verified at 4:3 and 21:9. OPENPETE_TERRAIN_OCCL=0 disables (A/B). */
        static tn_envvbl_t tocc_c;
        int s_tocc = tn_env_vbl(&tocc_c, "OPENPETE_TERRAIN_OCCL", 1, 1);
        if (s_tocc) {
            double oc1, oc2, oc3;
            ngte_mac(M, camy - SC->centreY, camz - SC->centreZ,
                     SC->centreX - camx, &oc1, &oc2, &oc3);
            if (oc3 > (double)SC->radius + 1.0) {   /* wholly in front of camera */
                double inv = s_H / oc3;
                float sx  = (float)(s_ofx + oc1 * inv);
                float sy  = (float)(s_ofy + oc2 * inv);
                float rpx = (float)((double)SC->radius * inv * 1.5 + 8.0);  /* generous bbox */
                float vz_near = (float)((oc3 - (double)SC->radius) * 16.0);  /* world/16 → full-world */
                if (openpete_native_occl_query(sx - rpx, sy - rpx, sx + rpx, sy + rpx, vz_near)) {
                    s_tocc_culled++;
                    continue;   /* fully behind nearer terrain — skip extraction */
                }
            }
        }

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
        /* Bounding-centre view-Z through the .s BINNING matrix (g_Camera+0x14 —
         * NOT the +0x00 projection matrix the vertex walk uses; the two differ
         * in scale). Input to the faithful tier lists and the fill's distance
         * gate. lodThr = ENV_LODDIST>>4 (the same lodDist that drives fog:
         * ir0 = fogRef−4·m3 crosses 0 exactly at m3 = lodThr). */
        double cbin;
        { double c1m, c2m;
          ngte_mac(Mbin, camy - SC->centreY, camz - SC->centreZ,
                   SC->centreX - camx, &c1m, &c2m, &cbin); }
        int lodThr = fogRefBase >> 2;
        int drawHP = !(flags & 0x4000) && nHpF > 0;
        int drawLP = !(flags & 0x2000) && nLpF > 0 && ((flags & 0x8000) || !drawHP);
        if (tier_faithful) {
            /* The .s sector binning (func @0x80025940): HP ends at the LOD
             * distance; LP substitutes beyond it. A straddling sector lands in
             * BOTH lists (LP rides behind HP via the ×1.03 tier pull, exactly
             * the PSX LP-first/HP-over order). */
            drawHP = drawHP && ((double)lodThr > cbin - SC->radius);
            drawLP = !(flags & 0x2000) && nLpF > 0 &&
                     ((flags & 0x8000) || (double)lodThr < cbin + SC->radius + 0x100);
        }
        if (s_fardbg_active)
            fprintf(stderr, "[fardbg-sec] s=%d flags=%04X cbin=%.1f rad=%d lodThr=%d fogRefBase=%d "
                            "drawHP=%d drawLP=%d nLpF=%d nHpF=%d\n",
                    (int)i, flags, cbin, SC->radius, lodThr, fogRefBase,
                    drawHP, drawLP, nLpF, nHpF);
        /* .s HP-list membership (lodThr > cbin − radius): the CLUT-fog ladder
         * is only ever evaluated by the .s INSIDE this envelope — beyond it
         * the .s draws LP, so ladder rows past it are unauthored for many
         * tiles (artisans far water/shore: row+Δ landed on green land
         * palettes, g1 metric 897→1681). Beyond-envelope HP (native's
         * no-pop-in extension) keeps the base palette, which og shows is
         * the closer stand-in for the LP look. The envelope stretches with
         * --fog-scale (k = 4/s_fogscale; ×1 at stock, bit-identical). */
        int secInLOD = ((double)lodThr * (4.0 / s_fogscale) > cbin - SC->radius);
        /* LOD-handoff experiment: LP also substitutes for the HP dropped beyond
         * the LOD distance (far-only; gated per face below).
         * lpGapOnly: when the handoff is OFF (--no-lod draws HP at all
         * distances), an HP sector's classified coverage-gap LP faces still
         * ride along — ONLY those — so the sparse-HP holes stay filled (the
         * near-LP-gap fix; per-face gate below). */
        int lpFarOnly = 0, lpGapOnly = 0;
        int drawLPfull = drawLP;   /* LP tier fully drawn (pre-substitution arms) */
        if (drawHP && !drawLP && !(flags & 0x2000) && nLpF > 0) {
            if (lod_handoff) { drawLP = 1; lpFarOnly = 1; }
            else if (s_gapfill_en && SC->nGapfill > 0) { drawLP = 1; lpGapOnly = 1; }
        }
        /* LP HOLE-FILL backing (W2.6-c): an HP sector that does NOT already
         * draw its LP re-emits it as WORLD_FILL — stencil-gated in core to the
         * pixels no world geometry covered, i.e. the LP↔HP T-junction seams
         * (the artisans distant-water gaps). Because of the stencil gate it can
         * never poke through the HP (the failure of the 2026-06-26 draw-LP-
         * everywhere attempt); it draws at TRUE LP depth, no tier bias.
         *
         * DISTANCE GATE — the .s LP-list binning condition (func @0x80025940):
         * LP is fed only when lodThr < centerZ + radius + 0x100 (the sector's
         * far edge crosses the LOD boundary; lodThr = ENV_LODDIST>>4, centerZ =
         * the projected bounding-CENTRE view-Z, radius = w1 & 0x1FFF). A NEAR
         * sector's LP is never drawn by the PSX, and feeding it here broke the
         * portal apertures: the near arch-hill's coarse LP bulged into the
         * opening (stencil==0 — nothing marks inside an aperture), painted at
         * near depth and depth-killed the portal-sky mask (gate tick 9775).
         * The seams the fill exists for sit exactly AT the LOD boundary, which
         * satisfies this condition by construction. Distance-dependence is
         * confined to sub-pixel seam slivers, so no visible pop-in. NB: reads
         * the RAW RAM lodDist — --no-lod's INT32_MAX saturation substitutes at
         * the .s READ sites only, so this stays the real LOD band marker. */
        /* OPENPETE_TERRAIN_HOLEFILL=0 disables (A/B) */
        static tn_envvbl_t hole_c;
        int s_holefill = tn_env_vbl(&hole_c, "OPENPETE_TERRAIN_HOLEFILL", 1, 1);
        /* The fill runs for EVERY far-band sector with LP — including sectors
         * whose LP already draws in WORLD (LP-only 0x4000 / force-LP). For
         * those the fill is a screen-DILATED duplicate (see the feed): the PSX
         * integer scanline fill is INCLUSIVE where native's float raster +
         * centre-sampling drops sub-pixel rows, so the far water renders as
         * dashed slivers near screen edges under rotation (the carved-out
         * distant-water horizon gaps, now also user-reported through the
         * green cyclorama). The dilated re-emit grows coverage ~a pixel
         * outward but paints only stencil==0 holes — overdraw is impossible,
         * and the old coverage quantifier's "PSX horizon sits ~1.75px above
         * native's" top-edge deficit is recovered the same way (the sky never
         * marks, so growing above the drawn edge is allowed). */
        /* Under EXTENDED LOD (--lod extended / openpete_no_lod), skip the fill
         * for HP-bearing sectors: their HP never drops (no fog-band tier swap
         * to bridge), so any stencil-0 pixel there is genuine void/sky — and
         * the fill's fully-fogged per-face gate rides the RAW-RAM lodThr, so
         * beyond the real fog band it painted each sector's ENTIRE fogged LP
         * silhouette; where the coarse LP overhangs the HP edge that landed on
         * sky as fog-coloured "LP terrain" fringes popping in with distance
         * (found in testing 2026-07-09, haunted_towers top-down pose pair, yaw
         * -1046 pitch 992: z=61079 clean vs z=63909 fringed). LP-only /
         * force-LP sectors keep the fill in every mode — the far-water raster
         * slivers it exists for are pass-0 LP, not a tier handoff. */
        int fillLP = s_holefill && !(flags & 0x2000) && nLpF > 0
                     && ((double)lodThr < cbin + SC->radius + 0x100)
                     && !(openpete_no_lod && drawHP);
        /* NEAR-FLOOR fill backstop (cliff_town crest strip, found in testing
         * 2026-07-09): a fully-near HP sector's dropped LP FLOOR faces can
         * cover sightlines NOTHING else covers — ground strips past the HP
         * mesh edge whose cov reads 1.0 because the samples hug the HP cliff
         * face within TOL (a fundamentally NON-LOCAL class: no point-geometry
         * rule can see it; the PSX hides it because everything beyond the
         * crest is fog-saturated, native's extended draw distance exposes
         * it).  Feed those floors through the stencil-gated fill pass at
         * true LP depth — the stencil makes over-inclusion safe for every
         * covered pixel (overdraw impossible); floors-only keeps the near
         * arch/aperture walls out (the failure that gated the fill to far
         * sectors in the first place).
         * DEFAULT OFF (OPENPETE_TERRAIN_FILLNEAR=1 opts in): the backstop
         * cannot distinguish an interior hole (cliff_town strip: SHOULD be
         * terrain) from a coarse-LP silhouette overhang (haunted_towers
         * island rims, sunny_flight horizon: 1-2px terrain slivers painted
         * on SKY — the fringe class the fully-fogged gate exists to block).
         * Both are stencil-0 pixels beside drawn terrain; the separator is
         * screen-space morphology (interior vs boundary), which the fill
         * pass cannot see.  Opt in to fill crest strips at the cost of rim
         * slivers. */
        static int s_fillnear = -1;
        if (s_fillnear < 0) { const char* e = getenv("OPENPETE_TERRAIN_FILLNEAR"); s_fillnear = (e && *e) ? (*e != '0') : 0; }
        int fillNearLP = s_fillnear && s_holefill && !fillLP && drawHP && !drawLPfull
                         && !(flags & 0x2000) && nLpF > 0
                         && !gp_level_skipped(cpu);   /* alternate-LP levels: no backstop either */

        s_walk_sectors++;

        /* per-sector accounting for OPENPETE_TERRAIN_SECTORDUMP (spans both passes) */
        int sd_triBefore = s_tri_n;
        double sd_m3min = 1e18, sd_m3max = -1e18;
        float sd_bx0 = 1e18f, sd_by0 = 1e18f, sd_bx1 = -1e18f, sd_by1 = -1e18f;
        int sd_vfront = 0, sd_nv = 0, sd_nf = 0;

      for (int pass = 0; pass < 3; pass++) {
        int passFill = (pass == 2);               /* LP hole-fill backing (W2.6-c) */
        int passLP = (pass != 1);                 /* 0 = WORLD LP (behind HP), 1 = HP, 2 = fill LP */
        int fmDumpOnly = 0;                       /* facemap: project + dump, never emit */
        if (pass == 0 && !drawLP) {
            if (s_facemap_active && !(flags & 0x2000) && nLpF > 0) fmDumpOnly = 1;
            else continue;
        }
        if (pass == 1 && !drawHP) continue;
        if (passFill && !fillLP && !fillNearLP) continue;
        if (passFill && SC->polyanim) continue;   /* stale LP bank — never backstop-fill */

        uint32_t faces; int nv, nf, faceStride;
        const tsvert_t* cverts; const tsface_t* cfaces;
        const float* cvF = NULL;   /* sub-tick fractional verts (poly-anim sectors only) */
        uint32_t lpColBase = 0, nearBase = 0, farBase = 0;   /* §9/§10 colour arrays */
        if (passLP) {
            cverts = SC->lpV; cfaces = SC->lpF; nv = nLpV; nf = nLpF;
            faces = SC->lpFaces; faceStride = 8;
            lpColBase = SC->lpColBase;                     /* LP colours (4B, no fog) */
            if (SC->vfracPres == s_vanim_pres) cvF = SC->lpVf;
        } else {
            cverts = SC->hpV; cfaces = SC->hpF; nv = nHpV; nf = nHpF;
            faces = SC->hpFaces; faceStride = 16;
            nearBase = SC->nearBase;                       /* HP near colours (4B) */
            farBase  = SC->farBase;                        /* HP far  colours (4B) */
            if (SC->vfracPres == s_vanim_pres) cvF = SC->hpVf;
        }
        if (!cverts || !cfaces) continue;                  /* alloc failure: skip pass */
        /* Colour bases → host pointers (fallback: the cpu read path). */
        const uint8_t* h_lp   = passLP ? (const uint8_t*)openpete_psx_to_host_ptr(lpColBase) : NULL;
        const uint8_t* h_near = passLP ? NULL : (const uint8_t*)openpete_psx_to_host_ptr(nearBase);
        const uint8_t* h_far  = passLP ? NULL : (const uint8_t*)openpete_psx_to_host_ptr(farBase);
        int host_col = passLP ? (h_lp != NULL) : (h_near != NULL && h_far != NULL);
        s_emit_noHP = passLP;     /* tag emitted tris with this pass's tier */
        s_emit_fill = passFill;   /* fill pass -> WORLD_FILL category in the feed */
        sd_nv += nv; sd_nf += nf;

        for (int k = 0; k < nv; k++) {
            double Xf, Yf, Zf;
            if (cvF) { Xf = cvF[k*3]; Yf = cvF[k*3+1]; Zf = cvF[k*3+2]; }
            else     { Xf = cverts[k].x; Yf = cverts[k].y; Zf = cverts[k].z; }
            double m1, m2, m3;
            ngte_mac(M, s5 - Yf, s6 - Zf, Xf + s4, &m1, &m2, &m3);
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
            s_emit_sec = (int)i; s_emit_face = f;   /* fardbg attribution identity */
            s_emit_env = (F->tgen != 0);            /* env-anim member (rewrite set) */
            /* Portal OT-conformance key: the face's .s OT bucket base — the
             * raw 4-corner view-Z sum ((Σ4 SZ)>>7 in the walk; .s tris sum
             * the shared i2==i3 corner twice, and so does this), expressed
             * as the full-world corner MEAN. Ambient per face so every ttri
             * it emits (direct, near-clipped, or the 2x2 subdivide) carries
             * the PARENT face's key — the .s verdict is per-face, and a
             * split tri's own 3-vert mean can land on the other side of the
             * sky's bucket (the fix4 left-edge blue triangle). */
            s_emit_otkey = (float)((sv[i0].m3 + sv[i1].m3 + sv[i2].m3 + sv[i3].m3) * 4.0);
            /* .s HP-walk texture-fog select (generated C block_80027040-809C,
             * tri twin 0x80027460): per-face key from the corner SZs (SZ=4·m3;
             * the same key feeds the OT bucket), t0 = key − fogRef + 4096.
             *   t0 > 0 (in/beyond the fog band): tiledef half +0, CLUT halfword
             *     += (t0>>8)<<6 — cly += Δ, a LADDER of pre-baked progressively
             *     fogged palette rows. This is the missing half of the PSX fog
             *     model (the DPCS vertex blend is the other half); without it
             *     the fog band rendered with the base (unfogged) palette — the
             *     flat washed-out far mountains and the parked ~4% brightness.
             *   t0 ≤ 0 (near): the +8 tiledef half (bpp8 near tile), Δ = 0.
             * Under --no-lod the .s suppresses both (saturated fogRef at the
             * walk's LOD loads + the no_lod_tier_bump channel) as a side effect
             * of the shared lodDist reads; natively we keep CANONICAL fog by
             * true distance — no-lod means mesh detail, not fog removal.
             * OPENPETE_TERRAIN_CLUTFOG=0 kills (A/B); force-tier debug modes
             * (OPENPETE_TERRAIN_TEXTBL != 0) keep the old static half.
             * ABE carve-out RESOLVED (2026-07-06 waterfall arc): the old
             * "indirect animated-tiledef branch" reading of block_80027080
             * was wrong — the 0x3E there is the PRIM CODE byte (ABE GT4),
             * not a packet length, and the .s reads the SAME tiledef entry
             * (masked &0x7F) and applies the SAME ladder to negative-texByte
             * faces. Animated tiles are plain in-place entry rewrites
             * (func_8002A6FC), now kept live by tsc_refresh_anim; the old
             * "land palettes on water" artifact was ladder-on-STALE-entry. */
            tmat matSel;
            int dbg_half = 0, dbg_delta = 0;   /* fardbg: chosen tiledef half + ladder row */
            if (!passLP && s_clutfog && secInLOD && texTblMode == 0 && F->mat.textured) {
                /* SZ = s_fogscale·m3 — the whole ladder ramp rides the
                 * --fog-scale curve (s_fogscale = 4/k; exact .s at k=1).
                 * ALWAYS four corners: .s HP tris are quads with i2==i3 and
                 * the walk sums the shared vertex TWICE. Summing only 3
                 * under-shot Δ by ~25% on every fog-band triangle (user-
                 * caught: confetti on the title's left mountain — .s row
                 * 138, native row 131 = unauthored palette content). */
                int szsum = (int)(sv[i0].m3 * s_fogscale) + (int)(sv[i1].m3 * s_fogscale)
                          + (int)(sv[i2].m3 * s_fogscale) + (int)(sv[i3].m3 * s_fogscale);
                int key = (szsum >> 7) << 5;
                int t0k = key - fogRef + 4096;
                if (t0k <= 0) {
                    if (F->matN.textured) { matSel = F->matN; mat = &matSel; tu = F->tuN; tv = F->tvN; dbg_half = 1; }
                } else {
                    /* Clamp to the DEEPEST AUTHORED row: the ladder is exactly
                     * 16 rows (Δ 0..15). Within the fog band t0 < 4096 so the
                     * .s only ever produces Δ ≤ 15 (title frame-1210 material
                     * census: .s emits base+13/+15, NEVER base+16); faces whose
                     * quantized key lands at/past saturation (straddling faces
                     * the handoff keeps — only all-corner-beyond faces drop)
                     * would compute Δ ≥ 16 and sample the NEIGHBOURING tile's
                     * palette (found in testing round 2: foliage confetti on the
                     * title mountain — 37 faces at (832,0) row 848,144). */
                    int delta = t0k >> 8;
                    if (delta > 15) delta = 15;
                    dbg_delta = delta;
                    if (delta > 0) {
                        matSel = F->mat;
                        uint16_t hw = (uint16_t)((uint32_t)matSel.clut_raw + ((uint32_t)delta << 6));
                        matSel.clx = (int)(hw & 0x3F) * 16;
                        matSel.cly = (int)(hw >> 6) & 0x1FF;
                        matSel.fog_delta = delta;
                        mat = &matSel;
                    }
                }
            }
            /* NCLIP back-face sign (2D cross product == GTE MAC0; flip negates it,
             * word3 & 0x2). front = nclip > 0. The GTE saturates SX/SY to
             * [-1024, 1023] BEFORE NCLIP — a far-offscreen corner (glancing
             * screen-edge face) must be clamped the same way or the cross
             * product flips sign vs the game's verdict (ice_cavern left-edge
             * waterfall chunk / artisans corner grass, the HP-cull probe's
             * only residuals). */
            /* (Integer-truncating the corners first, mirroring MAC0>>16, was
             * probed and made the residuals WORSE — the game's UNR divide ≠
             * float division, so sub-pixel rounding chases noise. Keep floats;
             * only the saturation window is mirrored.) */
            #define GTE_SAT(v) ((v) < -1024.0f ? -1024.0 : ((v) > 1023.0f ? 1023.0 : (double)(v)))
            /* NCLIP facing, mirroring the .s HP walk exactly (block_80026C30..
             * 80026CD0; tri variant block_8002720C..80027220):
             *  - HALF 1: the walk RTPT-loads SXY = corners (i0, i1, i3) for a
             *    quad (the GT4 slot order TL,TR,BL — NOT (i0,i1,i2)) and
             *    (i0, i1, i2) for a tri. Accept when the flip-adjusted MAC0
             *    (word3 & 0x2 negates) is >= 0.
             *  - HALF 2 (quads only, when half 1 flunks): SXY0 is re-loaded
             *    with corner i2 → NCLIP over (i2, i1, i3). word3 & 0x4 jumps
             *    straight to the emit path (a draw-anyway/two-sided bit);
             *    otherwise FLIP selects the accepted sign — set → accept
             *    n2 >= 0 (block_80026CC0), clear → accept n2 <= 0
             *    (block_80026CA8; the triple's orientation is inverted vs
             *    half 1, hence the opposite base sign).
             * The face draws when either half accepts: a twisted quad near
             * edge-on is visible through one half only (the ice_cavern
             * waterfall sheet s101/f15), while a genuine back-to-back wall
             * pair (the magic_crafters room/waterfall) fails both. */
            /* GTE UNR divide-overflow regime: RTPS saturates the quotient when
             * SZ3 <= H/2 (SZ = 4·m3, so m3 <= H/8) — the .s's own SXY for such
             * a corner is pinned garbage, and the saturated cross sign is
             * chaotic noise (the toasty floor tri at m3~19: saturation
             * collapses a 1275px span to 217px and the sign flips with a
             * 6-yaw-unit pan while the true cross is a stable ~+380k). The .s
             * verdict is not mirrorable there — use the TRUE facing
             * (unsaturated float cross), which still culls genuine
             * back-to-back near wall pairs. */
            int unrOvf = 0;
            {   const int ci[4] = { i0, i1, i2, i3 };
                int nc2 = quad ? 4 : 3;
                for (int q2 = 0; q2 < nc2; q2++)
                    if (sv[ci[q2]].m3 < s_H / 8.0) { unrOvf = 1; break; }
            }
            #define NSAT(v) (unrOvf ? (double)(v) : GTE_SAT(v))
            double nx0 = NSAT(sv[i0].sx), ny0 = NSAT(sv[i0].sy);
            double nx1 = NSAT(sv[i1].sx), ny1 = NSAT(sv[i1].sy);
            int h1 = quad ? i3 : i2;
            double nxA = NSAT(sv[h1].sx), nyA = NSAT(sv[h1].sy);
            double nclip = (nx1 - nx0) * (nyA - ny0) - (nxA - nx0) * (ny1 - ny0);
            if (flip) nclip = -nclip;
            int front = (nclip >= 0.0);
            int front2 = 0;
            if (quad) {
                double nx2 = NSAT(sv[i2].sx), ny2 = NSAT(sv[i2].sy);
                double nx3 = NSAT(sv[i3].sx), ny3 = NSAT(sv[i3].sy);
                double nclip2 = (nx1 - nx2) * (ny3 - ny2) - (nx3 - nx2) * (ny1 - ny2);
                int dside = (faceStride == 16) && (F->w3 & 0x4) != 0;
                front2 = dside || (flip ? (nclip2 >= 0.0) : (nclip2 <= 0.0));
            }
            /* Far-band identity dump: full per-face record for the offline
             * whole-face join vs captured prims. Backfaces included (fr=0 —
             * the .s NCLIP-culls them but native emit-all draws them; the
             * attribution needs them); fill pass skipped (duplicate of pass 0). */
            if (s_fardbg_active && !passFill) {
                const int ci[4] = { i0, i1, i2, i3 };
                const int cc[4] = { c0, c1, c2, c3 };
                int nc = quad ? 4 : 3;
                fprintf(stderr, "[fardbg-face] s=%d p=%d f=%d q=%d fb=%d fr=%d fogRef=%d "
                        "abe=%d half=%d dlt=%d cl=%d,%d tp=%d,%d bpp=%d",
                        (int)i, pass, f, quad, F->fogbias, front, fogRef,
                        F->abe, dbg_half, dbg_delta, mat->clx, mat->cly,
                        mat->tpx, mat->tpy, mat->bpp);
                for (int q2 = 0; q2 < nc; q2++) {
                    const lvert* V = &sv[ci[q2]];
                    uint32_t cn = 0, cf2 = 0, cb2 = 0;
                    if (passLP) {
                        cb2 = rd(cpu, lpColBase + (uint32_t)cc[q2]) & 0xFFFFFFu;
                    } else {
                        cn  = rd(cpu, nearBase + (uint32_t)cc[q2]) & 0xFFFFFFu;
                        cf2 = rd(cpu, farBase  + (uint32_t)cc[q2]) & 0xFFFFFFu;
                        cb2 = terrain_fog_blend(cn, cf2, V->m3, fogRef);
                    }
                    fprintf(stderr, " c%d=%.2f,%.2f,%.1f,%06X,%06X,%06X",
                            q2, V->sx, V->sy, V->m3, cb2, cn, cf2);
                }
                fputc('\n', stderr);
            }
            /* facemap: one line per LP face with projected corners (backfaces
             * and behind-camera corners included — m3 ≤ 0 corners are marked
             * by their value; the offline side clips). secdraw distinguishes
             * "sector's LP pass ran normally" from the forced dump-only walk. */
            if (s_facemap_active && passLP && !passFill) {
                const int ci[4] = { i0, i1, i2, i3 };
                int nc = quad ? 4 : 3;
                fprintf(stderr, "[facemap] s=%u f=%d q=%d gf=%d flr=%d fb=%d fr=%d secdraw=%d",
                        (unsigned)i, f, quad, F->gapfill, F->floorf, F->fogbias, front, !fmDumpOnly);
                for (int q2 = 0; q2 < nc; q2++) {
                    const lvert* V = &sv[ci[q2]];
                    fprintf(stderr, " v%d=%.2f,%.2f,%.2f", q2, V->sx, V->sy, V->m3);
                }
                fputc('\n', stderr);
            }
            if (fmDumpOnly) continue;
            if (passLP && !passFill && lpGapOnly && (!F->gapfill || gp_gf_excluded(i, f))) { continue; } /* --no-lod: gap fillers only */
            if (lod_handoff) {
                int nc = quad ? 4 : 3, beyond = 0;
                const int ci[4] = { i0, i1, i2, i3 };
                for (int q2 = 0; q2 < nc; q2++)
                    if (fogRef - (int)(sv[ci[q2]].m3 * s_fogscale) <= 0) beyond++;
                if (!passLP && beyond == nc) { continue; }        /* .s: HP ends at LOD */
                if (passLP && !passFill && lpFarOnly && beyond == 0) {
                    /* LP far-only — EXCEPT classified HP-coverage-gap fillers
                     * (tsc_classify_gapfill), which draw at all distances so a
                     * sparse near HP mesh doesn't open a hole to the cyclorama
                     * (the near-LP-gap fix; at the fog line the game itself
                     * draws these faces the moment one corner crosses, so this
                     * only stabilizes that verdict inward).
                     * OPENPETE_TERRAIN_LPKEEP=1 keeps ALL such faces (probe);
                     * LPKEEPDBG=<frame> logs each kept face — sector/face id,
                     * world verts, screen bbox — to identify hole culprits. */
                    static int s_lpkeep = -2, s_lpkeepdbg;
                    if (s_lpkeep == -2) {
                        const char* e = getenv("OPENPETE_TERRAIN_LPKEEP");
                        s_lpkeep = (e && *e && *e != '0');
                        e = getenv("OPENPETE_TERRAIN_LPKEEPDBG");   /* = frame to log at */
                        s_lpkeepdbg = (e && *e) ? atoi(e) : 0;
                    }
                    int gfKeep = s_gapfill_en && F->gapfill && !gp_gf_excluded(i, f);
                    if (!s_lpkeep && !gfKeep) continue;
                    if (s_lpkeepdbg && openpete_frame >= (unsigned)s_lpkeepdbg
                        && openpete_frame < (unsigned)s_lpkeepdbg + 4u) {
                        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                        const int ci2[4] = { i0, i1, i2, i3 };
                        fprintf(stderr, "[lpkeep] s=%u f=%d q=%d gf=%d", i, f, quad, F->gapfill);
                        for (int q2 = 0; q2 < nc; q2++) {
                            const tsvert_t* V = &cverts[ci2[q2]];
                            const lvert* P = &sv[ci2[q2]];
                            if (P->sx < bx0) bx0 = P->sx; if (P->sx > bx1) bx1 = P->sx;
                            if (P->sy < by0) by0 = P->sy; if (P->sy > by1) by1 = P->sy;
                            fprintf(stderr, " v%d=%d,%d,%d", q2,
                                    originX + V->x, originY + V->y, originZ + V->z);
                        }
                        fprintf(stderr, " bbox=%.0f,%.0f..%.0f,%.0f\n", bx0, by0, bx1, by1);
                    }
                }
            }
            /* HOLE-FILL per-face far gate (W2.6-c): only FULLY-FOGGED faces
             * (every corner at/past the exact fog saturation, ir0 = fogRef−4·m3
             * ≤ 0 — the proven W2.3 decode scale, independent of the shipped
             * s_fogscale). The T-junction seams the fill exists for are at the
             * fully-fogged horizon; a mid-band face admitted here painted a
             * dark 1-2px LP fringe along terrain-vs-sky silhouettes (native's
             * float raster is a hair tighter than the PSX integer edge — gate
             * tick 4888, peace-keepers cliffs). */
            if (passFill && fillNearLP) {
                /* Near-floor backstop: floors only (walls/aperture arches
                 * stay out); stencil-gated to pixels nothing else covered. */
                if (!F->floorf) continue;
            } else if (passFill) {
                /* Slack 1024 (face admitted when every corner is ≥75% fogged):
                 * a strict ir0≤0 dropped the LP of sectors STRADDLING the LOD
                 * boundary, leaving their far rows' raster gaps unfilled (the
                 * f2730 right-edge green band); 1024 admits those while still
                 * excluding the mid-band faces that fringed sky silhouettes
                 * (gate tick 4888). */
                static int s_fogslack = -1;
                if (s_fogslack < 0) { const char* e = getenv("OPENPETE_TERRAIN_FILL_FOGSLACK"); s_fogslack = (e && *e) ? atoi(e) : 0;   /* v4.1: back to v1 STRICT (fully-fogged only) — see the dilation block */ }
                int nc2 = quad ? 4 : 3, farAll = 1, fogged = 1;
                const int cf[4] = { i0, i1, i2, i3 };
                for (int q2 = 0; q2 < nc2 && farAll; q2++) {
                    int ir0 = fogRef - (int)(sv[cf[q2]].m3 * 4.0);
                    if (ir0 > 0) fogged = 0;
                    if (ir0 > s_fogslack) farAll = 0;
                }
                if (!farAll) continue;
                /* Slack-admitted (NOT fully fogged) faces pass ONLY when screen-
                 * THIN (bbox height ≤ 3px): the class the slack exists for is
                 * the 1px straddling water rows (f2680/f2730 dashes). An
                 * unrestricted slack admitted mid-band LP BODIES whose coarse
                 * silhouette ghosts into stencil-0 sky above terrain tops and
                 * depth-notches neighbouring geometry (user report 2026-07-03:
                 * ghosting/spikes at terrain tops, gaps-2 f2770 rocks). */
                if (!fogged) {
                    static float s_thin = -1.0f;
                    if (s_thin < 0.0f) { const char* e2 = getenv("OPENPETE_TERRAIN_FILL_THINPX"); s_thin = (e2 && *e2) ? (float)atof(e2) : 16.0f; }
                    float ty0 = 1e9f, ty1 = -1e9f; int thinOk = 1;
                    for (int q2 = 0; q2 < nc2; q2++) {
                        const lvert* V = &sv[cf[q2]];
                        if (V->m3 <= 0.0) { thinOk = 0; break; }  /* behind-camera: not the sliver class */
                        if (V->sy < ty0) ty0 = V->sy;
                        if (V->sy > ty1) ty1 = V->sy;
                    }
                    if (!thinOk || ty1 - ty0 > s_thin) continue;
                }
            }
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
            /* Portal-guard tag (PSX mask-bit mirror): the .s DEFERS this face
             * (DrawPrim-drawn, mask-protected from the OT walk incl. the portal
             * sky) — every tri it emits below (near-subdivide OR the regular
             * fall-through) gets re-fed as OPENPETE_SCENE_PORTAL_GUARD when
             * portals are active (terrain_feed_native). Tag from the QUEUE
             * ORACLE (the .s's own defer decision), NOT face_is_near — the
             * classifier over-covers the real queue by ~20x (see the defer
             * calibration note) and would wrongly protect the through-hole
             * floor the preview must cover. */
            s_emit_guard = (faceStride == 16 && s_defer_n > 0 &&
                            is_deferred(faces + (uint32_t)f * 16u));
            /* `front` is NCLIP on the PROJECTED corners — meaningless when a
             * corner sits behind the camera (proj_screen mirrors it), so a
             * straddling foreground cell flunked it ~half the time and fell to
             * the LQ tier (the user-visible "closest ground/wall goes low-res"
             * flicker). Waive the facing test for straddling cells; the
             * near-emit path near-plane-clips (clip_push_tri) and the Z-buffer
             * owns occlusion, so a waived back-face is harmless. */
            int straddle = (sv[i0].m3 <= 0.0 || sv[i1].m3 <= 0.0 ||
                            sv[i2].m3 <= 0.0 || sv[i3].m3 <= 0.0);
            int near = (faceStride == 16 && s_nearhq && hqTable && (front || straddle) && quad &&
                        face_is_near(sv[i0].m3, sv[i1].m3, sv[i2].m3, sv[i3].m3, hqW3));
            {   /* OPENPETE_TERRAIN_NEARDBG: census of STRADDLING foreground quads
                 * (a corner behind the camera) by route — near/HQ vs LQ fall-through. */
                static int s_ndbg = -1; static long s_nd_near, s_nd_excl, s_nd_lq; static int s_nd_n;
                if (s_ndbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_NEARDBG"); s_ndbg = (e && *e && *e != '0'); }
                int vis_straddle = straddle && (sv[i0].m3 > 0.0 || sv[i1].m3 > 0.0 ||
                                                sv[i2].m3 > 0.0 || sv[i3].m3 > 0.0);
                if (s_ndbg && faceStride == 16 && quad && vis_straddle) {
                    static long s_nd_q, s_nd_qtot;   /* .s-queue oracle agreement (needs VERIFY) */
                    if (near) s_nd_near++;
                    else if (hqW3 & 0x80) s_nd_excl++;   /* hard-excluded from HQ (no Tiledef) */
                    else s_nd_lq++;
                    if (s_defer_n > 0) { s_nd_qtot++;
                        if (is_deferred(faces + (uint32_t)f * 16u)) s_nd_q++; }
                    if (++s_nd_n >= 200) {
                        fprintf(stderr, "[neardbg] visible straddling quads: near=%ld excl80=%ld lq=%ld  queue: %ld/%ld deferred\n",
                                s_nd_near, s_nd_excl, s_nd_lq, s_nd_q, s_nd_qtot);
                        s_nd_n = 0;
                    }
                }
            }
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
                terrain_near_emit(cpu, hqTable + (uint32_t)F->hq_idx * 168u,
                                  F->hq_idx, abe, TL, TR, BL, BR);
                s_dbg_spike++;   /* near/HQ cells subdivided */
                continue;
            }
            /* NCLIP back-face cull, mirroring the .s walk. A Z-buffer hides
             * *occluded* back-faces, but NOT back-faces that sit nearer than
             * the front geometry: back-to-back wall pairs (a room interior
             * and the cliff/waterfall outside it) interpenetrate within a few
             * world units, so emit-all let the far side's back-face win depth
             * and punch through (magic_crafters waterfall room). The game
             * never draws those faces at all — NCLIP culls them — so cull is
             * the canonical visibility rule, not a perf knob.
             * Two carve-outs where the .s verdict can't be mirrored:
             *  - straddling faces (a corner behind the camera): projected
             *    corners are mirrored, the cross-product sign is garbage —
             *    waive (Z-buffer owns those; same waiver as the near/HQ gate).
             *  - fill passes: native-only hole cover with no .s counterpart,
             *    stencil-gated to pixels nothing else drew — stay emit-all.
             * LP quads are culled per-tri below (the .s LP loop NCLIPs
             * tri(i0,i1,i2) then tri(i3,i1,i2) — a saddle quad can be half
             * front-facing); everything else uses the whole-face verdict. */
            /* OPENPETE_TERRAIN_CULL is a bitmask while the per-tier NCLIP
             * convention is being validated: bit0 = cull HP faces, bit1 =
             * cull LP faces, bit2 = invert the LP front sign. */
            int cullable = ((faceStride == 16) ? (s_cull & 1) : (s_cull & 2))
                           && !straddle && !passFill;
            int cfront = ((faceStride == 8 && (s_cull & 4)) ? (nclip < 0.0) : front)
                         || front2;
            {   /* OPENPETE_TERRAIN_CULLDBG="frame,x0,y0,x1,y1" (PSX screen rect):
                 * log every back-verdict face whose bbox intersects the rect. */
                static int s_cdbg = -2; static long s_cd[5];
                if (s_cdbg == -2) {
                    const char* e = getenv("OPENPETE_TERRAIN_CULLDBG");
                    s_cdbg = (e && sscanf(e, "%ld,%ld,%ld,%ld,%ld",
                              &s_cd[0], &s_cd[1], &s_cd[2], &s_cd[3], &s_cd[4]) == 5);
                }
                if (s_cdbg > 0 && !cfront && openpete_frame >= (unsigned)s_cd[0]
                    && openpete_frame < (unsigned)s_cd[0] + 2u) {
                    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                    const int ci[4] = { i0, i1, i2, i3 };
                    int nc = quad ? 4 : 3;
                    for (int q2 = 0; q2 < nc; q2++) {
                        const lvert* V = &sv[ci[q2]];
                        if (V->sx < bx0) bx0 = V->sx; if (V->sx > bx1) bx1 = V->sx;
                        if (V->sy < by0) by0 = V->sy; if (V->sy > by1) by1 = V->sy;
                    }
                    if (bx1 >= s_cd[1] && bx0 <= s_cd[3] && by1 >= s_cd[2] && by0 <= s_cd[4]) {
                        fprintf(stderr, "[culldbg] s=%u f=%d hp=%d q=%d flip=%d abe=%d w3=%08X "
                                "nclip=%.2f straddle=%d pass=%d bbox=%.0f,%.0f,%.0f,%.0f m3=%.0f,%.0f,%.0f,%.0f\n",
                                i, f, faceStride == 16, quad, flip, F->abe, F->w3,
                                nclip, straddle, pass, bx0, by0, bx1, by1,
                                sv[i0].m3, sv[i1].m3, sv[i2].m3, sv[i3].m3);
                    }
                }
            }
            if (cullable && !cfront) { s_dbg_nclip++; continue; }
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
            /* Authored-data corrections (unpainted vertex colours) — see
             * k_lp_artfix. Applies to LP world faces and their fill twins. */
            if (passLP) {
                uint32_t fixrgb;
                if (lp_artfix_lookup(cpu, (int)i, f, &fixrgb))
                    rc0 = rc1 = rc2 = rc3 = fixrgb;
            }
            /* Sub-tick UV scroll (env texture anim): translate every corner V by
             * the tile's fractional offset. One block covers GT4, GT3 (the
             * rotation below only permutes which corner feeds which slot, so a
             * uniform translation is permutation-invariant), both LOD halves
             * (the game writes the same scroll byte into all four V slots of the
             * 16-byte entry, so tv and tvN share one offset), and near-plane
             * clipping (cv_lerp interpolates UV linearly). */
            float tvScroll[4];
            if (s_tanim_have && faceStride == 16 && F->tb != -1 && s_tanim_act[F->tb & 0x7F]) {
                float dv = s_tanim_dvlq[F->tb & 0x7F];
                for (int k2 = 0; k2 < 4; k2++) tvScroll[k2] = tv[k2] + dv;
                tv = tvScroll;
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
                 * terrain polys + brown artifacts.) Untextured, so corner UVs unused.
                 * Face visibility (either NCLIP half) was decided above; the GPU
                 * draws the whole G4, so both halves emit together. */
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
        {   /* OPENPETE_TERRAIN_BINDBG: calibrate the binning-centre projection
             * against the sector's ACTUAL projected vertex-Z range (one frame). */
            static int s_bindbg = -1; static unsigned s_bin_f; static int s_bin_lines;
            if (s_bindbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_BINDBG"); s_bindbg = (e && *e && *e != '0'); }
            (void)s_bin_f;
            if (s_bindbg && nvis >= 20 && s_bin_lines < 200) {
                s_bin_lines++;
                fprintf(stderr, "[bindbg] f=%u sec=%u flags=%04X cbin=%.0f r=%d m3=[%.0f..%.0f] lodThr=%d hp=%d lp=%d fill=%d\n",
                        openpete_frame, i, flags, cbin, SC->radius, sd_m3min, sd_m3max, lodThr, drawHP, drawLP, fillLP);
            }
        }
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
    if (tt_on) {   /* close the walk-cost census: last sector + window report */
        uint64_t tnow = openpete_phase_now_us();
        if (tt_prev_valid && tt_prev_i < 1024u) {
            uint32_t d = (uint32_t)(tnow - tt_prev);
            tt_us[tt_prev_i] += d; tt_present_us += d;
            tt_tris[tt_prev_i] += (uint32_t)(s_tri_n - tt_prev_tris);
        }
        tt_presents++; tt_nvis_sum += (uint32_t)nvis;
        if ((uint32_t)nvis > tt_nvis_max) tt_nvis_max = (uint32_t)nvis;
        tt_win_us += tt_present_us;
        if ((uint32_t)tt_present_us > tt_pres_max_us) tt_pres_max_us = (uint32_t)tt_present_us;
        if (tt_report_t == 0) tt_report_t = tnow;
        if (tnow - tt_report_t >= 1000000u && tt_presents > 0) {
            char line[512]; int off = 0;
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            "[terrain-tt f=%u] presents=%u nvis avg=%u max=%u walk avg=%uus max=%uus tsc hit=%ld miss=%ld | top",
                            openpete_frame, tt_presents, tt_nvis_sum / tt_presents, tt_nvis_max,
                            (uint32_t)(tt_win_us / tt_presents), tt_pres_max_us,
                            s_tsc_hit, s_tsc_miss);
            s_tsc_hit = 0; s_tsc_miss = 0;
            for (int k = 0; k < 6; k++) {           /* top-6 sectors by window µs */
                uint32_t best = 0, bi = 0;
                for (uint32_t j = 0; j < 1024u; j++)
                    if (tt_us[j] > best) { best = tt_us[j]; bi = j; }
                if (!best) break;
                off += snprintf(line + off, sizeof(line) - (size_t)off,
                                " s%u=%uus/%utri", bi, best / tt_presents, tt_tris[bi] / tt_presents);
                tt_us[bi] = 0;
            }
            fprintf(stderr, "%s  (per-present avgs over window)\n", line);
            memset(tt_us, 0, sizeof(tt_us)); memset(tt_tris, 0, sizeof(tt_tris));
            tt_win_us = 0; tt_presents = 0; tt_nvis_sum = 0; tt_nvis_max = 0;
            tt_pres_max_us = 0; tt_report_t = tnow;
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
        /* OPENPETE_TERRAIN_PIXBOX="x0,y0,x1,y1" (PSX screen px): dump every
         * captured .s prim and every native tri whose bbox intersects the box —
         * prim-level forensics for a coverage gap the grid localizes. */
        const char* pb = getenv("OPENPETE_TERRAIN_PIXBOX");
        if (pb && *pb) {
            float bx0, by0, bx1, by1;
            if (sscanf(pb, "%f,%f,%f,%f", &bx0, &by0, &bx1, &by1) == 4) {
                for (int p = 0; p < s_gte_n; p++) {
                    prim_t* g = &s_gte[p];
                    float mnx=1e9f,mny=1e9f,mxx=-1e9f,mxy=-1e9f;
                    for (int k = 0; k < g->nv; k++) {
                        if (g->x[k]<mnx)mnx=g->x[k]; if (g->x[k]>mxx)mxx=g->x[k];
                        if (g->y[k]<mny)mny=g->y[k]; if (g->y[k]>mxy)mxy=g->y[k];
                    }
                    if (mxx < bx0 || mnx > bx1 || mxy < by0 || mny > by1) continue;
                    fprintf(stderr, "  [pixbox-psx] code=%02X nv=%d tex=%d xy=", g->code, g->nv, g->textured);
                    for (int k = 0; k < g->nv; k++) fprintf(stderr, "(%.0f,%.0f)", g->x[k], g->y[k]);
                    fprintf(stderr, " rgb0=%06X\n", g->rgb[0]);
                }
                for (int t = 0; t < s_tri_n; t++) {
                    ttri* T = &s_tri[t];
                    float mnx=1e9f,mny=1e9f,mxx=-1e9f,mxy=-1e9f;
                    for (int k = 0; k < 3; k++) {
                        if (T->sx[k]<mnx)mnx=T->sx[k]; if (T->sx[k]>mxx)mxx=T->sx[k];
                        if (T->sy[k]<mny)mny=T->sy[k]; if (T->sy[k]>mxy)mxy=T->sy[k];
                    }
                    if (mxx < bx0 || mnx > bx1 || mxy < by0 || mny > by1) continue;
                    fprintf(stderr, "  [pixbox-nat] s=%d f=%d tier=%d fill=%d tex=%d blend=%d "
                                    "xy=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) vz=%.0f\n",
                            T->sec, T->face, T->tier, T->fill, T->textured, T->blend,
                            T->sx[0],T->sy[0],T->sx[1],T->sy[1],T->sx[2],T->sy[2], T->vz[0]);
                }
            }
        }
    }

    /* Far-band identity dump, captured side: every .s prim (same canonical
     * frame — capture_prims ran in the same hook invocation) + the prim-code
     * histogram. Tier attribution is the prim code itself: 0x30/0x38 = LP
     * (untextured gouraud), 0x34/0x3C = HP (textured gouraud). */
    if (s_fardbg_active) {
        fprintf(stderr, "[fardbg-hdr] frame=%u prims=%d sampled=%d fogRefBase=%d\n",
                openpete_frame, s_prim_total, s_gte_n, fogRefBase);
        for (int c = 0; c < 256; c++)
            if (s_code_hist[c])
                fprintf(stderr, "[fardbg-hist] code=%02X n=%d\n", c, s_code_hist[c]);
        for (int p = 0; p < s_gte_n; p++) {
            prim_t* g = &s_gte[p];
            fprintf(stderr, "[fardbg-prim] code=%02X n=%d", g->code, g->nv);
            for (int k = 0; k < g->nv; k++)
                fprintf(stderr, " c%d=%.0f,%.0f,%06X", k, g->x[k], g->y[k], g->rgb[k]);
            fputc('\n', stderr);
        }
        /* The post-clip triangle list = exactly what native rasterizes (the
         * offline face-cover replica got the quad ring order wrong; this is
         * ground truth for native-winner attribution). */
        for (int t2 = 0; t2 < s_tri_n; t2++) {
            ttri* T = &s_tri[t2];
            fprintf(stderr, "[fardbg-tri] s=%d f=%d tier=%d fill=%d tex=%d "
                    "v0=%.2f,%.2f,%.1f,%06X v1=%.2f,%.2f,%.1f,%06X v2=%.2f,%.2f,%.1f,%06X\n",
                    T->sec, T->face, T->tier, T->fill, T->textured,
                    T->sx[0], T->sy[0], T->vz[0], T->rgb[0],
                    T->sx[1], T->sy[1], T->vz[1], T->rgb[1],
                    T->sx[2], T->sy[2], T->vz[2], T->rgb[2]);
        }
        s_fardbg_done = 1;
        s_fardbg_active = 0;
    }
    if (s_facemap_active) {
        /* Header last so its presence marks a complete dump. Screen space is
         * the walk's 512-wide/240-line frame (8-line overscan crop):
         * screenshot px = (1.25*sx, 2*(sy-8)) at 640x448. */
        fprintf(stderr, "[facemap-hdr] frame=%u ofx=%.2f ofy=%.2f H=%.0f nolod=%d fogRefBase=%d\n",
                openpete_frame, s_ofx, s_ofy, s_H, openpete_no_lod, fogRefBase);
        s_facemap_done = 1;
        s_facemap_active = 0;
    }
}

/* Feed the built tris to the native scene. Textured faces sample the PSX VRAM
 * mirror via the Tiledef material (modulated by the per-vertex DPCS-fogged
 * colour); untextured faces are gouraud. Colour = native RGB888 + DPCS fog. */
static int s_notex = -1;   /* OPENPETE_TERRAIN_NOTEX: force untextured (debug) */
static int s_tiercolor = -1;  /* OPENPETE_TERRAIN_TIERCOLOR: LP=red, HP=blue */
static int s_tieronly = -2;   /* OPENPETE_TERRAIN_TIERONLY: 1=LP only, 2=HP only */

/* ---- portal OT-conformance clamp (the PSX per-face painter verdict) --------
 * The .s draws the portal sky at ONE OT bucket; a terrain face whose OT key
 * (mean view-Z) sorts BEHIND it is drawn BEFORE the sky in the walk, so the
 * sky covers ALL of it — including any part that pokes through the opening
 * plane toward the camera. The threshold/walkway at a portal base is exactly
 * that: one face spanning from just in front of the plane deep into the
 * throat behind it. Native's per-PIXEL depth let its near lip beat the sky —
 * a gray wedge ballooning out of the sill, breaking the sky↔ground connection
 * (portal-ground-disocclusion f2440; worse the more acute the camera angle).
 *
 * Mirror the verdict per face with the .s's OWN comparison: the sky inserts
 * at bucket B = (mean outline SZ) >> 7 (mirrored + validated in
 * portal_native, exported as ot_vz = 128·B); a terrain face's key is its
 * corner-mean view-Z >> 7. Key behind B ⇒ the face draws BEFORE the sky ⇒
 * the sky covers it whole-face. Any such face is sunk — max()'d, never
 * pulled forward — behind the sky's behind-ring spill depth, so it loses the
 * depth test (mask AND colours) in BOTH split regions (per-pixel plane
 * inside the outline, behind-ring on the rect spill). Faces with a nearer
 * key (the floor in front, the arch stone) are untouched and keep beating
 * the sky — the other half of the PSX verdict.
 *
 * Why not a geometric plane-straddle rule: the threshold pad at a portal
 * base can be DEPTH-COINCIDENT with the opening plane at every vertex
 * (f2440 probe: vz within 2 units of the plane field at all 3 verts) while
 * its .s key (26) sorts strictly behind the sky's (21) — B comes from the
 * OUTLINE MEAN, which at a downward/angled view sits several hundred units
 * NEARER than the plane's depth at the base. Only the bucket comparison
 * reproduces that. A half-bucket (+64) margin keeps sub-bucket jitter from
 * flapping the verdict; the tri's 3-vert mean stands in for the .s quad's
 * 4-corner sum (the two halves of a boundary quad can split the verdict —
 * accepted: the contest band is half a bucket wide, where PSX itself
 * flickers).
 *
 * Actors are deliberately NOT conformed: their native depth is true
 * per-pixel and the sky's plane depth preserves their layering exactly (a
 * bucket-depth sky would swallow a fairy hovering at the portal base).
 *
 * Depth-buffer side effects are bounded: anything genuinely nearer (Spyro's
 * feet on the sill, actors, the step's front face) still wins vs the sunken
 * face; the geometry that newly loses to it sits inside the portal throat —
 * behind the opening, where the sky covers everything anyway. */
/* full-world; 2x the sky's PORTAL_SKY_EPS_VZ margin, so a straddling terrain face
 * always loses to the sky and a coincident band always wins to it. Derived from
 * OPENPETE_PORTAL_SKY_EPS (default 24) so re-tuning the sky margin moves this in
 * lockstep; 0 disables both. */
static double portal_clamp_eps_vz(void) {
    static double v = -1.0;
    if (v < 0.0) {
        const char* e = getenv("OPENPETE_PORTAL_SKY_EPS");
        double s = (e && *e) ? atof(e) : 24.0;
        if (s < 0.0) s = 0.0;
        v = 2.0 * s;
    }
    return v;
}
static int s_pconform = -1;        /* OPENPETE_TERRAIN_PORTAL_CONFORM=0 disables (A/B) */
static openpete_portal_skyplane s_pplane[6];
static int s_pplane_n = 0;

/* Conform one built tri against the active portal skies. Returns 1 and fills
 * ovz[3] (world/16 scale, the ttri unit) when the face is sky-covered. */
static int s_pconform_dbg = -1;   /* OPENPETE_TERRAIN_PORTAL_CONFORM_DBG=x,y probe pixel */
static float s_pcdbg_x, s_pcdbg_y;
/* Point-in-polygon (crossing test) against a skyplane's projected outline. */
static int skyplane_contains(const openpete_portal_skyplane* pp, float x, float y) {
    int in = 0;
    for (int i = 0, j = pp->pn - 1; i < pp->pn; j = i++) {
        if ((pp->py[i] > y) != (pp->py[j] > y) &&
            x < (pp->px[j] - pp->px[i]) * (y - pp->py[i]) / (pp->py[j] - pp->py[i]) + pp->px[i])
            in = !in;
    }
    return in;
}
/* ---- portal inner-rim detection -------------------------------------------
 * Some arches carry a thin band of terrain lying IN the portal's opening plane.
 * It is coincident with the destination-sky preview, so no depth ordering can
 * separate them — the band must be excluded from the contest rather than
 * out-depthed. (Not every arch has one, so finding zero faces is a valid
 * result, not a failure: tree_tops and gnorc_cove have none from either side,
 * and dream_weavers authors its rims visible from one side only.)
 *
 * Coplanarity ALONE is not the predicate: a portal is a hole in a wall, and
 * that wall is coplanar with the opening by construction — beast_makers has
 * wall faces 20 units off the plane and thousands of units across. What
 * separates them is WHERE they sit, not how big they are: the rim intrudes
 * INTO the opening, the wall surrounds it. So the second term is containment
 * in the opening POLYGON — the cyclorama's own footprint — and not its
 * bounding box, which necessarily swallows the surrounding wall.
 *
 * Every term is measured in the opening plane or perpendicular to it, never on
 * screen. View space is a rigid transform of world space, so those are the
 * world-space answers and do not move as the camera does. A view-Z offset does
 * move: an earlier version classified the same face as rim head-on and as
 * frame at an angle. Screen-space size is unusable for the same reason — the
 * same faces measure 3.6 px and 29.8 px from two cameras. */
static double portal_rim_eps(void) {     /* max |perp distance|, world units */
    static double v = -1.0;
    if (v < 0.0) { const char* e = getenv("OPENPETE_PORTAL_RIM_EPS");
                   v = (e && *e) ? atof(e) : 384.0; if (v < 0.0) v = 0.0; }
    return v;
}
/* Max angle between a rim face and the opening plane, in degrees; returned as
 * the cosine so the test is a plain dot-product compare. 60 recovers a
 * stone_hill band but false-positives beast_makers levelId=43, which has no
 * rim at all; 45 helps nothing. */
static double portal_rim_cos(void) {
    static double v = -1.0;
    if (v < 0.0) {
        const char* e = getenv("OPENPETE_PORTAL_RIM_ANGLE");
        double deg = (e && *e) ? atof(e) : 30.0;
        if (deg < 0.0) deg = 0.0;
        if (deg > 90.0) deg = 90.0;
        v = cos(deg * 3.14159265358979323846 / 180.0);
    }
    return v;
}
/* How far OUTSIDE the opening polygon a face may still reach and count as rim,
 * in world units. Portals whose polygon is authored inset from the visible
 * stone carry their rim outside it entirely — at a pointed top the polygon's
 * straight edges cut inside the real peak, so those faces have no vertex
 * within the outline and no tolerance on distance or angle can recover them.
 * The rim is a band ADJACENT to the opening boundary; which side it falls on
 * is an authoring detail.
 *
 * DEFAULT 128, measured IN THE PLANE. A screen-space form of this was tried
 * first and falsified — it put ~7k px of false positive on beast_makers
 * levelId=43 at every value from 32 up, because "just outside the opening"
 * measured on screen depends on the camera. Re-measured in the plane the same
 * term is clean: bm_43 stays at zero for 64/128/256 while bm_44 goes 1432 ->
 * 6706. 256 is too far (peace_keepers_23 inflates 5106 -> 9201).
 *
 * NOTE this also absorbs up to 128 units of authoring drift, so it masks
 * openings that are simply mis-placed. Lower it once per-portal corrections
 * are baked in, and re-verify every arch. */
static double portal_rim_reach(void) {
    static double v = -1.0;
    if (v < 0.0) { const char* e = getenv("OPENPETE_PORTAL_RIM_REACH");
                   v = (e && *e) ? atof(e) : 128.0; if (v < 0.0) v = 0.0; }
    return v;
}
/* Classify a tri against the active portals. Returns -1 when the face is not a
 * candidate at all (not over an opening, or nowhere near its plane); otherwise
 * a BITMASK of the terms it fails, so 0 means accepted as inner rim:
 *   bit0 RIMFAIL_CONTAIN  does not intrude into the opening polygon
 *   bit1 RIMFAIL_DIST     a vertex further than eps from the opening plane
 *   bit2 RIMFAIL_ANGLE    face not within the angle tolerance of parallel
 * Reporting the exact combination (rather than a yes/no) is what lets a miss be
 * pointed at directly instead of inferred from a pixel count. */
#define RIMFAIL_CONTAIN 1
#define RIMFAIL_DIST    2
#define RIMFAIL_ANGLE   4
#define RIM_CAND_WINDOW 400.0   /* world units either side of the plane */
#define RIM_CAND_MARGIN 600.0   /* world units outside the opening, in-plane */

/* Containment measured IN THE OPENING PLANE rather than on screen.
 *
 * Screen containment tests the face's projection against the polygon's
 * projection, and those two carry different perspective shifts whenever the
 * face is offset from the plane — so a rim band 200 units behind the opening
 * lands inside the projected outline from one angle and outside it from
 * another. Observed directly: stone_hill's band alternated between
 * contain-fail and not-a-candidate as the camera moved, with distance and
 * angle both passing throughout.
 *
 * Instead: build a 2D basis on the plane from the view-space outline, drop
 * both the polygon and the face onto it along the normal, and test there. The
 * verdict then depends only on the geometry. */
typedef struct { double u, v; } rim_uv;
static int rim_plane_basis(const openpete_portal_skyplane* pp,
                           const double n[3], double O[3], double U[3], double W[3],
                           rim_uv* poly, int maxn) {
    if (pp->vn < 3 || pp->vn > maxn) return 0;
    O[0] = pp->vx[0]; O[1] = pp->vy[0]; O[2] = pp->vz[0];
    double e[3] = { pp->vx[1]-O[0], pp->vy[1]-O[1], pp->vz[1]-O[2] };
    const double d = e[0]*n[0] + e[1]*n[1] + e[2]*n[2];
    e[0] -= d*n[0]; e[1] -= d*n[1]; e[2] -= d*n[2];   /* into the plane */
    const double el = sqrt(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]);
    if (el < 1e-9) return 0;
    U[0] = e[0]/el; U[1] = e[1]/el; U[2] = e[2]/el;
    W[0] = n[1]*U[2] - n[2]*U[1];
    W[1] = n[2]*U[0] - n[0]*U[2];
    W[2] = n[0]*U[1] - n[1]*U[0];
    for (int i = 0; i < pp->vn; i++) {
        const double r[3] = { pp->vx[i]-O[0], pp->vy[i]-O[1], pp->vz[i]-O[2] };
        poly[i].u = r[0]*U[0] + r[1]*U[1] + r[2]*U[2];
        poly[i].v = r[0]*W[0] + r[1]*W[1] + r[2]*W[2];
    }
    return 1;
}
static double rim_uv_edge_dist(const rim_uv* poly, int pn, double u, double v) {
    double best = 1e30;
    for (int i = 0, j = pn - 1; i < pn; j = i++) {
        const double ax = poly[j].u, ay = poly[j].v;
        const double ex = poly[i].u - ax, ey = poly[i].v - ay;
        const double L2 = ex*ex + ey*ey;
        double t = (L2 > 1e-9) ? ((u-ax)*ex + (v-ay)*ey) / L2 : 0.0;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        const double dx = u - (ax + t*ex), dy = v - (ay + t*ey);
        const double d = sqrt(dx*dx + dy*dy);
        if (d < best) best = d;
    }
    return best;
}
static int rim_uv_contains(const rim_uv* poly, int pn, double u, double v) {
    int in = 0;
    for (int i = 0, j = pn - 1; i < pn; j = i++) {
        if ((poly[i].v > v) != (poly[j].v > v) &&
            u < (poly[j].u - poly[i].u) * (v - poly[i].v) / (poly[j].v - poly[i].v) + poly[i].u)
            in = !in;
    }
    return in;
}
static int portal_rim_classify(CPUState* cpu, const ttri* t) {
    if (!s_pplane_n) return -1;
    const double ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    const double ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    const double H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
    if (H < 1.0) return -1;
    const double eps = portal_rim_eps(), reach = portal_rim_reach();
    int best = -1;
    for (int pi = 0; pi < s_pplane_n; pi++) {
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        if (pp->vn < 3) continue;
        /* Unit view-space normal of the opening plane, recovered from the
         * inv-vz fit: the fit is inv = (n.x/(H k))x + (n.y/(H k))y +
         * (n.z - n.x ofx/H - n.y ofy/H)/k, so n is proportional to
         * (a H, b H, c + a ofx + b ofy). */
        double nx = pp->a * H, ny = pp->b * H;
        double nz = pp->c + pp->a * ofx + pp->b * ofy;
        double nn = sqrt(nx*nx + ny*ny + nz*nz);
        if (nn < 1e-12) continue;
        nx /= nn; ny /= nn; nz /= nn;
        /* perpendicular distances + view-space verts. Along the pixel ray
         * v = (x-ofx, y-ofy, H), a view-Z offset dz from the plane is a
         * perpendicular offset dz * (n.v)/H. */
        double V[3][3], dmin = 1e30, dmax = 0.0; int okz = 1;
        for (int k = 0; k < 3; k++) {
            const double inv = pp->a * t->sx[k] + pp->b * t->sy[k] + pp->c;
            if (inv <= 1e-9) { okz = 0; break; }
            const double vz = t->vz[k] * 16.0;
            V[k][0] = (t->sx[k]-ofx) * vz / H; V[k][1] = (t->sy[k]-ofy) * vz / H; V[k][2] = vz;
            const double dz = vz - 1.0 / inv;
            const double d = fabs(dz * (nx*(t->sx[k]-ofx) + ny*(t->sy[k]-ofy) + nz*H) / H);
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
        }
        if (!okz) continue;
        /* Candidate gate, entirely IN THE PLANE.
         *
         * Every screen-space gate on this path has turned out to be a bug:
         * measuring depth along view-Z, testing containment against the
         * projected polygon, and finally this — a face is near the opening or
         * it is not, and walking around the arch must not change the answer.
         * A face offset from the plane projects with a different perspective
         * shift than the polygon does, so a screen bbox overlap admitted it
         * from some angles and dropped it from others (stone_hill rendered
         * untinted at pos=(97051,49348,8798) yaw=581 pitch=162 and tinted a
         * few degrees away).
         *
         * So: drop the face onto the plane, and require its in-plane bbox to
         * overlap the opening's, grown by RIM_CAND_MARGIN world units. The
         * plane is infinite, so some spatial bound is needed — this one is a
         * property of the geometry rather than of the viewpoint. */
        double O[3], U[3], W[3]; rim_uv pv[16];
        const double nn3[3] = { nx, ny, nz };
        if (!rim_plane_basis(pp, nn3, O, U, W, pv, 16)) continue;
        if (dmin > RIM_CAND_WINDOW) continue;
        double pulo = pv[0].u, puhi = pv[0].u, pvlo = pv[0].v, pvhi = pv[0].v;
        for (int i = 1; i < pp->vn; i++) {
            if (pv[i].u < pulo) pulo = pv[i].u; if (pv[i].u > puhi) puhi = pv[i].u;
            if (pv[i].v < pvlo) pvlo = pv[i].v; if (pv[i].v > pvhi) pvhi = pv[i].v;
        }
        rim_uv fuv[3];
        double fulo = 1e30, fuhi = -1e30, fvlo = 1e30, fvhi = -1e30;
        for (int k = 0; k < 3; k++) {
            const double r[3] = { V[k][0]-O[0], V[k][1]-O[1], V[k][2]-O[2] };
            fuv[k].u = r[0]*U[0] + r[1]*U[1] + r[2]*U[2];
            fuv[k].v = r[0]*W[0] + r[1]*W[1] + r[2]*W[2];
            if (fuv[k].u < fulo) fulo = fuv[k].u; if (fuv[k].u > fuhi) fuhi = fuv[k].u;
            if (fuv[k].v < fvlo) fvlo = fuv[k].v; if (fuv[k].v > fvhi) fvhi = fuv[k].v;
        }
        if (fuhi < pulo - RIM_CAND_MARGIN || fulo > puhi + RIM_CAND_MARGIN ||
            fvhi < pvlo - RIM_CAND_MARGIN || fvlo > pvhi + RIM_CAND_MARGIN) continue;
        int flags = 0;
        /* contain: any vertex or the centroid, dropped onto the opening plane
         * along its normal and tested against the polygon there. A face can
         * straddle a non-convex opening with its centre in and every corner
         * out, hence both. */
        int touches = 0;
        double cu = 0.0, cv = 0.0;
        for (int k = 0; k < 3; k++) {
            cu += fuv[k].u; cv += fuv[k].v;
            if (!touches && rim_uv_contains(pv, pp->vn, fuv[k].u, fuv[k].v)) touches = 1;
        }
        if (!touches && rim_uv_contains(pv, pp->vn, cu/3.0, cv/3.0)) touches = 1;
        if (!touches && reach > 0.0)
            for (int k = 0; k < 3 && !touches; k++)
                if (rim_uv_edge_dist(pv, pp->vn, fuv[k].u, fuv[k].v) <= reach) touches = 1;
        if (!touches) flags |= RIMFAIL_CONTAIN;
        if (dmax > eps) flags |= RIMFAIL_DIST;
        const double u[3] = { V[1][0]-V[0][0], V[1][1]-V[0][1], V[1][2]-V[0][2] };
        const double w[3] = { V[2][0]-V[0][0], V[2][1]-V[0][1], V[2][2]-V[0][2] };
        double fx = u[1]*w[2]-u[2]*w[1], fy = u[2]*w[0]-u[0]*w[2], fz = u[0]*w[1]-u[1]*w[0];
        const double fl = sqrt(fx*fx + fy*fy + fz*fz);
        if (fl < 1e-9) flags |= RIMFAIL_ANGLE;
        else if (fabs((fx*nx + fy*ny + fz*nz) / fl) < portal_rim_cos()) flags |= RIMFAIL_ANGLE;
        if (best < 0 || flags < best) best = flags;
        if (best == 0) return 0;
    }
    return best;
}

static int portal_rim_face(CPUState* cpu, const ttri* t) {
    return portal_rim_classify(cpu, t) == 0;
}

/* Does a portal sky cover this face outright — the .s walk draws the preview
 * AFTER it, so on PSX it is invisible?
 *
 * The opaque path handles this with depth: the preview rasters nearer and
 * repaints over the terrain behind it. Transparent terrain cannot ride that.
 * The preview colours draw depth-test-only (no depth WRITE, so a moby in front
 * of the arch keeps its pixels and the player is never clipped mid-entry), and
 * the ALPHA / ADDITIVE / SUBTRACT classes draw AFTER the portal sequence — they
 * test against the depth the OPAQUE terrain left behind, pass, and blend on top
 * of the preview. A waterfall behind a Dream Weavers arch reads as its animated
 * texture shining through the portal.
 *
 * Mirror the walk instead of fighting the depth buffer: a transparent face the
 * sky covers is simply not drawn. */
static int portal_sky_covers(const ttri* t) {
    for (int pi = 0; pi < s_pplane_n; pi++) {
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        if (pp->ot_vz <= 0.0 || pp->pn < 3) continue;
        const float cx = (t->sx[0] + t->sx[1] + t->sx[2]) / 3.0f;
        const float cy = (t->sy[0] + t->sy[1] + t->sy[2]) / 3.0f;
        if (cx < pp->x0 || cx > pp->x1 || cy < pp->y0 || cy > pp->y1) continue;
        if (!skyplane_contains(pp, cx, cy)) continue;
        const double mean = (t->otkey > 0.0f) ? (double)t->otkey
                                              : (t->vz[0] + t->vz[1] + t->vz[2]) * 16.0 / 3.0;
        if (mean >= pp->ot_vz + 64.0) return 1;   /* sorts behind the sky's bucket */
    }
    return 0;
}

static int portal_conform_vz(const ttri* t, float ovz[3]) {
    for (int pi = 0; pi < s_pplane_n; pi++) {
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        if (pp->ot_vz <= 0.0 || pp->pn < 3) continue;   /* no projected polygon */
        /* Confinement: the face's screen CENTROID must be inside the outline
         * polygon — only there does the sky raster (fan / dome) and win;
         * sinking outside it punches through to the world behind (mc f3040). */
        float cx = (t->sx[0] + t->sx[1] + t->sx[2]) / 3.0f;
        float cy = (t->sy[0] + t->sy[1] + t->sy[2]) / 3.0f;
        int inbb = (cx >= pp->x0 && cx <= pp->x1 && cy >= pp->y0 && cy <= pp->y1)
                   && skyplane_contains(pp, cx, cy);
        int dbg = 0;
        if (s_pconform_dbg > 0) {   /* face covers the probe pixel's bbox? */
            float xlo = t->sx[0], xhi = t->sx[0], ylo = t->sy[0], yhi = t->sy[0];
            for (int k = 1; k < 3; k++) {
                if (t->sx[k] < xlo) xlo = t->sx[k]; if (t->sx[k] > xhi) xhi = t->sx[k];
                if (t->sy[k] < ylo) ylo = t->sy[k]; if (t->sy[k] > yhi) yhi = t->sy[k];
            }
            dbg = (s_pcdbg_x >= xlo && s_pcdbg_x <= xhi &&
                   s_pcdbg_y >= ylo && s_pcdbg_y <= yhi);
        }
        if (!inbb) {
            if (dbg) fprintf(stderr, "[pconf f=%u] sec=%d face=%d NOT-INBB bbox=(%.0f,%.0f)-(%.0f,%.0f)\n",
                             openpete_frame, t->sec, t->face, pp->x0, pp->y0, pp->x1, pp->y1);
            continue;
        }
        /* The face's .s OT key: the parent face's 4-corner mean when the walk
         * recorded it (always, today), else the tri's own 3-vert mean. */
        double mean = (t->otkey > 0.0f) ? (double)t->otkey
                                        : (t->vz[0] + t->vz[1] + t->vz[2]) * 16.0 / 3.0;
        int covered = (mean >= pp->ot_vz + 64.0);   /* face key behind the sky's B (half-bucket margin) */
        if (dbg)
            fprintf(stderr, "[pconf f=%u] sec=%d face=%d tier=%d mean=%.0f skyot=%.0f back=%.0f covered=%d\n",
                    openpete_frame, t->sec, t->face, t->tier, mean, pp->ot_vz,
                    pp->back_vz, covered);
        if (covered) {
            /* Sink each vertex to just behind the OPENING-PLANE FIELD on its
             * own pixel ray (never pulled forward) — the face then loses to
             * the sky exactly where the sky rasters (the sky sits at
             * plane − PORTAL_SKY_EPS_VZ inside the outline) while BELOW the
             * sky's rect it keeps beating the throat floor behind it, which
             * the .s also shows (the sill bar peeks out under the preview —
             * sinking to the behind-ring depth erased it there). Linear
             * screen interpolation of the sunk verts only sags DEEPER than
             * the convex plane field, so the sky still wins the interior. */
            int changed = 0;
            for (int k = 0; k < 3; k++) {
                ovz[k] = (float)t->vz[k];
                double inv = pp->a * t->sx[k] + pp->b * t->sy[k] + pp->c;
                if (inv <= 1e-9) continue;       /* plane behind camera on this ray */
                float sunk = (float)((1.0 / inv + portal_clamp_eps_vz()) / 16.0);
                if (ovz[k] < sunk) { ovz[k] = sunk; changed = 1; }
            }
            if (changed) return 1;
        }
    }
    return 0;
}

static void terrain_feed_native(CPUState* cpu) {
    {
        static tn_envvbl_t nt_c, tc_c, to_c;
        s_notex     = tn_env_vbl(&nt_c, "OPENPETE_TERRAIN_NOTEX", 0, 1);
        s_tiercolor = tn_env_vbl(&tc_c, "OPENPETE_TERRAIN_TIERCOLOR", 0, 1);
        s_tieronly  = tn_env_vbl(&to_c, "OPENPETE_TERRAIN_TIERONLY", 0, 0);
    }
    if (s_pconform < 0) { const char* e = getenv("OPENPETE_TERRAIN_PORTAL_CONFORM"); s_pconform = (e && *e) ? (*e != '0') : 1; }
    if (s_pconform_dbg < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_PORTAL_CONFORM_DBG");
        s_pconform_dbg = 0;
        if (e && *e && sscanf(e, "%f,%f", &s_pcdbg_x, &s_pcdbg_y) == 2) s_pconform_dbg = 1;
    }
    /* Active portal-sky opening planes, derived against THIS present's camera
     * bracket (canonical or sub-tick interpolated) so plane and verts agree. */
    s_pplane_n = s_pconform ? openpete_portal_sky_planes(cpu, s_pplane, 6) : 0;
    if (s_pconform_dbg > 0 && s_pplane_n > 0) {
        static int lines = 0;
        if (lines < 24) {
            lines++;
            fprintf(stderr, "[pconf f=%u] planes=%d", openpete_frame, s_pplane_n);
            for (int i = 0; i < s_pplane_n; i++)
                fprintf(stderr, " [%d]p=%08X skyot=%.0f back=%.0f bbox=(%.0f,%.0f)-(%.0f,%.0f)",
                        i, s_pplane[i].portal, s_pplane[i].ot_vz, s_pplane[i].back_vz,
                        s_pplane[i].x0, s_pplane[i].y0, s_pplane[i].x1, s_pplane[i].y1);
            fprintf(stderr, "\n");
        }
    }
    { static int s_cap = -1, s_peak = 0;
      if (s_cap < 0) { const char* e = getenv("OPENPETE_SCENE_CAPDBG"); s_cap = (e && *e && *e != '0'); }
      if (s_cap && s_tri_n > s_peak) { s_peak = s_tri_n;
          fprintf(stderr, "[terraincap] NEW PEAK terrain_tris=%d / MAX_TTRIS=%d\n", s_tri_n, MAX_TTRIS); } }
    /* OPENPETE_PORTAL_FIT_DBG=<frame>: one-shot dump of each active opening and
     * the terrain around it, expressed in the OPENING PLANE's own 2D basis.
     *
     * This is the measurement behind the per-portal art corrections. An arch's
     * true opening is the hole in the terrain mesh, not the authored outline
     * polygon, and on several arches the two disagree. Both are dumped in the
     * same (u,v) frame so the correction can be FITTED from geometry rather
     * than judged by eye — the outline in plane coords, then every nearby face
     * with its own plane coords and its rim classification, so the surviving
     * (post-cull) geometry can be separated from the rim that is dropped.
     * Units are world units throughout. */
    { static int s_fit = -2; static unsigned s_fit_f = 0;
      if (s_fit == -2) { const char* e = getenv("OPENPETE_PORTAL_FIT_DBG");
                         s_fit = (e && *e) ? 1 : -1; s_fit_f = (e && *e) ? (unsigned)atoi(e) : 0; }
      if (s_fit == 1 && openpete_frame >= s_fit_f && s_pplane_n > 0) {
        s_fit = -1;                                  /* one shot */
        const double ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
        const double ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
        const double H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
        for (int pi = 0; pi < s_pplane_n && H >= 1.0; pi++) {
          const openpete_portal_skyplane* pp = &s_pplane[pi];
          if (pp->vn < 3) continue;
          double nx = pp->a * H, ny = pp->b * H;
          double nz = pp->c + pp->a * ofx + pp->b * ofy;
          double nn = sqrt(nx*nx + ny*ny + nz*nz);
          if (nn < 1e-12) continue;
          nx /= nn; ny /= nn; nz /= nn;
          double O[3], U[3], W[3]; rim_uv pv[16];
          const double nn3[3] = { nx, ny, nz };
          if (!rim_plane_basis(pp, nn3, O, U, W, pv, 16)) continue;
          const int levelId = (int32_t)cpu->read_word(pp->portal + 0x1C);
          /* The CORRECTION basis, expressed in this census's (U,W) frame.
           *
           * portal_outline_world applies dh/dv/sh/sv about world-space in-plane
           * axes (vertical = world +Z projected into the opening plane,
           * horizontal = normal x vertical), while the basis above is built
           * from the outline's own first edge. The two span the same plane but
           * differ by an in-plane rotation, so a fit computed here would apply
           * along the wrong axes. Emitting both axes in (U,W) coordinates lets
           * the fit be converted exactly. */
          double zx, zy, zz;              /* world +Z, in view space */
          { int Mc[3][3]; ngte_read_matrix(cpu, G_CAMERA + 0x00, Mc);
            ngte_mac(Mc, 0.0, -1.0, 0.0, &zx, &zy, &zz); }
          { const double zd = zx*nx + zy*ny + zz*nz;    /* project into plane */
            zx -= zd*nx; zy -= zd*ny; zz -= zd*nz; }
          const double zl = sqrt(zx*zx + zy*zy + zz*zz);
          if (zl < 1e-9) continue;                      /* opening lies flat */
          zx /= zl; zy /= zl; zz /= zl;                 /* = code's vertical */
          const double hax[3] = { ny*zz - nz*zy, nz*zx - nx*zz, nx*zy - ny*zx };
          fprintf(stderr, "[pfit] portal=%08X levelId=%d vn=%d "
                  "vax=%.6f,%.6f hax=%.6f,%.6f\n",
                  pp->portal, levelId, pp->vn,
                  zx*U[0] + zy*U[1] + zz*U[2], zx*W[0] + zy*W[1] + zz*W[2],
                  hax[0]*U[0] + hax[1]*U[1] + hax[2]*U[2],
                  hax[0]*W[0] + hax[1]*W[1] + hax[2]*W[2]);
          for (int k = 0; k < pp->vn; k++)
            fprintf(stderr, "[pfit]   out %d %.2f %.2f\n", k, pv[k].u, pv[k].v);
          double pulo = pv[0].u, puhi = pv[0].u, pvlo = pv[0].v, pvhi = pv[0].v;
          for (int i = 1; i < pp->vn; i++) {
            if (pv[i].u < pulo) pulo = pv[i].u; if (pv[i].u > puhi) puhi = pv[i].u;
            if (pv[i].v < pvlo) pvlo = pv[i].v; if (pv[i].v > pvhi) pvhi = pv[i].v;
          }
          /* Search box: the opening grown by its own size, so the arch jamb and
           * the ground meeting the sill are both included without dragging in
           * the whole sector. */
          const double gu = (puhi - pulo), gv = (pvhi - pvlo);
          for (int i = 0; i < s_tri_n; i++) {
            const ttri* t = &s_tri[i];
            double V[3][3]; int okz = 1; double dmin = 1e30, dmax = 0.0;
            for (int k = 0; k < 3; k++) {
              const double inv = pp->a * t->sx[k] + pp->b * t->sy[k] + pp->c;
              if (inv <= 1e-9) { okz = 0; break; }
              const double vz = t->vz[k] * 16.0;
              V[k][0] = (t->sx[k]-ofx) * vz / H; V[k][1] = (t->sy[k]-ofy) * vz / H; V[k][2] = vz;
              const double dz = vz - 1.0 / inv;
              const double d = fabs(dz * (nx*(t->sx[k]-ofx) + ny*(t->sy[k]-ofy) + nz*H) / H);
              if (d < dmin) dmin = d;
              if (d > dmax) dmax = d;
            }
            if (!okz) continue;
            if (dmin > 1024.0) continue;          /* only geometry AT the opening */
            rim_uv f[3];
            double fulo = 1e30, fuhi = -1e30, fvlo = 1e30, fvhi = -1e30;
            for (int k = 0; k < 3; k++) {
              const double r[3] = { V[k][0]-O[0], V[k][1]-O[1], V[k][2]-O[2] };
              f[k].u = r[0]*U[0] + r[1]*U[1] + r[2]*U[2];
              f[k].v = r[0]*W[0] + r[1]*W[1] + r[2]*W[2];
              if (f[k].u < fulo) fulo = f[k].u; if (f[k].u > fuhi) fuhi = f[k].u;
              if (f[k].v < fvlo) fvlo = f[k].v; if (f[k].v > fvhi) fvhi = f[k].v;
            }
            if (fuhi < pulo - gu || fulo > puhi + gu ||
                fvhi < pvlo - gv || fvlo > pvhi + gv) continue;
            fprintf(stderr, "[pfit]   f %d %d %d %.1f %.1f %.2f %.2f %.2f %.2f %.2f %.2f\n",
                    t->sec, t->face, portal_rim_classify(cpu, t), dmin, dmax,
                    f[0].u, f[0].v, f[1].u, f[1].v, f[2].u, f[2].v);
          }
        }
      } }
    /* OPENPETE_PORTAL_RIM_DBG=<frame>: one-shot census of terrain faces near a
     * portal's opening plane, printing each face's per-term verdict. The rim
     * predicate has three terms and a yes/no highlight cannot say which one
     * rejected a face — that ambiguity is what made earlier misses guesswork.
     * Fires on >= the target frame, never ==: canonical feeds do not land on a
     * fixed parity, so an equality test can miss entirely. */
    { static int s_rim = -2; static unsigned s_rim_f = 0;
      if (s_rim == -2) { const char* e = getenv("OPENPETE_PORTAL_RIM_DBG");
                         s_rim = (e && *e) ? 1 : -1; s_rim_f = (e && *e) ? (unsigned)atoi(e) : 0; }
      if (s_rim == 1 && openpete_frame >= s_rim_f && s_pplane_n > 0) {
        s_rim = -1;                                  /* one shot */
        const double ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
        const double ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
        const double H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
        for (int pi = 0; pi < s_pplane_n; pi++) {
          const openpete_portal_skyplane* pp = &s_pplane[pi];
          double nx = pp->a * H, ny = pp->b * H;
          double nz = pp->c + pp->a * ofx + pp->b * ofy;
          double nn = sqrt(nx*nx + ny*ny + nz*nz);
          if (nn < 1e-12) continue;
          nx /= nn; ny /= nn; nz /= nn;
          fprintf(stderr, "[rim] portal=%08X bbox=(%.0f,%.0f)-(%.0f,%.0f) skyot=%.0f n=(%.3f,%.3f,%.3f)\n",
                  pp->portal, pp->x0, pp->y0, pp->x1, pp->y1, pp->ot_vz, nx, ny, nz);
          for (int i = 0; i < s_tri_n; i++) {
            const ttri* t = &s_tri[i];
            float cx = (t->sx[0]+t->sx[1]+t->sx[2])/3.0f, cy = (t->sy[0]+t->sy[1]+t->sy[2])/3.0f;
            if (cx < pp->x0-8 || cx > pp->x1+8 || cy < pp->y0-8 || cy > pp->y1+8) continue;
            /* PERPENDICULAR distance to the opening plane, in world units:
             * along the pixel ray v = (x-ofx, y-ofy, H), a view-Z offset dz
             * from the plane is a perpendicular offset dz * (n.v)/H. */
            double d[3]; int ok = 1;
            for (int k = 0; k < 3; k++) {
              double inv = pp->a * t->sx[k] + pp->b * t->sy[k] + pp->c;
              if (inv <= 1e-9) { ok = 0; break; }
              double dz = t->vz[k] * 16.0 - 1.0 / inv;   /* + = behind the plane */
              double vx = t->sx[k] - ofx, vy = t->sy[k] - ofy;
              d[k] = dz * (nx*vx + ny*vy + nz*H) / H;
            }
            if (!ok) continue;
            double amax = fabs(d[0]) > fabs(d[1]) ? fabs(d[0]) : fabs(d[1]);
            if (fabs(d[2]) > amax) amax = fabs(d[2]);
            if (amax > 400.0) continue;              /* only the near-plane band */
            double VV[3][3];
            for (int k = 0; k < 3; k++) {
              const double vz = t->vz[k] * 16.0;
              VV[k][0] = (t->sx[k]-ofx) * vz / H; VV[k][1] = (t->sy[k]-ofy) * vz / H; VV[k][2] = vz;
            }
            const double u0[3] = { VV[1][0]-VV[0][0], VV[1][1]-VV[0][1], VV[1][2]-VV[0][2] };
            const double w0[3] = { VV[2][0]-VV[0][0], VV[2][1]-VV[0][1], VV[2][2]-VV[0][2] };
            double fnx = u0[1]*w0[2]-u0[2]*w0[1], fny = u0[2]*w0[0]-u0[0]*w0[2], fnz = u0[0]*w0[1]-u0[1]*w0[0];
            const double fl = sqrt(fnx*fnx + fny*fny + fnz*fnz);
            double para = 0.0;
            if (fl > 1e-9) para = fabs((fnx*nx + fny*ny + fnz*nz) / fl);
            const double ang = acos(para > 1.0 ? 1.0 : para) * 180.0 / 3.14159265358979323846;
            const int cls = portal_rim_classify(cpu, t);
            fprintf(stderr, "  sec=%d face=%d tier=%d texid=%d d=(%.1f,%.1f,%.1f) "
                    "|d|max=%.1f ang=%.1fdeg at=(%.1f,%.1f) => %s%s%s%s\n",
                    t->sec, t->face, t->tier, t->texid, d[0], d[1], d[2], amax, ang, cx, cy,
                    cls < 0 ? "not-a-candidate" : cls == 0 ? "ACCEPT" : "REJECT",
                    (cls > 0 && (cls & RIMFAIL_CONTAIN)) ? " contain" : "",
                    (cls > 0 && (cls & RIMFAIL_DIST))    ? " dist" : "",
                    (cls > 0 && (cls & RIMFAIL_ANGLE))   ? " angle" : "");
          }
        }
      } }
    static float s_fill_dilate = -1.0f;  /* OPENPETE_TERRAIN_FILL_DILATE px (0 disables) */
    if (s_fill_dilate < 0.0f) {
        const char* e = getenv("OPENPETE_TERRAIN_FILL_DILATE");
        s_fill_dilate = (e && *e) ? (float)atof(e) : 0.0f;  /* v4.1: quantize/grow OFF by default (v2/v3 machinery caused silhouette ghosting/spikes + sub-tick crawl; envs re-enable for experiments) */
    }
    /* Untextured-band material (S8c): one resolve per feed — the whole
     * beyond-LOD gouraud band shares a single per-instance outcome. */
    const int untex_slot = openpete_mods_material_terrain_untex_slot(cpu);
    /* Texture-keyed materials (S9): one resolve per (feed, Tiledef id),
     * memoized — -1 = unresolved. */
    int16_t tex_slot[128];
    memset(tex_slot, 0xFF, sizeof tex_slot);
    /* Terrain self-snaps (push_tri floors under the integer snap; subdivide
     * midpoints carry exact on-edge positions) — the renderer chokepoint
     * must pass these coordinates through untouched. */
    openpete_native_scene_set_presnap(1);
    for (int i = 0; i < s_tri_n; i++) {
        ttri* t = &s_tri[i];
        if (s_tieronly == 1 && !t->tier) continue;   /* LP only */
        if (s_tieronly == 2 &&  t->tier) continue;   /* HP only */
        /* Portal OT-conformance clamp (see portal_conform_vz above). */
        float pcvz[3]; int pclamp = 0;
        if (s_pplane_n) pclamp = portal_conform_vz(t, pcvz);
        /* Transparent terrain the preview covers: drop it (portal_sky_covers). */
        if (s_pplane_n && t->blend && t->textured && portal_sky_covers(t)) continue;
        openpete_scene_tri tri; memset(&tri, 0, sizeof(tri));
        int tex = (!s_flat && !s_notex && !s_tiercolor) ? t->textured : 0;
        tri.textured = tex; tri.blend = tex ? t->blend : 0;
        tri.tpage_x = t->tpx; tri.tpage_y = t->tpy;
        tri.clut_x = t->clx; tri.clut_y = t->cly; tri.bpp = t->bpp;
        /* FILL tris are dilated a sub-pixel outward from their centroid: the
         * PSX integer scanline fill is inclusive where native's float raster +
         * GPU centre-sampling drops sub-pixel rows (the dashed far-water
         * slivers). Stencil==0 confines the growth to genuine holes. */
        float dsx[3], dsy[3];
        for (int k = 0; k < 3; k++) { dsx[k] = t->sx[k]; dsy[k] = t->sy[k]; }
        if (t->fill && s_fill_dilate > 0.0f) {
            /* FILL coords are floor-QUANTIZED to mirror the PSX's truncating
             * GTE (pixbox forensics f2730: the same LP quads sit +0.2..+1.0px
             * below the PSX integer coords with per-row VARIANCE — at the
             * horizon, where rows are ~1px tall, that variance opens the
             * dashed water gaps; floor() reproduces the PSX row edges almost
             * everywhere), then grown by the residual dilate PER AXIS (not
             * radially — these rows are ~80px wide and ~1px tall, so a
             * normalized radial offset is nearly all horizontal and closes
             * nothing) to bridge the occasional remaining ±1 divergence.
             * Stencil==0 confines all growth to genuine holes. */
            /* Vertical growth is DOWN-only: floor() already lifts the top edge
             * by the truncated fraction (~0.5px avg, matching the PSX edge);
             * additionally growing UP pushed the fill's fully-fogged LP-only
             * strips ABOVE the terrain silhouette into the sky (a dark fringe
             * on the peace-keepers cliff line, gate tick 4888 round 2). */
            static float s_fill_up = -1.0f;   /* OPENPETE_TERRAIN_FILL_UP: upward growth px */
            if (s_fill_up < 0.0f) { const char* e = getenv("OPENPETE_TERRAIN_FILL_UP"); s_fill_up = (e && *e) ? (float)atof(e) : 0.7f; }
            /* CONTINUOUS growth (2026-07-03, replaces the v2 floor()-quantize
             * as the DEFAULT): floor() snapped the fill verts to the pixel
             * grid EVERY PRESENT, so under the sub-tick spine the fill fringe
             * crawled/jittered against the smoothly-interpolated canonical
             * terrain (user report: ghosting / spikes at terrain tops, all
             * levels — a temporal artifact, invisible in canonical stills).
             * Same coverage envelope without snapping: shift UP by the mean
             * truncation bias (~0.55px — native float sits +0.2..+1.0px below
             * the PSX ints) and grow per-axis from the centroid.
             * OPENPETE_TERRAIN_FILL_QUANT=1 restores floor() (A/B lever). */
            static int s_fill_quant = -1;
            if (s_fill_quant < 0) { const char* e = getenv("OPENPETE_TERRAIN_FILL_QUANT"); s_fill_quant = e ? (*e && *e != '0') : 1;   /* default floor (canonical fidelity); 0 = continuous (smoother sub-tick) */ }
            float cx = (dsx[0]+dsx[1]+dsx[2]) / 3.0f, cy = (dsy[0]+dsy[1]+dsy[2]) / 3.0f;
            for (int k = 0; k < 3; k++) {
                if (s_fill_quant) {
                    dsx[k] = floorf(dsx[k]) + (dsx[k] > cx ? s_fill_dilate : -s_fill_dilate);
                    dsy[k] = floorf(dsy[k]) + (dsy[k] > cy ? s_fill_dilate : -s_fill_up);
                } else {
                    dsx[k] += (dsx[k] > cx ? s_fill_dilate : -s_fill_dilate);
                    dsy[k] += -0.55f + (dsy[k] > cy ? s_fill_dilate : -s_fill_up);
                }
            }
        }
        for (int k = 0; k < 3; k++) {
            tri.v[k].sx = dsx[k]; tri.v[k].sy = dsy[k];
            /* Terrain view-Z is in world/16 units (cam>>4, verts /16); the
             * actors feed FULL-scale view-Z. depth_from_viewz expects ONE shared
             * scale, so ×16 to match — else terrain's depth is ~16× too small
             * (always nearest) and draws on top of every actor.
             * LP ×1.03 (W2.6-c resolution): with the stencil hole-fill carrying
             * every INTER-sector LP backing role, this pull's one remaining
             * consumer is the 0x8000 force-LP set — a sector whose LP is the
             * PRIMARY surface backing a sparse HP detail overlay (stone-hill
             * sector 79: 3 HP vs 12 LP faces). That LP must OCCLUDE geometry
             * behind it, so it cannot ride the additive-only stencil fill; and
             * it is NOT a coplanar tie-break, so the W2.5 constant hardware
             * bias is the wrong shape — the two meshes diverge in WORLD units,
             * which a distance-PROPORTIONAL pull models correctly. Retained,
             * scoped, no longer scene-magic. (Also applies to no-HP 0x4000
             * LP, where it is inert — nothing shares its surface.) */
            /* (A face-mean depth clamp — pushing a face's near half back to
             * its .s OT-key mean so mobys beat the ground they interpenetrate
             * — was tried here and REMOVED: the flattened depth corrupts the
             * shadow decal's world-height reconstruction (a hard bright wedge
             * where Spyro's drop shadow belongs), and the moby-side one-slot
             * tie pull covers the pedestal cases without touching terrain.) */
            openpete_worldvz wz = openpete_worldvz_scaled(
                pclamp ? pcvz[k] : (float)t->vz[k], 16.0f);
            /* FILL depth = the SAME ×1.03 as its canonical LP twin, so the two
             * copies TIE exactly (fill drawn later wins where quantize+grow
             * differ) instead of the fill sitting 3% NEARER — which let a
             * grown fill row depth-beat neighbouring canonical LP bodies
             * (rock-edge notch, gaps-2 f2770). The stencil gate — not depth —
             * is what keeps the fill behind HP (W2.6-c). */
            {
                static int s_fillpull = -1;
                if (s_fillpull < 0) { const char* e2 = getenv("OPENPETE_TERRAIN_FILL_TIERPULL"); s_fillpull = (e2 && *e2) ? atoi(e2) : 1; }
                /* LP rides behind HP by a CONSTANT, mirroring the .s exactly.
                 *
                 * The original keys the world OT at viewZ/128 for BOTH tiers
                 * (LP sums four SZ3 in 16-world units then >>5; HP sums four in
                 * 4-world units then >>7 — the same scale), and then adds a
                 * flat +0x40 slots to every LP face's index. One slot is 128
                 * world units, so the offset is 8192 world units of view-Z,
                 * applied before projection — which is where this sits.
                 *
                 * That constant is what makes the tiers a PARTITION rather than
                 * a tie-break: at the stock LOD distance HP cannot exceed slot
                 * 283 while LP starts at 257, so LP is behind every co-located
                 * HP face by construction, whatever the two meshes do. It also
                 * cannot misbehave with distance the way a proportional scale
                 * does — as a fraction of depth it is ~25% at the LOD line and
                 * decays outward, instead of a fixed 3% that is far too little
                 * exactly where the tiers overlap and far too much out at
                 * portal range.
                 *
                 * 64 slots is nominal; 36 (0x40 minus the largest authored HP
                 * bias) is the floor that still guarantees the ordering. The
                 * per-face OT bias fields the .s also carries — LP word1 bits
                 * 3-7 scaled x8, HP word0 bits 3-5 scaled x4 — are NOT modelled
                 * here yet; they shift individual faces by up to 248 and 28
                 * slots respectively.
                 *
                 * DEFAULT 0 (legacy proportional path) — the constant is NOT
                 * shippable on its own. Ported alone it regresses: the .s
                 * admits LP faces by `viewZ >= lodDist - 128*((w0&0x1F)*8) -
                 * 4096`, an authored per-face field spanning nearly the whole
                 * LOD range, which this feeder does not read — so the LP face
                 * SET here is not the .s's, and a faithful ordering constant
                 * over an unfaithful face set is not faithful. Measured: jamb
                 * 72.8 -> 278.9, artisans waterline 16.0 -> 42.6 (shimmer does
                 * improve, 254 -> 223). The constant only pays off alongside
                 * the admission field and both per-face OT bias fields.
                 * OPENPETE_TERRAIN_LP_OTSLOTS sets the slot count (0 = legacy).
                 * OPENPETE_TERRAIN_LP_BIAS 1/0 = on/off. Live, re-read per VBL. */
                static tn_envvbl_t lpb_c, lpc_c, lpm_c, lps_c;
                const int lp_bias  = tn_env_vbl(&lpb_c, "OPENPETE_TERRAIN_LP_BIAS", 1, 1);
                const int lp_cap   = tn_env_vbl(&lpc_c, "OPENPETE_TERRAIN_LP_BIAS_CAP", 768, 0);
                const int lp_mil   = tn_env_vbl(&lpm_c, "OPENPETE_TERRAIN_LP_BIAS_MIL", 30, 0);
                const int lp_slots = tn_env_vbl(&lps_c, "OPENPETE_TERRAIN_LP_OTSLOTS", 0, 0);
                if (t->tier && (s_fillpull || !t->fill)) {
                    tri.v[k].pvz = wz.vz;      /* perspective W = the UNPULLED view-Z */
                    if (lp_bias)              /* tier==1 → LP */
                        wz = lp_slots > 0
                           /* negative pull = push FARTHER, by whole OT slots */
                           ? openpete_worldvz_pull(wz, -(float)lp_slots * 128.0f)
                           : openpete_worldvz_bias_capped(wz, 1.0f + lp_mil * 0.001f,
                                                          (float)lp_cap);
                }
            }
            tri.v[k].z  = openpete_native_depth_from_viewz(wz);
            tri.v[k].u = t->u[k]; tri.v[k].v = t->v[k];
            /* Textured: modulate the sampled texel by the fogged colour (the
             * pipeline does tex*colour/128, so feed the full RGB). Untextured:
             * the gouraud colour; s_flat = a debug coverage colour;
             * s_tiercolor = LP red / HP blue / fill green. */
            tri.v[k].rgba = s_tiercolor ? (t->fill ? 0xFF00FF00u
                                         : t->tier ? 0xFF0000FFu : 0xFFFF0000u)
                          : s_flat       ? 0xFF20C040u
                          : (0xFF000000u | (t->rgb[k] & 0xFFFFFFu));
        }
        int tid = (tex && t->texid >= 0 && t->texid < 128) ? t->texid : -1;
        if (tid >= 0 && tex_slot[tid] < 0) {
            tex_slot[tid] = (int16_t)openpete_mods_material_terrain_tex_slot(cpu, tid);
            static int s_tsr = -2;
            if (s_tsr == -2) { const char* e2 = getenv("OPENPETE_TEXSEL_DBG");
                               s_tsr = e2 ? atoi(e2) : -1; }
            if (s_tsr == tid && openpete_frame > 2000u) { static int pr2;
                if (pr2++ < 12) fprintf(stderr, "[texsel-resolve f=%u] tid=%d -> slot=%d\n",
                                        openpete_frame, tid, tex_slot[tid]); }
        }
        {   /* OPENPETE_TEXSEL_DBG=<tid>: once per present, count that id's
             * textured tris + missing-texid textured tris + its slot. */
            static int s_tsdbg = -2;
            if (s_tsdbg == -2) { const char* e2 = getenv("OPENPETE_TEXSEL_DBG");
                                 s_tsdbg = e2 ? atoi(e2) : -1; }
            if (s_tsdbg >= 0) {
                static unsigned lf = 0xFFFFFFFFu; static int nt, nm;
                if (openpete_frame != lf) {
                    if (lf != 0xFFFFFFFFu)
                        fprintf(stderr, "[texsel f=%u] tid=%d tris=%d slot=%d "
                                "textured-missing-texid=%d\n", lf, s_tsdbg, nt,
                                tex_slot[s_tsdbg], nm);
                    lf = openpete_frame; nt = 0; nm = 0;
                }
                if (tid == s_tsdbg) nt++;
                if (tex && tid < 0) nm++;
            }
        }
        openpete_native_scene_set_tag(tex
            ? (tid >= 0
                   ? OPENPETE_TAG_TERRAIN_TEX(t->tier, t->env,
                                              t->sec < 0 ? 0 : t->sec,
                                              tid, tex_slot[tid])
                   : OPENPETE_TAG_TERRAIN(t->tier, t->env, t->sec < 0 ? 0 : t->sec))
            : OPENPETE_TAG_TERRAIN_UNTEX(t->tier, t->env,
                                         t->sec < 0 ? 0 : t->sec, untex_slot));
        openpete_native_scene_set_fog_delta(t->fog_delta);
        /* Portal inner-rim CULL. The rim lies in the opening plane, coincident
         * with the destination-sky preview, so no depth ordering can separate
         * them — on the original it never shows, and the only reason it did not
         * show here was PORTAL_SKY_EPS dragging the whole sky forward to
         * overpower it, which is what broke the silhouette everywhere else.
         * Dropping the rim removes the contest instead of trying to win it.
         * OPENPETE_PORTAL_RIM_CULL=0 restores the old behaviour for A/B. */
        {   static int s_cull = -1;
            if (s_cull < 0) { const char* e = getenv("OPENPETE_PORTAL_RIM_CULL");
                              s_cull = (e && *e) ? (*e != '0') : 1; }
            if (s_cull && portal_rim_face(cpu, t)) continue;
        }
        /* OPENPETE_PORTAL_RIM_SHOW=1: paint classified faces one colour per
         * failure COMBINATION, so the picture says which TERM is responsible
         * rather than merely that something was rejected. A yes/no highlight
         * left every miss to inference; this is what made the predicate
         * debuggable. Pair with OPENPETE_PORTAL_RIM_CULL=0 — the cull runs
         * first, so accepted faces are already gone by here — and with
         * --render-paths terrain, which excludes the previews that would
         * otherwise z-fight the very faces under test. */
        {   static int s_show = -1;
            if (s_show < 0) { const char* e = getenv("OPENPETE_PORTAL_RIM_SHOW");
                              s_show = (e && *e && *e != '0'); }
            if (s_show) {
                const int cls = portal_rim_classify(cpu, t);
                if (cls >= 0) {
                    /* vertex rgba packs R in the low byte:
                     *   RED     accepted (all terms pass)
                     *   BLUE    contain            GREEN   distance
                     *   CYAN    contain+distance   YELLOW  angle
                     *   MAGENTA contain+angle      ORANGE  distance+angle
                     *   WHITE   all three */
                    static const uint32_t k_rim_cols[8] = {
                        0xFF0000FFu, 0xFFFF0000u, 0xFF00FF00u, 0xFFFFFF00u,
                        0xFF00FFFFu, 0xFFFF00FFu, 0xFF0080FFu, 0xFFFFFFFFu,
                    };
                    tri.textured = 0;
                    const uint32_t c = k_rim_cols[cls & 7];
                    for (int k = 0; k < 3; k++) tri.v[k].rgba = c;
                }
            }
        }
        openpete_native_scene_add_tri(&tri, t->fill ? OPENPETE_SCENE_WORLD_FILL
                                                    : OPENPETE_SCENE_WORLD, 0);
        openpete_native_scene_set_tag(0);
        /* PSX mask-bit mirror: re-feed .s-deferred (near-band) tris as the
         * portal guard, drawn after the portal sky (see the category doc).
         * Opaque only (the defer test excludes ABE); gated on a portal having
         * fed sky within the last couple of presents. */
        {   /* OPENPETE_PORTAL_GUARD=1 (EXPERIMENT, default OFF): re-feed
             * .s-deferred tris as the portal guard. Disabled by default: at
             * magic-crafters f3000 the PsyCross reference shows the preview
             * covering MOST deferred faces (only the arch pillar behaves
             * protected), so redrawing all of them regressed the probes by
             * ~+12k strong-diff px — the mask/drain model is incomplete
             * (open question: PsyCross FBO stencil attachment + drain-vs-
             * DrawOTag split order). */
            static int gen = -1;
            if (gen < 0) { const char* e = getenv("OPENPETE_PORTAL_GUARD"); gen = (e && *e && *e != '0'); }
            if (gen && t->guard && tri.blend == 0 && openpete_portal_sky_recent())
                openpete_native_scene_add_tri(&tri, OPENPETE_SCENE_PORTAL_GUARD, 0);
        }
        openpete_native_scene_set_fog_delta(0);   /* never leak past this feeder */
    }
    openpete_native_scene_set_presnap(0);   /* never leak past this feeder */
    {   /* guard census (debug, stderr-throttled) */
        static int cen = -1; static int lines;
        if (cen < 0) { const char* e = getenv("OPENPETE_PORTAL_GUARD_DBG"); cen = (e && *e && *e != '0'); }
        if (cen && lines < 40 && openpete_frame >= 2980) {
            int g = 0;
            for (int i = 0; i < s_tri_n; i++) if (s_tri[i].guard) g++;
            lines++;
            fprintf(stderr, "[portal-guard] f=%u tris=%d guard=%d recent=%d defer_n=%d\n",
                    openpete_frame, s_tri_n, g, openpete_portal_sky_recent(), s_defer_n);
        }
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
            if (s_fogdbg) {
                static int pr; if (pr < 6000) { pr++;
                    fprintf(stderr, "[colcmp] xy=(%.0f,%.0f) m3=%.1f native=%06X psx=%06X  dRGB=(%d,%d,%d)\n",
                            s_cvx[i], s_cvy[i], s_cvz[i], n, crgb, dr, dg, db); }
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

    /* OPENPETE_ENV_LODDIST=<int>: substitute g_Environment.m_LodDistance for
     * the DURATION OF THE TERRAIN RENDER CALL ONLY (poke, call, restore).
     * Measurement legs only (never a shipping mode): saturating it
     * (0x7FFFFFFF) makes the .s drop ALL LP and keep HP everywhere, so a
     * PsyCross -og capture becomes a pure-HP reference — "does authored HP
     * cover this pixel" from the game's own renderer, the ground-truth leg
     * for extended-LOD fidelity and the fill-tag sweep ({og-orig covers ∧
     * og-sat uncovered} = LP-only sightlines, a level-data property).
     * The bracket keeps every non-terrain reader of the field canonical.
     * Value ceiling: past ~2x the authored 0x8000 the big swamp levels
     * (misty_bog/beast_makers/ice_cavern) exhaust the frame's prim budget
     * under all-HP sector lists — empty renders, OT-walk hangs, or crashes —
     * so measurement legs use 0x10000, which is already far beyond anything
     * visible through fog. */
    static int s_lodpoke = -2;
    if (s_lodpoke == -2) { const char* e = getenv("OPENPETE_ENV_LODDIST");
                           s_lodpoke = (e && *e) ? (int)strtol(e, NULL, 0) : -1; }
    uint32_t lodsave = 0;
    if (s_lodpoke >= 0) {
        lodsave = cpu->read_word(G_ENVIRONMENT + ENV_LODDIST);
        cpu->write_word(G_ENVIRONMENT + ENV_LODDIST, (uint32_t)s_lodpoke);
    }
    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_800258F0(cpu);                            /* run the real renderer (verdicts intact) */
    if (s_lodpoke >= 0)
        cpu->write_word(G_ENVIRONMENT + ENV_LODDIST, lodsave);
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_TERRAIN, startp, endp);

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_TERRAIN)) {
        terrain_build(cpu, occGroup);               /* native walk + tris */
        openpete_native_scene_begin_frame(openpete_frame);  /* frame-keyed (composes) */
        terrain_feed_native(cpu);                   /* push tris to the scene */
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
 * — extract-only (no func_800258F0 call, no verdict writes). Terrain is mostly
 * static world geometry (the interpolated camera alone moves it smoothly), but
 * poly-anim sectors (the Sunny-Flight wall) have their verts INTPL-rewritten in
 * RAM once per canonical tick — those lerp prev→cur decoded banks at the
 * spine's `alpha` (tsc_vert_anim_refresh) so they move every present instead
 * of stepping at 29.97 Hz. `interp_on` = camera bracketed (else snap to cur,
 * matching the rest of the scene on unpaired/camera-cut presents). */
void terrain_native_reextract(CPUState* cpu, double alpha, int interp_on) {
    if (!openpete_native_render_enabled()) return;
    if (!render_path_enabled(RP_TERRAIN)) return;
    /* Reproduce terrain ONLY if the canonical frame drew it. During a level
     * transition the game stops calling func_800258F0 (no world to render), but
     * s_terrain_saved_occGroup + the level data still in RAM are stale; replaying
     * them paints the old level's geometry over the cyclorama. The canon-drawn
     * mask is clear for RP_TERRAIN on those frames, so we skip. */
    if (!render_path_canon_drawn(RP_TERRAIN)) return;
    /* OPENPETE_TERRAIN_VANIM=0: snap poly-anim verts to cur (A/B)
     * OPENPETE_TERRAIN_TANIM=0: snap UV-scroll texture anim to cur (A/B) */
    static tn_envvbl_t vanim_c, tanim_c;
    int s_vanim_en = tn_env_vbl(&vanim_c, "OPENPETE_TERRAIN_VANIM", 1, 1);
    s_tanim_en = tn_env_vbl(&tanim_c, "OPENPETE_TERRAIN_TANIM", 1, 1);
    s_vanim_sub = 1; s_vanim_on = interp_on && s_vanim_en; s_vanim_alpha = alpha;
    terrain_build(cpu, s_terrain_saved_occGroup);
    terrain_feed_native(cpu);
    s_vanim_sub = 0; s_vanim_on = 0; s_tanim_en = 0;
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
