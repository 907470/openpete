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

    /* TODO (next increment): native gamestate walk + scene feed.
     *   if (feed && render_path_enabled(RP_TERRAIN)) terrain_feed_native(cpu); */

    if (verify && openpete_frame >= terrain_verify_frame()) {
        static int s_reports = 0;
        if (s_reports < 8) { terrain_dump(cpu); s_reports++; }
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
