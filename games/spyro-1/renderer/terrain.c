// games/spyro-1/renderer/terrain.c — native world/terrain renderer.
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
#include "psx_runtime_internal.h"   /* full CPUState (read_word, gte_ctrl, ...) */
#include "openpete_native_render.h"
#include "openpete_psx.h"
#include "openpete_subtick_view.h"  /* openpete_psx_to_host_ptr (sector colour host reads) */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/reextract.h"  /* sub-tick re-extract entry */
#include "openpete_interp.h"         /* openpete_interp_present_now (scroll-anim alpha) */
#include "openpete_interp_objs.h"    /* per-descriptor scroll snapshots */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "no_lod.h"  /* openpete_no_lod + openpete_fog_scale (--fog-scale) */
#include "features/levelwarp.h" /* level-id -> verdicts-file spelling (fill-tag pick) */
#include "openpete_fillpick.h"       /* LP fill-tag inspector (P): session verdicts + pick */
#include "rhi.h"                     /* rhi_terrain_vertex (the chunk packer) */
#include "openpete_terrain_cache.h"  /* terrain sector-mesh GPU cache (retained terrain) */

#include "renderer/portal.h" /* portal-guard gate + the sky-plane provider (OT-conformance clamp) */
#include "texture_pack.h"           /* identity resolve for the record dumper */
#include "openpete_mods.h"          /* untextured-band material resolve (S8c) */
#include "openpete_native_vram.h"   /* VRAM model reads for the record dumper */
#include "openpete_workpool.h"      /* parallel sector walk (--extract-threads) */
#include "openpete_profile.h"       /* ter.* sub-scopes (--profile-out) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"
#include "game_layout.h"          /* game_layout: the engine RAM addresses this walk reads */

extern void     func_800258F0(CPUState* cpu);  /* the real world renderer */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map / recompiled C / r_environment.s) -------- */


/* Sub-tick UV-scroll offsets (see tsc_scroll_refresh). The scrolling-texture
 * animation writes WHOLE TEXELS into the tiledef V bytes once per canonical
 * tick, so waterfalls step at 29.913 Hz while the world around them interpolates.
 * These tables carry the fractional V offset — in texels, relative to the
 * integer decode the game committed — for the tiles that scrolled this tick.
 * They are filled only on sub-tick re-extract builds and zeroed on the canonical
 * path, so the 30 Hz render stays byte-exact. */
static uint8_t s_tanim_act[128];      /* tile carries a fractional offset this build */
static float   s_tanim_dvlq[128];     /* LQ per-quad tiledef  (game writes scroll>>2) */
static float   s_tanim_dvhq[128][2];  /* HQ record entries 1-2 / 3-4 (scroll>>1) */
static int     s_tanim_have;          /* any tile active — skips the per-face test */
static int     s_tanim_en;            /* OPENPETE_TERRAIN_TANIM latch (re-extract only) */
/* Tiles on the live scroller list, refreshed EVERY build (canonical included).
 * The retained-GPU path claims any sector touching one: a baked mesh can
 * neither ride the per-present fractional offset above, nor track the HQ
 * sibling record, which advances even on ticks where the 16-byte LQ entry is
 * byte-identical (scroll>>2 unchanged) — so the LQ-snapshot generations that
 * schedule rebuilds legitimately miss those advances. */
static uint8_t s_tscroll_tile[128];
static int     s_tscroll_have;

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
    OP_PROF_ACCUM_SCOPE("terrain.capture");
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
    uint32_t sectorPtr = rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = rd(cpu, game_layout.env.sector_count);
    uint32_t occCnt    = rd(cpu, game_layout.env.occ_group_count);
    uint32_t texCnt    = rd(cpu, game_layout.env.tex_count);
    uint32_t lqTex     = rd(cpu, game_layout.env.lq_tex);
    uint32_t hqTex     = rd(cpu, game_layout.env.hq_tex);
    int lodDist        = (int)rd(cpu, game_layout.env.lod_dist);
    int cullDist       = (int)rd(cpu, game_layout.env.cull_dist);
    int camX = (int)rd(cpu, game_layout.cam.position);
    int camY = (int)rd(cpu, game_layout.cam.position + 4u);
    int camZ = (int)rd(cpu, game_layout.cam.position + 8u);

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
        uint8_t vis = (uint8_t)(rd(cpu, (game_layout.visibility.base + i) & ~3u) >> ((i & 3) * 8));
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

/* Projection params (file-static so the clip/emit path can re-project clipped
 * vertices). Set per frame in terrain_build. */
static double s_ofx, s_ofy, s_H;

/* Float RTPS mirror, TR=0: camera-space MAC = (M*V)>>12 = ngte_mac
 * (shared). SX/SY = OFX/OFY + MAC1/2*H/MAC3 (proj_screen). The LP-world/16 vs
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
    uint8_t nearband;                              /* .s near-band classified: no og key */
    uint8_t fill;                                  /* LP hole-fill backing -> WORLD_FILL */
    uint8_t fptag;                                 /* fill-tag inspector: 1 = listed FILL, 2 = preview candidate */
    int16_t sec, face;                             /* source identity (fardbg attribution) */
    uint8_t env;                                   /* env-anim tile member (provenance tag) */
    float otkey;                                   /* parent face's .s OT key (full-world mean
                                                    * vz of the 4 corners; portal conformance) */
    uint16_t otslots;                              /* .s OT-slot depth offset for the TEST depth:
                                                    * LP 0x40+8*((w1>>3)&0x1F), HP 4*((w3>>3)&0x7);
                                                    * 1 slot = 128 world units */
} ttri;
static int s_emit_noHP = 0;     /* tier of the sector currently being emitted */
static int s_emit_guard = 0;    /* current face is .s-DEFERRED (near band) -> portal-guard dup */
static int s_emit_fill = 0;     /* current pass is the LP hole-fill backing */
static int s_emit_fptag = 0;    /* current LP face's fill-tag class (inspector tint) */
static int s_emit_sec = -1, s_emit_face = -1;  /* fardbg identity of the current face */
static int s_emit_env = 0;      /* current face is in the env-anim rewrite set */
static float s_emit_otkey = 0.0f;  /* current face's .s OT key (portal conformance) */
static uint16_t s_emit_otslots = 0;  /* current face's authored OT-slot offset */
static uint8_t  s_emit_nearband = 0; /* current face is .s near-band (defer/HQ classified):
                                      * NEVER an OT face, so it neither emits nor contests
                                      * the og key raster — near geometry resolves by
                                      * TRUE depth, like the mask-bit-protected defer queues */
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
     * (magic_crafters waterfall room). The .s NCLIP verdict — the
     * exact two-half quad rule, see the emit-site comment — is the canonical
     * visibility rule; the old "dropped ~half the faces" failure was the
     * single-triple mirror missing the second-half acceptance.
     * OPENPETE_TERRAIN_CULL is a bitmask: bit0 = HP cull, bit1 = LP cull,
     * both default ON. Each tier mirrors its own walk's exact per-half NCLIP
     * rule (HP: (i0,i1,i3)/(i2,i1,i3) with the w3 flip/two-sided bits; LP:
     * (i0,i1,i2)/(i3,i1,i2), fixed strict signs, w0 bit7 double-sided — see
     * the emit-site comment). The old "mixed winding" LP failure was the HP
     * corner order and sign applied to the LP walk. "0" disables everything. */
    static tn_envvbl_t cc, fc;
    s_cull = tn_env_vbl(&cc, "OPENPETE_TERRAIN_CULL", 3, 0);
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
 * title-vista captured prims (fog A/B: implied-ir0 regression
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
 * SHIPPED DEFAULT: the EXACT decode. The old
 * "exact worsens the vista (9.0→13.0)" park was a two-error cancellation:
 * PSX terrain fog is TWO channels — the DPCS vertex blend here AND a
 * per-face CLUT-ROW ladder of pre-baked progressively-fogged palettes
 * (see the .s texture-fog select at the face loop) — and native had
 * neither; the under-fogged lerp partially compensated for the missing
 * palette fog. With both channels implemented the exact decode is strictly
 * closer (identity join: 100% within ±4 at EVERY band; the lerp is Δ67-112
 * off in deep fog). OPENPETE_TERRAIN_FOGDPCS=0 restores the lerp (A/B);
 * OPENPETE_TERRAIN_LOD_HANDOFF=0 restores HP-everywhere beyond fog saturation (A/B).
 * Colour words are 0x00BBGGRR (R=byte0), matching the captured prim RGB. LP
 * faces are NEVER fogged in the .s (raw working palette at all distances —
 * identity-join-proven); noHP=1 keeps that. */
static double s_fogscale = -1.0;    /* SZ = fogscale·m3; shipped default 1.0 (see above) */
static int s_fogdpcs = -1;          /* OPENPETE_TERRAIN_FOGDPCS: GTE-exact DPCS blend */
static int s_lod_handoff = -1;      /* OPENPETE_TERRAIN_LOD_HANDOFF: HP->LP swap beyond fog saturation, default on */
static int s_gapfill_en = -1;       /* OPENPETE_TERRAIN_GAPFILL (near-LP-gap fill, default on) */
static int s_tier_faithful = -1;    /* OPENPETE_TERRAIN_LOD_FAITHFUL: .s sector binning + exact fog */
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

/* ---- LP fill-tag inspector: the crosshair pick (openpete_fillpick.h) -------
 * "Which face am I looking at, and does authored HP cover this sightline?" —
 * answered in the walk's own projected space rather than by a world-space ray.
 * The pick point is the projection centre (OFX, OFY): that IS the optical
 * axis, so it lands on the window centre at any aspect and under the 240->224
 * overscan crop, and nothing has to reconstruct the (non-orthonormal) Spyro
 * camera basis. Every face the walk projects is tested, drawn or dropped, so
 * the LP face that WOULD cover a hole is pickable before it is tagged.
 *
 * Two winners are kept per present: the nearest LP face (the fill-tag
 * candidate) and the nearest HP face (the coverage question). Their depths
 * settle the verdict — no HP hit means nothing authored covers the sightline
 * (FILL); an HP hit BEHIND the LP means the coarse LP is punching through
 * cover that already exists (NOFILL). */
/* The pick point set. Point 0 is the crosshair, published to the overlay;
 * OPENPETE_FILLPICK_GRID="COLSxROWS" adds a grid over the visible frame so one
 * pose enumerates every fill-needed sightline in view instead of one at a
 * time. Frame extents come from the projection itself: OFX is the horizontal
 * centre (so the half-width is OFX) and the visible band is the 224 lines
 * centred on OFY, after the 240->224 overscan crop. */
#define FP_MAX_PTS 4096
typedef struct {
    double px, py;
    int    lp_sec, lp_face, lp_quad, lp_cand;
    double lp_z;
    int    hp_sec, hp_face;
    double hp_z;
} fp_point;

static int      s_fp_on;                  /* inspector engaged for this walk */
static fp_point s_fp_pt[FP_MAX_PTS];
static int      s_fp_np;
static double   s_fp_bx0, s_fp_by0, s_fp_bx1, s_fp_by1;   /* bbox of the point set */
static int      s_fp_grid_cols, s_fp_grid_rows;

/* Lay out this walk's pick points. Point 0 sits at the crosshair, moved by
 * OPENPETE_FILLPICK_XY="dx,dy" — offsets in [-1,1] of the visible half-frame,
 * so a scripted aim needs no knowledge of the frame width at this aspect. */
static void fp_begin(void) {
    double hw = s_ofx, hh = 112.0;        /* visible half-extents, walk frame */
    s_fp_np = 0;
    fp_point* p0 = &s_fp_pt[s_fp_np++];
    memset(p0, 0, sizeof *p0);
    p0->px = s_ofx; p0->py = s_ofy;
    const char* xy = getenv("OPENPETE_FILLPICK_XY");
    if (xy && *xy) {
        double dx, dy;
        if (sscanf(xy, "%lf,%lf", &dx, &dy) == 2) {
            p0->px = s_ofx + dx * hw;
            p0->py = s_ofy + dy * hh;
        }
    }
    s_fp_grid_cols = s_fp_grid_rows = 0;
    const char* g = getenv("OPENPETE_FILLPICK_GRID");
    if (g && *g) {
        int cols = 0, rows = 0;
        if (sscanf(g, "%dx%d", &cols, &rows) == 2 && cols > 0 && rows > 0) {
            if ((long)cols * rows > FP_MAX_PTS - 1) { rows = (FP_MAX_PTS - 1) / cols; }
            s_fp_grid_cols = cols; s_fp_grid_rows = rows;
            /* Cell centres, so a sample never lands on a shared edge. */
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++) {
                    fp_point* p = &s_fp_pt[s_fp_np++];
                    memset(p, 0, sizeof *p);
                    p->px = s_ofx + (2.0 * (c + 0.5) / cols - 1.0) * hw;
                    p->py = s_ofy + (2.0 * (r + 0.5) / rows - 1.0) * hh;
                }
        }
    }
    s_fp_bx0 = s_fp_by0 = 1e30; s_fp_bx1 = s_fp_by1 = -1e30;
    for (int k = 0; k < s_fp_np; k++) {
        fp_point* p = &s_fp_pt[k];
        p->lp_sec = p->lp_face = p->hp_sec = p->hp_face = -1;
        if (p->px < s_fp_bx0) s_fp_bx0 = p->px;
        if (p->px > s_fp_bx1) s_fp_bx1 = p->px;
        if (p->py < s_fp_by0) s_fp_by0 = p->py;
        if (p->py > s_fp_by1) s_fp_by1 = p->py;
    }
}

/* Keep each point's nearest hit of one projected triangle. Perspective-correct
 * depth: 1/z interpolates linearly in screen space, so the reciprocal of the
 * barycentric-weighted reciprocals is the exact surface view-Z at the sample
 * for a planar triangle. */
static void fp_hit_tri(int lp, int sec, int face, int quad, int cand,
                       const lvert* A, const lvert* B, const lvert* C) {
    if (A->m3 <= 1e-6 || B->m3 <= 1e-6 || C->m3 <= 1e-6) return;  /* straddles the camera plane */
    double ax = A->sx, ay = A->sy, bx = B->sx, by = B->sy, cx = C->sx, cy = C->sy;
    /* Screen bbox reject against the whole point set first: with a grid armed
     * this is what keeps the sweep from costing points x faces. */
    double tx0 = ax < bx ? ax : bx; if (cx < tx0) tx0 = cx;
    double tx1 = ax > bx ? ax : bx; if (cx > tx1) tx1 = cx;
    double ty0 = ay < by ? ay : by; if (cy < ty0) ty0 = cy;
    double ty1 = ay > by ? ay : by; if (cy > ty1) ty1 = cy;
    if (tx1 < s_fp_bx0 || tx0 > s_fp_bx1 || ty1 < s_fp_by0 || ty0 > s_fp_by1) return;
    double d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (d > -1e-9 && d < 1e-9) return;                            /* degenerate on screen */
    for (int k = 0; k < s_fp_np; k++) {
        fp_point* P = &s_fp_pt[k];
        if (P->px < tx0 || P->px > tx1 || P->py < ty0 || P->py > ty1) continue;
        double w0 = ((by - cy) * (P->px - cx) + (cx - bx) * (P->py - cy)) / d;
        double w1 = ((cy - ay) * (P->px - cx) + (ax - cx) * (P->py - cy)) / d;
        double w2 = 1.0 - w0 - w1;
        if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
        double inv = w0 / A->m3 + w1 / B->m3 + w2 / C->m3;
        if (inv <= 1e-12) continue;
        double z = 1.0 / inv;
        if (lp) {
            if (P->lp_sec < 0 || z < P->lp_z) {
                P->lp_sec = sec; P->lp_face = face;
                P->lp_quad = quad; P->lp_cand = cand; P->lp_z = z;
            }
        } else if (P->hp_sec < 0 || z < P->hp_z) {
            P->hp_sec = sec; P->hp_face = face; P->hp_z = z;
        }
    }
}

/* Both strip triangles of a quad, matching the emit's (i0,i1,i2)+(i1,i2,i3)
 * triangulation so the picked region is exactly the drawn region. */
static void fp_test_face(int lp, int sec, int face, int cand, const lvert* sv,
                         int i0, int i1, int i2, int i3, int quad) {
    fp_hit_tri(lp, sec, face, quad, cand, &sv[i0], &sv[i1], &sv[i2]);
    if (quad) fp_hit_tri(lp, sec, face, quad, cand, &sv[i1], &sv[i2], &sv[i3]);
}

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

/* ---- parallel sector walk (--extract-threads) ------------------------------
 * The face walk fans out over visible sectors on the extraction worker pool.
 * Each worker owns a tn_wctx: its output triangle buffer, the per-sector
 * projection scratch, the ambient per-face emit state the push path reads,
 * and every census counter the walk increments. Workers only read shared
 * state that the build prologue latched (matrices, env toggles, the decoded
 * sector caches — resolved serially in the prepass); everything they WRITE
 * lives here. Per-sector output runs are recorded and merged into s_tri in
 * sector order, so the merged stream is byte-identical to the serial walk. */
typedef struct {
    ttri* tri; int tri_n, tri_cap;      /* output (serial ctx aliases s_tri) */
    lvert sv[256];                       /* per-sector projected verts */
    /* ambient per-face emit state (consumed by push_tri) */
    int emit_noHP, emit_guard, emit_fill, emit_fptag, emit_sec, emit_face, emit_env;
    float emit_otkey; uint16_t emit_otslots; uint8_t emit_nearband;
    /* census (summed into the s_dbg_* / s_tex_* globals after the walk) */
    int dbg_faces, dbg_oob, dbg_nclip, dbg_behind, dbg_spike, dbg_clipped;
    int tex_faces, tex_tris;
    int near_q_agree, near_c_only;
    int walk_sectors;
    int serial;   /* 1 = single-thread walk: also fills the s_tv/s_cv A/B arrays */
} tn_wctx;

/* (tn_wcfg — the per-walk shared-input struct — is defined further down,
 * after tscache_t which it references.) */

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
static void push_tri(tn_wctx* W, cvert a, cvert b, cvert c, const tmat* m) {
    if (W->tri_n >= W->tri_cap) return;
    cvert cc[3] = { a, b, c };
    ttri* t = &W->tri[W->tri_n++];
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
        if (W->serial && s_cv_n < MAX_TTRIS * 3) {   /* colour-A/B verts (verify) */
            s_cvx[s_cv_n]=t->sx[k]; s_cvy[s_cv_n]=t->sy[k]; s_cvz[s_cv_n]=cc[k].m3; s_cvrgb[s_cv_n]=cc[k].rgb; s_cv_n++;
        }
    }
    t->textured=m->textured; t->tpx=m->tpx; t->tpy=m->tpy;
    t->clx=m->clx; t->cly=m->cly; t->bpp=m->bpp; t->blend=m->blend;
    t->fog_delta=m->fog_delta;
    t->texid=(int16_t)(m->textured ? m->texid : -1);
    t->tier = W->emit_noHP;
    t->guard = (uint8_t)W->emit_guard;
    t->fill = (uint8_t)W->emit_fill;
    t->fptag = (uint8_t)W->emit_fptag;
    t->sec = (int16_t)W->emit_sec; t->face = (int16_t)W->emit_face;
    t->env = (uint8_t)W->emit_env;
    t->otkey = W->emit_otkey;
    t->otslots = W->emit_otslots;
    t->nearband = W->emit_nearband;
    if (m->textured) W->tex_tris++;
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
static void clip_push_tri(tn_wctx* W, cvert a, cvert b, cvert c, const tmat* m) {
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
        if (nout < 3) { W->dbg_behind++; return; } /* fully behind the near plane */
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
        if (clipped) W->dbg_clipped++;
        push_tri(W, out[0], out[1], out[2], m);         /* fan the clipped polygon */
        if (nout == 4) push_tri(W, out[0], out[2], out[3], m);
        return;
    }
}

/* Emit a triangle (sv[] index form) with near-plane clipping. Replaces the old
 * behind-drop + spike cull: near/straddling faces are clipped & rendered. */
static void emit_tri(tn_wctx* W, const lvert* sv, int a, int b, int c,
                     lcorner ka, lcorner kb, lcorner kc, const tmat* m) {
    cvert A = { sv[a].m1, sv[a].m2, sv[a].m3, ka.rgb, ka.u, ka.v };
    cvert B = { sv[b].m1, sv[b].m2, sv[b].m3, kb.rgb, kb.u, kb.v };
    cvert C = { sv[c].m1, sv[c].m2, sv[c].m3, kc.rgb, kc.u, kc.v };
    clip_push_tri(W, A, B, C, m);
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
static void emit_quad_cv(tn_wctx* W, cvert tl, cvert tr, cvert bl, cvert br,
                         const tmat* m, const float u[4], const float v[4]) {
    tl.u=u[0]; tl.v=v[0]; tr.u=u[1]; tr.v=v[1]; bl.u=u[2]; bl.v=v[2]; br.u=u[3]; br.v=v[3];
    /* Portal OT-conformance key: near-subdivided (deferred) faces are drained
     * into the OT as 4 SEPARATE sub-quad prims, EACH bucketed by its own
     * corner sum — the .s sky-vs-floor boundary at a portal base is at
     * SUB-QUAD granularity (og f2440: the walkway's near sub-quads paint over
     * the preview, its far ones are covered). Override the parent face's key
     * with this sub-quad's own. */
    W->emit_otkey = (float)((tl.m3 + tr.m3 + bl.m3 + br.m3) * 4.0);
    clip_push_tri(W, tl, tr, bl, m);
    clip_push_tri(W, tr, bl, br, m);
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

static void terrain_near_emit(tn_wctx* W, CPUState* cpu, uint32_t hqBase, int texid, int abe,
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
        emit_quad_cv(W, q[i][0], q[i][1], q[i][2], q[i][3], &mat, u, v);
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
 * core principle forbids consuming that; and under the retired recompiler-side
 * m_LodDistance saturation it was outright garbage via perturbed scratch-buffer
 * reuse. The queue is kept ONLY as a verify-path oracle to calibrate this
 * against.)
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
/* The CANONICAL .s foreground classification (no --no-lod pin): true iff the
 * canonical walk would route this face to the near/HQ defer queues — i.e. it
 * never receives a main-OT index. Consumers that model og's OT (the authored
 * bias gate) must use THIS; the render-routing wrapper below layers the
 * --no-lod always-HQ pin on top. */
static int face_is_near_canonical(double z0, double z1, double z2, double z3, uint32_t word3) {
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
    if (sum >= 8192.0) return 0;                       /* not foreground (0x2000) */
    double mn = sz0; if (sz1<mn) mn=sz1; if (sz2<mn) mn=sz2; if (sz3<mn) mn=sz3;
    if (mn < 320.0) return 1;                          /* a corner SZ3 < 0x140 → tier 4 */
    return !(word3 & 0x40);                            /* else tier 2 unless flag 0x40 */
}
static int face_is_near(double z0, double z1, double z2, double z3, uint32_t word3) {
    /* --no-lod: pin the bpp8 HQ near-subdivide ON for every eligible face,
     * regardless of distance, so the coarse bpp4 LQ far tier is never shown. The
     * canonical game switches LQ→HQ only inside the 0x2000 foreground band;
     * under --no-lod we drop all the distance-derived gates and keep only the
     * two that aren't distance: the behind-camera guard and the word3 & 0x80
     * hard-exclusion (those faces never carry a valid HQ Tiledef). Quads only —
     * near TRIANGLES still fall to the GT3 LQ emit (the existing
     * detail-descriptor carve-out, see the near-dispatch site). Display-only;
     * reads no gameplay state. */
    if (openpete_no_lod) {
        if (s_hqscale < 0.0) {
            const char* e = getenv("OPENPETE_TERRAIN_HQSCALE"); s_hqscale = (e && *e) ? atof(e) : 4.0;
        }
        double s = s_hqscale;
        double sz0 = z0 > 0.0 ? z0*s : 0.0, sz1 = z1 > 0.0 ? z1*s : 0.0;
        double sz2 = z2 > 0.0 ? z2*s : 0.0, sz3 = z3 > 0.0 ? z3*s : 0.0;
        if (sz0 + sz1 + sz2 + sz3 <= 0.0) return 0;    /* FULLY behind camera */
        return !(word3 & 0x80);
    }
    return face_is_near_canonical(z0, z1, z2, z3, word3);
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
    uint8_t  gapfill;                              /* LP only: frozen fill-tag face (terrain_fill_tags.h) */
    uint8_t  fillcand;                             /* LP only: gapfill set by the inspector preview, not the list */
    uint8_t  floorf;                               /* LP only: up-facing floor by winding (n_z < 0, |n_z| > 0.8) */
    uint8_t  admit;                                /* LP only: authored admission field w0 & 0x1F */
    uint8_t  dside;                                /* LP only: w0 & 0x80 — double-sided, emits past both NCLIP halves */
    uint8_t  otbias;                               /* authored OT-bias field: LP (w1>>3)&0x1F,
                                                    * HP (w3>>3)&0x7 (slot units before x8/x4) */
    int8_t   tb;                                   /* HP texByte (int8 of w2; -1 = untextured) */
    uint32_t w3;                                   /* HP word3 (face_is_near gates) */
    uint32_t tgen;                                 /* s_tgen[tile] at texmat decode (anim liveness) */
    uint32_t tgenHq;                               /* s_tgen[hq_idx] at last refresh — the scroll
                                                    * accumulator gen for the HQ sibling record
                                                    * (bake liveness; the CPU near path reads it live) */
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
    int nLpC, nHpC;                          /* colour-array entry counts (GPU-cache liveness hash) */
    int nGapfill;                            /* LP faces classified HP-coverage-gap fillers */
    unsigned fillGen;                        /* openpete_fillpick_generation() at last tagging */
    int originX, originY, originZ;
    int centreX, centreY, centreZ, radius;   /* header bounding sphere (w0/w1) — the .s LP/HP binning inputs */
    /* Vertex AABB over BOTH tiers' decoded verts (origin-relative decode
     * units, same frame the walk's per-vertex transform consumes). The
     * near-claim refinement transforms its 8 corners: view-Z is linear in
     * xyz, so the corner minimum bounds every vertex — a sector whose
     * box-min m3 stays at or beyond the claim threshold provably contains
     * no vertex the sphere test was guarding against. Invalid (vMinX >
     * vMaxX) when the sector decoded no verts. */
    int16_t vMinX, vMinY, vMinZ, vMaxX, vMaxY, vMaxZ;
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
    /* Pack-aware LP grade (tsc_lp_grade): face-weighted mean stock->pack
     * affine of this sector's HP textures. 0 = uncomputed, 1 = fit,
     * 2 = no signal (untextured/unmatched sector — level mean applies). */
    float lpg_p[3], lpg_q[3];
    uint8_t lpg_state; unsigned lpg_gen;
    /* GPU sector-mesh cache liveness: decodeGen bumps on every (re)decode of
     * this slot (a global monotonic counter, so slot reuse for a different
     * sector always mismatches); texGen when the env texture anim re-decoded
     * any cached texmat; vertGen when the geometry env-anim actually CHANGED
     * the decoded verts (content hash, not the unconditional per-tick
     * re-decode). */
    uint32_t decodeGen, texGen, vertGen, vertHash;
} tscache_t;
#define TSC_SLOTS 512
/* Linear-probe window for lookup, placement, and eviction (must agree
 * everywhere a sector's slot is derived from its hash). Sized so a fully
 * GPU-owned window — the one case eviction cannot pick a victim without
 * un-drawing an owner — needs 17 owned sectors hashing together. */
#define TSC_WINDOW 16u
static tscache_t s_tsc[TSC_SLOTS];
static int tgpu_slot_hot(uint32_t idx);   /* GPU-owned this present (defined with s_tgpu) */
static uint32_t s_tsc_stamp;                 /* bumped once per terrain_build call */
static long s_tsc_hit, s_tsc_miss;

/* ---- pack-aware LP grade ---------------------------------------------------
 * LP colours are AUTHORED against the sector's stock textures: raw at every
 * distance, they read seamlessly against fully-fogged stock HP — the .s
 * LP/HP handoff is invisible purely by that colour agreement. A replacement
 * pack regrades the textured tiers and breaks it: the handoff contour shows
 * as a stepped bright-vs-dark seam. Restore the authoring relationship by
 * regrading LP the way the pack regraded THIS sector's own ground: the
 * face-weighted mean of the sector's HP textures' stock->pack affine fits
 * (texture_pack_grade_fit). Sectors with no textured/matched HP (the LP-only
 * 0x4000 hedges) take the level mean of the fitted sectors, accumulated per
 * walk (one-present lag on the very first frame). Display-only; inert
 * without an active pack. OPENPETE_TEXPACK_LP_FIT=0 disables (A/B). */
static int    s_lpg_en = -1;
static int    s_lpg_active;                  /* this walk: enabled && pack active */
static double s_lpg_acc[6];                  /* this walk's Σp,Σq (level mean) */
static int    s_lpg_acc_n;
static float  s_lpg_mean[6];                 /* last walk's mean (p,q) */
static int    s_lpg_mean_ok;

static void tsc_lp_grade(tscache_t* C) {
    unsigned gen = texture_pack_generation();
    if (C->lpg_state && C->lpg_gen == gen) return;
    C->lpg_gen = gen; C->lpg_state = 2;
    if (!C->hpF) return;
    double sp[3] = {0}, sq[3] = {0};
    int n = 0;
    for (int f = 0; f < C->nHpF; f++) {
        const tsface_t* F = &C->hpF[f];
        const tmat* m = &F->mat;
        if (!m->textured) continue;
        float u0 = F->tu[0], v0 = F->tv[0], u1 = u0, v1 = v0;
        for (int k = 1; k < 4; k++) {
            if (F->tu[k] < u0) u0 = F->tu[k];
            if (F->tu[k] > u1) u1 = F->tu[k];
            if (F->tv[k] < v0) v0 = F->tv[k];
            if (F->tv[k] > v1) v1 = F->tv[k];
        }
        texture_pack_id id; texture_pack_repl repl;
        if (!texture_pack_resolve(m->tpx, m->tpy, m->clx, m->cly, m->bpp,
                                  (int)u0, (int)v0, (int)u1, (int)v1, &id)) continue;
        if (!texture_pack_match(&id, &repl)) continue;
        float p[3], q[3];
        if (!texture_pack_grade_fit(&id, &repl, m->tpx, m->tpy, m->clx, m->cly,
                                    m->bpp, (int)u0 - (id.box_x - repl.box_x),
                                    (int)v0 - (id.box_y - repl.box_y), p, q)) continue;
        for (int c = 0; c < 3; c++) { sp[c] += p[c]; sq[c] += q[c]; }
        n++;
    }
    if (n > 0) {
        for (int c = 0; c < 3; c++) {
            C->lpg_p[c] = (float)(sp[c] / n);
            C->lpg_q[c] = (float)(sq[c] / n);
        }
        C->lpg_state = 1;
    }
}

/* Apply a sector's grade (or the level mean) to one 0x00BBGGRR colour word. */
static uint32_t lp_grade_apply(const tscache_t* C, uint32_t rgb) {
    const float* p; const float* q;
    if (C->lpg_state == 1) { p = C->lpg_p; q = C->lpg_q; }
    else if (s_lpg_mean_ok) { p = s_lpg_mean; q = s_lpg_mean + 3; }
    else return rgb;
    uint32_t out = 0;
    for (int c = 0; c < 3; c++) {
        int v = (int)(p[c] * (float)((rgb >> (8 * c)) & 0xFF) + q[c] * 255.0f + 0.5f);
        if (v < 0) v = 0; if (v > 255) v = 255;
        out |= (uint32_t)v << (8 * c);
    }
    return out;
}
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
            F->otbias = (uint8_t)((w3 >> 3) & 0x7);
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
            F->admit = (uint8_t)(w0 & 0x1F);       /* authored LP admission (crossover in 1024-world steps) */
            F->dside = (w0 & 0x80) != 0;           /* double-sided: draw regardless of facing */
            F->otbias = (uint8_t)((w1 >> 3) & 0x1F);
            F->c0 = (uint16_t)((w1 >> 24) & 0xFC); F->c1 = (uint16_t)((w1 >> 18) & 0xFC);
            F->c2 = (uint16_t)((w1 >> 12) & 0xFC); F->c3 = (uint16_t)((w1 >> 6) & 0xFC);
            F->tb = -1;
            F->mat.bpp = 16;                       /* untextured flat gouraud */
            /* w1 bit 2 = ABE: the .s LP emit (quad 0x80026504 / tri 0x800266D4)
             * ORs 0x02 into the G4/G3 prim code and precedes the prim with a
             * DR_TPAGE carrying ABR = w1 & 3 — untextured SEMI-TRANSPARENT
             * gouraud (the distant-waterfall veils). Same scene blend classes
             * (1..4 = ABR 0..3) as the HP tpage decode. */
            if (w1 & 0x4) F->mat.blend = (int)(w1 & 0x3) + 1;
        }
        F->quad = (F->i2 != F->i3);
    }
}

/* ---- LP fill-face tags (frozen, measurement-derived) -----------------------
 * Which LP faces may back-fill sightlines no authored HP covers is a FROZEN,
 * human-reviewed list (terrain_fill_tags.h, generated by
 * scripts/gen-fill-tags.py from tests/freecam/fill-tags.verdicts). Candidates
 * are measured per pixel from the game's own renderer — og-original coverage
 * minus pure-HP coverage (m_LodDistance doubled) — attributed to LP faces by
 * projected screen footprint, then reviewed face-by-face. FILL faces are
 * og-terrain-over-void (water planes, level-boundary channels); every
 * rejected candidate was a silhouette sliver between the coarse/alternate LP
 * shape and the HP silhouette, where filling paints LP wedges over sky.
 * Absence from the table = not a fill face: a face the sweep never measured
 * fails loud (an unfilled hole at a new camera) instead of silently shipping
 * a guess — the failure mode is visible and local, unlike the threshold
 * classifier this replaces (six tuned constants + a per-level kill list,
 * v1-v12, each version patching a newly discovered misprediction). */
#include "renderer/terrain_fill_tags.h"

static int tft_frozen(int level, int sec, int face) {
    int lo = 0, hi = (int)(sizeof(k_terrain_fill_tags)/sizeof(k_terrain_fill_tags[0])) - 1;
    long q = ((long)level << 20) | ((long)sec << 8) | face;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const tft_entry_t* e = &k_terrain_fill_tags[mid];
        long k = ((long)e->level << 20) | ((long)e->sec << 8) | e->face;
        if (k == q) return 1;
        if (k < q) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* The effective verdict: an in-game inspector session (P key) overrides the
 * frozen table for the faces it has re-judged, so a Y/N press re-renders the
 * scene with the new verdict on the next present and the audit runs against
 * the live image instead of a rebuild. Inert — and free — with no session
 * verdicts recorded. */
static int tft_fill(int level, int sec, int face) {
    int ov = openpete_fillpick_lookup(level, sec, face);
    if (ov != OPENPETE_FILLPICK_NONE) return ov == OPENPETE_FILLPICK_FILL;
    return tft_frozen(level, sec, face);
}

/* Grid sweep report (OPENPETE_FILLPICK_GRID) — one shot per pose, tallying the
 * sample cells each LP face owns and how they split on cover:
 *   bare   = an LP face is the nearest terrain and NO authored HP is on the
 *            sightline. This is the fill case: at og's LOD the LP is what
 *            covers those pixels, and without a FILL tag nothing does.
 *   behind = an HP face is on the sightline but FARTHER than the LP, i.e. the
 *            coarse LP is in front of cover that already exists (the
 *            punch-through class a wrong FILL tag ships).
 *   cover  = HP is nearer; the LP is hidden whatever its tag says.
 * A pose's report is a candidate list, not a verdict: the cells are samples,
 * so a face with a handful of `bare` cells along a silhouette is the sliver
 * class the frozen list rejects, while a face with a broad bare region is the
 * og-terrain-over-void class it accepts. */
static void fp_grid_report(int level, const char* level_name) {
    if (!s_fp_grid_cols) return;
    static int s_done;
    if (s_done) return;
    s_done = 1;
    struct { int sec, face, bare, behind, cover, tagged; } acc[512];
    int n = 0;
    for (int k = 1; k < s_fp_np; k++) {
        const fp_point* P = &s_fp_pt[k];
        if (P->lp_sec < 0) continue;
        int j = 0;
        for (; j < n; j++) if (acc[j].sec == P->lp_sec && acc[j].face == P->lp_face) break;
        if (j == n) {
            if (n >= (int)(sizeof acc / sizeof acc[0])) continue;
            acc[n].sec = P->lp_sec; acc[n].face = P->lp_face;
            acc[n].bare = acc[n].behind = acc[n].cover = 0;
            acc[n].tagged = tft_fill(level, P->lp_sec, P->lp_face);
            n++;
        }
        if (P->hp_sec < 0)          acc[j].bare++;
        else if (P->hp_z > P->lp_z) acc[j].behind++;
        else                        acc[j].cover++;
    }
    fprintf(stderr, "[fillgrid-hdr] %s grid=%dx%d faces=%d\n",
            level_name, s_fp_grid_cols, s_fp_grid_rows, n);
    for (int j = 0; j < n; j++)
        fprintf(stderr, "[fillgrid] %s %d %d tag=%s bare=%d behind=%d cover=%d\n",
                level_name, acc[j].sec, acc[j].face,
                acc[j].tagged ? "FILL" : "NOFILL",
                acc[j].bare, acc[j].behind, acc[j].cover);
}

/* Hand this walk's crosshair winners to the host (overlay + key bindings), and
 * log every change so a scripted/headless run is a transcript. The level name
 * is the verdicts-file spelling, so a logged line is one column short of being
 * a verdicts row. */
static void fp_publish(CPUState* cpu) {
    openpete_fillpick_hit h;
    memset(&h, 0, sizeof h);
    h.level = (int)(int32_t)rd(cpu, game_layout.level_id);
    size_t nlv = 0;
    const spyro1_level_t* lv = spyro1_level_table(&nlv);
    snprintf(h.level_name, sizeof h.level_name, "level%d", h.level);
    for (size_t k = 0; k < nlv; k++)
        if (lv[k].id == h.level) {
            snprintf(h.level_name, sizeof h.level_name, "%s", lv[k].name);
            break;
        }
    const fp_point* X = &s_fp_pt[0];
    if (X->lp_sec >= 0) {
        h.valid = 1;
        h.sec = X->lp_sec; h.face = X->lp_face;
        h.quad = X->lp_quad; h.vz = X->lp_z;
        /* The face's own tag bit is unreliable here: preview sets it on
         * everything. Ask the list. */
        h.verdict = tft_fill(h.level, h.sec, h.face) ? OPENPETE_FILLPICK_FILL
                                                     : OPENPETE_FILLPICK_NOFILL;
        h.overridden = openpete_fillpick_lookup(h.level, h.sec, h.face)
                       != OPENPETE_FILLPICK_NONE;
    }
    if (X->hp_sec >= 0) {
        h.hp_valid = 1;
        h.hp_sec = X->hp_sec; h.hp_face = X->hp_face; h.hp_vz = X->hp_z;
    }
    openpete_fillpick_publish(&h);
    fp_grid_report(h.level, h.level_name);

    static int last_sec = -2, last_face = -2, last_lvl = -2, last_verdict = -2;
    if (h.valid && (h.sec != last_sec || h.face != last_face ||
                    h.level != last_lvl || h.verdict != last_verdict)) {
        last_sec = h.sec; last_face = h.face;
        last_lvl = h.level; last_verdict = h.verdict;
        fprintf(stderr, "[fillpick] %s %d %d  tag=%s%s  lp_vz=%.0f  hp=%s",
                h.level_name, h.sec, h.face,
                h.verdict == OPENPETE_FILLPICK_FILL ? "FILL" : "NOFILL",
                h.overridden ? "*" : "", h.vz,
                h.hp_valid ? "" : "none");
        if (h.hp_valid)
            fprintf(stderr, "s%d/f%d vz=%.0f (%s LP by %.0f)",
                    h.hp_sec, h.hp_face, h.hp_vz,
                    h.hp_vz < h.vz ? "in front of" : "behind",
                    fabs(h.hp_vz - h.vz));
        fputc('\n', stderr);
    }
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
        s_lvl = (int)(int32_t)rd(cpu, game_layout.level_id);
        s_lvl_frame = (int)openpete_frame;
    }
    for (size_t k = 0; k < sizeof(k_lp_artfix)/sizeof(k_lp_artfix[0]); k++)
        if (k_lp_artfix[k].level == s_lvl && k_lp_artfix[k].sec == sec
            && k_lp_artfix[k].face == face) { *rgb = k_lp_artfix[k].rgb; return 1; }
    return 0;
}

/* OPENPETE_TERRAIN_FIELDCENSUS=1 — one-shot per level: walk the FULL sector
 * table directly in emulated RAM (decode is lazy and camera-dependent; the
 * census must not be) and dump every face's authored LOD fields + the packed
 * local verts, for the offline distribution/correlation/coplanar-overlay
 * censuses. Layout mirrors tsc_get/tsc_decode_faces exactly. */
static void tsc_field_census(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    static uint32_t last_key;
    uint32_t ck = sectorPtr ^ (sectorCnt * 2654435761u) ^ 0x5CA1AB1Eu;
    if (ck == last_key) return;
    last_key = ck;
    int level = (int)(int32_t)rd(cpu, game_layout.level_id);
    uint32_t n = sectorCnt > 1024 ? 1024 : sectorCnt;
    fprintf(stderr, "[fc-hdr] level=%d sectors=%u\n", level, n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4); if (!s) continue;
        uint32_t g0 = rd(cpu, s + 0x04), g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int nLpV = (int)(g3 & 0xFF), nLpC = (int)((g3 >> 8) & 0xFF), nLpF = (int)((g3 >> 16) & 0xFF);
        int nHpV = (int)(g4 & 0xFF), nHpC = (int)((g4 >> 8) & 0xFF), nHpF = (int)((g4 >> 16) & 0xFF);
        uint32_t hpVerts = s + 0x1C + ((g4 >> 24) & 0xFF) * 4;
        fprintf(stderr, "[fc-sec] s=%u flags=%04X rad=%d nLpV=%d nLpF=%d nHpV=%d nHpF=%d\n",
                i, (unsigned)(g0 & 0xE000), (int)(g0 & 0x1FFF), nLpV, nLpF, nHpV, nHpF);
        for (int bank = 0; bank < 2; bank++) {
            uint32_t verts = bank ? hpVerts : s + 0x1C;
            int nv = bank ? nHpV : nLpV;
            if (nv <= 0) continue;
            fprintf(stderr, "[fc-%sv] s=%u", bank ? "hp" : "lp", i);
            for (int k = 0; k < nv && k < 256; k++) {
                uint32_t v = rd(cpu, verts + (uint32_t)k * 4);
                fprintf(stderr, " %d,%d,%d", (int)((v >> 21) & 0x7FF),
                        (int)((v >> 10) & 0x7FF), (int)(v & 0x3FF));
            }
            fputc('\n', stderr);
        }
        uint32_t lpFaces = s + 0x1C + (uint32_t)(nLpV + nLpC) * 4;
        for (int f = 0; f < nLpF; f++) {
            uint32_t w0 = rd(cpu, lpFaces + (uint32_t)f * 8);
            uint32_t w1 = rd(cpu, lpFaces + (uint32_t)f * 8 + 4);
            fprintf(stderr, "[fc-lpf] s=%u f=%d a=%u b=%u ds=%u i=%u,%u,%u,%u\n",
                    i, f, (unsigned)(w0 & 0x1F), (unsigned)((w1 >> 3) & 0x1F),
                    (unsigned)((w0 >> 7) & 1),
                    (unsigned)((w0 >> 26) & 0x3F), (unsigned)((w0 >> 20) & 0x3F),
                    (unsigned)((w0 >> 14) & 0x3F), (unsigned)((w0 >> 8) & 0x3F));
        }
        uint32_t hpFaces = hpVerts + (uint32_t)(nHpV + nHpC * 2) * 4;
        for (int f = 0; f < nHpF; f++) {
            uint32_t w0 = rd(cpu, hpFaces + (uint32_t)f * 16);
            uint32_t w3 = rd(cpu, hpFaces + (uint32_t)f * 16 + 12);
            fprintf(stderr, "[fc-hpf] s=%u f=%d b=%u fp=%u fl=%u ds=%u ns=%u i=%u,%u,%u,%u\n",
                    i, f, (unsigned)((w3 >> 3) & 0x7), (unsigned)(w3 & 1),
                    (unsigned)((w3 >> 1) & 1), (unsigned)((w3 >> 2) & 1),
                    (unsigned)((w3 >> 6) & 3),
                    (unsigned)((w0 >> 24) & 0xFF), (unsigned)((w0 >> 16) & 0xFF),
                    (unsigned)((w0 >> 8) & 0xFF), (unsigned)(w0 & 0xFF));
        }
    }
}

static void tsc_tag_gapfill(CPUState* cpu, tscache_t* C,
                            uint32_t sectorPtr, uint32_t sectorCnt) {
    if (getenv("OPENPETE_TERRAIN_FIELDCENSUS"))
        tsc_field_census(cpu, sectorPtr, sectorCnt);
    if (!C->lpF || !C->lpV) return;
    C->nGapfill = 0;
    /* The table keys on the sector INDEX; the cache keys on the sector
     * address — recover the index from the table (decode-time only). */
    int secIdx = -1;
    uint32_t n = sectorCnt > 1024 ? 1024 : sectorCnt;
    for (uint32_t i = 0; i < n; i++)
        if (rd(cpu, sectorPtr + i * 4) == C->s) { secIdx = (int)i; break; }
    int level = (int)(int32_t)rd(cpu, game_layout.level_id);
    /* The credits overlay (func_credits_8007AA50) loads its own scene builds
     * through LoadLevelData, indexed by g_CreditsSequence, and never writes
     * g_LevelId — it keeps the id of the level the credits were entered from.
     * Those scenes are re-indexed subsets of their source levels (no sector
     * keeps its index), so a g_LevelId-keyed lookup lands the entering
     * level's tags on unrelated sectors: an Artisans fill face became the
     * near ground of the Stone Hill credits scene, drawn as untextured LP over
     * the HP. No credits scene has been swept, so under GS_Credits every face
     * is untagged (the table's absent-means-not-a-fill contract). */
    if (rd(cpu, 0x800757D8) == 15u) secIdx = -1;    /* g_Gamestate == GS_Credits */
    for (int f = 0; f < C->nLpF; f++) {
        tsface_t* F = &C->lpF[f];
        F->gapfill = 0;
        if (F->i0 >= C->nLpV || F->i1 >= C->nLpV || F->i2 >= C->nLpV || F->i3 >= C->nLpV) continue;
        {   /* up-facing-floor bit (facemap metadata; game winding: floors have
             * n_z < 0, |n_z| >= 0.8 of the normal). */
            const tsvert_t* a = &C->lpV[F->i0];
            const tsvert_t* b = &C->lpV[F->i1];
            const tsvert_t* c = &C->lpV[F->i2];
            double e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
            double e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
            double nz = e1[0]*e2[1] - e1[1]*e2[0];
            double nx = e1[1]*e2[2] - e1[2]*e2[1];
            double ny = e1[2]*e2[0] - e1[0]*e2[2];
            double nl = nx*nx + ny*ny + nz*nz;
            F->floorf = (nl > 0.0 && nz < 0.0 && nz*nz >= 0.64*nl);
        }
        if (secIdx >= 0) {
            F->gapfill = (uint8_t)tft_fill(level, secIdx, f);
            /* Inspector preview: force every LP face through the gapfill
             * exemptions so the UNTAGGED candidates are visible too — an
             * under-fill site is a hole the covering face does not draw into
             * until it is tagged, so it cannot be found by looking at the
             * tagged set. `fillcand` keeps the two apart for the tint (and
             * keeps the pick honest about what the shipped list says).
             * Deliberately rides `gapfill` rather than a parallel gate: every
             * exemption the tags already have — the sector-level lpGapOnly
             * arm, the authored LP admission, the handoff far-only drop — is
             * then exercised exactly as a real tag would be. */
            F->fillcand = 0;
            if (!F->gapfill && openpete_fillpick_preview()) {
                F->gapfill = 1;
                F->fillcand = 1;
            }
            C->nGapfill += F->gapfill;
        }
    }
}

/* Re-derive a CACHED sector's fill tags. The tags are static level data, so
 * they are normally decoded once — but the inspector's session verdicts and
 * preview mode change what the list says while the level is live, and a
 * sector already in the cache would otherwise keep the old tagging until it
 * is evicted. Keyed on the inspector's generation counter, so this costs one
 * integer compare per cache hit when nothing has changed. */
static void tsc_retag_fill(CPUState* cpu, tscache_t* C,
                           uint32_t sectorPtr, uint32_t sectorCnt) {
    C->fillGen = openpete_fillpick_generation();
    tsc_tag_gapfill(cpu, C, sectorPtr, sectorCnt);
    if (C->polyanim && C->lpF) {          /* stale-bank guard: see tsc_sector_polyanim */
        for (int f = 0; f < C->nLpF; f++) C->lpF[f].gapfill = 0;
        C->nGapfill = 0;
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
        uint32_t off  = list ? game_layout.env_anim.hp_poly : game_layout.env_anim.lp_poly;
        uint32_t cnt  = rd(cpu, off);
        uint32_t base = rd(cpu, off + 4);
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
    /* Probe window + LRU eviction. The old 2-way (h, h^1) alternating
     * eviction thrashed: gnorc_cove freecam at the level's long axis pulls
     * ~170 sectors into one present, and any 3+ visible sectors sharing a slot
     * pair evicted each other EVERY present — each miss is a full re-decode +
     * gapfill classification (ms-scale for big sectors: s148 alone burned
     * ~5ms/present; 17-19 misses/present at the hot heading = the
     * direction-dependent perf cliff). The window makes thrash
     * need TSC_WINDOW+1 hot sectors in one window; LRU (stamped per
     * terrain_build call) evicts a stale sector, never this present's
     * working set. Probes are five-word compares — window width is noise
     * next to one re-decode. */
    tscache_t* C = NULL;
    for (uint32_t k = 0; k < TSC_WINDOW; k++) {
        tscache_t* W = &s_tsc[(h + k) & (TSC_SLOTS - 1)];
        if (W->s == s && W->sig[0]==g[0] && W->sig[1]==g[1] && W->sig[2]==g[2]
            && W->sig[3]==g[3] && W->sig[4]==g[4]) {
            s_tsc_hit++; W->lastUse = s_tsc_stamp;
            if (W->fillGen != openpete_fillpick_generation())
                tsc_retag_fill(cpu, W, sectorPtr, sectorCnt);
            return W;
        }
    }
    {
        s_tsc_miss++;
        tscache_t* hot = NULL;                       /* fallback: LRU among GPU-owned */
        for (uint32_t k = 0; k < TSC_WINDOW; k++) {  /* empty slot first, else LRU */
            uint32_t idx = (h + k) & (TSC_SLOTS - 1);
            /* OPTC_SLOT_DEAD's 9-bit id is the dead-stamp face_flags
             * sentinel (chunks baked with it are culled as dead), so the
             * decode cache never holds a sector there. */
            if (idx == OPTC_SLOT_DEAD) continue;
            tscache_t* W = &s_tsc[idx];
            if (!W->s) { C = W; break; }
            /* Never evict a slot the GPU cache validated as owned THIS
             * present: the re-resolve would free the owner's chunks and
             * republish the slot after its face walk was already skipped,
             * so the owner vanishes for a present (the gnorc_cove
             * water-border wedge). The incoming sector takes another slot;
             * only when every candidate is owned (TSC_WINDOW+ owned
             * sectors hashing into one window) does plain LRU resume, and
             * the loser then rides tgpu_ensure's contest claim back to
             * the CPU walk. */
            if (tgpu_slot_hot(idx)) {
                if (!hot || W->lastUse < hot->lastUse) hot = W;
                continue;
            }
            if (!C || W->lastUse < C->lastUse) C = W;
        }
        if (!C) C = hot;
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
        C->nLpC = nLpC; C->nHpC = nHpC;
        {   /* GPU-cache liveness: any (re)decode invalidates the slot's chunks */
            static uint32_t s_decode_ctr;
            C->decodeGen = ++s_decode_ctr;
        }
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
        C->lpg_state = 0;              /* re-decoded sector: re-derive the LP grade */
        C->fillGen = openpete_fillpick_generation();
        tsc_tag_gapfill(cpu, C, sectorPtr, sectorCnt);
        {   /* Vertex AABB (both tiers) for the near-claim refinement. */
            int16_t mnx = 32767, mny = 32767, mnz = 32767;
            int16_t mxx = -32768, mxy = -32768, mxz = -32768;
            for (int t2 = 0; t2 < 2; t2++) {
                const tsvert_t* vv = t2 ? C->hpV : C->lpV;
                int nvv = t2 ? C->nHpV : C->nLpV;
                for (int k = 0; vv && k < nvv; k++) {
                    if (vv[k].x < mnx) mnx = vv[k].x;
                    if (vv[k].x > mxx) mxx = vv[k].x;
                    if (vv[k].y < mny) mny = vv[k].y;
                    if (vv[k].y > mxy) mxy = vv[k].y;
                    if (vv[k].z < mnz) mnz = vv[k].z;
                    if (vv[k].z > mxz) mxz = vv[k].z;
                }
            }
            C->vMinX = mnx; C->vMinY = mny; C->vMinZ = mnz;
            C->vMaxX = mxx; C->vMaxY = mxy; C->vMaxZ = mxz;
        }
        C->polyanim = tsc_sector_polyanim(cpu, s, sectorPtr, sectorCnt);
        if (C->polyanim && C->lpF) {   /* stale-bank guard: see tsc_sector_polyanim */
            for (int f = 0; f < C->nLpF; f++) C->lpF[f].gapfill = 0;
            C->nGapfill = 0;
        }
    }
    return C;
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
 * geometry moves at present rate, not 29.913 Hz. A handful of descriptors per
 * level, ≤256 words per sector — negligible next to the walk itself. Colours
 * already read live; texmats are covered by tsc_tile_gens/tsc_refresh_anim. */
/* Sub-tick vert interp state (set around terrain_build by terrain_reextract;
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
        uint32_t off  = list ? game_layout.env_anim.hp_poly : game_layout.env_anim.lp_poly;
        uint32_t cnt  = rd(cpu, off);
        uint32_t base = rd(cpu, off + 4);
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
            for (uint32_t w = 0; w < TSC_WINDOW; w++) {   /* same probe window as tsc_get */
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
                {   /* GPU-cache liveness: the re-decode is unconditional per tick,
                     * so hash the decoded verts and bump vertGen only when the
                     * geometry actually moved (else anim-listed sectors would
                     * rebuild their GPU chunks every tick for nothing). */
                    uint32_t h = 2166136261u;
                    const uint8_t* hp = (const uint8_t*)C->lpV;
                    for (size_t n = sizeof(tsvert_t) * (size_t)(C->lpV ? nLp : 0), j = 0; j < n; j++)
                        h = (h ^ hp[j]) * 16777619u;
                    hp = (const uint8_t*)C->hpV;
                    for (size_t n = sizeof(tsvert_t) * (size_t)(C->hpV ? nHp : 0), j = 0; j < n; j++)
                        h = (h ^ hp[j]) * 16777619u;
                    if (h != C->vertHash) { C->vertHash = h; C->vertGen++; }
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
    s_tscroll_have = 0;
    memset(s_tscroll_tile, 0, sizeof(s_tscroll_tile));
    int dbg_desc = 0, dbg_paired = 0;
    {
    uint32_t cnt  = rd(cpu, game_layout.env_anim.scroll);
    uint32_t base = rd(cpu, game_layout.env_anim.scroll + 4);
    if (!base || cnt == 0 || cnt > 1024) goto scan;
    /* Fractional offsets exist only on sub-tick re-extract builds; the
     * canonical path renders the committed whole-texel decode, byte-exact.
     * The claim mask is filled regardless. */
    int pair = (s_vanim_sub && s_tanim_en);
    op_time_t now = pair ? openpete_interp_present_now() : 0;
    if (pair && now <= 0) pair = 0;
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t desc = rd(cpu, base + k * 4);
        if (!desc) continue;
        dbg_desc++;
        {   uint32_t ct = rd(cpu, desc + 4) & 0xFFFFu;
            if (ct < 128) {
                s_tscroll_tile[ct] = 1; s_tscroll_have = 1;
                /* Accumulator-change generation: the scroller advances the
                 * HQ sibling record on ticks where the 16-byte LQ entry is
                 * byte-identical (scroll>>2 unchanged), so tsc_tile_gens
                 * alone would leave a stale GPU bake. Track the raw 7-bit
                 * accumulator and bump the tile generation on every advance
                 * (canonical builds only — it moves at tick boundaries);
                 * the re-decode is a no-op for the CPU path when the LQ
                 * bytes coincide, and texGen rebuilds the bake. */
                static uint16_t s_tscroll_acc[128];
                uint16_t acc = (uint16_t)(cpu->read_half(desc + 6) & 0x7Fu);
                if (!s_vanim_sub && s_tscroll_acc[ct] != acc) {
                    s_tscroll_acc[ct] = acc;
                    s_tgen[ct]++; s_tgen_epoch++;
                }
            } }
        if (!pair) continue;
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
    int changed = 0;
    for (int f = 0; f < C->nHpF; f++) {
        tsface_t* F = &C->hpF[f];
        /* HQ sibling liveness: the scroll accumulator gen for the record id
         * (which can differ from tb). The near path decodes the record live
         * per emit, so nothing re-decodes here — but the GPU bake reads it
         * at build time and must go stale (texGen) when it advances. */
        if (F->quad && !(F->w3 & 0x80)) {
            uint32_t gh = s_tgen[F->hq_idx & 0x7F];
            if (F->tgenHq != gh) { F->tgenHq = gh; changed = 1; }
        }
        if (F->tb == -1) continue;
        uint32_t g = s_tgen[F->tb & 0x7F];
        if (F->tgen == g) continue;
        F->tgen = g;
        changed = 1;
        terrain_texmat(cpu, tbl, F->tb, hiDetail, &F->mat, F->tu, F->tv);
        terrain_texmat(cpu, tbl, F->tb, 1, &F->matN, F->tuN, F->tvN);
    }
    /* GPU-cache liveness: baked materials (incl. the HQ sub-quad tiledefs,
     * which the env anim rewrites in the sibling record) went stale. */
    if (changed) C->texGen++;
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
/* OPENPETE_TERRAIN_VISDBG: falsify the widescreen candidate set's correctness
 * invariant. Prints ONCE (once loaded): the .s's canonical 4:3 visible count
 * (D_800771C8==0xFF) vs the occlusion candidate set, and how many visible
 * sectors are MISSING from the candidate set. The candidate set must be a
 * SUPERSET of the 4:3 visible set — if missing>0 the occlusion-group walk is
 * wrong (offset/format) and widescreen would gap even at 4:3. Display-only. */
static int s_visdbg = -1;
static void terrain_visdbg(CPUState* cpu, uint32_t sectorCnt, int group,
                           uint32_t occCnt, const uint16_t* cand, int cn, int n, int ws) {
    static int s_done = 0;
    if (s_done || openpete_frame < 1800u) return;
    s_done = 1;
    static char inset[1024]; memset(inset, 0, sizeof(inset));
    for (int j = 0; j < cn; j++) if (cand[j] < 1024) inset[cand[j]] = 1;
    int vis43 = 0, missing = 0;
    for (uint32_t i = 0; i < sectorCnt && i < 1024; i++) {
        uint8_t vis = (uint8_t)(rd(cpu, (game_layout.visibility.base + i) & ~3u) >> ((i & 3) * 8));
        if (vis == 0xFF) { vis43++; if (!inset[i]) missing++; }
    }
    fprintf(stderr,
        "[terrain-vis] frame=%u group=%d occCnt=%u candidates=%d  vis4:3=%d "
        "missingFromCandidates=%d  n=%d (sectors=%u ws=%d)\n",
        openpete_frame, group, occCnt, cn, vis43, missing, n, sectorCnt, ws);
}

/* Build the set of sector indices to render this frame.
 *
 * Native 4:3: use the .s's own canonical visibility verdict D_800771C8
 * (== occlusion sublist ∩ the .s 4:3 frustum cull) directly — pixel-identical
 * to the .s visible set, zero risk.
 *
 * WIDESCREEN: the .s verdict is too narrow — its 4:3 frustum cull dropped the
 * side sectors the wider frustum now reveals (the terrain sibling of the
 * cyclorama wide-AR gap). We re-derive the candidate set
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
/* The projection view of the current terrain pass (camera matrix and
 * position, GTE offsets, canonical and effective H, Hor+ bounds). Built once
 * at terrain_build entry; the feed that follows (rim frames, portal planes,
 * the conform census) reads the same view, so the plane fit and the frames
 * derived from it share one H. */
static ngte_view s_view;

static int terrain_vislist(CPUState* cpu, uint32_t sectorCnt, uint16_t* out, int32_t group) {
    if (sectorCnt > 1024) sectorCnt = 1024;
    int ws = s_view.extended;   /* Hor+ or a wider FOV: the 4:3 verdict is too narrow */
    if (s_visdbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_VISDBG"); s_visdbg = (e && *e && *e != '0'); }
    {   /* OPENPETE_TERRAIN_ALLVIS=1: walk ALL sectors (bypass the visibility verdict /
         * occlusion groups) — A/B probe for view-dependent sector culling. */
        static int s_allvis = -1;
        if (s_allvis < 0) { const char* e = getenv("OPENPETE_TERRAIN_ALLVIS"); s_allvis = (e && *e && *e != '0'); }
        if (s_allvis) {
            int n = 0;
            for (uint32_t i = 0; i < sectorCnt && n < 1024; i++) out[n++] = (uint16_t)i;
            return n;
        }
    }

    uint32_t occCnt    = rd(cpu, game_layout.env.occ_group_count);
    uint32_t occGroups = rd(cpu, game_layout.env.occ_groups);
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
        if (s_visdbg) terrain_visdbg(cpu, sectorCnt, group, occCnt, out, n, n, ws);
        return n;
    }

    /* native 4:3 — the canonical .s verdict */
    int n = 0;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint8_t vis = (uint8_t)(rd(cpu, (game_layout.visibility.base + i) & ~3u) >> ((i & 3) * 8));
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
        terrain_visdbg(cpu, sectorCnt, group, occCnt, cand, cn, n, ws);
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
    uint32_t lqTable = rd(cpu, game_layout.env.lq_tex);
    uint32_t hqTable = rd(cpu, game_layout.env.hq_tex);
    uint32_t texCnt  = rd(cpu, game_layout.env.tex_count);
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
        uint32_t lt = rd(cpu, game_layout.env.lq_tex), ht = rd(cpu, game_layout.env.hq_tex);
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
    uint32_t lqTable   = rd(cpu, game_layout.env.lq_tex);
    uint32_t hqTable   = rd(cpu, game_layout.env.hq_tex);
    uint32_t texCnt    = rd(cpu, game_layout.env.tex_count);
    uint32_t sectorPtr = rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = rd(cpu, game_layout.env.sector_count);
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

/* Per-sector view-space bounding sphere, stamped each walk (s_vanim_pres).
 * Shared input of the occlusion cull and the portal-rim sector broadphase
 * (rim_sector_cands): sector-table index -> centre through the projection
 * matrix (world/16 units), header radius, poly-anim membership. */
static float    s_secv[1024][4];
static uint8_t  s_secv_anim[1024];
static uint32_t s_secv_stamp[1024];
static double   s_rim_rowmax = 1.0;   /* max ||M row||/4096 — world->view scale bound */

/* ==== terrain sector-mesh GPU cache: game-side builder + claims ============
 *
 * Packs a decoded sector (tscache_t) into rhi_terrain_vertex chunks — one per
 * draw-class arena — and keeps them valid against the moving-terrain inputs:
 * decodeGen (slot re-decode / reuse), texGen (env texture anim re-decoded
 * texmats + the HQ sibling records), vertGen (geometry env anim content
 * change), and a per-VBL colour-array hash (terrain colours are read live by
 * the CPU walk; gameplay can only change them at tick boundaries). Everything
 * baked here is exactly what the CPU face walk reads per present; the
 * camera-dependent selections happen in terrain.vert.
 *
 * CLAIMED back to the CPU walk (sub-tick or per-face semantics the retained
 * mesh cannot bake): near-plane-approaching sectors (the no-clip guarantee),
 * portal-outline overlaps (rim cull / transparent-cover drop / plane-cover —
 * the S2c plane-cover migration retires this one), vert-anim sectors (the
 * fractional lerp banks), scroll-anim tiles (the 7-bit accumulator lerp),
 * fill-duty sectors (the feed's screen-space dilation), arena-full and
 * unmapped-colour sectors, sectors an HD texture pack would replace into
 * (per-run replacement rides the feed's draw_tex_runs), and sectors a mod
 * material selector could override (custom pipelines resolve off the feed's
 * provenance tags). The pack/selector claims are tier 1 of the
 * exclusion→claim→fold ladder; the S2c texpack GPU fold retires them for
 * replaced sectors. */

typedef struct {
    uint32_t s;                       /* sector vaddr chunks were built from (0 = none) */
    uint32_t decodeGen, texGen, vertGen, colHash;
    uint32_t lastColFrame;            /* VBL of the last colour-hash check */
    int      unbuildable;             /* claim reason latched at this decodeGen:
                                       * 0 fine, 1 arena-full, 2 scroll-anim,
                                       * 3 unmapped colours (4 = decode-slot
                                       * contested, 5 = texture pack; both
                                       * returned but never latched here).
                                       * Code 6 was the mod-material selector
                                       * claim and is retired: a material is
                                       * baked into the face as a bucket and
                                       * drawn from the cache. */
    uint32_t first[OPTC_ARENA_COUNT];
    int      count[OPTC_ARENA_COUNT];
    uint32_t ownFrame;                /* VBL this slot last validated as GPU-owned */
    /* Pack/selector claim latches: the scan answers depend on the decoded
     * texmats (decodeGen/texGen) and on the pack registry / selector set
     * (their own generations); 0 = unscanned (both counters start at 1). */
    uint32_t scanDecodeGen, scanTexGen;
    uint32_t tpGen;                   /* texture_pack_generation at last pack scan */
    unsigned selEpoch;                /* openpete_mods_material_terrain_epoch at last scan */
    uint8_t  tpClaim;
    uint8_t  gen;                     /* chunk generation (3 bits published) */
    uint8_t  bakeVar;                 /* the last build for this sector emitted
                                       * near-variant chunks alongside the plain
                                       * ones — sticky for the sector's whole
                                       * residency, so a sector oscillating
                                       * across the near threshold re-bakes at
                                       * most once (and a variant build that
                                       * failed is not retried every present) */
    uint32_t animRoll;                /* poly-anim: SC->vrollPres at build — a
                                       * new canonical roll re-bakes (prev,cur)
                                       * even when the content hash is stable
                                       * (a paused anim must stop lerping the
                                       * last step) */
    uint32_t bakeTpGen;               /* HD fold: texture_pack_generation at
                                       * build — a moved registry (new files
                                       * loaded) re-bakes so faces that now
                                       * match gain their entries */
    uint32_t bakeHdEpoch;             /* HD fold: hdfold_epoch at build — a
                                       * fold-claimed (unbuildable=5) sector
                                       * retries when interns or an upload
                                       * drain moved it */
    unsigned bakeSelEpoch;            /* material_epoch at build — the baked
                                       * faces carry material BUCKET ids, so a
                                       * selector set that moved makes them
                                       * stale (wrong pass, or a face left in
                                       * the stock pass a material now wants) */
} tgpu_slot_t;
static tgpu_slot_t s_tgpu[TSC_SLOTS];
/* Does this decode slot hold a sector the GPU cache validated as owned this
 * present? tsc_get's eviction skips these (see the contest note there). With
 * the cache off, ownFrame never advances, so this is always false. */
static int tgpu_slot_hot(uint32_t idx) {
    return s_tgpu[idx].s != 0 && s_tgpu[idx].ownFrame == openpete_frame;
}
/* census: window sums (reset each print) + per-walk counts (op_prof rows) */
static long s_tgpu_secs, s_tgpu_claims, s_tgpu_rebuilds, s_tgpu_nvsecs;
static long s_tgcl_near, s_tgcl_portal, s_tgcl_anim, s_tgcl_scroll,
            s_tgcl_fill, s_tgcl_build, s_tgcl_colour, s_tgcl_slot,
            s_tgcl_texpack, s_tgcl_nearb, s_tgcl_nodraw,
            s_tgcl_prim, s_tgcl_pcov;   /* portal sub-buckets: rim-candidate /
                                         * blended-cover (portal = their sum
                                         * plus claim-all + no-refine claims) */
static int  s_tgpu_secs_walk, s_tgpu_claims_walk;

/* Active portal-sky planes for the per-sector portal claim. Of the feed's
 * portal semantics, only two ever DROP a CPU-fed terrain tri — the inner-rim
 * cull (bounded by the rim sector broadphase: a sector failing it cannot
 * contain a rim candidate) and the transparent-cover drop (blended faces
 * only). Opaque plane-cover needs no claim at all: covered pixels draw at
 * true depth and the preview repaints over them in core, whoever drew them.
 * So a sector claims iff it passes the rim broadphase, or it has a blended
 * face and its projected bbox overlaps an active outline; everything else
 * near a portal keeps the GPU path and draws identically by construction.
 * s_tgpu_claim_all forces every sector to the CPU when a portal is recent
 * but its planes are momentarily unavailable (safe degenerate); with the
 * plane provider disabled (OPENPETE_TERRAIN_PORTAL_CONFORM=0) the feed
 * drops nothing, and the pre-refinement blanket bbox claim is kept. */
static openpete_portal_skyplane s_tgpu_pplane[6];
static int s_tgpu_pplane_n = 0;
static int s_tgpu_claim_all = 0;
static int s_tgpu_rim_ok = 0;         /* rim frames derived for THIS walk */
static int tgpu_portal_frames(CPUState* cpu);            /* defined post-feed-statics */
static int tgpu_rim_sphere_cand(double oc1, double oc2, double oc3, int radius);
static void tgpu_rim_globals_fill(openpete_tercache_globals* g);
static void tgpu_cover_globals_fill(openpete_tercache_globals* g);

/* Near-claim refinement: does the sector's vertex AABB stay wholly at or
 * beyond the 16.0 view-Z the near claim guards? View-Z is linear in xyz, so
 * the minimum over the 8 transformed corners bounds every decoded vertex
 * (the corners transform through the walk's own per-vertex expression). A
 * clear box means no vertex can enter the near band, clip, or straddle the
 * eye plane — the identical regime every sphere-passing owned sector
 * renders in, so the plain chunks are exact by construction. */
static int tgpu_near_box_clear(const tscache_t* SC, const int M[3][3],
                               double camx, double camy, double camz) {
    if (SC->vMinX > SC->vMaxX) return 0;             /* no verts decoded */
    const double s4 = SC->originX - camx, s5 = camy - SC->originY,
                 s6 = camz - SC->originZ;
    const double xs[2] = { (double)SC->vMinX, (double)SC->vMaxX };
    const double ys[2] = { (double)SC->vMinY, (double)SC->vMaxY };
    const double zs[2] = { (double)SC->vMinZ, (double)SC->vMaxZ };
    for (int c = 0; c < 8; c++) {
        double m1, m2, m3;
        ngte_mac(M, s5 - ys[(c >> 1) & 1], s6 - zs[(c >> 2) & 1],
                 xs[c & 1] + s4, &m1, &m2, &m3);
        if (m3 < 16.0) return 0;
    }
    return 1;
}
static double s_tgpu_l3 = 1.0;        /* ||M row 2||/4096 (near-claim bound) */

/* Near-variant chunks (the vertex-stage clip fan) for sectors whose geometry
 * can reach or cross the eye plane, instead of claiming them to the CPU walk.
 * OPENPETE_TERRAIN_NEARVAR=0 restores the claim (A/B lever). */
static int tgpu_nearvar_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TERRAIN_NEARVAR");
                 v = !(e && *e && *e == '0'); }
    return v;
}
static int s_otbias_on;               /* tentative: defined (=1) at the feed */

static void tgpu_free_slot(tgpu_slot_t* T) {
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (T->count[a] > 0) openpete_tercache_chunk_free(a, T->first[a], T->count[a]);
        T->count[a] = 0;
    }
    T->s = 0;
}

/* Debug/experiment envs whose semantics live in the CPU walk: any of them set
 * forces the pure-CPU path (per-run latch — these are dev levers). */
static int tgpu_env_block(void) {
    static int v = -1;
    if (v >= 0) return v;
    static const char* const keys[] = {
        "TIERCOLOR", "NOTEX", "FLAT", "TIERONLY", "FARDBG", "SECTORDUMP",
        "CALDBG", "NEARQ", "VERIFY", "CULLDBG", "NEARDBG", "BINDBG",
        "LPKEEP", "LPKEEPDBG", "FILL_DILATE", "FILL_FOGSLACK", "FILL_UP",
        "FILL_THINPX", "GAPFILL_EXCL", "FOGSCALE", "HQSCALE", "NEARZ",
        "HIDETAIL", "TEXTBL", "PORTAL_CONFORM_DBG",
        /* A/B levers whose non-default value changes walk math the shader
         * mirrors only at the shipped default: */
        "LP_FARCULL", "LP_ADMIT", "HP_DROP", "LOD_FAITHFUL",
        "NEARBAND_CONTEST",
    };
    v = 0;
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]) && !v; k++) {
        char name[64];
        snprintf(name, sizeof name, "OPENPETE_TERRAIN_%s", keys[k]);
        const char* e = getenv(name);
        if (e && *e) v = 1;
    }
    if (!v) { const char* e = getenv("OPENPETE_TERRAIN_FOGDPCS");   /* =0: legacy lerp fog */
              if (e && *e && *e == '0') v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_TERRAIN_CULL");      /* non-default cull mask */
              if (e && *e && atoi(e) != 3) v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_PORTAL_GUARD");      /* guard re-feed needs CPU tris */
              if (e && *e && *e != '0') v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_MOBY_TRUESCOPE");    /* terrain.frag mirrors scope ON */
              if (e && *e && *e == '0') v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_TRUE_W");            /* W-mask bisect instrument */
              if (e && *e) v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_NOTEX");             /* scene NOTEX spec constant */
              if (e && *e && *e != '0') v = 1; }
    return v;
}

static uint32_t tgpu_pack_mat0(const tmat* m) {
    int blend = (m->blend > 3) ? 0 : m->blend;   /* ABR-3 "B+F/4" clamps opaque (scene rule) */
    return (uint32_t)(m->tpx & 0x3FF)
         | ((uint32_t)(m->tpy & 0x1FF) << 10)
         | ((uint32_t)(m->bpp == 4 ? 0 : m->bpp == 8 ? 1 : 2) << 19)
         | ((uint32_t)(blend & 3) << 21)
         | ((uint32_t)(m->textured ? 1 : 0) << 23);
}
/* Draw-class arena by BLEND alone (untextured blended faces route to the
 * blend pipes exactly like the feed's untextured gouraud-blend groups; only
 * blend-0 untextured LP takes the LP arena — the caller decides that). */
static int tgpu_arena_of(const tmat* m) {
    int blend = (m->blend > 3) ? 0 : m->blend;
    switch (blend) {
        case 1:  return OPTC_ARENA_ALPHA;
        case 2:  return OPTC_ARENA_ADD;
        case 3:  return OPTC_ARENA_SUB;
        default: return OPTC_ARENA_OPAQUE;
    }
}

typedef struct { rhi_terrain_vertex* v; rhi_terrain_prev* pv;
                 rhi_terrain_nearctx* cx; int n, cap;
                 int anim; /* fill pv alongside v (poly-anim sector)    */
                 int nearv; /* fill cx alongside v (near-variant chunk) */ } tgpu_buf;
static int tgpu_push(tgpu_buf* b, const rhi_terrain_vertex* v,
                     const rhi_terrain_prev* p, const rhi_terrain_nearctx* c) {
    if (b->n >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 256;
        rhi_terrain_vertex* nv = (rhi_terrain_vertex*)realloc(b->v, sizeof(*nv) * (size_t)nc);
        if (!nv) return -1;
        b->v = nv; b->cap = nc;
        if (b->anim) {
            rhi_terrain_prev* np = (rhi_terrain_prev*)realloc(b->pv, sizeof(*np) * (size_t)nc);
            if (!np) return -1;
            b->pv = np;
        }
        if (b->nearv) {
            rhi_terrain_nearctx* ncx =
                (rhi_terrain_nearctx*)realloc(b->cx, sizeof(*ncx) * (size_t)nc);
            if (!ncx) return -1;
            b->cx = ncx;
        }
    }
    if (b->anim) b->pv[b->n] = *p;
    if (b->nearv) b->cx[b->n] = *c;
    b->v[b->n++] = *v;
    return 0;
}

/* x4 fixed-point corner (sector-local world/16, 2 fractional bits). */
typedef struct { int16_t x, y, z; } tgpu_p4;
static tgpu_p4 tgpu_p4_of(const tsvert_t* v) {
    tgpu_p4 p = { (int16_t)(v->x * 4), (int16_t)(v->y * 4), (int16_t)(v->z * 4) };
    return p;
}
static tgpu_p4 tgpu_p4_mid(tgpu_p4 a, tgpu_p4 b) {   /* exact: x4 sums are even */
    tgpu_p4 p = { (int16_t)((a.x + b.x) >> 1), (int16_t)((a.y + b.y) >> 1),
                  (int16_t)((a.z + b.z) >> 1) };
    return p;
}
static uint32_t tgpu_rgb_mid(uint32_t a, uint32_t b) {   /* cv_mid's per-channel (a+b)>>1 */
    return (uint32_t)((((a & 0xFF) + (b & 0xFF)) >> 1))
         | (uint32_t)(((((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) >> 1) << 8)
         | (uint32_t)(((((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) >> 1) << 16);
}

/* Emit one triangle (3 corners) built from a face-level template. Each
 * corner carries its flags16 role bits (HQ: sub-quad + corner slot; the
 * vertex stage rebuilds the node from the TRANSFORMED parent corners). */
typedef struct {
    tgpu_p4  c[4];        /* parent-face corners, i0..i3 order */
    tgpu_p4  pc[4];       /* poly-anim: previous-tick corners (else unset) */
    uint32_t face_flags;
    uint32_t matf0, matf1, matn0, matn1;
    uint16_t flags16;
} tgpu_face_t;
typedef struct { tgpu_p4 p, pp; uint32_t rgb_far, rgb_near;
                 uint8_t uf, vf, un, vn, role; } tgpu_corner_t;

static void tgpu_fill_vert(rhi_terrain_vertex* v, const tgpu_face_t* F,
                           const tgpu_corner_t* k, int tri2, int spare) {
    memset(v, 0, sizeof(*v));
    v->px = k->p.x; v->py = k->p.y; v->pz = k->p.z;
    v->flags16 = (uint16_t)(F->flags16 | ((uint16_t)k->role << 3)
                            | (tri2  ? 0x2000u : 0u)
                            | (spare ? 0x8000u : 0u));
    v->uvf_u = k->uf; v->uvf_v = k->vf;
    v->uvn_u = k->un; v->uvn_v = k->vn;
    v->rgba_far  = 0xFF000000u | (k->rgb_far  & 0xFFFFFFu);
    v->rgba_near = 0xFF000000u | (k->rgb_near & 0xFFFFFFu);
    v->mat_far0 = F->matf0; v->mat_far1 = F->matf1;
    v->mat_near0 = F->matn0; v->mat_near1 = F->matn1;
    v->face_flags = F->face_flags;
    v->c0x = F->c[0].x; v->c0y = F->c[0].y; v->c0z = F->c[0].z;
    v->c1x = F->c[1].x; v->c1y = F->c[1].y; v->c1z = F->c[1].z;
    v->c2x = F->c[2].x; v->c2y = F->c[2].y; v->c2z = F->c[2].z;
    v->c3x = F->c[3].x; v->c3y = F->c[3].y; v->c3z = F->c[3].z;
}

static int tgpu_emit_tri(tgpu_buf* b, const tgpu_face_t* F,
                         tgpu_corner_t a, tgpu_corner_t c1, tgpu_corner_t c2,
                         int tri2 /* second half / sub-tri (flags16 bit13) */) {
    const tgpu_corner_t cs[3] = { a, c1, c2 };
    for (int k = 0; k < 3; k++) {
        rhi_terrain_prev pr;
        if (b->anim) {
            memset(&pr, 0, sizeof(pr));
            pr.px = cs[k].pp.x; pr.py = cs[k].pp.y; pr.pz = cs[k].pp.z;
            pr.c0x = F->pc[0].x; pr.c0y = F->pc[0].y; pr.c0z = F->pc[0].z;
            pr.c1x = F->pc[1].x; pr.c1y = F->pc[1].y; pr.c1z = F->pc[1].z;
            pr.c2x = F->pc[2].x; pr.c2y = F->pc[2].y; pr.c2z = F->pc[2].z;
            pr.c3x = F->pc[3].x; pr.c3y = F->pc[3].y; pr.c3z = F->pc[3].z;
        }
        rhi_terrain_vertex v;
        tgpu_fill_vert(&v, F, &cs[k], tri2, 0);
        if (tgpu_push(b, &v, b->anim ? &pr : NULL, NULL) != 0) return -1;
    }
    return 0;
}

/* Near-variant twin of one emitted triangle: SIX vertices — the primary fan
 * triangle then the spare (flags16 bit 15) — each carrying the other two
 * members' UVs and colours so the vertex stage can rebuild and near-plane
 * clip the whole triangle. Context members are in emit order relative to
 * self (a = self+1, b = self+2 mod 3), the order the clip fan indexes them
 * in; both halves repeat the same three corner records, so a vertex's slot
 * in its triangle is simply its index mod 3. */
static int tgpu_emit_tri_near(tgpu_buf* b, const tgpu_face_t* F,
                              tgpu_corner_t a, tgpu_corner_t c1,
                              tgpu_corner_t c2, int tri2) {
    const tgpu_corner_t cs[3] = { a, c1, c2 };
    for (int spare = 0; spare < 2; spare++) {
        for (int k = 0; k < 3; k++) {
            rhi_terrain_vertex v;
            rhi_terrain_nearctx cx;
            const tgpu_corner_t* A = &cs[(k + 1) % 3];
            const tgpu_corner_t* B = &cs[(k + 2) % 3];
            tgpu_fill_vert(&v, F, &cs[k], tri2, spare);
            memset(&cx, 0, sizeof(cx));
            cx.a_uf = A->uf; cx.a_vf = A->vf; cx.a_un = A->un; cx.a_vn = A->vn;
            cx.a_far  = 0xFF000000u | (A->rgb_far  & 0xFFFFFFu);
            cx.a_near = 0xFF000000u | (A->rgb_near & 0xFFFFFFu);
            cx.b_uf = B->uf; cx.b_vf = B->vf; cx.b_un = B->un; cx.b_vn = B->vn;
            cx.b_far  = 0xFF000000u | (B->rgb_far  & 0xFFFFFFu);
            cx.b_near = 0xFF000000u | (B->rgb_near & 0xFFFFFFu);
            if (tgpu_push(b, &v, NULL, &cx) != 0) return -1;
        }
    }
    return 0;
}

/* Emit one triangle into this sector's chunk buffers: the plain chunk
 * always, plus the near-variant twin while the sector carries one. */
static int tgpu_emit_face_tri(tgpu_buf* buf, int arena, int ab, int with_var,
                              const tgpu_face_t* F, tgpu_corner_t a,
                              tgpu_corner_t c1, tgpu_corner_t c2, int tri2) {
    if (tgpu_emit_tri(&buf[arena + ab], F, a, c1, c2, tri2) != 0) return -1;
    if (with_var && tgpu_emit_tri_near(&buf[arena + (int)OPTC_ARENA_NEAR_BASE],
                                       F, a, c1, c2, tri2) != 0) return -1;
    return 0;
}

/* FNV over the sector's live colour arrays (host pointers): the per-tick
 * liveness for baked colours. Returns -1 when a range is unmapped (claim). */
static int tgpu_col_hash(const tscache_t* SC, uint32_t* out) {
    uint32_t h = 2166136261u;
    struct { uint32_t base; int bytes; } r[3] = {
        { SC->lpColBase, SC->nLpC * 4 },
        { SC->nearBase,  SC->nHpC * 4 },
        { SC->farBase,   SC->nHpC * 4 },
    };
    for (int i = 0; i < 3; i++) {
        if (r[i].bytes <= 0) continue;
        const uint8_t* p = (const uint8_t*)openpete_subtick_host_ptr(r[i].base);
        if (!p) return -1;
        for (int j = 0; j < r[i].bytes; j++) h = (h ^ p[j]) * 16777619u;
    }
    *out = h;
    return 0;
}

/* HD fold: intern one textured half's replacement entry for a face's emitted
 * tris (the draw-time per-tri resolve+match, evaluated at bake). u/v arrays
 * are the half's corner texels in emit order; the second tri is NULL for
 * GT3s. Returns 0 = no replacement, >0 = the entry id (both tris agree),
 * -1 = unbakeable — blended target (sub-runs draw those inline, in order),
 * tris resolving to different files, or table/atlas overflow — the sector
 * claims back to the CPU walk. */
static int tgpu_hdfold_face(const tmat* m,
                            const float* ua, const float* va,
                            const float* ub, const float* vb)
{
    if (!m->textured) return 0;
    int a = openpete_native_hdfold_intern(m->tpx, m->tpy, m->clx, m->cly,
                                          m->bpp, ua, va);
    if (a < 0) return -1;
    int b = a;
    if (ub) {
        b = openpete_native_hdfold_intern(m->tpx, m->tpy, m->clx, m->cly,
                                          m->bpp, ub, vb);
        if (b < 0) return -1;
    }
    if (a != b) return -1;
    if (a > 0 && m->blend) return -1;
    return a;
}

/* Build all of one sector's chunks. Returns 0 on success, 1 arena-full,
 * 2 scroll-anim claim (the flat/HQ UV-scroll rides a fractional per-present
 * accumulator lerp the mesh cannot bake), 5 = HD-fold unbakeable face (see
 * tgpu_hdfold_face). sec_index = the level sector index (the
 * authored-correction key). */
static int tgpu_build_sector(CPUState* cpu, int slot, const tscache_t* SC,
                             uint32_t hqTable, int sec_index, int with_var) {
    /* UV-scroll is mirrored: the accumulator-change tile generations rebuild
     * the bake per whole-texel advance (incl. the HQ-sibling-only ticks, via
     * tgenHq), and sub-tick fractional offsets ride the globals' per-tile dv
     * table (flags16 carries the face's tile id). */
    tgpu_buf buf[OPTC_ARENA_COUNT];
    memset(buf, 0, sizeof(buf));
    int ok = 0;
    int fail = 1;                     /* claim code when !ok (1 = arena-full) */
    const int hdfold = openpete_native_hdfold_on();
    uint32_t base_ff = ((uint32_t)slot & 0x1FFu)
                     | (((uint32_t)s_tgpu[slot].gen & 7u) << 29);
    /* Poly-anim sectors bake into the ANIM twin arenas with a parallel
     * previous-tick record per vertex (the terrain_anim vertex stage lerps
     * prev->cur at the build's alpha). Snap rule mirror: a sector with no
     * prev bank yet, or whose banks jumped (keyframe reset — the CPU skips
     * the lerp fill and draws cur), bakes prev == cur. */
    const int anim = SC->polyanim != 0;
    const int ab = anim ? (int)OPTC_ARENA_ANIM_BASE : 0;
    /* Near-variant chunks are never poly-anim: those sectors keep the CPU
     * claim, so the two twin arena families are mutually exclusive (one
     * binding-1 twin per chunk — prev record OR member context). */
    if (anim) with_var = 0;
    if (with_var)
        for (int a = OPTC_ARENA_NEAR_BASE; a < OPTC_ARENA_COUNT; a++)
            buf[a].nearv = 1;
    int vsnap = 1;
    if (anim) {
        for (int a = OPTC_ARENA_ANIM_BASE; a < OPTC_ARENA_NEAR_BASE; a++)
            buf[a].anim = 1;
        const int nLp = (SC->lpV && SC->lpVprev) ? SC->nLpV : 0;
        const int nHp = (SC->hpV && SC->hpVprev) ? SC->nHpV : 0;
        const int havePrev = (!SC->lpV || SC->lpVprev) && (!SC->hpV || SC->hpVprev);
        const int jump = (nLp && tsc_vanim_jump(SC->lpVprev, SC->lpV, nLp)) ||
                         (nHp && tsc_vanim_jump(SC->hpVprev, SC->hpV, nHp));
        vsnap = !havePrev || jump;
    }

    /* ---- LP tier: untextured; drawn in WORLD and re-drawn in FILL ---- */
    /* Every LP face keys the same (untextured, LP tier), so its material
     * bucket is a per-sector constant. 0 = no material claims these faces. */
    const int lp_bucket = openpete_mods_material_terrain_bucket(-1, 0, 1);
    for (int f = 0; SC->lpV && SC->lpF && f < SC->nLpF; f++) {
        const tsface_t* F = &SC->lpF[f];
        if (F->i0 >= SC->nLpV || F->i1 >= SC->nLpV || F->i2 >= SC->nLpV || F->i3 >= SC->nLpV)
            continue;   /* mirrors the walk's OOB skip */
        tgpu_face_t T;
        memset(&T, 0, sizeof(T));
        T.c[0] = tgpu_p4_of(&SC->lpV[F->i0]); T.c[1] = tgpu_p4_of(&SC->lpV[F->i1]);
        T.c[2] = tgpu_p4_of(&SC->lpV[F->i2]); T.c[3] = tgpu_p4_of(&SC->lpV[F->i3]);
        T.face_flags = base_ff | (1u << 9)                       /* tier LP */
                     | (F->gapfill ? (1u << 11) : 0u)
                     | (F->dside   ? (1u << 12) : 0u)            /* LP w0&0x80 */
                     | (F->quad    ? (1u << 14) : 0u)
                     | (F->floorf  ? (1u << 18) : 0u)
                     | (((uint32_t)F->admit  & 0x1Fu) << 19)
                     | (((uint32_t)F->otbias & 0x1Fu) << 24);
        T.matf0 = tgpu_pack_mat0(&F->mat)    /* untextured (blend may be set) */
                | ((uint32_t)lp_bucket << 24);
        uint32_t rc[4];
        rc[0] = rd(cpu, SC->lpColBase + F->c0) & 0xFFFFFFu;
        rc[1] = rd(cpu, SC->lpColBase + F->c1) & 0xFFFFFFu;
        rc[2] = rd(cpu, SC->lpColBase + F->c2) & 0xFFFFFFu;
        rc[3] = F->quad ? (rd(cpu, SC->lpColBase + F->c3) & 0xFFFFFFu) : 0u;
        {   /* authored-data corrections (unpainted vertex colours) — the
             * walk applies these per emit; bake them (static per level). */
            uint32_t fixrgb;
            if (lp_artfix_lookup(cpu, sec_index, f, &fixrgb))
                rc[0] = rc[1] = rc[2] = rc[3] = fixrgb;
        }
        tgpu_corner_t k[4];
        memset(k, 0, sizeof(k));
        const int vi[4] = { F->i0, F->i1, F->i2, F->i3 };
        for (int q = 0; q < 4; q++) {
            k[q].p = tgpu_p4_of(&SC->lpV[vi[q]]);
            k[q].pp = (anim && !vsnap) ? tgpu_p4_of(&SC->lpVprev[vi[q]]) : k[q].p;
            T.pc[q] = k[q].pp;
            k[q].rgb_far = k[q].rgb_near = rc[q];
        }
        /* flags16 bit 14 = blended face: the vertex stage's portal
         * transparent-cover drop keys on it (raw mat blend, not the draw
         * arena — ABR-3 clamps to the opaque arena but the feed still
         * drops it under a cover). */
        if (F->mat.blend) T.flags16 |= 0x4000u;
        int da = F->mat.blend ? tgpu_arena_of(&F->mat) : (int)OPTC_ARENA_LP;
        /* LP quad: strip diagonal i1-i2 (no i2/i3 swap — see the LP emit) */
        if (tgpu_emit_face_tri(buf, da, ab, with_var, &T, k[0], k[1], k[2], 0) != 0) goto done;
        if (F->quad &&
            tgpu_emit_face_tri(buf, da, ab, with_var, &T, k[1], k[2], k[3], 1) != 0) goto done;
    }

    /* ---- HP tier: flat variant + the HQ 2x2 subdivide variant ---- */
    for (int f = 0; SC->hpV && SC->hpF && f < SC->nHpF; f++) {
        const tsface_t* F = &SC->hpF[f];
        if (F->i0 >= SC->nHpV || F->i1 >= SC->nHpV || F->i2 >= SC->nHpV || F->i3 >= SC->nHpV)
            continue;
        int hq_elig = F->quad && hqTable != 0 && !(F->w3 & 0x80);
        tgpu_face_t T;
        memset(&T, 0, sizeof(T));
        T.c[0] = tgpu_p4_of(&SC->hpV[F->i0]); T.c[1] = tgpu_p4_of(&SC->hpV[F->i1]);
        T.c[2] = tgpu_p4_of(&SC->hpV[F->i2]); T.c[3] = tgpu_p4_of(&SC->hpV[F->i3]);
        T.face_flags = base_ff
                     | (F->fogbias ? (1u << 10) : 0u)
                     | ((F->w3 & 0x4)  ? (1u << 12) : 0u)   /* dside */
                     | (F->flip ? (1u << 13) : 0u)
                     | (F->quad ? (1u << 14) : 0u)
                     | ((F->w3 & 0x40) ? (1u << 15) : 0u)
                     | ((F->w3 & 0x80) ? (1u << 16) : 0u)
                     | (hq_elig ? (1u << 17) : 0u)
                     | (((uint32_t)F->otbias & 0x1Fu) << 24);
        /* Material bucket, keyed exactly as the walk's provenance tag is:
         * Tiledef id + env-rewrite membership. Both LOD halves of a face
         * share it (the selector keys on the Tiledef, not on the page), so
         * it rides mat_far0 alone and the near half needs no copy. */
        const int hp_bucket = openpete_mods_material_terrain_bucket(
            (F->tb == -1) ? -1 : (F->tb & 0x7F), F->tgen != 0, 0);
        T.matf0 = tgpu_pack_mat0(&F->mat) | ((uint32_t)hp_bucket << 24);
        T.matf1 = (uint32_t)(F->mat.clut_raw & 0xFFFF);
        T.matn0 = tgpu_pack_mat0(&F->matN); T.matn1 = (uint32_t)(F->matN.clut_raw & 0xFFFF);
        /* Scroll tile id (flags16 bits 5-12): the vertex stage adds the
         * per-build fractional V offset for this Tiledef (zero when the
         * tile isn't scrolling). */
        if (F->tb != -1)
            T.flags16 = (uint16_t)((((uint32_t)F->tb & 0x7Fu) << 5) | 0x1000u);
        if (F->mat.blend) T.flags16 |= 0x4000u;   /* cover-drop key (bit 14) */
        uint32_t nearC[4], farC[4];
        const uint16_t co[4] = { F->c0, F->c1, F->c2, F->c3 };
        for (int q = 0; q < 4; q++) {
            nearC[q] = rd(cpu, SC->nearBase + co[q]) & 0xFFFFFFu;
            farC[q]  = rd(cpu, SC->farBase  + co[q]) & 0xFFFFFFu;
        }
        const int vi[4] = { F->i0, F->i1, F->i2, F->i3 };
        tgpu_corner_t k[4];
        memset(k, 0, sizeof(k));
        for (int q = 0; q < 4; q++) {
            k[q].p = tgpu_p4_of(&SC->hpV[vi[q]]);
            k[q].pp = (anim && !vsnap) ? tgpu_p4_of(&SC->hpVprev[vi[q]]) : k[q].p;
            T.pc[q] = k[q].pp;
            k[q].rgb_near = nearC[q]; k[q].rgb_far = farC[q];
        }
        int da = tgpu_arena_of(&F->mat);
        if (F->quad) {
            /* GT4: i0=TL, i1=TR, i3=BL, i2=BR; UV [TL,TR,BL,BR]; diagonal i1-i3 */
            k[0].uf = (uint8_t)F->tu[0]; k[0].vf = (uint8_t)F->tv[0];
            k[1].uf = (uint8_t)F->tu[1]; k[1].vf = (uint8_t)F->tv[1];
            k[3].uf = (uint8_t)F->tu[2]; k[3].vf = (uint8_t)F->tv[2];
            k[2].uf = (uint8_t)F->tu[3]; k[2].vf = (uint8_t)F->tv[3];
            k[0].un = (uint8_t)F->tuN[0]; k[0].vn = (uint8_t)F->tvN[0];
            k[1].un = (uint8_t)F->tuN[1]; k[1].vn = (uint8_t)F->tvN[1];
            k[3].un = (uint8_t)F->tuN[2]; k[3].vn = (uint8_t)F->tvN[2];
            k[2].un = (uint8_t)F->tuN[3]; k[2].vn = (uint8_t)F->tvN[3];
            if (hdfold) {   /* replacement entries per emitted tri, both halves */
                float uA[3] = { k[0].uf, k[1].uf, k[3].uf }, vA[3] = { k[0].vf, k[1].vf, k[3].vf };
                float uB[3] = { k[1].uf, k[3].uf, k[2].uf }, vB[3] = { k[1].vf, k[3].vf, k[2].vf };
                float nA[3] = { k[0].un, k[1].un, k[3].un }, wA[3] = { k[0].vn, k[1].vn, k[3].vn };
                float nB[3] = { k[1].un, k[3].un, k[2].un }, wB[3] = { k[1].vn, k[3].vn, k[2].vn };
                int hdF = tgpu_hdfold_face(&F->mat,  uA, vA, uB, vB);
                int hdN = tgpu_hdfold_face(&F->matN, nA, wA, nB, wB);
                if (hdF < 0 || hdN < 0) { fail = 5; goto done; }
                T.matf1 |= (uint32_t)hdF << 16;
                T.matn1 |= (uint32_t)hdN << 16;
            }
            if (tgpu_emit_face_tri(buf, da, ab, with_var, &T, k[0], k[1], k[3], 0) != 0) goto done;
            if (tgpu_emit_face_tri(buf, da, ab, with_var, &T, k[1], k[3], k[2], 1) != 0) goto done;
        } else {
            /* GT3 with the 2-bit UV orientation (triRot) */
            static const int triRot[4][3] = {{0,1,2},{1,3,0},{3,2,1},{2,0,3}};
            const int* tc = triRot[F->trirot & 3];
            for (int q = 0; q < 3; q++) {
                k[q].uf = (uint8_t)F->tu[tc[q]]; k[q].vf = (uint8_t)F->tv[tc[q]];
                k[q].un = (uint8_t)F->tuN[tc[q]]; k[q].vn = (uint8_t)F->tvN[tc[q]];
            }
            if (hdfold) {
                float uA[3] = { k[0].uf, k[1].uf, k[2].uf }, vA[3] = { k[0].vf, k[1].vf, k[2].vf };
                float nA[3] = { k[0].un, k[1].un, k[2].un }, wA[3] = { k[0].vn, k[1].vn, k[2].vn };
                int hdF = tgpu_hdfold_face(&F->mat,  uA, vA, NULL, NULL);
                int hdN = tgpu_hdfold_face(&F->matN, nA, wA, NULL, NULL);
                if (hdF < 0 || hdN < 0) { fail = 5; goto done; }
                T.matf1 |= (uint32_t)hdF << 16;
                T.matn1 |= (uint32_t)hdN << 16;
            }
            if (tgpu_emit_face_tri(buf, da, ab, with_var, &T, k[0], k[1], k[2], 0) != 0) goto done;
        }
        /* HQ 2x2 subdivide variant (terrain_near_emit mirror): corners
         * TL=i0, TR=i1, BL=i3, BR=i2 with RAW near colours; midpoint
         * positions exact ((a+b)>>1 on even x4 sums), midpoint colours via
         * the walk's (a+b)>>1; sub-quad q uses HQ Tiledef entry (q+1) with
         * the D_8006D058 UV-orientation adjust; triangulation
         * (TL,TR,BL)+(TR,BL,BR). Camera-space node values are rebuilt in
         * the vertex stage from the parent corners + the role bits. */
        if (hq_elig) {
            tgpu_corner_t TL = k[0], TR = k[1], BL = k[3], BR = k[2];
            TL.rgb_far = TL.rgb_near = nearC[0];
            TR.rgb_far = TR.rgb_near = nearC[1];
            BL.rgb_far = BL.rgb_near = nearC[3];
            BR.rgb_far = BR.rgb_near = nearC[2];
            tgpu_corner_t mT, mB, mL, mR, Cc;
            memset(&mT, 0, sizeof(mT)); memset(&mB, 0, sizeof(mB));
            memset(&mL, 0, sizeof(mL)); memset(&mR, 0, sizeof(mR));
            memset(&Cc, 0, sizeof(Cc));
            mT.p = tgpu_p4_mid(TL.p, TR.p); mT.rgb_far = tgpu_rgb_mid(TL.rgb_far, TR.rgb_far);
            mB.p = tgpu_p4_mid(BL.p, BR.p); mB.rgb_far = tgpu_rgb_mid(BL.rgb_far, BR.rgb_far);
            mL.p = tgpu_p4_mid(TL.p, BL.p); mL.rgb_far = tgpu_rgb_mid(TL.rgb_far, BL.rgb_far);
            mR.p = tgpu_p4_mid(TR.p, BR.p); mR.rgb_far = tgpu_rgb_mid(TR.rgb_far, BR.rgb_far);
            Cc.p = tgpu_p4_mid(mT.p, mB.p); Cc.rgb_far = tgpu_rgb_mid(mT.rgb_far, mB.rgb_far);
            /* prev twin midpoints (HQ node positions derive from the lerped
             * PARENT corners in-shader; these per-vertex values are unread
             * for hqv verts but kept coherent). */
            mT.pp = tgpu_p4_mid(TL.pp, TR.pp); mB.pp = tgpu_p4_mid(BL.pp, BR.pp);
            mL.pp = tgpu_p4_mid(TL.pp, BL.pp); mR.pp = tgpu_p4_mid(TR.pp, BR.pp);
            Cc.pp = tgpu_p4_mid(mT.pp, mB.pp);
            mT.rgb_near = mT.rgb_far; mB.rgb_near = mB.rgb_far;
            mL.rgb_near = mL.rgb_far; mR.rgb_near = mR.rgb_far;
            Cc.rgb_near = Cc.rgb_far;
            tgpu_corner_t sq[4][4] = {
                { TL, mT, mL, Cc },   /* SQ0 [TL,TR,BL,BR] order */
                { mT, TR, Cc, mR },
                { mL, Cc, BL, mB },
                { Cc, mR, mB, BR },
            };
            uint32_t hqBase = hqTable + (uint32_t)F->hq_idx * 168u;
            for (int q = 0; q < 4; q++) {
                tmat m; float u[4], v[4];
                memset(&m, 0, sizeof(m)); m.bpp = 16;
                uint32_t e = hqBase + (uint32_t)(q + 1) * 8u;   /* entries 1..4 (.s delay-slot bump) */
                decode_tiledef(cpu, e, F->abe, &m, u, v);
                uint32_t ori = rd(cpu, e + 4) >> 25;
                if (ori) {   /* HQ UV orientation (D_8006D058 offset table) */
                    uint32_t w0 = rd(cpu, e), w1 = rd(cpu, e + 4);
                    uint32_t te = 0x8006D058u + ori;
                    uint32_t ew0 = rd(cpu, te), ew1 = rd(cpu, te + 4);
                    uint32_t tl = ((w0 & 0xFFFF) + (ew0 & 0xFFFF)) & 0xFFFF;
                    uint32_t tr = ((w1 & 0xFFFF) + (ew0 >> 16))    & 0xFFFF;
                    uint32_t bl = ((w0 & 0xFFFF) + (ew1 & 0xFFFF)) & 0xFFFF;
                    uint32_t br = ((w0 & 0xFFFF) + (ew1 >> 16))    & 0xFFFF;
                    u[0]=(float)(tl & 0xFF); v[0]=(float)((tl >> 8) & 0xFF);
                    u[1]=(float)(tr & 0xFF); v[1]=(float)((tr >> 8) & 0xFF);
                    u[2]=(float)(bl & 0xFF); v[2]=(float)((bl >> 8) & 0xFF);
                    u[3]=(float)(br & 0xFF); v[3]=(float)((br >> 8) & 0xFF);
                }
                tgpu_face_t H = T;
                /* HQ variant + sub-quad + the HQ RECORD's scroll tile id
                 * (the near-emit's dv lookup keys on hq_idx, not tb;
                 * hq_idx is w2 & 0x7F, always in range). */
                H.flags16 = (uint16_t)(1u | ((uint16_t)q << 1)
                          | (((uint32_t)F->hq_idx & 0x7Fu) << 5) | 0x1000u
                          | (m.blend ? 0x4000u : 0u));   /* cover-drop key */
                /* HQ cells are sub-quads of this HP face: same Tiledef, so
                 * the same bucket (the pack overwrites the whole word). */
                H.matf0 = tgpu_pack_mat0(&m) | ((uint32_t)hp_bucket << 24);
                H.matf1 = (uint32_t)(m.clut_raw & 0xFFFF);
                H.matn0 = 0; H.matn1 = 0;   /* near cells never re-select */
                tgpu_corner_t c0 = sq[q][0], c1 = sq[q][1], c2 = sq[q][2], c3 = sq[q][3];
                c0.uf = (uint8_t)u[0]; c0.vf = (uint8_t)v[0]; c0.role = 0;
                c1.uf = (uint8_t)u[1]; c1.vf = (uint8_t)v[1]; c1.role = 1;
                c2.uf = (uint8_t)u[2]; c2.vf = (uint8_t)v[2]; c2.role = 2;
                c3.uf = (uint8_t)u[3]; c3.vf = (uint8_t)v[3]; c3.role = 3;
                if (hdfold) {   /* HQ tiles replace like any textured half */
                    float uA[3] = { u[0], u[1], u[2] }, vA[3] = { v[0], v[1], v[2] };
                    float uB[3] = { u[1], u[2], u[3] }, vB[3] = { v[1], v[2], v[3] };
                    int hd = tgpu_hdfold_face(&m, uA, vA, uB, vB);
                    if (hd < 0) { fail = 5; goto done; }
                    H.matf1 |= (uint32_t)hd << 16;
                }
                int ha = tgpu_arena_of(&m);
                if (tgpu_emit_face_tri(buf, ha, ab, with_var, &H, c0, c1, c2, 0) != 0) goto done;
                if (tgpu_emit_face_tri(buf, ha, ab, with_var, &H, c1, c2, c3, 1) != 0) goto done;
            }
        }
    }

    /* ---- upload ---- */
    {
        tgpu_slot_t* S = &s_tgpu[slot];
        uint32_t first[OPTC_ARENA_COUNT] = {0};
        int a;
        for (a = 0; a < OPTC_ARENA_COUNT; a++) {
            if (buf[a].n == 0) continue;
            if (openpete_tercache_chunk_alloc(a, buf[a].n, &first[a]) != 0) break;
        }
        if (a < OPTC_ARENA_COUNT) {   /* arena full: roll back this sector */
            for (int b = 0; b < a; b++)
                if (buf[b].n) openpete_tercache_chunk_free(b, first[b], buf[b].n);
            goto done;
        }
        for (a = 0; a < OPTC_ARENA_COUNT; a++) {
            if (buf[a].n == 0) { S->count[a] = 0; continue; }
            openpete_tercache_chunk_write(a, first[a], buf[a].v, buf[a].n);
            if (buf[a].anim)
                openpete_tercache_chunk_write_prev(a, first[a], buf[a].pv, buf[a].n);
            if (buf[a].nearv)
                openpete_tercache_chunk_write_ctx(a, first[a], buf[a].cx, buf[a].n);
            S->first[a] = first[a]; S->count[a] = buf[a].n;
        }
        ok = 1;
    }
done:
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        free(buf[a].v); free(buf[a].pv); free(buf[a].cx);
    }
    return ok ? 0 : fail;
}

/* Texture-pack claim scan: does any pack file touch any face's texel box?
 * Both tiledef halves — the walk picks far/near per present, and draws query
 * per-tri sub-boxes, so the FACE box is tested by intersection
 * (texture_pack_overlaps), the conservative cover of every draw-time
 * containment query. LP is untextured, so HP faces only. A resolve MISS
 * (texture not yet uploaded) is a consistent non-claim: the draw path's
 * resolve misses identically, and the upload bumps the registry generation,
 * which re-runs this scan. Serial prepass only (the resolve pipeline is not
 * audited for concurrent callers — same rule as the LP-grade pre-warm). */
static int tgpu_pack_scan(const tscache_t* SC) {
    for (int f = 0; SC->hpF && f < SC->nHpF; f++) {
        const tsface_t* F = &SC->hpF[f];
        for (int half = 0; half < 2; half++) {
            const tmat*  m  = half ? &F->matN : &F->mat;
            const float* tu = half ? F->tuN : F->tu;
            const float* tv = half ? F->tvN : F->tv;
            if (!m->textured) continue;
            float u0 = tu[0], v0 = tv[0], u1 = u0, v1 = v0;
            for (int k = 1; k < 4; k++) {
                if (tu[k] < u0) u0 = tu[k];
                if (tu[k] > u1) u1 = tu[k];
                if (tv[k] < v0) v0 = tv[k];
                if (tv[k] > v1) v1 = tv[k];
            }
            texture_pack_id id;
            if (!texture_pack_resolve(m->tpx, m->tpy, m->clx, m->cly, m->bpp,
                                      (int)u0, (int)v0, (int)u1, (int)v1, &id))
                continue;
            if (texture_pack_overlaps(&id)) return 1;
        }
    }
    return 0;
}

/* Validate-or-rebuild a sector's chunks. Returns 0 when the GPU may draw it
 * this present; a nonzero claim reason (tgpu_slot_t.unbuildable codes)
 * claims it back to the CPU walk. want_near = this present needs the sector's
 * near-variant (clip-fan) chunks; a slot without them yet rebuilds to add
 * them and keeps them for the rest of the sector's residency. */
static int tgpu_ensure(CPUState* cpu, const tscache_t* SC, uint32_t hqTable,
                       int sec_index, int want_near) {
    int slot = (int)(SC - s_tsc);
    /* Backstop only: tsc_get never places a sector in OPTC_SLOT_DEAD (its
     * 9-bit face_flags id is the dead-stamp sentinel, so baked chunks for it
     * would be killed as dead). */
    if (slot < 0 || slot >= (int)TSC_SLOTS || slot == (int)OPTC_SLOT_DEAD) return 1;
    tgpu_slot_t* T = &s_tgpu[slot];
    /* Decode-cache slot CONTESTED this build: two visible sectors share one
     * probe window, and the decode cache re-resolved this slot to a second
     * sector after the first already validated as GPU-owned. Stealing the
     * slot here would free the first sector's chunks and republish the
     * slot's origin AFTER its face walk was skipped — the first sector
     * simply vanishes (the gnorc_cove water-border wedge). The loser walks
     * CPU instead; the first keeper's retained state stays intact. */
    if (T->s != SC->s && T->s != 0 && T->ownFrame == openpete_frame) return 4;
    /* Pack/selector claims (tier 1 of the exclusion→claim→fold ladder),
     * latched per slot and re-scanned only when an input generation moves.
     * Checked BEFORE the dirty build: a claimed sector renders via the CPU
     * walk, so validating or rebuilding chunks for it would be waste — its
     * existing chunks stay allocated but unpublished (SF_DRAWN clear) until
     * the claim lifts, when the normal dirty check below takes over. */
    if (T->scanDecodeGen != SC->decodeGen || T->scanTexGen != SC->texGen) {
        T->scanDecodeGen = SC->decodeGen; T->scanTexGen = SC->texGen;
        T->tpGen = 0; T->selEpoch = 0;      /* force both rescans */
    }
    {
        uint32_t g = texture_pack_generation();
        if (openpete_native_hdfold_on()) {
            /* HD fold: matched faces bake replacement entries instead of
             * claiming (tgpu_build_sector interns per emitted tri; an
             * unbakeable face returns the tier-1 claim from the build).
             * A moved registry re-bakes so newly-loaded files take. */
            T->tpClaim = 0;
            if (T->tpGen != g) T->tpGen = g;
        } else {
            if (T->tpGen != g) {
                T->tpGen = g;
                T->tpClaim = texture_pack_replacements_active()
                           ? (uint8_t)tgpu_pack_scan(SC) : 0;
            }
            if (T->tpClaim) return 5;
        }
    }
    /* Mod materials no longer claim: a face carries its material's banked
     * bucket, and the retained draw issues a pass per bucket. The epoch is
     * still latched here because the BAKE depends on it — a selector set
     * that moved leaves the stamped buckets naming the wrong pass. */
    T->selEpoch = openpete_mods_material_terrain_epoch();
    /* Near-variant chunks: kept for as long as the slot holds this sector, so
     * a sector oscillating across the near threshold re-bakes at most once. */
    int with_var = want_near || (T->bakeVar && T->s == SC->s);
    int dirty = (with_var && !T->bakeVar)
             || (T->s != SC->s) || (T->decodeGen != SC->decodeGen)
             || (T->texGen != SC->texGen) || (T->vertGen != SC->vertGen)
             /* poly-anim: every canonical roll re-bakes (prev,cur) — the
              * content hash alone would leave a paused anim's chunk lerping
              * its last step forever. Sub-tick builds don't roll, so this
              * stays clean between ticks. */
             || (SC->polyanim && T->animRoll != SC->vrollPres)
             /* HD fold: the pack registry moved since this bake (files
              * uploaded/cleared) — re-intern the sector's entries; a
              * fold-claimed sector retries when the fold state moved
              * (a queue drain can unblock its overflow). */
             || (openpete_native_hdfold_on() &&
                 (T->bakeTpGen != T->tpGen ||
                  (T->unbuildable == 5 &&
                   T->bakeHdEpoch != openpete_native_hdfold_epoch())))
             /* Material buckets are baked into the faces, so a selector set
              * that moved leaves them naming the wrong pass. */
             || (T->bakeSelEpoch != T->selEpoch);
    if (!dirty && T->lastColFrame != openpete_frame) {
        /* Colour liveness once per VBL (colours can only change per tick). */
        uint32_t h;
        if (tgpu_col_hash(SC, &h) != 0) return 3;
        T->lastColFrame = openpete_frame;
        if (h != T->colHash) { dirty = 1; T->colHash = h; }
    }
    static int s_tcdbg = -1;
    if (s_tcdbg < 0) { const char* e = getenv("OPENPETE_TC_DBG"); s_tcdbg = (e && *e && *e != '0'); }
    if (dirty) {
        if (s_tcdbg)
            fprintf(stderr, "[tcf] rebuild slot=%d sec=%u gen=%u->%u stamp=%08x var=%d->%d s=%u->%u vert=%u->%u tex=%u->%u col=%d freed=%u/%u\n",
                    slot, (unsigned)SC->s, T->gen, (T->gen + 1u) & 7u, openpete_native_scene_build_stamp(),
                    T->bakeVar, with_var, (unsigned)T->s, (unsigned)SC->s, T->vertGen, SC->vertGen, T->texGen, SC->texGen,
                    T->lastColFrame == openpete_frame, T->first[0], T->count[0]);
        tgpu_free_slot(T);
        T->gen = (uint8_t)((T->gen + 1u) & 7u);
        T->s = SC->s;
        T->decodeGen = SC->decodeGen; T->texGen = SC->texGen; T->vertGen = SC->vertGen;
        T->animRoll = SC->vrollPres;
        T->bakeTpGen = T->tpGen;
        T->bakeHdEpoch = openpete_native_hdfold_epoch();
        T->bakeSelEpoch = T->selEpoch;
        T->lastColFrame = openpete_frame;
        if (tgpu_col_hash(SC, &T->colHash) != 0) { T->unbuildable = 3; return 3; }
        T->bakeVar = (uint8_t)(with_var != 0);
        T->unbuildable = tgpu_build_sector(cpu, slot, SC, hqTable, sec_index, with_var);
        s_tgpu_rebuilds++;
    }
    if (T->unbuildable == 0) T->ownFrame = openpete_frame;
    return T->unbuildable;
}

static void tgpu_census(void) {   /* OPENPETE_TERRAIN_GPU_DBG: ~1/s */
    static int on = -1, n;
    if (on < 0) { const char* e = getenv("OPENPETE_TERRAIN_GPU_DBG"); on = (e && *e && *e != '0'); }
    if (!on || ++n < 120) return;
    n = 0;
    openpete_tercache_stats_t st;
    openpete_tercache_get_stats(&st);
    fprintf(stderr, "[ter-gpu f=%u] sectors=%ld nvar=%ld claims=%ld (near=%ld portal=%ld "
            "anim=%ld scroll=%ld fill=%ld arena=%ld col=%ld slot=%ld tp=%ld "
            "nearb=%ld nodraw=%ld prim=%ld pcov=%ld) rebuilds=%ld "
            "chunks=%u verts=%u/%u draws=%u frees-pend=%u\n",
            openpete_frame, s_tgpu_secs, s_tgpu_nvsecs, s_tgpu_claims,
            s_tgcl_near, s_tgcl_portal, s_tgcl_anim, s_tgcl_scroll,
            s_tgcl_fill, s_tgcl_build, s_tgcl_colour, s_tgcl_slot,
            s_tgcl_texpack, s_tgcl_nearb, s_tgcl_nodraw,
            s_tgcl_prim, s_tgcl_pcov,
            s_tgpu_rebuilds,
            st.chunks, st.verts_used, st.verts_cap, st.draws_last_present,
            st.frees_pending);
    s_tgpu_secs = s_tgpu_claims = s_tgpu_rebuilds = s_tgpu_nvsecs = 0;
    s_tgcl_near = s_tgcl_portal = s_tgcl_anim = s_tgcl_scroll = 0;
    s_tgcl_fill = s_tgcl_build = s_tgcl_colour = s_tgcl_slot = 0;
    s_tgcl_texpack = s_tgcl_nearb = s_tgcl_nodraw = 0;
    s_tgcl_prim = s_tgcl_pcov = 0;
}


/* Per-walk shared inputs, latched by the build prologue BEFORE dispatch and
 * read-only during the walk (contract C2: the game fiber is parked for the
 * whole build; workers read guest RAM + latched state only, and write only
 * their own tn_wctx). */
typedef struct {
    CPUState* cpu;
    int M[3][3], Mbin[3][3];
    double camx, camy, camz;
    int fogRefBase, cullRefS2, texTblMode;
    int tier_faithful, lod_handoff;
    uint32_t hqTable;
    const uint16_t* visidx; int nvis;
    const tscache_t** SC;                /* prepass-resolved; NULL = skip */
    const uint32_t* secptr;              /* raw sector pointers (debug identity) */
    /* mid-walk env toggles, hoisted from the tn_env_vbl call sites (the
     * per-VBL re-latch is not thread-safe; the prologue latches per walk) */
    int holefill, lp_farcull, lp_admit, hp_drop;
    /* Per-sector verdicts + retained-GPU ownership, computed by the serial
     * prepass (one computation site; workers only read). Indexed by vi. */
    const struct tn_secverd_s* verd;
    const uint8_t* own;
} tn_wcfg;

/* Per-sector tier/fill verdicts (the walk head's sector-level block, hoisted
 * into the prepass so the GPU-claim decision and the face walk share ONE
 * computation). cbin stays double: the debug prints quote it raw. */
typedef struct tn_secverd_s {
    double  cbin;
    uint8_t drawHP, drawLP, lpFarOnly, lpGapOnly, fillLP, secInLOD;
} tn_secverd;
static tn_secverd s_pre_verd[1024];
static uint8_t    s_tgpu_own[1024];      /* GPU owns this sector this walk */

/* Sector-level tier/fill verdicts (pure function of SC + latched walk
 * config). The comment blocks explaining each rule live at the original
 * decision sites in walk_sector — this is the same math, computed once per
 * sector in the prepass. */
static void sector_verdicts(const tscache_t* SC, const tn_wcfg* G, tn_secverd* V) {
    int flags = SC->flags;
    int nLpF = SC->nLpF, nHpF = SC->nHpF;
    double cbin;
    { double c1m, c2m;
      ngte_mac(G->Mbin, G->camy - SC->centreY, G->camz - SC->centreZ,
               SC->centreX - G->camx, &c1m, &c2m, &cbin); }
    int lodThr = G->fogRefBase >> 2;
    int drawHP = !(flags & 0x4000) && nHpF > 0;
    int drawLP = !(flags & 0x2000) && nLpF > 0 && ((flags & 0x8000) || !drawHP);
    if (G->tier_faithful) {
        drawHP = drawHP && ((double)lodThr > cbin - SC->radius);
        drawLP = !(flags & 0x2000) && nLpF > 0 &&
                 ((flags & 0x8000) || (double)lodThr < cbin + SC->radius + 0x100);
    }
    int secInLOD = ((double)lodThr * (4.0 / s_fogscale) > cbin - SC->radius);
    int lpFarOnly = 0, lpGapOnly = 0;
    if (!drawLP && !(flags & 0x2000) && nLpF > 0) {
        if (G->lod_handoff && !G->tier_faithful && drawHP) { drawLP = 1; lpFarOnly = 1; }
        else if (s_gapfill_en && SC->nGapfill > 0) { drawLP = 1; lpGapOnly = 1; }
    }
    int fillLP = G->holefill && !(flags & 0x2000) && nLpF > 0
                 && ((double)lodThr < cbin + SC->radius + 0x100)
                 && !(openpete_no_lod && drawHP);
    V->cbin = cbin;
    V->drawHP = (uint8_t)drawHP; V->drawLP = (uint8_t)drawLP;
    V->lpFarOnly = (uint8_t)lpFarOnly; V->lpGapOnly = (uint8_t)lpGapOnly;
    V->fillLP = (uint8_t)fillLP; V->secInLOD = (uint8_t)secInLOD;
}

#define TN_MAX_WORKERS 8
/* Walked-face count below which the face walk runs inline instead of fanning
 * out — see the dispatch break-even derivation at the workpool call site. */
#define TN_FANOUT_MIN_FACES 400
static tn_wctx s_wctx[TN_MAX_WORKERS];
static ttri*   s_wbuf[TN_MAX_WORKERS];   /* parallel-mode output buffers (lazy) */
static const tscache_t* s_pre_SC[1024];
static uint32_t s_pre_sec[1024];
static struct { uint8_t worker; int start, count; } s_pre_rec[1024];

/* One sector's face walk (all three passes), emitting into W. Extracted
 * verbatim from the build loop; the aliases below stand in for what used to
 * be the enclosing function's locals. */
static void walk_sector(tn_wctx* W, tn_wcfg* G, int vi) {
    CPUState* cpu = G->cpu;
    int (*M)[3] = G->M; int (*Mbin)[3] = G->Mbin;
    double camx = G->camx, camy = G->camy, camz = G->camz;
    int fogRefBase = G->fogRefBase, cullRefS2 = G->cullRefS2, texTblMode = G->texTblMode;
    int tier_faithful = G->tier_faithful, lod_handoff = G->lod_handoff;
    uint32_t hqTable = G->hqTable;
    int nvis = G->nvis;
    uint32_t i = G->visidx[vi];
    uint32_t s = G->secptr[vi];
    const tscache_t* SC = G->SC[vi];
    lvert* sv = W->sv;
    (void)M; (void)Mbin; (void)nvis; (void)s; (void)cullRefS2;
    (void)tier_faithful;
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
         * gate. lodThr = m_LodDistance>>4 (the same lodDist that drives fog:
         * ir0 = fogRef−4·m3 crosses 0 exactly at m3 = lodThr). */
        /* Sector-level verdicts come precomputed from the prepass
         * (sector_verdicts — ONE computation site shared with the
         * retained-GPU claim; the .s binning/tier rules are documented
         * there and at the historical sites below). */
        const tn_secverd* SV = &G->verd[vi];
        double cbin = SV->cbin;
        int lodThr = fogRefBase >> 2;
        int drawHP = SV->drawHP;
        int drawLP = SV->drawLP;
        if (s_fardbg_active)
            fprintf(stderr, "[fardbg-sec] s=%d flags=%04X cbin=%.1f rad=%d lodThr=%d fogRefBase=%d "
                            "drawHP=%d drawLP=%d nLpF=%d nHpF=%d\n",
                    (int)i, flags, cbin, SC->radius, lodThr, fogRefBase,
                    drawHP, drawLP, nLpF, nHpF);
        /* OPENPETE_TERRAIN_BINCHECK=<frame> — gate: compare the native
         * sector-bin verdict against the .s's OWN HP list, decoded from the
         * render arena after this tick's func_800258F0 ran. The HP list
         * (arena+0x1C00, `ptr|bits` entries, NULL-terminated) SURVIVES the
         * call; the LP list (+0x2000) is clobbered by the HP walk's subdivide
         * lists, so LP membership is attested by the face-set join instead.
         * arena+0x44D4 (D_800771C8-D_8006FCF4) is the pre-pass's surviving
         * frustum-visible byte map (0xFF = passed), scoping the compare:
         * native deliberately draws sectors the .s frustum-culls (GPU clips).
         * Expected: native drawHP == HP-list membership for every
         * frustum-visible sector. */
        {
            static int s_binchk = -2;
            if (s_binchk == -2) {
                const char* e = getenv("OPENPETE_TERRAIN_BINCHECK");
                s_binchk = (e && *e) ? atoi(e) : 0;
            }
            if (s_binchk && openpete_frame >= (unsigned)s_binchk
                && openpete_frame < (unsigned)s_binchk + 2u && !openpete_no_lod) {
                extern uint32_t psx_runtime_g_sony_image_base;
                uint32_t B = psx_runtime_g_sony_image_base;
                static uint32_t hp_list[192]; static int hp_n = -1;
                static unsigned hp_frame;
                if (hp_frame != openpete_frame || hp_n < 0) {
                    hp_frame = openpete_frame; hp_n = 0;
                    for (int k = 0; k < 192; k++) {
                        uint32_t e2 = rd(cpu, B + 0x1C00u + (uint32_t)k * 4);
                        if (!e2) break;
                        hp_list[hp_n++] = e2 & ~3u;
                    }
                    fprintf(stderr, "[bincheck-hdr] frame=%u hpList=%d\n",
                            openpete_frame, hp_n);
                }
                int inHP = 0;
                for (int k = 0; k < hp_n; k++) if (hp_list[k] == s) { inHP = 1; break; }
                uint32_t vw = rd(cpu, game_layout.visibility.base + (i & ~3u));
                int vis = (int)((vw >> ((i & 3u) * 8)) & 0xFF) == 0xFF;
                fprintf(stderr, "[bincheck] s=%u nativeHP=%d listHP=%d vis=%d flags=%04X "
                        "cbin=%.1f rad=%d%s\n",
                        (unsigned)i, drawHP, inHP, vis, flags, cbin, SC->radius,
                        (vis && drawHP != inHP) ? " MISMATCH" : "");
            }
        }
        /* .s HP-list membership (lodThr > cbin − radius): the CLUT-fog ladder
         * is only ever evaluated by the .s INSIDE this envelope — beyond it
         * the .s draws LP, so ladder rows past it are unauthored for many
         * tiles (artisans far water/shore: row+Δ landed on green land
         * palettes, g1 metric 897→1681). Beyond-envelope HP (native's
         * no-pop-in extension) keeps the base palette, which og shows is
         * the closer stand-in for the LP look. The envelope stretches with
         * --fog-scale (k = 4/s_fogscale; ×1 at stock, bit-identical). */
        int secInLOD = SV->secInLOD;
        /* LP ride-alongs for sectors the bin leaves LP-unlisted:
         * - lpFarOnly (legacy, faithful-bin OFF only): the handoff substitutes
         *   a both-tier sector's LP for its dropped far HP. Under the faithful
         *   bin this must NOT fire — an LP-unlisted sector there is one the .s
         *   walks no LP for (fully-near; straddlers land in BOTH lists), and
         *   re-adding it would draw near-sector a=31 faces the game culls at
         *   the sector level.
         * - lpGapOnly: frozen fill-tag faces still ride — ONLY those — so
         *   og-attested hole cover survives the bin (a tag in a fully-near
         *   both-tier or no-HP sector; and under --no-lod, the sparse-HP
         *   holes of HP-drawing sectors; per-face gate below). */
        int lpFarOnly = SV->lpFarOnly, lpGapOnly = SV->lpGapOnly;
        /* LP HOLE-FILL backing: an HP sector that does NOT already
         * draw its LP re-emits it as WORLD_FILL — stencil-gated in core to the
         * pixels no world geometry covered, i.e. the LP↔HP T-junction seams
         * (the artisans distant-water gaps). Because of the stencil gate it can
         * never poke through the HP (the failure of the draw-LP-everywhere
         * attempt); it draws at TRUE LP depth, no tier bias.
         *
         * DISTANCE GATE — the .s LP-list binning condition (func_800258F0's
         * list pre-pass; 0x80025940 is an instruction inside it):
         * LP is fed only when lodThr < centerZ + radius + 0x100 (the sector's
         * far edge crosses the LOD boundary; lodThr = m_LodDistance>>4, centerZ =
         * the projected bounding-CENTRE view-Z, radius = w1 & 0x1FFF). A NEAR
         * sector's LP is never drawn by the PSX, and feeding it here broke the
         * portal apertures: the near arch-hill's coarse LP bulged into the
         * opening (stencil==0 — nothing marks inside an aperture), painted at
         * near depth and depth-killed the portal-sky mask (gate tick 9775).
         * The seams the fill exists for sit exactly AT the LOD boundary, which
         * satisfies this condition by construction. Distance-dependence is
         * confined to sub-pixel seam slivers, so no visible pop-in. NB: reads
         * the RAW RAM lodDist — --lod extended never writes it (host-policy
         * global only), so this stays the real LOD band marker. */
        /* OPENPETE_TERRAIN_HOLEFILL=0 disables (A/B) */
        /* The fill runs for EVERY far-band sector with LP — including sectors
         * whose LP already draws in WORLD (LP-only 0x4000 / force-LP). For
         * those the fill is a screen-DILATED duplicate (see the feed): the PSX
         * integer scanline fill is INCLUSIVE where native's float raster +
         * centre-sampling drops sub-pixel rows, so the far water renders as
         * dashed slivers near screen edges under rotation (the carved-out
         * distant-water horizon gaps, also visible through the green
         * cyclorama). The dilated re-emit grows coverage ~a pixel
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
         * (haunted_towers top-down pose pair, yaw
         * -1046 pitch 992: z=61079 clean vs z=63909 fringed). LP-only /
         * force-LP sectors keep the fill in every mode — the far-water raster
         * slivers it exists for are pass-0 LP, not a tier handoff. */
        int fillLP = SV->fillLP;
        /* (The v9 near-floor fill backstop is retired: its cliff_town
         * crest-strip motivation measured as a NATIVE HP coverage deficit —
         * the pure-HP og reference covers those sightlines — and its
         * rim-sliver false positives were the fringe class the frozen fill
         * tags exclude by review.) */

        /* ---- retained-GPU ownership (terrain sector-mesh cache) ----
         * The prepass claimed/validated this sector's chunks; the GPU renders
         * it from them with the tier/fill verdicts above (published via the
         * sector-params UBO) and the face walk is SKIPPED entirely. The
         * sector_set staging write is host memory, distinct slot per sector —
         * safe from pool workers (contract C3 keeps rhi_* on the owner). */
        if (G->own && G->own[vi]) {
            /* own == 2: owned, NO-DRAW — the whole bounding sphere sits
             * behind the near threshold, so the CPU walk would emit nothing
             * for this sector. Publish no flags at all: the per-build sector
             * reset left SF_DRAWN clear, which is exactly "draw nothing",
             * and the walk is skipped like any owned sector. */
            if (G->own[vi] == 2) return;
            uint32_t sf = OPTC_SF_DRAWN
                | (drawHP    ? OPTC_SF_DRAWHP    : 0u)
                | (drawLP    ? OPTC_SF_DRAWLP    : 0u)
                | (lpFarOnly ? OPTC_SF_LPFARONLY : 0u)
                | (lpGapOnly ? OPTC_SF_LPGAPONLY : 0u)
                | (fillLP    ? OPTC_SF_FILLLP    : 0u)
                | (secInLOD  ? OPTC_SF_SECINLOD  : 0u)
                /* own == 3: near-variant present — draw the clip-fan chunks
                 * and kill the plain ones (the two sets are exclusive). */
                | (G->own[vi] == 3 ? OPTC_SF_NEARVAR : 0u)
                | (((uint32_t)s_tgpu[(int)(SC - s_tsc)].gen & 7u) << 12);
            /* Pack LP grade (lp_grade_apply's source choice, same priority):
             * the bake carries raw LP colours, so the affine rides the
             * sector params for the vertex stage to apply. */
            float grade6[6]; const float* gr = NULL;
            if (s_lpg_active) {
                const float *p = NULL, *q = NULL;
                if (SC->lpg_state == 1)  { p = SC->lpg_p;   q = SC->lpg_q; }
                else if (s_lpg_mean_ok) { p = s_lpg_mean;  q = s_lpg_mean + 3; }
                if (p) {
                    for (int c = 0; c < 3; c++) { grade6[c] = p[c]; grade6[3 + c] = q[c]; }
                    gr = grade6; sf |= OPTC_SF_LPGRADE;
                }
            }
            openpete_tercache_sector_set((int)(SC - s_tsc),
                                         (float)originX, (float)originY,
                                         (float)originZ, sf, gr);
            return;
        }

        W->walk_sectors++;

        /* per-sector accounting for OPENPETE_TERRAIN_SECTORDUMP (spans both passes) */
        int sd_triBefore = W->tri_n;
        double sd_m3min = 1e18, sd_m3max = -1e18;
        float sd_bx0 = 1e18f, sd_by0 = 1e18f, sd_bx1 = -1e18f, sd_by1 = -1e18f;
        int sd_vfront = 0, sd_nv = 0, sd_nf = 0;

      for (int pass = 0; pass < 3; pass++) {
        int passFill = (pass == 2);               /* LP hole-fill backing */
        int passLP = (pass != 1);                 /* 0 = WORLD LP (behind HP), 1 = HP, 2 = fill LP */
        int fmDumpOnly = 0;                       /* facemap: project + dump, never emit */
        if (pass == 0 && !drawLP) {
            /* The inspector forces the same dump-only walk facemap uses, so an
             * LP face is pickable even in a sector whose LP tier never draws —
             * aiming at a hole then names the face that would cover it. */
            if ((s_facemap_active || s_fp_on) && !(flags & 0x2000) && nLpF > 0) fmDumpOnly = 1;
            else continue;
        }
        if (pass == 1 && !drawHP) continue;
        if (passFill && !fillLP) continue;
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
            /* SC->lpg_* is pre-warmed by the serial prepass (tsc_lp_grade),
             * which also feeds the level-mean accumulator; workers only read
             * it here (lp_grade_apply below). */
        } else {
            cverts = SC->hpV; cfaces = SC->hpF; nv = nHpV; nf = nHpF;
            faces = SC->hpFaces; faceStride = 16;
            nearBase = SC->nearBase;                       /* HP near colours (4B) */
            farBase  = SC->farBase;                        /* HP far  colours (4B) */
            if (SC->vfracPres == s_vanim_pres) cvF = SC->hpVf;
        }
        if (!cverts || !cfaces) continue;                  /* alloc failure: skip pass */
        /* Colour bases → host pointers (fallback: the cpu read path). */
        const uint8_t* h_lp   = passLP ? (const uint8_t*)openpete_subtick_host_ptr(lpColBase) : NULL;
        const uint8_t* h_near = passLP ? NULL : (const uint8_t*)openpete_subtick_host_ptr(nearBase);
        const uint8_t* h_far  = passLP ? NULL : (const uint8_t*)openpete_subtick_host_ptr(farBase);
        int host_col = passLP ? (h_lp != NULL) : (h_near != NULL && h_far != NULL);
        W->emit_noHP = passLP;     /* tag emitted tris with this pass's tier */
        W->emit_fill = passFill;   /* fill pass -> WORLD_FILL category in the feed */
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
            if (W->serial && m3 > 0.0 && s_tv_n < MAX_TVERTS) {       /* feed the geometry A/B */
                s_tvx[s_tv_n] = sv[k].sx; s_tvy[s_tv_n] = sv[k].sy; s_tvz[s_tv_n] = m3; s_tv_n++;
            }
        }

        for (int f = 0; f < nf && W->tri_n < W->tri_cap - 4; f++) {  /* clipped quad → ≤4 tris */
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
            if (mat->textured) W->tex_faces++;
            if (i0 >= nv || i1 >= nv || i2 >= nv || i3 >= nv) { W->dbg_oob++; continue; }
            int quad = F->quad;
            W->dbg_faces++;
            W->emit_sec = (int)i; W->emit_face = f;   /* fardbg attribution identity */
            /* Inspector tint class: a listed FILL tag vs a face preview forced
             * through (the untagged candidates). HP faces carry neither. */
            W->emit_fptag = (passLP && F->gapfill) ? (F->fillcand ? 2 : 1) : 0;
            W->emit_env = (F->tgen != 0);            /* env-anim member (rewrite set) */
            /* Portal OT-conformance key: the face's .s OT bucket base — the
             * raw 4-corner view-Z sum ((Σ4 SZ)>>7 in the walk; .s tris sum
             * the shared i2==i3 corner twice, and so does this), expressed
             * as the full-world corner MEAN. Ambient per face so every ttri
             * it emits (direct, near-clipped, or the 2x2 subdivide) carries
             * the PARENT face's key — the .s verdict is per-face, and a
             * split tri's own 3-vert mean can land on the other side of the
             * sky's bucket (the fix4 left-edge blue triangle). */
            W->emit_otkey = (float)((sv[i0].m3 + sv[i1].m3 + sv[i2].m3 + sv[i3].m3) * 4.0);
            /* Authored OT-slot depth offset: the .s adds bias*8 + 0x40 slots
             * to every LP face's OT index and bias*4 to HP's; ported as
             * world units on the TEST depth at the emit (1 slot = 128). Set
             * ambient here so the near/HQ subdivide and clip paths inherit
             * the PARENT face's offset, like otkey. */
            W->emit_nearband = 0;
            W->emit_otslots = passLP ? (uint16_t)(0x40u + 8u * F->otbias)
                                    : (uint16_t)(4u * F->otbias);
            /* EXCEPT any face in the .s's FOREGROUND band (corner-depth sum
             * < 0x2000).
             *
             * HP: the classifier (the sum + flag/corner terms — the exact .s
             * test from gamestate) routes those faces to the near/HQ queues,
             * where they are DrawPrim-drawn, mask-bit-protected, and never
             * inserted into the OT — there is no OT index for the authored
             * bias to offset. Foreground faces contest each other by
             * protected draw order, which raw per-pixel depth approximates;
             * a whole-face world offset instead moves a grazing face's
             * crossing line across the floor (magic_crafters hub: one 4-slot
             * bias flipped a ~30k px slab of near ground to the wrong face;
             * the artisans bridge rail lost its corner poly to the deck
             * underside). The CLASSIFIER is the right gate, not the .s's
             * filled queue: a foreground face the .s clip-rejects before
             * classification (extreme grazing projection — native draws it,
             * GPU-clipped) never reaches the queue yet must not carry a bias
             * either — og never gave it an OT key at all.
             *
             * LP: the LP walk has no defer path, so og DOES key foreground
             * LP (a=31 / force-LP 0x8000 / fill-tag cover — the only LP the
             * admission lets survive this near). But an 8192+-world stored
             * offset on WALKABLE foreground LP (stone_hill's force-LP
             * primary surfaces, the fill water planes) breaks the port's
             * true moby/Spyro-vs-terrain occlusion exactly where it is most
             * contested. Zero it: true depth already orders the s79-style
             * base-under-overlay pair (the HP detail physically sits above
             * its LP base), and exact coplanar ties are the stencil/bias
             * machinery's job, not a world-scale push's. The flag terms are
             * HP-record semantics, so LP tests the distance band alone
             * (word3 = 0 makes the classifier exactly that test). */
            if (face_is_near_canonical(sv[i0].m3, sv[i1].m3, sv[i2].m3,
                                       (quad ? sv[i3].m3 : sv[i2].m3),
                                       passLP ? 0u : hqW3))
                { W->emit_otslots = 0; W->emit_nearband = 1; }
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
             * ABE carve-out RESOLVED: the old
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
                     * palette (foliage confetti on the
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
                /* Every face that reached this select fades on the published
                 * ladder ramp — mark it (plus its fogRef bias) for the
                 * CONTINUOUS pack-fog model. Copy-on-mark: `mat` may still
                 * point into the shared sector cache. */
                if (mat != &matSel) { matSel = *mat; mat = &matSel; }
                matSel.fog_delta = (matSel.fog_delta & 0xFF) | OPENPETE_TP_FOG_CONT
                                 | (F->fogbias ? OPENPETE_TP_FOG_FOGBIAS : 0);
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
            int unrOvf = 0, unrEff = 0;
            {   const int ci[4] = { i0, i1, i2, i3 };
                int nc2 = quad ? 4 : 3;
                for (int q2 = 0; q2 < nc2; q2++) {
                    if (sv[ci[q2]].m3 < s_view.H_canon / 8.0) unrOvf = 1;  /* the GTE's H: the regime does not move with --fov */
                    if (sv[ci[q2]].m3 < s_view.H_eff / 8.0) unrEff = 1;
                }
            }
            NGTE_LIMIT(NGTE_SITE_TERRAIN_UNR, unrOvf, unrEff);
            #define NSAT(v) (unrOvf ? (double)(v) : GTE_SAT(v))
            double nx0 = NSAT(sv[i0].sx), ny0 = NSAT(sv[i0].sy);
            double nx1 = NSAT(sv[i1].sx), ny1 = NSAT(sv[i1].sy);
            double nclip;
            int front, front2 = 0;
            if (faceStride == 8) {
                /* LP walk (block_80026404 quad / block_80026604 tri): no flip
                 * bit, fixed signs, and the corner ORDER differs from HP —
                 *  - HALF 1: NCLIP over (i0, i1, i2) (scanline order, not the
                 *    GT4 slot order), accepted STRICTLY positive.
                 *  - HALF 2 (quads, half 1 flunked): SXY0 re-loaded with i3 →
                 *    NCLIP over (i3, i1, i2), accepted STRICTLY negative (the
                 *    triple's orientation inverts vs half 1).
                 *  - w0 bit 7 = double-sided: the quad path jumps past the
                 *    half-2 verdict straight to the far-cull/admission gates
                 *    (block_80026448→80026484); the tri path adds bit7<<23 =
                 *    0x40000000 to MAC0, which dwarfs any saturated cross.
                 * (The old EXPERIMENTAL LP cull mirrored HP's (i0,i1,i3) order
                 * and sign — the "mixed winding" both-signs regression.) */
                double nx2 = NSAT(sv[i2].sx), ny2 = NSAT(sv[i2].sy);
                nclip = (nx1 - nx0) * (ny2 - ny0) - (nx2 - nx0) * (ny1 - ny0);
                front = (nclip > 0.0);
                if (quad) {
                    double nx3 = NSAT(sv[i3].sx), ny3 = NSAT(sv[i3].sy);
                    double nclip2 = (nx1 - nx3) * (ny2 - ny3) - (nx2 - nx3) * (ny1 - ny3);
                    front2 = (nclip2 < 0.0);
                }
                if (F->dside) front2 = 1;
            } else {
                int h1 = quad ? i3 : i2;
                double nxA = NSAT(sv[h1].sx), nyA = NSAT(sv[h1].sy);
                nclip = (nx1 - nx0) * (nyA - ny0) - (nxA - nx0) * (ny1 - ny0);
                if (flip) nclip = -nclip;
                front = (nclip >= 0.0);
                if (quad) {
                    double nx2 = NSAT(sv[i2].sx), ny2 = NSAT(sv[i2].sy);
                    double nx3 = NSAT(sv[i3].sx), ny3 = NSAT(sv[i3].sy);
                    double nclip2 = (nx1 - nx2) * (ny3 - ny2) - (nx3 - nx2) * (ny1 - ny2);
                    int dside = (F->w3 & 0x4) != 0;
                    front2 = dside || (flip ? (nclip2 >= 0.0) : (nclip2 <= 0.0));
                }
            }
            /* Far-band identity dump: full per-face record for the offline
             * whole-face join vs captured prims. Backfaces included (fr=0 —
             * dumped BEFORE the cull gate so the attribution can name faces
             * neither side draws); fill pass skipped (duplicate of pass 0). */
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
            /* Fill-tag inspector pick — BEFORE every drop gate, so a face the
             * gates reject is still identifiable (that is the under-fill case:
             * the surface that should cover the hole is the one not drawn).
             * The fill pass is skipped: it re-walks faces pass 0 already
             * offered. */
            if (s_fp_on && !passFill)
                fp_test_face(passLP, (int)i, f, F->fillcand, sv, i0, i1, i2, i3, quad);
            if (fmDumpOnly) continue;
            /* fardbg verdict trail: every face dropped AFTER its [fardbg-face]
             * row announces itself, so the offline face-set join can subtract
             * (the dump alone is the pre-verdict universe). r= names the gate. */
            #define FARDBG_DROP(reason) do { \
                if (s_fardbg_active && !passFill) \
                    fprintf(stderr, "[fardbg-drop] s=%d p=%d f=%d r=%s\n", \
                            (int)i, pass, f, (reason)); } while (0)
            if (passLP && !passFill && lpGapOnly && !F->gapfill) { FARDBG_DROP("gaponly"); continue; } /* --no-lod: fill-tag faces only */
            /* Authored per-face LP admission (the .s test, exact): with
             * slot = (four corner SZ3 summed, tris doubling the shared corner)
             * >> 5 and s3 = (m_LodDistance >> 7) - 0x20, an LP face draws iff
             * slot + 8*admit >= s3; a deficit of -31..-1 falls back to
             * per-vertex "draw if ANY corner SZ3 >= (s3 - 8*admit)*8"; deeper
             * deficits reject. LP is a FAR tier — the .s drops it NEAR (the
             * blowhard floating-triangle class: coarse alternates the player
             * is never meant to see up close). Applies in every mode;
             * fill-tag faces are exempt (og-attested hole cover at any range)
             * — but only while OPENPETE_TERRAIN_GAPFILL=1, so the tag A/B
             * knob keeps its contract in original mode too.
             * OPENPETE_TERRAIN_LP_ADMIT=0 restores the legacy fog-corner-only
             * behaviour (A/B). */
            /* .s LP far cull (r_environment.s:792-793 quad / 930-931 tri):
             * slot = ΣSZ3>>5 tested against s2 = m_CullingDistance>>7 — drop
             * iff slot >= s2. A distinct, larger distance than m_LodDistance;
             * every LP face takes it, fill tags included (og-attested cover
             * cannot extend past og's own draw distance). Skipped under
             * --lod extended: extending the draw distance is that mode's
             * charter. OPENPETE_TERRAIN_LP_FARCULL=0 disables (A/B). */
            if (passLP && !passFill && !openpete_no_lod) {
                if (G->lp_farcull) {
                    double fcz3 = quad ? sv[i3].m3 : sv[i2].m3;
                    double fcslot = (sv[i0].m3 + sv[i1].m3 + sv[i2].m3 + fcz3) / 32.0;
                    if (fcslot >= (double)cullRefS2) { FARDBG_DROP("farcull"); continue; }
                }
            }
            if (passLP && !passFill && !(s_gapfill_en && F->gapfill)) {
                if (G->lp_admit) {
                    double sz0 = sv[i0].m3, sz1 = sv[i1].m3, sz2 = sv[i2].m3;
                    double sz3 = quad ? sv[i3].m3 : sz2;   /* .s tris sum the shared corner twice */
                    double slot = (sz0 + sz1 + sz2 + sz3) / 32.0;
                    double s3v = (double)((fogRefBase >> 5) - 0x20);
                    double a1 = slot + 8.0 * (double)F->admit - s3v;
                    if (a1 < 0.0) {
                        if (a1 <= -32.0) { FARDBG_DROP("admit"); continue; }
                        double thr = (s3v - 8.0 * (double)F->admit) * 8.0;
                        if (!(sz0 >= thr || sz1 >= thr || sz2 >= thr ||
                              (quad && sv[i3].m3 >= thr))) { FARDBG_DROP("admit"); continue; }
                    }
                }
            }
            if (lod_handoff) {
                int nc = quad ? 4 : 3, beyond = 0;
                const int ci[4] = { i0, i1, i2, i3 };
                for (int q2 = 0; q2 < nc; q2++)
                    if (fogRef - (int)(sv[ci[q2]].m3 * s_fogscale) <= 0) beyond++;
                if (!passLP) {
                    /* .s HP LOD drop, exact (r_environment.s:1385-1387; tri
                     * 1759-1760): the walk sums the four corner SZ3s (tri
                     * records store i2==i3, so the shared corner counts twice)
                     * and drops the face iff sum >= m_LodDistance — one strict
                     * compare. No per-vertex band, no per-face modifier: the
                     * w3&1 flag pushes FOG saturation out by 0x2000 but not
                     * the drop, so its faces retire at the same distance while
                     * still partly unfogged. SZ3 = viewZ/4 = m3·(s_fogscale at
                     * stock), so the sum compare rides the --fog-scale curve
                     * exactly like the fog it must stay hidden inside
                     * (bit-exact vs the .s at stock fog).
                     * OPENPETE_TERRAIN_HP_DROP=0 restores the legacy
                     * all-corners-fully-fogged approximation, which kept
                     * straddling faces the .s mean-drops and honoured the
                     * fog-push flag the .s drop ignores (A/B). */
                    if (G->hp_drop) {
                        double hpz3 = quad ? sv[i3].m3 : sv[i2].m3;
                        double hpsum = (sv[i0].m3 + sv[i1].m3 + sv[i2].m3 + hpz3)
                                       * s_fogscale;
                        if (hpsum >= 4.0 * (double)fogRefBase) { FARDBG_DROP("hpdrop"); continue; }
                    } else if (beyond == nc) { FARDBG_DROP("hpdrop"); continue; }
                }
                if (passLP && !passFill && lpFarOnly && beyond == 0
                    && !G->lp_admit) {
                    /* Legacy (LP_ADMIT=0) near gate for handoff-riding LP: the
                     * fog-corner test dropped LP until a corner crossed the fog
                     * line, but the .s's actual near cull is the authored
                     * admission above — it admits LP well INSIDE the fog band
                     * (the authored LP/HP overlap is wide), so running both
                     * re-drops mid-band LP the game draws. Original text:
                     * LP far-only — EXCEPT classified HP-coverage-gap fillers
                     * (the frozen fill tags), which draw at all distances so a
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
                    int gfKeep = s_gapfill_en && F->gapfill;
                    if (!s_lpkeep && !gfKeep) { FARDBG_DROP("lpfar"); continue; }
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
            /* HOLE-FILL per-face far gate: only FULLY-FOGGED faces
             * (every corner at/past the exact fog saturation, ir0 = fogRef−4·m3
             * ≤ 0 — the proven decode scale, independent of the shipped
             * s_fogscale). The T-junction seams the fill exists for are at the
             * fully-fogged horizon; a mid-band face admitted here painted a
             * dark 1-2px LP fringe along terrain-vs-sky silhouettes (native's
             * float raster is a hair tighter than the PSX integer edge — gate
             * tick 4888, peace-keepers cliffs). */
            if (passFill) {
                /* Transparent LP never rides the fill: it blends instead of
                 * covering, so a stencil-gated twin over the face's own raster
                 * (transparents write no stencil) would blend the same pixels
                 * TWICE. The fill exists to patch opaque raster slivers. */
                if (F->mat.blend) continue;
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
                 * depth-notches neighbouring geometry (ghosting/spikes at terrain tops, gaps-2 f2770 rocks). */
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
            W->emit_guard = (faceStride == 16 && s_defer_n > 0 &&
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
                if (is_deferred(faces + (uint32_t)f * 16u)) W->near_q_agree++; else W->near_c_only++;
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
                terrain_near_emit(W, cpu, hqTable + (uint32_t)F->hq_idx * 168u,
                                  F->hq_idx, abe, TL, TR, BL, BR);
                W->dbg_spike++;   /* near/HQ cells subdivided */
                FARDBG_DROP("nearhq");  /* emitted subdivided — the face-set
                                         * join can't corner-match the parent */
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
             * Both tiers are whole-face verdicts: the .s emits the full
             * GT4/GT3 when either half accepts (LP: block_80026490's emit
             * path is shared by the front, double-sided and half-2 routes). */
            /* OPENPETE_TERRAIN_CULL bitmask: bit0 = cull HP faces, bit1 =
             * cull LP faces (both default on; each mirrors its walk's exact
             * per-half NCLIP rule above). */
            int cullable = ((faceStride == 16) ? (s_cull & 1) : (s_cull & 2))
                           && !straddle && !passFill;
            int cfront = front || front2;
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
            if (cullable && !cfront) { W->dbg_nclip++; FARDBG_DROP("nclip"); continue; }
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
            /* Pack-aware LP grade: regrade the authored (stock-space) LP
             * colours the way the pack regraded this sector's own textures,
             * restoring the colour agreement the .s handoff relies on. */
            if (passLP && s_lpg_active) {
                rc0 = lp_grade_apply(SC, rc0);
                rc1 = lp_grade_apply(SC, rc1);
                rc2 = lp_grade_apply(SC, rc2);
                if (quad) rc3 = lp_grade_apply(SC, rc3);
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
                emit_tri(W, sv, i0, i1, i2, k0, k1, k2, mat);
                emit_tri(W, sv, i1, i2, i3, k1, k2, k3, mat);
            } else if (quad) {
                emit_tri(W, sv, i0, i1, i3, k0, k1, k3, mat);
                emit_tri(W, sv, i1, i3, i2, k1, k3, k2, mat);
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
                emit_tri(W, sv, i0, i1, i2, t0, t1, t2, mat);
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
            r->nv = sd_nv; r->nf = sd_nf; r->tris = W->tri_n - sd_triBefore; r->vfront = sd_vfront;
            r->ox = originX; r->oy = originY; r->oz = originZ;
            r->m3min = sd_m3min; r->m3max = sd_m3max;
            r->bx0 = sd_bx0; r->by0 = sd_by0; r->bx1 = sd_bx1; r->by1 = sd_by1;
        }
}

static void walk_sector_job(int item, int worker, void* arg) {
    tn_wcfg* G = (tn_wcfg*)arg;
    tn_wctx* W = &s_wctx[worker];
    s_pre_rec[item].count = 0;
    if (!G->SC[item]) return;
    s_pre_rec[item].worker = (uint8_t)worker;
    s_pre_rec[item].start  = W->tri_n;
    walk_sector(W, G, item);
    s_pre_rec[item].count  = W->tri_n - s_pre_rec[item].start;
}

static void terrain_build(CPUState* cpu, int32_t occGroup) {
    ngte_view_build(cpu, &s_view);
    s_vanim_pres++;                /* new build → prior lerp/roll bank stamps go stale */
    terrain_texrecords(cpu);
    terrain_animdbg(cpu);
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
    /* FAITHFUL LOD (OPENPETE_TERRAIN_LOD_FAITHFUL, DEFAULT ON): mirror the .s
     * sector binning — HP list = lodThr > centreZ − radius (HP ends at the LOD
     * distance), LP list = 0x8000 || lodThr < centreZ + radius + 0x100 (LP
     * substitutes beyond it), noLP 0x2000 tested first (overrides force-LP).
     * With the exact per-face tests in place the bin is almost pure pre-pass:
     * every face of an HP-unlisted sector fails the per-face drop anyway
     * (all corners ≥ c − r ≥ lodDist), and the LP bin's +0x100 equals the
     * admission s3's −0x20 (both 4096 world), so a=0 faces agree at the
     * boundary by construction. The behavioural content is the near-sector
     * LP exclusion the per-face admission alone cannot see: an a=31
     * (admit-to-camera) face in a fully-near sector is culled by the .s at
     * the sector level. (The 2026-06 "pop-in" rejection of mirrored lists
     * predated the exact fog + exact per-face model.) Forced OFF under
     * --no-lod: HP-everywhere IS that mode's intent. NB the bin uses the raw
     * lodThr — under a user-stretched --fog-scale the per-face tests ride the
     * curve and this doesn't; stock is exact. =0 restores admission-only. */
    if (s_tier_faithful < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_LOD_FAITHFUL");
        s_tier_faithful = (e && *e) ? (*e != '0') : 1;
    }
    /* no-lod override applied per walk (not baked into the latch): the
     * draw-distance toggle can flip openpete_no_lod mid-level. */
    int tier_faithful = s_tier_faithful && !openpete_no_lod;
    if (s_fogscale < 0.0) {  /* viewZ = m3 (m3 already in SZ3 scale); env-tunable */
        const char* e = getenv("OPENPETE_TERRAIN_FOGSCALE");
        s_fogscale = (e && *e) ? atof(e) : 1.0;
    }
    if (s_fogdpcs < 0) {
        /* DEFAULT ON: the exact decode is
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
     * the fully-fogged band (distant LP "snapping"
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
     * DEFAULT ON: beyond saturation the .s
     * picture IS the LP tier (raw palette — identity-join-proven), and any HP
     * content native paints there is invented (the artisans far-water green
     * stripe once the CLUT ladder landed). The swap happens strictly inside
     * fully-fogged pixels, so there is no visible pop-in — which is why this
     * stays on under --no-lod too (no-lod removes MESH pop-in; fog saturation
     * is the one place a tier swap is invisible by construction). The
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
     * must NOT run: it keys off the RAW-RAM lodDist (--lod extended is pure
     * host-side policy — the openpete_no_lod global; the old recompiler-side
     * read-site saturation is retired and RAM stays 0x8000), so it
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
    int M[3][3]; memcpy(M, s_view.cam_M, sizeof M);   /* m_ProjectionMatrix */
    int Mbin[3][3]; ngte_read_matrix(cpu, game_layout.cam.view_matrix, Mbin); /* .s sector-binning matrix (LP/HP LOD lists) */
    {   /* World->view scale bound for the rim broadphase's sphere radius: M is
         * not orthonormal (the Y row bakes a ~0.625 squash), so a world-space
         * radius is only valid in view space scaled by the largest row norm. */
        double rm = 0.0;
        for (int r = 0; r < 3; r++) {
            double n2 = (double)M[r][0]*M[r][0] + (double)M[r][1]*M[r][1]
                      + (double)M[r][2]*M[r][2];
            if (n2 > rm) rm = n2;
        }
        s_rim_rowmax = sqrt(rm) / 4096.0;
        if (s_rim_rowmax < 1e-6) s_rim_rowmax = 1.0;
        /* Row-2 norm alone bounds view-Z growth per world unit — the
         * retained-GPU near claim's radius scale. */
        s_tgpu_l3 = sqrt((double)M[2][0]*M[2][0] + (double)M[2][1]*M[2][1] +
                         (double)M[2][2]*M[2][2]) / 4096.0;
        if (s_tgpu_l3 < 1e-6) s_tgpu_l3 = 1.0;
    }
    s_ofx = s_view.ofx;
    s_ofy = s_view.ofy;
    s_H   = s_view.H_eff;
    /* Arm the fill-tag inspector's pick point set for this walk (see fp_begin:
     * the crosshair, plus an optional grid sweep). */
    s_fp_on = openpete_fillpick_active();
    if (s_fp_on) fp_begin();
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
    double camx = s_view.cam[0] / 16.0, camy = s_view.cam[1] / 16.0, camz = s_view.cam[2] / 16.0;
    int fogRefBase = (int32_t)rd(cpu, game_layout.env.lod_dist) >> 2;  /* §10 */
    /* Publish this walk's fog-ladder ramp for the CONTINUOUS pack-fog model
     * (HD-replaced draws only; see texture_pack_fog_ladder_set): the .s row
     * select is Δ = (key − fogRef + 4096) >> 8 with key ≈ face-mean
     * m3·fogscale and 16 authored rows, so in full-world view-Z (16·m3)
     * t = Δ/15 = vz·fogscale/(16·3840) + (4096 − fogRef)/3840. Face-level
     * +0x2000 fogRef bias rides the per-draw fog_delta flags. */
    texture_pack_fog_ladder_set(
        s_clutfog ? (float)(s_fogscale / (16.0 * 3840.0)) : 0.0f,
        s_clutfog ? (float)((4096.0 - (double)fogRefBase) / 3840.0) : 0.0f);
    /* Pack-aware LP grade (tsc_lp_grade): fold the previous walk's level mean
     * for no-signal sectors, then arm and reset this walk's accumulator. */
    if (s_lpg_en < 0) { const char* e = getenv("OPENPETE_TEXPACK_LP_FIT");
                        s_lpg_en = !(e && *e == '0'); }
    s_lpg_active = s_lpg_en && texture_pack_replacements_active();
    /* The mean is a stand-in for sectors of THIS level that yielded no fit; it
     * must not survive a level change. A pack covering one level leaves every
     * sector of the next unmatched, so the accumulator stays empty there and
     * the fold below would pin the old level's mean over its whole LP tier
     * indefinitely. */
    { int lvl = (int)(int32_t)rd(cpu, game_layout.level_id);
      static int s_lpg_lvl = -1;
      if (lvl != s_lpg_lvl) {
          s_lpg_lvl = lvl;
          s_lpg_mean_ok = 0;
          s_lpg_acc_n = 0;
          memset(s_lpg_acc, 0, sizeof s_lpg_acc);
          /* HD-fold entry table + atlas are level-lifetime (the pack's
           * identities die with the level's VRAM uploads); baked entry ids
           * die with them — the registry-generation dirty check rebakes. */
          openpete_native_hdfold_reset();
      } }
    if (s_lpg_acc_n > 0) {
        for (int c = 0; c < 6; c++) s_lpg_mean[c] = (float)(s_lpg_acc[c] / s_lpg_acc_n);
        s_lpg_mean_ok = 1;
    }
    memset(s_lpg_acc, 0, sizeof s_lpg_acc);
    s_lpg_acc_n = 0;
    /* .s LP far cull threshold, slot units (r_environment.s:637-642: the walk
     * precomputes s2 = m_CullingDistance >> 7 once, before the sector loop). */
    int cullRefS2 = (int32_t)rd(cpu, game_layout.env.cull_dist) >> 7;
    uint32_t lqTable = rd(cpu, game_layout.env.lq_tex);  /* HP-walk per-quad Tiledef (§8) */
    uint32_t hqTable = rd(cpu, game_layout.env.hq_tex);  /* near/detail Tiledef (bpp8 tiles) */
    tsc_tile_gens(cpu, lqTable, rd(cpu, game_layout.env.tex_count));  /* env-anim liveness */
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
    uint32_t sectorPtr = rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = rd(cpu, game_layout.env.sector_count);
    tsc_vert_anim_refresh(cpu, sectorPtr, sectorCnt);  /* geometry env-anim liveness */
    /* Visible-sector index set: canonical 4:3 verdict (D_800771C8) at native
     * aspect; occlusion-group candidate set in widescreen (terrain_vislist). */
    static uint16_t visidx[1024];
    int nvis = terrain_vislist(cpu, sectorCnt, visidx, occGroup);
    s_tsc_stamp++;   /* LRU epoch: everything touched this walk outlives strangers */
    /* OPENPETE_TERRAIN_TIMERS=1: per-SECTOR walk-cost census (~1/s). Attributes
     * the terrain feeder's µs to individual sectors (accounted at the top of
     * the NEXT iteration so every exit path of the huge loop body is covered)
     * and names the top offenders — built for the gnorc_cove freecam
     * direction-dependent perf hunt. Inert unless the env is set. */
    static int tt_on = -1;
    if (tt_on < 0) { const char* e = getenv("OPENPETE_TERRAIN_TIMERS"); tt_on = (e && *e && *e != '0'); }
    static uint32_t tt_us[1024], tt_tris[1024];      /* window-accumulated per sector */
    static uint64_t tt_win_us, tt_report_t;
    static uint32_t tt_presents, tt_nvis_sum, tt_nvis_max, tt_pres_max_us;
    uint64_t tt_prev = 0; uint32_t tt_prev_i = 0; int tt_prev_valid = 0, tt_prev_tris = 0;
    uint64_t tt_present_us = 0;
    /* ---- parallel sector walk (S1) ----------------------------------------
     * Serial mode (1 thread, any debug/verify instrument active, or the very
     * first walk so every one-time getenv latch initializes single-threaded)
     * fuses walk_sector into the prepass loop below — the exact legacy order,
     * writing s_tri directly. Parallel mode: the prepass resolves the decode
     * cache + occlusion serially (tsc_get mutates shared slots), then the
     * pool walks sectors into per-worker buffers, merged in sector order —
     * byte-identical output to serial by construction (contract C4). */
    {   /* lp_artfix_lookup latches g_LevelId once per VBL inside the walk;
         * resolve it here, serially, so workers only hit the cached path. */
        uint32_t afx_dummy;
        (void)lp_artfix_lookup(cpu, -1, -1, &afx_dummy);
    }
    tn_wcfg G;
    G.cpu = cpu;
    memcpy(G.M, M, sizeof(G.M)); memcpy(G.Mbin, Mbin, sizeof(G.Mbin));
    G.camx = camx; G.camy = camy; G.camz = camz;
    {   /* OPENPETE_TERRAIN_CAMDBG=1 (temp): per-build game-camera pose in
         * --freecam-pose form, for replanting a movie frame's view. */
        static int s_camdbg = -1;
        if (s_camdbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_CAMDBG");
                            s_camdbg = (e && *e && *e != '0'); }
        if (s_camdbg)
            fprintf(stderr, "[cam f=%u] pos=(%d, %d, %d) yaw=%d pitch=%d sub=%d\n",
                    openpete_frame,
                    (int32_t)rd(cpu, game_layout.cam.position),
                    (int32_t)rd(cpu, game_layout.cam.position + 4u),
                    (int32_t)rd(cpu, game_layout.cam.position + 8u),
                    (int16_t)(rd(cpu, game_layout.cam.rotation + 4u) & 0xFFFFu),
                    (int16_t)(rd(cpu, game_layout.cam.rotation) >> 16),
                    s_vanim_sub);
    }
    G.fogRefBase = fogRefBase; G.cullRefS2 = cullRefS2; G.texTblMode = texTblMode;
    G.tier_faithful = tier_faithful; G.lod_handoff = lod_handoff;
    G.hqTable = hqTable;
    G.visidx = visidx; G.nvis = nvis;
    G.SC = s_pre_SC; G.secptr = s_pre_sec;
    {   static tn_envvbl_t hole_c, fc_c, adm_c, hpd_c;
        G.holefill   = tn_env_vbl(&hole_c, "OPENPETE_TERRAIN_HOLEFILL", 1, 1);
        G.lp_farcull = tn_env_vbl(&fc_c,  "OPENPETE_TERRAIN_LP_FARCULL", 1, 1);
        G.lp_admit   = tn_env_vbl(&adm_c, "OPENPETE_TERRAIN_LP_ADMIT", 1, 1);
        G.hp_drop    = tn_env_vbl(&hpd_c, "OPENPETE_TERRAIN_HP_DROP", 1, 1);
    }
    G.verd = s_pre_verd;
    G.own  = s_tgpu_own;
    /* ---- terrain sector-mesh GPU cache: per-walk gate + globals ----
     * When ON, unclaimed sectors skip the CPU face walk and render from
     * their retained GPU chunks; terrain.vert mirrors every per-face
     * verdict. Whole-walk fallback triggers: renderer-side conditions
     * (openpete_native_tercache_usable), any debug env whose semantics live
     * in the CPU walk, the fill-tag inspector (every face must project to be
     * pickable), and non-default texture-table modes. Portal recency claims
     * per-SECTOR (the outline broad-phase below), not the whole walk. */
    /* One canonical tick can invoke this hook SEVERAL times, appending to
     * one scene stamp (the .s renders more than one environment pass — the
     * portal-preview destination render is the common case — and the interp
     * bracket can flip the camera between the calls). One retained state
     * cannot represent a multi-camera scene, so only the FIRST hook build
     * of a stamp drives it; later same-stamp builds walk pure CPU and must
     * NOT touch the staging (a restage would wipe the first build's
     * sectors). Re-extract builds (s_vanim_sub) always drive: their build
     * IS the displayed state. */
    static uint32_t s_tb_last_stamp = 0xFFFFFFFFu;
    int tb_first = s_vanim_sub || openpete_frame != s_tb_last_stamp;
    if (!s_vanim_sub) s_tb_last_stamp = openpete_frame;
    int tgpu_on = tb_first
               && openpete_tercache_ready()
               && openpete_native_tercache_usable()
               && !tgpu_env_block()
               && !s_fp_on
               && texTblMode == 0;
    s_tgpu_pplane_n = 0; s_tgpu_claim_all = 0; s_tgpu_rim_ok = 0;
    if (tgpu_on && openpete_portal_sky_recent()) {
        s_tgpu_pplane_n = openpete_portal_sky_planes(cpu, &s_view, s_tgpu_pplane, 6);
        /* Recent portal but planes momentarily gone: claim everything (safe). */
        if (s_tgpu_pplane_n == 0) s_tgpu_claim_all = 1;
        else s_tgpu_rim_ok = tgpu_portal_frames(cpu);
    }
    openpete_tercache_globals tcg;
    if (tgpu_on) {
        memset(&tcg, 0, sizeof(tcg));
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) tcg.mrow[r][c] = (float)M[r][c];
        tcg.ofx = s_ofx; tcg.ofy = s_ofy; tcg.H = s_H; tcg.H_canon = s_view.H_canon;
        tcg.camx = camx; tcg.camy = camy; tcg.camz = camz;
        tcg.fogscale = s_fogscale;
        /* Poly-anim lerp alpha: sub-tick paired builds carry the spine's
         * alpha; canonical (and unpaired) builds draw cur exactly. */
        tcg.vanim_alpha = (s_vanim_sub && s_vanim_on) ? s_vanim_alpha : 1.0;
        tcg.nearz = s_nearz;   /* near-variant clip plane (clip_push_tri) */
        /* Per-tile UV-scroll offsets for this build (zero when the tile has
         * no fractional offset — canonical builds render the committed
         * whole-texel decode). */
        if (s_tanim_have)
            for (int t = 0; t < 128; t++)
                if (s_tanim_act[t]) {
                    tcg.scroll_dv[t][0] = s_tanim_dvlq[t];
                    tcg.scroll_dv[t][1] = s_tanim_dvhq[t][0];
                    tcg.scroll_dv[t][2] = s_tanim_dvhq[t][1];
                }
        tcg.fog_ref_base = fogRefBase;
        tcg.cull_ref_s2  = cullRefS2;
        tcg.gflags = ((s_nearhq && hqTable) ? OPTC_GF_NEARHQ : 0u)
                   | (s_clutfog       ? OPTC_GF_CLUTFOG : 0u)
                   | ((s_cull & 1)    ? OPTC_GF_CULLHP  : 0u)
                   | ((s_cull & 2)    ? OPTC_GF_CULLLP  : 0u)
                   | (lod_handoff     ? OPTC_GF_HANDOFF : 0u)
                   | (s_gapfill_en    ? OPTC_GF_GAPFILL : 0u)
                   | (openpete_no_lod ? OPTC_GF_NOLOD   : 0u)
                   | ((s_otbias_on && openpete_native_key_resolve_on())
                                      ? OPTC_GF_KEYON   : 0u);
        {   /* OPENPETE_TERRAIN_GPU_TINT=1: retained draws tint by sector id
             * (temp bring-up attribution; unregistered). */
            const char* e = getenv("OPENPETE_TERRAIN_GPU_TINT");
            if (e && *e && *e != '0')
                tcg.gflags |= (1u << 29) | ((uint32_t)(atoi(e) & 7) << 26);
        }
        /* Portal inner-rim frames for the vertex-stage cull (zeroed rim_n
         * from the memset when frames are absent — retained tris then carry
         * no rim verdict). */
        tgpu_rim_globals_fill(&tcg);
        tgpu_cover_globals_fill(&tcg);
    }
    /* OPENPETE_TERRAIN_GPU_CENSUS=1: one stderr line per hook walk with THIS
     * walk's claim counts by reason (deltas of the window counters, printed
     * before tgpu_census can reset them) — the S2b claim-ratchet metric,
     * harvested by scripts/terrain-claim-census.py. Temp instrument;
     * unregistered; GPU_-prefixed so it is safe with the cache engaged. */
    static int s_cen = -1;
    if (s_cen < 0) { const char* e = getenv("OPENPETE_TERRAIN_GPU_CENSUS");
                     s_cen = (e && *e && *e != '0'); }
    long cen0[13];
    /* Any active walk instrument forces serial: their censuses/dumps are
     * whole-walk statics with serial semantics (per-sector timing, ordered
     * dumps), and several latch mid-walk. */
    static int s_dbg_env = -1;
    if (s_dbg_env < 0)
        s_dbg_env = (getenv("OPENPETE_TERRAIN_TIMERS")   || getenv("OPENPETE_TERRAIN_SECTORDUMP") ||
                     getenv("OPENPETE_TERRAIN_FARDBG")   || getenv("OPENPETE_TERRAIN_FACEMAP") ||
                     getenv("OPENPETE_TERRAIN_CALDBG")   || getenv("OPENPETE_TERRAIN_NEARQ") ||
                     getenv("OPENPETE_TERRAIN_FOGDBG")   || getenv("OPENPETE_TERRAIN_CULLDBG") ||
                     getenv("OPENPETE_TERRAIN_BINDBG")   || getenv("OPENPETE_TERRAIN_BINCHECK") ||
                     getenv("OPENPETE_TERRAIN_NEARDBG")  || getenv("OPENPETE_TERRAIN_LPKEEPDBG") ||
                     getenv("OPENPETE_TERRAIN_VERIFY")) ? 1 : 0;
    static int s_first_walk = 1;
    int nw = openpete_workpool_threads();
    if (nw > TN_MAX_WORKERS) nw = TN_MAX_WORKERS;
    int wserial = s_dbg_env || s_fp_on || s_first_walk || nw <= 1;
    s_first_walk = 0;
  tn_retry_serial:;
    /* Per-walk counters reset AFTER the retry label: an eviction-conflict
     * serial retry replaces the aborted walk's numbers instead of summing
     * with them (summed lines inflated census rows exactly 2x). */
    s_tgpu_secs_walk = 0; s_tgpu_claims_walk = 0;
    if (s_cen) {
        cen0[0] = s_tgcl_near;   cen0[1] = s_tgcl_portal;
        cen0[2] = s_tgcl_anim;   cen0[3] = s_tgcl_scroll;
        cen0[4] = s_tgcl_fill;   cen0[5] = s_tgcl_build;
        cen0[6] = s_tgcl_colour; cen0[7] = s_tgcl_slot;
        cen0[8] = s_tgcl_texpack; cen0[9] = s_tgcl_nearb;
        cen0[10] = s_tgcl_nodraw; cen0[11] = s_tgcl_prim;
        cen0[12] = s_tgcl_pcov;
    }
    /* The build stamp: a scene id while an eager scene is being extracted
     * (each scene of a burst gets its own retained state), else the VBL. */
    const uint32_t tc_stamp = openpete_native_scene_build_stamp();
    if (tgpu_on) openpete_tercache_frame_begin(&tcg, tc_stamp, s_vanim_sub);
    else if (openpete_tercache_ready() && tb_first)
        openpete_tercache_frame_skip(tc_stamp, s_vanim_sub);
    int nw_used = wserial ? 1 : nw;
    for (int w = 0; w < nw_used; w++) {
        tn_wctx* W = &s_wctx[w];
        if (wserial) { W->tri = s_tri; W->tri_cap = MAX_TTRIS; }
        else {
            if (!s_wbuf[w]) s_wbuf[w] = (ttri*)malloc(sizeof(ttri) * MAX_TTRIS);
            W->tri = s_wbuf[w]; W->tri_cap = s_wbuf[w] ? MAX_TTRIS : 0;
        }
        W->tri_n = 0;
        W->dbg_faces = W->dbg_oob = W->dbg_nclip = W->dbg_behind = 0;
        W->dbg_spike = W->dbg_clipped = 0;
        W->tex_faces = W->tex_tris = 0;
        W->near_q_agree = W->near_c_only = 0;
        W->walk_sectors = 0;
        W->serial = wserial;
    }
    /* Faces the CPU face walk will actually process this build: sectors the
     * retained-GPU cache owns return from walk_sector before touching a face,
     * so their geometry is not walk work and must not count toward the
     * fan-out decision below. With the cache warm this total collapses to
     * near zero even on a whole-level vista. */
    long walk_faces = 0;
    if (!wserial && op_prof_on()) {
        static int id_ = -1;
        if (id_ < 0) id_ = op_prof_intern("ter.prepass");
        op_prof_begin(id_);
    }
    for (int vi = 0; vi < nvis; vi++) {
        uint32_t i = visidx[vi];
        if (tt_on) {
            uint64_t tnow = openpete_phase_now_us();
            if (tt_prev_valid && tt_prev_i < 1024u) {
                uint32_t d = (uint32_t)(tnow - tt_prev);
                tt_us[tt_prev_i] += d; tt_present_us += d;
                tt_tris[tt_prev_i] += (uint32_t)(s_wctx[0].tri_n - tt_prev_tris);
            }
            tt_prev = tnow; tt_prev_i = i; tt_prev_tris = s_wctx[0].tri_n; tt_prev_valid = 1;
        }
        s_pre_SC[vi] = NULL;
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        s_pre_sec[vi] = s;
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
        /* The fill-tag inspector must see every sector the frustum keeps: a
         * culled sector's faces never project, so they would be silently
         * unpickable. The cull is a conservative perf skip, so dropping it
         * costs only frame rate. */
        if (s_fp_on) s_tocc = 0;
        /* View-space bounding sphere — consumed by the occlusion cull below
         * and stamped for the portal-rim sector broadphase (rim_sector_cands),
         * which needs it for every walked sector, culled or not (a culled
         * sector emits nothing, so its candidacy is moot, but the stamp must
         * cover every sector that CAN emit this walk). */
        double oc1, oc2, oc3;
        ngte_mac(M, camy - SC->centreY, camz - SC->centreZ,
                 SC->centreX - camx, &oc1, &oc2, &oc3);
        if (i < 1024u) {
            s_secv[i][0] = (float)oc1; s_secv[i][1] = (float)oc2;
            s_secv[i][2] = (float)oc3; s_secv[i][3] = (float)SC->radius;
            s_secv_anim[i] = (uint8_t)(SC->polyanim != 0);
            s_secv_stamp[i] = s_vanim_pres;
        }
        if (s_tocc) {
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

        /* Pack-aware LP grade pre-warm: the texture-pack resolve pipeline is
         * not audited for concurrent callers, so the lazy per-sector fit is
         * computed here, serially; the walk only reads SC->lpg_*. */
        if (s_lpg_active) tsc_lp_grade((tscache_t*)SC);
        /* Sector-level tier/fill verdicts — one computation site, consumed
         * by both the retained-GPU claim below and the face walk. */
        sector_verdicts(SC, &G, &s_pre_verd[vi]);
        /* Level-mean accumulator for the pack LP grade: every visible fitted
         * sector whose LP tier draws contributes — the same set the walk's
         * pass-0 entry used to accumulate, now gathered here (serially) so
         * retained sectors contribute without walking. */
        if (s_lpg_active && s_pre_verd[vi].drawLP && SC->lpg_state == 1) {
            for (int c = 0; c < 3; c++) {
                s_lpg_acc[c]     += SC->lpg_p[c];
                s_lpg_acc[3 + c] += SC->lpg_q[c];
            }
            s_lpg_acc_n++;
        }
        /* ---- retained-GPU claim (main thread: chunk builds write RHI
         * buffers — contract C3). An unclaimed sector's chunks are validated
         * or rebuilt here; the walk then publishes its verdict flags and
         * skips the face loop. Claim reasons, in test order:
         *  - near: the bounding sphere may reach the near plane; the CPU
         *    walk near-clips, the GPU path guarantees m3 > 0 instead.
         *  - portal: the sector may carry a feed-time portal drop — a rim
         *    broadphase candidate (inner-rim cull) or a blended-face sector
         *    over an active outline (transparent-cover drop). Opaque
         *    plane-cover needs no claim (per-pixel in core).
         *  - anim: vert-anim sectors lerp fractional banks per present.
         *  - fill: WORLD_FILL twins are screen-dilated in the feed.
         *  - (the pack-aware LP regrade no longer claims: the bake carries
         *    raw colours and the per-sector grade affine rides the sector
         *    params — lp_grade_apply is mirrored in the vertex stage under
         *    OPTC_SF_LPGRADE, own fit and level mean alike.)
         *  - build claims (scroll tiles, arena-full, unmapped colours,
         *    pack-replaced faces, selector-matched faces) come back from
         *    tgpu_ensure. */
        s_tgpu_own[vi] = 0;
        if (tgpu_on) {
            int claim = 0, want_near = 0;
            const double nearBound = s_tgpu_l3 * ((double)SC->radius + 16.0);
            if (oc3 + nearBound < 16.0) {
                /* NO-DRAW verdict: the whole bounding sphere sits behind the
                 * near threshold, so the CPU walk near-culls every face and
                 * emits nothing — owning the sector WITHOUT publishing (or
                 * validating chunks) reproduces that exactly, with zero CPU
                 * walk cost. Genuine near-CROSSERS (sphere straddling the
                 * threshold) still claim below: they need the walk's
                 * near-clip and near-band HQ semantics. */
                s_tgpu_own[vi] = 2;
                s_tgcl_nodraw++;
            } else if (oc3 - nearBound < 16.0 &&
                       (SC->polyanim || !tgpu_near_box_clear(SC, M, camx, camy, camz))) {
                /* Sphere-crossing sector: refined by the vertex AABB. The
                 * sphere bound (centre - l3*(radius+16)) inflates in every
                 * direction, so ~1/5 of the sectors it flags provably keep
                 * every VERTEX at or beyond the same 16.0 view-Z the sphere
                 * test guards — the exact regime every owned sector already
                 * renders in — and stay on the GPU. Sectors with real
                 * near-band/clip/straddle vertices (the bulk: the ground the
                 * camera stands on straddles the eye plane) still claim.
                 * Poly-anim sectors skip the refinement: their live verts
                 * ride the anim banks, not the decoded AABB.
                 *
                 * What survives the refinement takes the NEAR VARIANT: chunks
                 * whose vertex stage re-runs the walk's near-plane clip fan
                 * and its straddle waivers, so the sector still renders from
                 * the GPU. Poly-anim sectors keep the claim (no anim x near
                 * twin), as does the lever-off A/B. */
                if (!SC->polyanim && tgpu_nearvar_on()) {
                    want_near = 1;
                } else {
                    claim = 1; s_tgcl_near++;
                    if (oc3 + nearBound < 16.0)
                        s_tgcl_nearb++;   /* census: fully-behind sub-bucket */
                }
            } else if (s_tgpu_claim_all) {
                claim = 1; s_tgcl_portal++;
            } else if (s_tgpu_pplane_n && oc3 > 1.0) {
                /* Portal claim, refined (see the s_tgpu_pplane comment). With
                 * the rim frames derived, NOTHING portal-side claims: the
                 * inner-rim cull AND the blended-face transparent-cover drop
                 * both run in the vertex stage on the emitted tri rebuilt
                 * from the corner attributes (portal_rim_classify /
                 * portal_sky_covers mirrors — the cover planes ride the
                 * build's globals, blended faces carry flags16 bit 14). */
                int pcl = 0;
                if (!s_tgpu_rim_ok) {
                    /* Refinement frames unavailable (plane provider disabled):
                     * blanket projected-bbox claim, the pre-refinement rule. */
                    const double csx = s_ofx + oc1 * s_H / oc3;
                    const double csy = s_ofy + oc2 * s_H / oc3;
                    const double rpx = (double)SC->radius * s_H / oc3 * 1.5 + 16.0;
                    for (int pi = 0; pi < s_tgpu_pplane_n; pi++) {
                        const openpete_portal_skyplane* pp = &s_tgpu_pplane[pi];
                        if (csx + rpx >= pp->x0 && csx - rpx <= pp->x1 &&
                            csy + rpx >= pp->y0 && csy - rpx <= pp->y1) {
                            pcl = 1; break;
                        }
                    }
                }
                if (pcl) { claim = 1; s_tgcl_portal++; }
            }
            if (!claim && s_tgpu_own[vi] != 2) {
                /* own == 2 (behind-camera no-draw) skips the validate/build
                 * entirely — publishing such a sector DRAWN would hand the
                 * shader mirror behind-camera vertices (negative W), whose
                 * wrapped rasterization poisons the depth prepass. That
                 * guarantee is the near-claim's whole job; no-draw keeps it
                 * by never entering the drawn set. */
                int r = tgpu_ensure(cpu, SC, hqTable, (int)i, want_near);
                if (r == 0) {
                    s_tgpu_own[vi] = want_near ? 3 : 1;
                    if (want_near) s_tgpu_nvsecs++;
                    s_tgpu_secs++; s_tgpu_secs_walk++;
                } else {
                    claim = 1;
                    if (r == 2) s_tgcl_scroll++;
                    else if (r == 3) s_tgcl_colour++;
                    else if (r == 4) s_tgcl_slot++;
                    else if (r == 5) s_tgcl_texpack++;
                    else s_tgcl_build++;
                }
            }
            if (claim) { s_tgpu_claims++; s_tgpu_claims_walk++; }
        }
        s_pre_SC[vi] = SC;
        if (!s_tgpu_own[vi]) walk_faces += SC->nLpF + SC->nHpF;
        if (wserial) walk_sector(&s_wctx[0], &G, vi);
    }
    if (!wserial) {
        /* Decode-cache eviction hazard: the prepass resolves EVERY visible
         * sector's slot before any walk, so two visible sectors colliding in
         * one probe window leave the earlier SC pointer describing the WRONG
         * sector (the serial walk re-resolves per sector and never sees
         * this). Validate every resolved slot still owns its sector; any
         * mismatch falls the whole present back to the serial fused walk,
         * whose per-sector re-resolve is the legacy (thrash-but-correct)
         * behaviour. Rare: needs 9+ visible sectors in one 8-way window
         * (the gnorc_cove-class layouts). */
        int tsc_evicted = 0;
        for (int vi = 0; vi < nvis; vi++)
            if (s_pre_SC[vi] && s_pre_SC[vi]->s != s_pre_sec[vi]) { tsc_evicted = 1; break; }
        if (tsc_evicted) {
            static unsigned s_ev_last; static int s_ev_n;
            if (openpete_frame >= s_ev_last + 600u) {   /* ~10s throttle */
                fprintf(stderr, "[terrain-walk] tsc eviction conflict (nvis=%d) x%d"
                        " -- serial fallback\n", nvis, s_ev_n + 1);
                s_ev_last = openpete_frame; s_ev_n = 0;
            } else s_ev_n++;
            if (op_prof_on()) op_prof_end();   /* ter.prepass */
            wserial = 1;
            goto tn_retry_serial;
        }
        if (op_prof_on()) {
            op_prof_end();   /* ter.prepass */
            static int id_ = -1;
            if (id_ < 0) id_ = op_prof_intern("ter.walk");
            op_prof_begin(id_);
        }
        /* Fan out only when the walk is big enough to repay the dispatch.
         * Waking the helpers and joining them costs ~20us whatever the run
         * holds, and the pool's own item stream is the same work in the same
         * order either way, so running the jobs inline below the threshold is
         * bit-identical and strictly cheaper. Break-even is where the ~7/8 of
         * the walk the helpers absorb equals that floor: at the measured
         * 0.032-0.056us per walked face the crossover sits between ~410 and
         * ~720 faces depending on the scene, so the constant sits under the
         * whole range. Erring low is the cheap direction — fanning out 400
         * faces too eagerly wastes ~20us, while serializing a cache-cold
         * 10k-face vista would cost ~300. With the retained-GPU cache warm
         * walk_faces is 0 even on a whole-level vista (every sector is owned
         * and returns before touching a face), which is the case this gate
         * exists for. */
        /* Calibration override: re-derive the bound on other hardware without
         * a rebuild (sweep it against the ter.walk / ter.faces census). */
        static int s_fanmin = -1;
        if (s_fanmin < 0) { const char* e = getenv("OPENPETE_TERRAIN_FANOUT_MIN");
                            s_fanmin = (e && *e) ? atoi(e) : TN_FANOUT_MIN_FACES; }
        if (walk_faces < s_fanmin)
            for (int vi = 0; vi < nvis; vi++) walk_sector_job(vi, 0, &G);
        else
            openpete_workpool_run(nvis, walk_sector_job, &G);
        if (op_prof_on()) {
            op_prof_end();   /* ter.walk */
            static int idf_ = -1;
            if (idf_ < 0) idf_ = op_prof_intern("ter.faces");
            op_prof_count(idf_, (uint64_t)walk_faces);
            /* 1 when this walk fanned out, 0 when it ran inline: the census
             * average is the fraction of walks above the threshold. */
            static int idp_ = -1;
            if (idp_ < 0) idp_ = op_prof_intern("ter.par");
            op_prof_count(idp_, walk_faces < s_fanmin ? 0u : 1u);
            static int id_ = -1;
            if (id_ < 0) id_ = op_prof_intern("ter.merge");
            op_prof_begin(id_);
        }
        /* Ordered merge: worker runs land in s_tri in sector order, so the
         * stream (and every downstream consumer) matches the serial walk. */
        for (int vi = 0; vi < nvis; vi++) {
            int n = s_pre_rec[vi].count;
            if (n <= 0) continue;
            if (s_tri_n + n > MAX_TTRIS) n = MAX_TTRIS - s_tri_n;
            if (n <= 0) break;
            memcpy(&s_tri[s_tri_n], &s_wbuf[s_pre_rec[vi].worker][s_pre_rec[vi].start],
                   (size_t)n * sizeof(ttri));
            s_tri_n += n;
        }
        if (op_prof_on()) op_prof_end();   /* ter.merge */
    } else {
        s_tri_n = s_wctx[0].tri_n;
    }
    for (int w = 0; w < nw_used; w++) {   /* census roll-up */
        tn_wctx* W = &s_wctx[w];
        s_dbg_faces += W->dbg_faces; s_dbg_oob += W->dbg_oob;
        s_dbg_nclip += W->dbg_nclip; s_dbg_behind += W->dbg_behind;
        s_dbg_spike += W->dbg_spike; s_dbg_clipped += W->dbg_clipped;
        s_tex_faces += W->tex_faces; s_tex_tris += W->tex_tris;
        s_near_q_agree += W->near_q_agree; s_near_c_only += W->near_c_only;
        s_walk_sectors += W->walk_sectors;
    }
    if (tgpu_on) {
        openpete_tercache_frame_commit();
        if (s_cen)
            fprintf(stderr, "[tgpu-cen f=%u] sub=%d secs=%d claims=%d "
                    "near=%ld portal=%ld anim=%ld scroll=%ld fill=%ld "
                    "arena=%ld col=%ld slot=%ld tp=%ld nearb=%ld "
                    "nodraw=%ld prim=%ld pcov=%ld\n",
                    openpete_frame, s_vanim_sub,
                    s_tgpu_secs_walk, s_tgpu_claims_walk,
                    s_tgcl_near - cen0[0], s_tgcl_portal - cen0[1],
                    s_tgcl_anim - cen0[2], s_tgcl_scroll - cen0[3],
                    s_tgcl_fill - cen0[4], s_tgcl_build - cen0[5],
                    s_tgcl_colour - cen0[6], s_tgcl_slot - cen0[7],
                    s_tgcl_texpack - cen0[8],
                    s_tgcl_nearb - cen0[9], s_tgcl_nodraw - cen0[10],
                    s_tgcl_prim - cen0[11], s_tgcl_pcov - cen0[12]);
        tgpu_census();
        if (op_prof_on()) {   /* the claim-ratchet metric, per present */
            static int id_secs = -1, id_claims = -1;
            if (id_secs < 0)   id_secs   = op_prof_intern("tc.secs");
            if (id_claims < 0) id_claims = op_prof_intern("tc.claims");
            op_prof_count(id_secs,   (uint64_t)s_tgpu_secs_walk);
            op_prof_count(id_claims, (uint64_t)s_tgpu_claims_walk);
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
        fprintf(stderr, "[fardbg-hdr] frame=%u prims=%d sampled=%d fogRefBase=%d cullS2=%d cullRaw=0x%X\n",
                openpete_frame, s_prim_total, s_gte_n, fogRefBase, cullRefS2,
                (unsigned)rd(cpu, game_layout.env.cull_dist));
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
    if (s_fp_on) fp_publish(cpu);
}

/* Feed the built tris to the native scene. Textured faces sample the PSX VRAM
 * mirror via the Tiledef material (modulated by the per-vertex DPCS-fogged
 * colour); untextured faces are gouraud. Colour = native RGB888 + DPCS fog. */
static int s_notex = -1;   /* OPENPETE_TERRAIN_NOTEX: force untextured (debug) */
static int s_tiercolor = -1;  /* OPENPETE_TERRAIN_TIERCOLOR: LP=red, HP=blue */
static int s_tieronly = -2;   /* OPENPETE_TERRAIN_TIERONLY: 1=LP only, 2=HP only */

/* ---- portal sky cover (the plane-cover rule) -------------------------------
 * Inside an opening's footprint the destination preview covers everything at
 * or behind the opening plane − M (openpete_portal_plane_margin, default
 * 256 world). The portal feeder's aperture mask and preview colours test at
 * plane − M, so covered OPAQUE terrain needs no CPU treatment: it draws at
 * TRUE depth and the preview repaints over it per pixel — including the
 * threshold-pad class (a face spanning from just in front of the plane into
 * the throat, depth-coincident with the plane at the base: its coincident
 * and behind portions fall inside M and are covered; only a genuinely-near
 * lip shows, the portal-ground-disocclusion f2440 requirement). Transparent
 * covered terrain keeps a CPU whole-face drop (it blends AFTER the portal
 * sequence, over the preview) — same predicate, applied at the centroid.
 *
 * This replaces the og-OT key mirror (the sky-bucket TERRAIN_KEY fan + the
 * face-insertion comparison): the .s's OT pushes are tier-asymmetric (fan
 * mean+64 slots, LP mean+64+8·bias, HP mean+4·bias), so the og verdict is a
 * rendering artifact wherever tier and distance decouple — with LOD forced
 * off, the .s itself paints HP terrain over its own preview fan. */
static int s_pconform = -1;        /* OPENPETE_TERRAIN_PORTAL_CONFORM=0 disables (A/B) */
static int s_otbias_on = 1;        /* OPENPETE_TERRAIN_OT_BIAS this VBL (shared: the
                                    * emit-site pull and the conform front cap must
                                    * agree, or the cap's pre-offset compensation
                                    * lands at the wrong final depth) */
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
/* Max |perp distance| of a rim face's FARTHEST vertex from the opening plane,
 * in world units. Paired with portal_rim_near below, which bounds the NEAREST:
 * this one alone bounds a face's extent, not where it sits. */
static double portal_rim_eps(void) {
    static double v = -1.0;
    if (v < 0.0) { const char* e = getenv("OPENPETE_PORTAL_RIM_EPS");
                   v = (e && *e) ? atof(e) : 384.0; if (v < 0.0) v = 0.0; }
    return v;
}
/* How far the NEAREST vertex of a rim face may sit from the opening plane, in
 * world units. The rim lies IN the plane, so some part of it must touch the
 * plane; RIM_EPS above only bounds how far the face's FAR side may stray, which
 * lets a face that is wholly offset from the plane through as long as it is
 * small — exactly what an arch's front wall face is. The peace_keepers 25 jamb
 * (sec=180 face=25, 287..377 units in FRONT of the plane, 8 deg off parallel,
 * dropping inside the opening) was culled for that reason and left a hole in
 * the arch at pos=(15962,121384,50254) yaw=1420 pitch=228.
 *
 * The populations are far apart, so the bound is not delicate: censused over
 * every arch from both a near and a far camera, 401 of 421 culled faces sit
 * within 12.5 units of the plane and the other 20 are all beyond 240 — 16 of
 * them on gnorc_gnexus levelId=61, an arch with no rim at all and nothing to
 * cull. 128 sits an order of magnitude clear of the real rims and
 * matches the in-plane reach, so it absorbs the same authoring drift
 * perpendicular to the plane that reach absorbs within it. */
static double portal_rim_near(void) {
    static double v = -1.0;
    if (v < 0.0) { const char* e = getenv("OPENPETE_PORTAL_RIM_NEAR");
                   v = (e && *e) ? atof(e) : 128.0; if (v < 0.0) v = 0.0; }
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
 *   bit1 RIMFAIL_DIST     does not straddle the plane's band: the nearest
 *                         vertex is further than `near`, or the farthest
 *                         further than `eps`
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
/* Per-present frame of an opening plane — everything portal_rim_classify
 * needs that depends only on the PLANE, not the face under test: the unit
 * view-space normal, the in-plane 2D basis, the outline dropped onto it and
 * that outline's UV bbox. Derived once per feed (rim_frames_derive) instead
 * of per classified triangle: with a portal sky active every WORLD tri runs
 * the classifier, and rebuilding the basis per tri (two sqrts + the outline
 * projection) dominated the whole terrain feed on portal-heavy scenes. */
typedef struct {
    int    valid;
    double n[3];                      /* unit view-space plane normal */
    double O[3], U[3], W[3];          /* in-plane basis (rim_plane_basis) */
    rim_uv pv[16];                    /* outline in (U,W) coords */
    double pulo, puhi, pvlo, pvhi;    /* outline UV bbox */
} rim_frame_t;
static rim_frame_t s_rimfr[6];
static double s_rim_ofx, s_rim_ofy, s_rim_H;

static void rim_frames_derive(CPUState* cpu) {
    s_rim_ofx = s_view.ofx;
    s_rim_ofy = s_view.ofy;
    s_rim_H   = s_view.H_eff;
    for (int pi = 0; pi < s_pplane_n; pi++) {
        rim_frame_t* rf = &s_rimfr[pi];
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        rf->valid = 0;
        if (s_rim_H < 1.0 || pp->vn < 3) continue;
        /* Unit view-space normal of the opening plane, recovered from the
         * inv-vz fit: the fit is inv = (n.x/(H k))x + (n.y/(H k))y +
         * (n.z - n.x ofx/H - n.y ofy/H)/k, so n is proportional to
         * (a H, b H, c + a ofx + b ofy). */
        double nx = pp->a * s_rim_H, ny = pp->b * s_rim_H;
        double nz = pp->c + pp->a * s_rim_ofx + pp->b * s_rim_ofy;
        double nn = sqrt(nx*nx + ny*ny + nz*nz);
        if (nn < 1e-12) continue;
        rf->n[0] = nx / nn; rf->n[1] = ny / nn; rf->n[2] = nz / nn;
        if (!rim_plane_basis(pp, rf->n, rf->O, rf->U, rf->W, rf->pv, 16)) continue;
        rf->pulo = rf->pv[0].u; rf->puhi = rf->pv[0].u;
        rf->pvlo = rf->pv[0].v; rf->pvhi = rf->pv[0].v;
        for (int i = 1; i < pp->vn; i++) {
            if (rf->pv[i].u < rf->pulo) rf->pulo = rf->pv[i].u;
            if (rf->pv[i].u > rf->puhi) rf->puhi = rf->pv[i].u;
            if (rf->pv[i].v < rf->pvlo) rf->pvlo = rf->pv[i].v;
            if (rf->pv[i].v > rf->pvhi) rf->pvhi = rf->pv[i].v;
        }
        rf->valid = 1;
    }
}

/* Portal-rim sector BROADPHASE — conservative, exact by the subset property.
 *
 * With any portal sky active, every WORLD tri runs portal_rim_classify; on
 * portal-heavy scenes that per-tri work dominated the feed even after the
 * per-plane frame hoist. But the classifier's candidate gates are spatial
 * (within RIM_CAND_WINDOW of the plane, in-plane bbox within
 * RIM_CAND_MARGIN), so a SECTOR whose whole bounding sphere provably fails
 * a gate for every plane cannot contain a rim candidate: every emitted tri
 * (both quad halves, near-band sub-quads, near-clip fragments — all subsets
 * of the sector's geometry) would return "not a candidate", and skipping
 * the classifier for it draws exactly the same picture.
 *
 * Conservative inflation of the sphere: rowmax covers the non-orthonormal
 * projection matrix; the vz/H term covers the classifier reconstructing
 * view verts from (possibly integer-snapped) SCREEN coords, an error that
 * grows with view-Z; poly-anim sectors always classify (animated verts can
 * leave the header sphere).
 *
 * OPENPETE_PORTAL_RIM_BROAD: 1 (default) on, 0 off, 2 = verify — classify
 * every tri live (old behaviour) and report any rim verdict the broadphase
 * would have skipped. */
static uint8_t s_sec_rimcand[1024];
static int s_rimbroad = -1;

static int rim_sec_cand(int sec) {
    return (sec >= 0 && sec < 1024) ? s_sec_rimcand[sec] : 1;
}

/* Sphere-vs-plane candidate core, shared by the feed broadphase and the
 * retained-GPU portal claim so the two verdicts cannot drift. Inputs are
 * view-space world units; rv already carries the rowmax scale. */
static int rim_sphere_cand_core(double cx, double cy, double cz, double rv) {
    double slack = rv * 0.05 + 64.0;
    if (s_rim_H >= 1.0) {
        double vzmax = cz + rv; if (vzmax < 16.0) vzmax = 16.0;
        slack += 4.0 * vzmax / s_rim_H;   /* ~4 px of screen-snap reconstruction */
    }
    const double r = rv + slack;
    for (int pi = 0; pi < s_pplane_n; pi++) {
        const rim_frame_t* rf = &s_rimfr[pi];
        if (!rf->valid) continue;
        const double dx = cx - rf->O[0], dy = cy - rf->O[1], dz = cz - rf->O[2];
        const double dperp = fabs(dx*rf->n[0] + dy*rf->n[1] + dz*rf->n[2]);
        if (dperp - r > RIM_CAND_WINDOW) continue;   /* sphere wholly beyond the window */
        const double cu = dx*rf->U[0] + dy*rf->U[1] + dz*rf->U[2];
        const double cv = dx*rf->W[0] + dy*rf->W[1] + dz*rf->W[2];
        if (cu + r < rf->pulo - RIM_CAND_MARGIN || cu - r > rf->puhi + RIM_CAND_MARGIN ||
            cv + r < rf->pvlo - RIM_CAND_MARGIN || cv - r > rf->pvhi + RIM_CAND_MARGIN)
            continue;                                /* sphere wholly outside the in-plane bbox */
        return 1;
    }
    return 0;
}

static void rim_sector_cands(void) {
    if (s_rimbroad < 0) { const char* e = getenv("OPENPETE_PORTAL_RIM_BROAD");
                          s_rimbroad = (e && *e) ? atoi(e) : 1; }
    if (!s_pplane_n) return;
    for (int i = 0; i < 1024; i++) {
        if (s_secv_stamp[i] != s_vanim_pres) { s_sec_rimcand[i] = 0; continue; }
        if (s_secv_anim[i]) { s_sec_rimcand[i] = 1; continue; }
        s_sec_rimcand[i] = (uint8_t)rim_sphere_cand_core(
            (double)s_secv[i][0] * 16.0, (double)s_secv[i][1] * 16.0,
            (double)s_secv[i][2] * 16.0,
            (double)s_secv[i][3] * 16.0 * s_rim_rowmax);
    }
}

/* ---- retained-GPU portal-claim refinement (walk side) ----------------------
 * Derive the rim frames for the walk's claim tests, from the walk's own plane
 * fetch. The feed re-derives them later in the same extraction with the
 * same camera and plane set, so both sides test identical geometry; the
 * claim must only be at least as inclusive as the feed's rim_sec_cand, which
 * it is exactly (same core, same inputs — the claim site feeds the very
 * values it stamps into s_secv through the same float round-trip). */
static int tgpu_portal_frames(CPUState* cpu) {
    if (s_pconform < 0) { const char* e = getenv("OPENPETE_TERRAIN_PORTAL_CONFORM");
                          s_pconform = (e && *e) ? (*e != '0') : 1; }
    if (!s_pconform) return 0;
    memcpy(s_pplane, s_tgpu_pplane,
           sizeof(openpete_portal_skyplane) * (size_t)s_tgpu_pplane_n);
    s_pplane_n = s_tgpu_pplane_n;
    rim_frames_derive(cpu);
    return 1;
}
static int tgpu_rim_sphere_cand(double oc1, double oc2, double oc3, int radius) {
    return rim_sphere_cand_core(
        (double)(float)oc1 * 16.0, (double)(float)oc2 * 16.0,
        (double)(float)oc3 * 16.0,
        (double)(float)radius * 16.0 * s_rim_rowmax);
}

/* Stage the walk's rim frames + classifier knobs into the retained globals,
 * so terrain.vert can run portal_rim_classify on each emitted tri (rebuilt
 * from the corner attributes + the flags16 half/sub-tri bit). rim_n stays 0
 * — no vertex-stage cull — when frames are absent or the rim-claim lever
 * routes rim candidates to the CPU walk. */
static void tgpu_rim_globals_fill(openpete_tercache_globals* g) {
    if (!s_tgpu_rim_ok) return;
    g->rim_ofx = s_rim_ofx; g->rim_ofy = s_rim_ofy; g->rim_H = s_rim_H;
    g->rim_knobs[0] = portal_rim_eps();   g->rim_knobs[1] = portal_rim_near();
    g->rim_knobs[2] = portal_rim_reach(); g->rim_knobs[3] = portal_rim_cos();
    int n = s_pplane_n > 6 ? 6 : s_pplane_n;
    for (int pi = 0; pi < n; pi++) {
        const rim_frame_t* rf = &s_rimfr[pi];
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        g->rim_abc[pi][0] = pp->a; g->rim_abc[pi][1] = pp->b;
        g->rim_abc[pi][2] = pp->c;
        g->rim_abc[pi][3] = rf->valid ? 1.0 : 0.0;
        if (!rf->valid) continue;
        for (int k = 0; k < 3; k++) {
            g->rim_O[pi][k]   = rf->O[k]; g->rim_U[pi][k] = rf->U[k];
            g->rim_W[pi][k]   = rf->W[k]; g->rim_nrm[pi][k] = rf->n[k];
        }
        g->rim_nrm[pi][3] = (double)pp->vn;
        g->rim_bbox[pi][0] = rf->pulo; g->rim_bbox[pi][1] = rf->puhi;
        g->rim_bbox[pi][2] = rf->pvlo; g->rim_bbox[pi][3] = rf->pvhi;
        for (int k = 0; k < pp->vn && k < 16; k++) {
            g->rim_pv[pi * 16 + k][0] = rf->pv[k].u;
            g->rim_pv[pi * 16 + k][1] = rf->pv[k].v;
        }
    }
    g->rim_n = n;
}

static int portal_rim_classify(CPUState* cpu, const ttri* t) {
    (void)cpu;   /* per-present camera constants come from rim_frames_derive */
    if (!s_pplane_n) return -1;
    const double ofx = s_rim_ofx, ofy = s_rim_ofy, H = s_rim_H;
    if (H < 1.0) return -1;
    const double eps = portal_rim_eps(), reach = portal_rim_reach();
    const double near = portal_rim_near();
    int best = -1;
    for (int pi = 0; pi < s_pplane_n; pi++) {
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        const rim_frame_t* rf = &s_rimfr[pi];
        if (!rf->valid) continue;
        const double nx = rf->n[0], ny = rf->n[1], nz = rf->n[2];
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
        const double *O = rf->O, *U = rf->U, *W = rf->W;
        const rim_uv* pv = rf->pv;
        if (dmin > RIM_CAND_WINDOW) continue;
        const double pulo = rf->pulo, puhi = rf->puhi;
        const double pvlo = rf->pvlo, pvhi = rf->pvhi;
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
        /* dist: the face must both REACH the plane (nearest vertex within
         * `near`) and not stray past `eps` on its far side. */
        if (dmax > eps || dmin > near) flags |= RIMFAIL_DIST;
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
/* Plane-cover rule: a transparent face is covered iff its screen centroid
 * lies inside an opening's footprint and its mean view-Z sits at or behind
 * the opening plane − M at that point. The former og-OT key comparison
 * (face insertion vs the sky bucket) is retired: the .s's OT pushes are
 * tier-asymmetric, so that verdict is a rendering artifact once tier and
 * distance decouple (with LOD forced off, the .s itself paints HP terrain
 * over its own fan) — the aperture is defined geometrically instead, in
 * both --lod modes. */
/* OPENPETE_PORTAL_TRANSP_COVER=0: disable the transparent-terrain cover drop
 * (A/B lever — shows blended terrain through the preview again). */
static int transp_cover_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL_TRANSP_COVER");
                 v = (e && *e) ? (*e != '0') : 1; }
    return v;
}
/* Cover planes for the vertex stage's portal_sky_covers mirror: the same
 * screen-space openings the feed's drop tests, snapshotted into the build's
 * globals. Filled only when the rim frames are up (tgpu_portal_frames copied
 * this build's planes into s_pplane); otherwise the blanket portal claim
 * keeps every overlap sector on the CPU walk and no retained blend face can
 * reach the test. The feed-side lever (OPENPETE_PORTAL_TRANSP_COVER=0)
 * disables both sides alike, so lever-off keeps covers visible everywhere. */
static void tgpu_cover_globals_fill(openpete_tercache_globals* g) {
    if (!s_tgpu_rim_ok || !transp_cover_on()) return;
    int n = s_pplane_n > 6 ? 6 : s_pplane_n;
    g->cov_margin = (double)openpete_portal_plane_margin();
    for (int pi = 0; pi < n; pi++) {
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        g->cov_abc[pi][0] = pp->a;
        g->cov_abc[pi][1] = pp->b;
        g->cov_abc[pi][2] = pp->c;
        g->cov_pn[pi] = pp->pn > 16 ? 16 : pp->pn;
        g->cov_bbox[pi][0] = pp->x0; g->cov_bbox[pi][1] = pp->y0;
        g->cov_bbox[pi][2] = pp->x1; g->cov_bbox[pi][3] = pp->y1;
        for (int k = 0; k < pp->pn && k < 16; k++) {
            g->cov_pxy[pi][k][0] = pp->px[k];
            g->cov_pxy[pi][k][1] = pp->py[k];
        }
    }
    g->cov_n = n;
}

static int portal_sky_covers(const ttri* t) {
    const double M = (double)openpete_portal_plane_margin();
    for (int pi = 0; pi < s_pplane_n; pi++) {
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        if (pp->pn < 3) continue;
        const float cx = (t->sx[0] + t->sx[1] + t->sx[2]) / 3.0f;
        const float cy = (t->sy[0] + t->sy[1] + t->sy[2]) / 3.0f;
        if (cx < pp->x0 || cx > pp->x1 || cy < pp->y0 || cy > pp->y1) continue;
        if (!skyplane_contains(pp, cx, cy)) continue;
        const double inv = pp->a * (double)cx + pp->b * (double)cy + pp->c;
        if (inv <= 1e-9) continue;                /* plane behind the camera here */
        const double mean = (t->vz[0] + t->vz[1] + t->vz[2]) * 16.0 / 3.0;
        if (mean >= 1.0 / inv - M) return 1;
    }
    return 0;
}

static void terrain_feed_native(CPUState* cpu) {
    OP_PROF_SCOPE("ter.feed");
    {
        static tn_envvbl_t nt_c, tc_c, to_c, ob_c;
        s_notex     = tn_env_vbl(&nt_c, "OPENPETE_TERRAIN_NOTEX", 0, 1);
        s_tiercolor = tn_env_vbl(&tc_c, "OPENPETE_TERRAIN_TIERCOLOR", 0, 1);
        s_tieronly  = tn_env_vbl(&to_c, "OPENPETE_TERRAIN_TIERONLY", 0, 0);
        s_otbias_on = tn_env_vbl(&ob_c, "OPENPETE_TERRAIN_OT_BIAS", 1, 1);
    }
    if (s_pconform < 0) { const char* e = getenv("OPENPETE_TERRAIN_PORTAL_CONFORM"); s_pconform = (e && *e) ? (*e != '0') : 1; }
    if (s_pconform_dbg < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_PORTAL_CONFORM_DBG");
        s_pconform_dbg = 0;
        if (e && *e && sscanf(e, "%f,%f", &s_pcdbg_x, &s_pcdbg_y) == 2) s_pconform_dbg = 1;
    }
    /* Active portal-sky opening planes, derived against THIS present's camera
     * bracket (canonical or sub-tick interpolated) so plane and verts agree. */
    s_pplane_n = s_pconform ? openpete_portal_sky_planes(cpu, &s_view, s_pplane, 6) : 0;
    /* Republish the openings for the core-side plane-cover invariant sweep
     * (OPENPETE_PORTAL_COVER_CHECK) — core sees the complete scene but not
     * the game-side plane provider. */
    {
        openpete_native_cover_plane cvp[6];
        int cn = 0;
        for (int pi = 0; pi < s_pplane_n && cn < 6; pi++) {
            const openpete_portal_skyplane* pp = &s_pplane[pi];
            if (pp->pn < 3) continue;
            openpete_native_cover_plane* o = &cvp[cn++];
            o->a = pp->a; o->b = pp->b; o->c = pp->c; o->pn = pp->pn;
            for (int k = 0; k < pp->pn; k++) { o->px[k] = pp->px[k]; o->py[k] = pp->py[k]; }
        }
        openpete_native_set_cover_planes(cvp, cn, openpete_portal_plane_margin());
    }
    rim_frames_derive(cpu);
    rim_sector_cands();
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
        const double ofx = s_view.ofx, ofy = s_view.ofy, H = s_view.H_eff;
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
          ngte_mac(s_view.cam_M, 0.0, -1.0, 0.0, &zx, &zy, &zz);
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
        const double ofx = s_view.ofx, ofy = s_view.ofy, H = s_view.H_eff;
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
    /* Level-scoped selector keys resolve at draw time, which has no cpu —
     * push the id from here, the feed that runs whenever a world renders. */
    openpete_mods_set_level((int)cpu->read_word(game_layout.level_id));
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
    /* OPENPETE_TERRAIN_PXPROBE=x,y[,f0,f1] (walk space): print every terrain
     * tri covering that point with its identity + depth inputs, for VBLs in
     * [f0,f1] (default 1899-1904). Gives the sector/face/tier/slot attribution
     * behind a pixel, which the completed scene no longer carries. */
    { static int s_pxp = -1; static float ppx, ppy;
      static unsigned pf0 = 1899, pf1 = 1904;
      if (s_pxp < 0) { const char* e = getenv("OPENPETE_TERRAIN_PXPROBE");
          s_pxp = (e && *e && sscanf(e, "%f,%f,%u,%u", &ppx, &ppy, &pf0, &pf1) >= 2) ? 1 : 0; }
      if (s_pxp && openpete_frame >= pf0 && openpete_frame <= pf1) {
        for (int i = 0; i < s_tri_n; i++) {
            const ttri* t = &s_tri[i];
            const float ax = t->sx[0], ay = t->sy[0];
            const float e1x = t->sx[1]-ax, e1y = t->sy[1]-ay;
            const float e2x = t->sx[2]-ax, e2y = t->sy[2]-ay;
            const float det = e1x*e2y - e2x*e1y;
            if (det > -1e-6f && det < 1e-6f) continue;
            const float rx = ppx-ax, ry = ppy-ay;
            const float b1 = (rx*e2y - e2x*ry)/det;
            const float b2 = (e1x*ry - rx*e1y)/det;
            const float b0 = 1.0f - b1 - b2;
            if (b0 < -0.001f || b1 < -0.001f || b2 < -0.001f) continue;
            const float vz = b0*t->vz[0] + b1*t->vz[1] + b2*t->vz[2];
            fprintf(stderr, "[pxprobe f=%u %.0f,%.0f] tri#%d sec=%d face=%d tier=%d "
                    "fill=%d blend=%d slots=%d otkey=%.0f vz16=%.1f (full %.0f, +slots %.0f) "
                    "xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) vzv=(%.0f,%.0f,%.0f)\n",
                    openpete_frame, (double)ppx, (double)ppy, i, t->sec, t->face,
                    t->tier, t->fill, t->blend, (int)t->otslots, (double)t->otkey,
                    (double)vz, (double)vz*16.0, (double)(vz*16.0 + 128.0*t->otslots),
                    (double)t->sx[0], (double)t->sy[0], (double)t->sx[1], (double)t->sy[1],
                    (double)t->sx[2], (double)t->sy[2],
                    (double)t->vz[0]*16.0, (double)t->vz[1]*16.0, (double)t->vz[2]*16.0);
        }
      } }
    /* OPENPETE_TERRAIN_TRIDUMP=<frame>[,path] — one-shot full-feed dump of
     * every terrain tri with its FACE IDENTITY and depth inputs, on the first
     * feed whose VBL is >= <frame>. The identity-granular sibling of the
     * scene dump: sec/face/tier is what per-pixel winner attribution (og OT
     * raster vs native raster) needs and the scene dump cannot carry. */
    { static int s_td = -2; static unsigned s_td_f = 0; static char s_td_path[512];
      if (s_td == -2) { const char* e = getenv("OPENPETE_TERRAIN_TRIDUMP");
          s_td = -1;
          if (e && *e) {
              const char* c = strchr(e, ',');
              s_td = 1; s_td_f = (unsigned)atoi(e);
              snprintf(s_td_path, sizeof s_td_path, "%s",
                       (c && c[1]) ? c + 1 : "/tmp/terrain-tridump.txt");
          } }
      if (s_td == 1 && openpete_frame >= s_td_f) {
        s_td = -1;
        FILE* fp = fopen(s_td_path, "w");
        if (fp) {
            fprintf(fp, "# frame=%u n=%d\n", openpete_frame, s_tri_n);
            fprintf(fp, "# sec,face,tier,fill,blend,textured,slots,"
                        "x0,y0,vz0,x1,y1,vz1,x2,y2,vz2,otkey,nearband"
                        "  (vz/otkey true world units)\n");
            for (int i = 0; i < s_tri_n; i++) {
                const ttri* t = &s_tri[i];
                fprintf(fp, "%d,%d,%d,%d,%d,%d,%d,"
                            "%.2f,%.2f,%.1f,%.2f,%.2f,%.1f,%.2f,%.2f,%.1f,"
                            "%.1f,%d\n",
                        t->sec, t->face, t->tier, t->fill, t->blend, t->textured,
                        (int)t->otslots,
                        (double)t->sx[0], (double)t->sy[0], (double)t->vz[0]*16.0,
                        (double)t->sx[1], (double)t->sy[1], (double)t->vz[1]*16.0,
                        (double)t->sx[2], (double)t->sy[2], (double)t->vz[2]*16.0,
                        (double)t->otkey, (int)t->nearband);
            }
            fclose(fp);
        }
      } }
    /* Fill-tag inspector tint: the listed FILL faces green, the preview's
     * untagged candidates magenta, the face under the crosshair yellow. LP is
     * untextured gouraud, so a flat colour replaces the whole surface and the
     * tag set reads at a glance while the HP scene around it stays normal. */
    openpete_fillpick_hit fp_hit;
    int fp_show = openpete_fillpick_active() && openpete_fillpick_get(&fp_hit);
    if (!fp_show) memset(&fp_hit, 0, sizeof fp_hit);
    int fp_tint = openpete_fillpick_active();

    for (int i = 0; i < s_tri_n; i++) {
        ttri* t = &s_tri[i];
        if (s_tieronly == 1 && !t->tier) continue;   /* LP only */
        if (s_tieronly == 2 &&  t->tier) continue;   /* HP only */
        /* Transparent terrain the preview covers (plane-cover rule): drop it.
         * Opaque covered faces need no CPU treatment: they draw at TRUE
         * depth behind the opening plane, and the preview — mask and colours
         * testing at plane − M — repaints over them per pixel. Transparent
         * classes draw AFTER the portal sequence and would blend on top of
         * the preview (a waterfall shining through the arch), so the mirror
         * of the cover is not drawing them at all. Both tiers: the HP
         * waterfall is textured but its LP twin is blended UNTEXTURED
         * gouraud (LP has no textures), and it needs the same drop — the
         * per-pixel key discard that used to catch it went with the
         * TERRAIN_KEY fan. */
        if (s_pplane_n && t->blend && transp_cover_on() && portal_sky_covers(t)) continue;
        openpete_scene_tri tri; memset(&tri, 0, sizeof(tri));
        uint32_t fp_col = 0;
        if (fp_tint) {
            if (fp_show && t->tier && t->sec == fp_hit.sec && t->face == fp_hit.face)
                fp_col = 0xFF00FFFFu;                          /* yellow: under the crosshair */
            else if (t->fptag == 1) fp_col = 0xFF00FF00u;      /* green:   listed FILL tag */
            else if (t->fptag == 2) fp_col = 0xFFFF00FFu;      /* magenta: preview candidate */
        }
        int tex = (!s_flat && !s_notex && !s_tiercolor) ? t->textured : 0;
        if (fp_col) tex = 0;
        /* blend rides for untextured faces too: LP semi-transparent gouraud
         * (face w1 bit 2) has no texture but still routes to the ALPHA/ADD
         * scene groups, mirroring the .s's 0x32/0x3A prim codes. */
        tri.textured = tex; tri.blend = t->blend;
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
            /* CONTINUOUS growth (replaces the v2 floor()-quantize
             * as the DEFAULT): floor() snapped the fill verts to the pixel
             * grid EVERY PRESENT, so under the sub-tick spine the fill fringe
             * crawled/jittered against the smoothly-interpolated canonical
             * terrain (ghosting / spikes at terrain tops, all
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
             * LP ×1.03: with the stencil hole-fill carrying
             * every INTER-sector LP backing role, this pull's one remaining
             * consumer is the 0x8000 force-LP set — a sector whose LP is the
             * PRIMARY surface backing a sparse HP detail overlay (stone-hill
             * sector 79: 3 HP vs 12 LP faces). That LP must OCCLUDE geometry
             * behind it, so it cannot ride the additive-only stencil fill; and
             * it is NOT a coplanar tie-break, so the constant hardware
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
            openpete_worldvz wz = openpete_worldvz_scaled((float)t->vz[k], 16.0f);
            /* TRUE stored depth, BOTH tiers. The .s's authored OT-slot
             * offsets (LP += ((w1>>3)&0x1F)*8 + 0x40 slots, HP +=
             * ((w3>>3)&0x7)*4, 128 world/slot) are ordering KEYS, not
             * geometry: they ride tri.koff into the per-pixel key-resolve
             * contest (the HP key pass + the LP fragment discard in
             * quad_key.frag / psx_scene_key.frag) and never enter the depth
             * buffer — og's keys decide WHICH terrain surface owns a pixel;
             * true geometry decides everything against that surface
             * (mobys, Spyro, particles, decals, the occlusion grid). */
            tri.v[k].pvz = wz.vz;      /* perspective W == the stored view-Z */
            tri.v[k].z  = openpete_native_depth_from_viewz(wz);
            tri.v[k].u = t->u[k]; tri.v[k].v = t->v[k];
            /* Textured: modulate the sampled texel by the fogged colour (the
             * pipeline does tex*colour/128, so feed the full RGB). Untextured:
             * the gouraud colour; s_flat = a debug coverage colour;
             * s_tiercolor = LP red / HP blue / fill green. */
            tri.v[k].rgba = fp_col       ? fp_col
                          : s_tiercolor  ? (t->fill ? 0xFF00FF00u
                                         : t->tier ? 0xFF0000FFu : 0xFFFF0000u)
                          : s_flat       ? 0xFF20C040u
                          : (0xFF000000u | (t->rgb[k] & 0xFFFFFFu));
        }
        /* og OT key channel: koff carries the face's ABSOLUTE og
         * OT key (+0.5 sentinel) — the .s insertion index in world units:
         * corner-mean view-Z + 128*(authored slot bias). Every opaque
         * non-fill face both EMITS its flat key into the key image and
         * CONTESTS it (strict less-by-half-slot, so a face never loses to
         * its own key and og ties fall through to true depth). One rule
         * covers the LP<->HP partition, intra-tier authored order, AND the
         * portal sky contest (the sky fans below emit bucket+64).
         *
         * NEAR-BAND faces are CONTEST-ONLY (NEGATIVE koff = |key|, no
         * slot offsets — the subdivide queue inserts at raw slots): the
         * OT dump proves near cells ride the SAME painter as everything
         * else (dark_hollow: the beast face at slot 671 of 694 paints
         * OVER the wall quads at slots 254-399, and the og framebuffer
         * shows the beast), so an authored-biased moby key must be able
         * to win near ground/walls per-pixel (the embedded pedestal-disc
         * rim, and og's own moby-over-near-wall cases). They still never
         * EMIT: a huge under-camera face's flat-key raster would discard
         * everything behind it, and true depth already wins the
         * viewer-side surface's own pixels. (Adjudication trap: dying
         * mobys death-flash per VBL — judge these cases on SURFACE
         * identity, never colour.)
         *
         * FILL twins stay 0 — stencil-gated to uncovered pixels, they must
         * never self-discard. Transparent terrain CONTESTS but never EMITS
         * (the key-pass pack drops blend != 0): it doesn't occlude, but the
         * og painter still covers it — the cyclorama's sky-bucket keys must
         * beat a beyond-the-sky LP waterfall veil exactly as they beat the
         * opaque terrain next to it. Blended NEAR-BAND faces stay 0: the
         * negative-koff branch carries the moby embed rule, which a veil
         * blending OVER a moby must not inherit. OT_BIAS=0 zeroes the
         * channel: raw true depth (the A/B leg). */
        if (s_otbias_on && !t->fill &&
            openpete_native_key_resolve_on()) {
            static int s_nbc = -1;   /* OPENPETE_TERRAIN_NEARBAND_CONTEST=0 disables the near-band contest-only keys (A/B) */
            if (s_nbc < 0) { const char* e = getenv("OPENPETE_TERRAIN_NEARBAND_CONTEST");
                s_nbc = (e && *e) ? (*e != '0') : 1; }
            double mean = (t->otkey > 0.0f) ? (double)t->otkey
                        : (t->vz[0] + t->vz[1] + t->vz[2]) * 16.0 / 3.0;
            tri.koff = t->nearband ? ((s_nbc && !tri.blend) ? (float)-(mean + 0.5) : 0.0f)
                                   : (float)(mean + 128.0 * (double)t->otslots + 0.5);
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
            if (s_cull) {
                if (s_rimbroad == 2) {          /* verify: classify live, audit the broadphase */
                    const int rim = portal_rim_face(cpu, t);
                    if (rim && !rim_sec_cand(t->sec)) {
                        static int logged = 0;
                        if (logged < 32) { logged++;
                            fprintf(stderr, "[rim-broad] VIOLATION f=%u sec=%d face=%d "
                                    "(rim face in a sector the broadphase rejected)\n",
                                    openpete_frame, t->sec, t->face); }
                    }
                    if (rim) continue;
                } else if ((s_rimbroad != 1 || rim_sec_cand(t->sec))
                           && portal_rim_face(cpu, t)) continue;
            }
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
    /* The former TERRAIN_KEY aperture fan (the sky bucket's og key rastered
     * into the key image, discarding og-covered opaque terrain per pixel) is
     * RETIRED with the plane-cover rule: opaque terrain behind the opening
     * draws at true depth and the preview repaints over it at plane − M. */
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
 * capture/feed runs at native aspect so that's fine.) */
/* Occlusion group from the last real render call — reused by the sub-tick
 * re-extract (which has no a0). -1 = walk-all (valid index or -1, as the .s). */
static int32_t s_terrain_saved_occGroup = -1;

static int on_world_draw_terrain(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = terrain_verify_enabled();
    int feed   = openpete_native_scene_enabled();
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
        lodsave = cpu->read_word(game_layout.env.lod_dist);
        cpu->write_word(game_layout.env.lod_dist, (uint32_t)s_lodpoke);
    }
    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    { OP_PROF_ACCUM_SCOPE("terrain.srend");
      func_800258F0(cpu); }                        /* run the real renderer (verdicts intact) */
    if (s_lodpoke >= 0)
        cpu->write_word(game_layout.env.lod_dist, lodsave);
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_TERRAIN, startp, endp);

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    /* `endp > startp` = the .s renderer emitted at least one prim, i.e. it had
     * something visible. Under EXTENDED LOD that is the wrong gate: the native
     * walk draws past the .s's LP culling distance by design, and a viewpoint
     * where EVERY sector is beyond that distance (freecam high above a level)
     * leaves the .s emitting nothing at all — so the feed never ran and the
     * level vanished while mobys, fed by their own producer, still drew.
     * Being inside this hook is the state signal that matters: during a level
     * transition the game stops calling func_800258F0 entirely, which is what
     * keeps a stale level out of the scene. */
    if (feed && (endp > startp || openpete_no_lod) && render_path_enabled(RP_TERRAIN)) {
        /* Canon-drawn is latched from the polybuf intervals, so an .s that
         * emitted nothing reads as NOT drawn and the sub-tick re-extract would
         * skip terrain — painting it on canonical presents and dropping it on
         * the intermediates. Mark it explicitly: the frame DID paint terrain,
         * natively. */
        if (endp <= startp) render_path_mark_drawn(RP_TERRAIN);
        { OP_PROF_ACCUM_SCOPE("terrain.build");
          terrain_build(cpu, occGroup); }            /* native walk + tris */
        openpete_native_scene_begin_frame(openpete_frame);  /* frame-keyed (composes) */
        { OP_PROF_ACCUM_SCOPE("terrain.feed");
          terrain_feed_native(cpu); }                /* push tris to the scene */
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
 * of stepping at 29.913 Hz. `interp_on` = camera bracketed (else snap to cur,
 * matching the rest of the scene on unpaired/camera-cut presents). */
void terrain_reextract(CPUState* cpu, double alpha, int interp_on) {
    if (!openpete_native_scene_enabled()) return;
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

/* Whole-level top-down pose (--freecam-pose topdown): centre of the sector
 * bounding spheres, looking straight down from a height at which every
 * sphere projects inside the visible frame. Sector centres/radii are the
 * same world/16 header fields the LP/HP binning uses (decode mirrors
 * tsc_get); CAM_POS wants full world units, so the result scales by 16.
 * The vertical visible half-extent (224-line crop → 112) is the limiting
 * axis at every supported aspect; 100 keeps a margin on top, and the
 * per-sector radius inflates both the lateral offset and the height so a
 * sphere's edge can't clip the frame. Pitch 992 = the freecam drive's own
 * clamp (~87°) — writing more gets clamped on the first driven tick. */
/* Per-vertex accumulator for the two passes below. The decode mirrors
 * tsc_decode_verts + the origin application (x adds, y/z SUBTRACT — see the
 * hpVert dump in terrain_dump): world/16 point = (originX + vx,
 * originY - vy, originZ - vz). Bounding SPHERES were tried first and ran
 * ~2x too high: the header spheres bake vertical extent and padding into
 * the radius, so the lateral bound they give is far fatter than the mesh. */
typedef struct { double px, py, H, need; } td_acc;
static void td_scan_verts(CPUState* cpu, uint32_t verts, int nv,
                          int ox, int oy, int oz,
                          double mm[5], td_acc* acc) {
    for (int k = 0; k < nv; k++) {
        uint32_t w = rd(cpu, verts + (uint32_t)k * 4);
        double x = (double)ox + (double)((w >> 21) & 0x7FF);
        double y = (double)oy - (double)((w >> 10) & 0x7FF);
        double z = (double)oz - (double)(w & 0x3FF);
        if (mm) {
            if (x < mm[0]) mm[0] = x;
            if (x > mm[1]) mm[1] = x;
            if (y < mm[2]) mm[2] = y;
            if (y > mm[3]) mm[3] = y;
            if (z > mm[4]) mm[4] = z;
        }
        if (acc) {
            /* yaw-0 top-down maps world X to screen-horizontal (half extent
             * 256 walk px at 4:3, wider under Hor+) and world Y to vertical
             * (224-line crop → half 112). 244/100 leave margin for the ~3°
             * tilt (pitch 992 = the drive clamp, not a true 90°). */
            double nx = fabs(x - acc->px) * acc->H / 244.0;
            double ny = fabs(y - acc->py) * acc->H / 100.0;
            double need = z + (nx > ny ? nx : ny);
            if (need > acc->need) acc->need = need;
        }
    }
}
/* Waypoint `corner` of a low-altitude circuit of the level. Shares pass 1 of
 * the top-down pose (true vertex bounds over both LOD banks); skips pass 2,
 * since this camera sits just above the geometry rather than far enough back
 * to frame all of it. */
int terrain_native_sweep_pose(CPUState* cpu, int corner, int32_t v[5]) {
    uint32_t sectorPtr = rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = rd(cpu, game_layout.env.sector_count);
    if (!sectorPtr || !sectorCnt || sectorCnt > 1024) return 0;

    double mm[5] = { 1e18, -1e18, 1e18, -1e18, -1e18 };  /* minx,maxx,miny,maxy,maxz */
    int seen = 0;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t g1 = rd(cpu, s + 0x08), g2 = rd(cpu, s + 0x0C);
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int ox = (int)(g1 >> 16), oy = (int)(g1 & 0xFFFF), oz = (int)(g2 >> 16);
        int nLpV = (int)(g3 & 0xFF), nHpV = (int)(g4 & 0xFF);
        uint32_t hpOff = (g4 >> 24) & 0xFF;
        td_scan_verts(cpu, s + 0x1C, nLpV, ox, oy, oz, mm, NULL);
        td_scan_verts(cpu, s + 0x1C + hpOff * 4, nHpV, ox, oy, oz, mm, NULL);
        seen++;
    }
    if (!seen || mm[1] < mm[0]) return 0;

    /* Inset 30% from the centre so the circuit stays over the level rather
     * than orbiting its empty corners, and ride 64 (world/16) above the
     * highest vertex so the camera clears geometry instead of burrowing. */
    static const double sx[4] = { -1.0,  1.0,  1.0, -1.0 };
    static const double sy[4] = { -1.0, -1.0,  1.0,  1.0 };
    const int c = ((corner % 4) + 4) % 4;
    const int nxt = (c + 1) % 4;

    const double cx = (mm[0] + mm[1]) * 0.5, cy = (mm[2] + mm[3]) * 0.5;
    const double hx = (mm[1] - mm[0]) * 0.30, hy = (mm[3] - mm[2]) * 0.30;

    const double px = cx + sx[c] * hx,   py = cy + sy[c] * hy;
    const double nx = cx + sx[nxt] * hx, ny = cy + sy[nxt] * hy;

    /* Face the next waypoint: the drive basis is forward = (cos yaw, sin yaw). */
    const double yaw = atan2(ny - py, nx - px) * (4096.0 / (2.0 * 3.14159265358979323846));

    v[0] = (int32_t)(px * 16.0);
    v[1] = (int32_t)(py * 16.0);
    v[2] = (int32_t)((mm[4] + 64.0) * 16.0);
    v[3] = (int32_t)(int16_t)(uint16_t)(int32_t)yaw;
    v[4] = 0;      /* horizontal — looking into the level, not down at it */
    return 1;
}

int terrain_native_topdown_pose(CPUState* cpu, int32_t v[5]) {
    uint32_t sectorPtr = rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = rd(cpu, game_layout.env.sector_count);
    if (!sectorPtr || !sectorCnt || sectorCnt > 1024) return 0;
    /* The pose is written into g_Camera by freecam, so it is derived at the
     * canonical H: a display FOV must not reach that write. */
    ngte_view pv, cv; ngte_view_build(cpu, &pv); ngte_view_canon(&pv, &cv);
    double H = cv.H_eff;
    if (H < 1.0) H = 400.0;
    /* Pass 1: true vertex bounds (both banks) → centre + level top. */
    double mm[5] = { 1e18, -1e18, 1e18, -1e18, -1e18 };  /* minx,maxx,miny,maxy,maxz */
    int seen = 0;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t g1 = rd(cpu, s + 0x08), g2 = rd(cpu, s + 0x0C);
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int ox = (int)(g1 >> 16), oy = (int)(g1 & 0xFFFF), oz = (int)(g2 >> 16);
        int nLpV = (int)(g3 & 0xFF), nHpV = (int)(g4 & 0xFF);
        uint32_t hpOff = (g4 >> 24) & 0xFF;
        td_scan_verts(cpu, s + 0x1C, nLpV, ox, oy, oz, mm, NULL);
        td_scan_verts(cpu, s + 0x1C + hpOff * 4, nHpV, ox, oy, oz, mm, NULL);
        seen++;
    }
    if (!seen || mm[1] < mm[0]) return 0;
    /* Pass 2: lowest camera that still projects every vertex in-frame. */
    td_acc acc = { (mm[0] + mm[1]) * 0.5, (mm[2] + mm[3]) * 0.5, H, mm[4] + 64.0 };
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t g1 = rd(cpu, s + 0x08), g2 = rd(cpu, s + 0x0C);
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int ox = (int)(g1 >> 16), oy = (int)(g1 & 0xFFFF), oz = (int)(g2 >> 16);
        int nLpV = (int)(g3 & 0xFF), nHpV = (int)(g4 & 0xFF);
        uint32_t hpOff = (g4 >> 24) & 0xFF;
        td_scan_verts(cpu, s + 0x1C, nLpV, ox, oy, oz, NULL, &acc);
        td_scan_verts(cpu, s + 0x1C + hpOff * 4, nHpV, ox, oy, oz, NULL, &acc);
    }
    double pz = acc.need * 1.02;
    /* Ceiling: past view-Z ~32767 (world/16) the .s's 16-bit frustum stage
     * rejects every sector, the visibility verdict goes empty and the native walk —
     * whose 4:3 candidate set IS that verdict — draws nothing (stone_hill
     * proved it at 553651 world). Clamp under it; a level too wide to fit
     * from there gets cropped edges rather than a blank frame. */
    if (pz > 30000.0) {
        fprintf(stderr, "[terrain] topdown pose height clamped (%d -> 30000 world/16): "
                "level too wide for the .s frustum ceiling — edges may crop\n",
                (int)pz);
        pz = 30000.0;
    }
    v[0] = (int32_t)(acc.px * 16.0);
    v[1] = (int32_t)(acc.py * 16.0);
    v[2] = (int32_t)(pz * 16.0);
    v[3] = 0;      /* yaw */
    v[4] = 992;    /* pitch: straight down at the drive clamp */
    return 1;
}

/* The hook address is the layout's world-draw site, bound at boot: a
 * static initializer cannot read another TU's const. */
static feature_dispatch_entry_t terrain_dispatch[] = {
    { 0u, on_world_draw_terrain },
    { 0u, NULL },
};

static void terrain_apply_options(const openpete_options_t* opts) {
    (void)opts;
    terrain_dispatch[0].addr = game_layout.hooks.world_draw;
}

const feature_module_t terrain_module = {
    .name           = "terrain_native",
    .apply_options  = terrain_apply_options,
    .dispatch_table = terrain_dispatch,
};
