// games/spyro-1/features/shadow_native.c — Phase 3b.2 of
// notes/plans/openpete-native-renderer.md: the SHADOW renderers
// (func_80059F8C moby shadows + func_80059A48 Spyro silhouette shadow /
// r_shadows.s) re-derived from gamestate in float + their A/B harness. Both
// are PSX semi-transparent ABR mode 2 (B-F, SUBTRACTIVE — the shadow grey
// darkens the ground). The draw-mode word 0xE1000640 / tpage 0x805F both carry
// ABR bits = 0b10 = mode 2 (the format-spike doc's "mode 0" guess was wrong,
// caught by the A/B brightness inversion — see the format notes):
//   - moby shadows  = textured FT3 fan (GPU code 0x26) per drawn moby, faded
//     with camera distance; the textured subtract pipeline (blend=3).
//   - Spyro shadow  = untextured gouraud G3 fan (GPU code 0x32) projecting
//     Spyro's silhouette onto the floor; the FIRST untextured-subtract consumer
//     (blend=3). See the format-spike doc.
//
// STATIC ASM/recompiled-C FINDINGS (correct the format-spike doc):
//   * g_MobyShadows.shadow_list (loop terminator) is at +0x08, NOT +0x10.
//   * the moby shadow is 4 separate FT3 tris fanned from the ground anchor
//     (not one quad); entry[1] of the 8-byte drawn-moby list record is the
//     shadow radius/scale, not an index. The moby reads its m_Position +
//     m_ShadowDistance DIRECTLY (not the +0x1600 render record).
//   * the Spyro shadow bundles an E1 draw-mode mini-packet (DMA tag len 2)
//     ahead of each G3 (DMA tag len 6); the G3 fan is (anchor, strip[i],
//     strip[i+1]) with color0=0x608080 and color1=color2=0.
//
// Determinism: never writes PSX RAM. Calls each real renderer exactly once
// (behaviour-identical to normal dispatch) then only READS state afterwards.
// Gated by OPENPETE_SHADOW_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk|ab|diff; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "psx_runtime.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "render_paths.h"  /* --render-paths isolation */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void     func_80059F8C(CPUState* cpu);  /* the real MOBY-shadows renderer */
extern void     func_80059A48(CPUState* cpu);  /* the real SPYRO-shadow renderer */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map / recompiled C) ------------------------- */
#define G_CAMERA       0x80076DD0u   /* m_ProjectionMatrix +0x00, m_Position +0x28 */
#define D_SONY         0x8006FCF4u   /* g_SonyImage scratch base                 */
#define DRAWN_LIST     (D_SONY + 0x2800u) /* drawn-moby list: 8-byte entries     */
#define G_MOBYSHADOWS  0x80075EF8u   /* {Tiledef shadow; int *shadow_list@+0x08} */
#define D_SIN_TABLE    0x8006CBF8u   /* SINE_8 sin[idx]; cos at +0x80 (moby)     */
#define D_COS_TABLE    0x8006CC78u   /* COSINE_8 (Spyro shadow heading)          */
#define D_SPYROSHADOW  0x8007AA10u   /* SpyroShadow {.. pos@+0x10 .. overwater@+0x24} */
#define G_SPYRO        0x80078A58u   /* g_Spyro                                  */
#define G_MODELS       0x80076378u   /* Model* g_Models[]                        */
#define D_RING         0x8006E268u   /* silhouette ring offset table (packed XY) */
#define POLYBUF_CURSOR 0x800757B0u

/* Moby struct offsets (moby.h) */
#define MOBY_POS       0x0Cu         /* Vector3D m_Position (x,y,z)              */
#define MOBY_SHADOWDIST 0x1Cu        /* int m_ShadowDistance (+orientation bits) */

static int s16(int v) { return (int)(int16_t)(uint32_t)v; }
static int rd_s16(CPUState* cpu, uint32_t addr) {
    uint32_t w = cpu->read_word(addr & ~3u);
    return (int)(int16_t)((addr & 2u) ? (w >> 16) : (w & 0xFFFF));
}

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
    uint32_t raw[20];
    int      nraw;
} prim_t;

#define MAX_PRIMS 4096
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
static int s_code_hist[256];

/* Walk the polybuf [start,end) by OT tag length (stride=((w0>>24)+1)*4); decode
 * each prim by its code byte (word[1]>>24) with the general PSX polygon decode
 * (bit4 gouraud / bit3 quad / bit2 textured). Same scheme moby/Spyro/flame use.
 * The Spyro shadow's E1 draw-mode mini-packet (tag len 2, code byte 0) decodes
 * to nv=0 and is skipped by consumers. */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_gte_n = 0;
    memset(s_code_hist, 0, sizeof(s_code_hist));
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < MAX_PRIMS) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFF;
        if (len == 0 || len > 24) break;          /* not a tag / corrupt — stop */
        uint32_t stride = (len + 1) * 4;
        uint8_t code = (uint8_t)(cpu->read_word(p + 4) >> 24);
        if (s_gte_n < MAX_PRIMS) {
            prim_t* t = &s_gte[s_gte_n];
            memset(t, 0, sizeof(*t));
            t->code = code; t->len = (uint8_t)len;
            t->nraw = (int)(stride / 4); if (t->nraw > 20) t->nraw = 20;
            for (int i = 0; i < t->nraw; i++) t->raw[i] = cpu->read_word(p + (uint32_t)i*4);
            if ((code & 0xE0) == 0x20) {
                int gouraud = (code & 0x10) != 0;
                int quad    = (code & 0x08) != 0;
                int tex     = (code & 0x04) != 0;
                t->gouraud = gouraud; t->textured = tex; t->quad = quad;
                t->nv = quad ? 4 : 3;
                uint32_t rgb0 = t->raw[1] & 0xFFFFFF;
                int c = 2;
                for (int k = 0; k < t->nv; k++) {
                    t->rgb[k] = (gouraud && k > 0) ? (t->raw[c++] & 0xFFFFFF) : rgb0;
                    uint32_t xy = t->raw[c++];
                    t->x[k] = (float)(int16_t)(xy & 0xFFFF);
                    t->y[k] = (float)(int16_t)(xy >> 16);
                    if (tex) {
                        uint32_t uv = t->raw[c++];
                        t->u[k] = (int)(uv & 0xFF); t->v[k] = (int)((uv >> 8) & 0xFF);
                        if (k == 0)      t->clut  = (int)(uv >> 16);
                        else if (k == 1) t->tpage = (int)(uv >> 16);
                    }
                }
            }
            s_code_hist[code & 0xFF]++;
            s_gte_n++;
        }
        p += stride;
    }
}

static void dump_prims(const char* tag) {
    fprintf(stderr, "  [%s] prim code histogram:", tag);
    for (int c = 0; c < 256; c++) if (s_code_hist[c]) fprintf(stderr, " %02X:%d", c, s_code_hist[c]);
    fprintf(stderr, "  (total=%d)\n", s_gte_n);
    int n = s_gte_n < 6 ? s_gte_n : 6;
    for (int p = 0; p < n; p++) {
        prim_t* t = &s_gte[p];
        fprintf(stderr, "    prim[%d] code=%02X len=%d nv=%d tex=%d gour=%d  raw:",
                p, t->code, t->len, t->nv, t->textured, t->gouraud);
        for (int i = 0; i < t->nraw; i++) fprintf(stderr, " %08X", t->raw[i]);
        fprintf(stderr, "\n      decoded:");
        for (int k = 0; k < t->nv; k++)
            fprintf(stderr, " v%d(xy=%.0f,%.0f rgb=%06X uv=%d,%d)",
                    k, t->x[k], t->y[k], t->rgb[k], t->u[k], t->v[k]);
        if (t->textured) fprintf(stderr, "  clut=%04X tpage=%04X", t->clut, t->tpage);
        fprintf(stderr, "\n");
    }
}

/* ---- moby-shadow dump ----------------------------------------------------- */
static void shadow_moby_dump(CPUState* cpu) {
    fprintf(stderr, "\n=== [shadow-moby-dump f=%u] ===\n", openpete_frame);
    uint32_t uv0 = cpu->read_word(G_MOBYSHADOWS + 0x00);
    uint32_t uv1 = cpu->read_word(G_MOBYSHADOWS + 0x04);
    uint32_t list_end = cpu->read_word(G_MOBYSHADOWS + 0x08);
    fprintf(stderr, "  g_MobyShadows: uv0=%08X uv1=%08X shadow_list(end)=%08X\n", uv0, uv1, list_end);
    fprintf(stderr, "  camera projMat (+0x00):");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(G_CAMERA + (uint32_t)i*4));
    fprintf(stderr, "\n  camera pos (+0x28) = (%d, %d, %d)\n",
            (int)cpu->read_word(G_CAMERA+0x28), (int)cpu->read_word(G_CAMERA+0x2C),
            (int)cpu->read_word(G_CAMERA+0x30));
    fprintf(stderr, "  drawn-moby list [%08X, %08X):\n", DRAWN_LIST, list_end);
    int shown = 0;
    for (uint32_t e = DRAWN_LIST; e < list_end && shown < 6; e += 8, shown++) {
        uint32_t mob = cpu->read_word(e + 0);
        uint32_t v1  = cpu->read_word(e + 4);
        if (mob < 0x80000000u || mob >= 0x80800000u) { fprintf(stderr, "    [%d] mob=%08X v1=%08X (bad ptr)\n", shown, mob, v1); continue; }
        int px = (int)cpu->read_word(mob + MOBY_POS + 0);
        int py = (int)cpu->read_word(mob + MOBY_POS + 4);
        int pz = (int)cpu->read_word(mob + MOBY_POS + 8);
        uint32_t sd = cpu->read_word(mob + MOBY_SHADOWDIST);
        fprintf(stderr, "    [%d] mob=%08X entry1=%08X  pos=(%d,%d,%d) shadowDist=%08X(lo=%d idxB=%d idxA=%d)\n",
                shown, mob, v1, px, py, pz, sd, sd & 0xFFFF, (sd >> 16) & 0x3F, (sd >> 22) & 0x3F);
    }
    dump_prims("shadow-moby");
    fprintf(stderr, "=== end shadow-moby-dump ===\n\n");
}

/* ---- spyro-shadow dump ---------------------------------------------------- */
static void shadow_spyro_dump(CPUState* cpu) {
    fprintf(stderr, "\n=== [shadow-spyro-dump f=%u] ===\n", openpete_frame);
    int overwater = (int)cpu->read_word(D_SPYROSHADOW + 0x24);
    int sx = (int)cpu->read_word(D_SPYROSHADOW + 0x10);
    int sy = (int)cpu->read_word(D_SPYROSHADOW + 0x14);
    int sz = (int)cpu->read_word(D_SPYROSHADOW + 0x18);
    fprintf(stderr, "  SpyroShadow: overwater=%d pos=(%d,%d,%d)  +0x00..0x0C=%08X %08X %08X %08X  +0x1C=%08X\n",
            overwater, sx, sy, sz,
            cpu->read_word(D_SPYROSHADOW+0), cpu->read_word(D_SPYROSHADOW+4),
            cpu->read_word(D_SPYROSHADOW+8), cpu->read_word(D_SPYROSHADOW+0xC),
            cpu->read_word(D_SPYROSHADOW+0x1C));
    fprintf(stderr, "  g_Spyro: +0x0E(heading)=%d +0x18=%04X +0x1E=%04X +0x24=%d\n",
            (int)cpu->read_byte(G_SPYRO+0x0E), (int)cpu->read_half(G_SPYRO+0x18),
            (int)cpu->read_half(G_SPYRO+0x1E), (int)cpu->read_byte(G_SPYRO+0x24));
    fprintf(stderr, "  g_Models[0]=%08X\n", cpu->read_word(G_MODELS));
    fprintf(stderr, "  camera pos (+0x28) = (%d, %d, %d)\n",
            (int)cpu->read_word(G_CAMERA+0x28), (int)cpu->read_word(G_CAMERA+0x2C),
            (int)cpu->read_word(G_CAMERA+0x30));
    fprintf(stderr, "  ring D_8006E268[0..7]:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu->read_word(D_RING + (uint32_t)i*4));
    fprintf(stderr, "\n");
    dump_prims("shadow-spyro");
    fprintf(stderr, "=== end shadow-spyro-dump ===\n\n");
}

/* ===========================================================================
 *  NATIVE WALK — re-derive the moby shadows from gamestate in float (3b.2).
 *  The scaffold's int16 PSX-resolution prims give neither sub-pixel/HD geometry
 *  nor sub-tick interpolation; this reproduces the per-moby ground-point
 *  projection, the oriented-quad matrix assembly (two MVMVA passes per packed
 *  orientation index), the 4 fan corners and the distance fade, projecting in
 *  float. Validated SXY-shape against the captured FT3 prims (the .s int16
 *  output as the oracle), then it replaces the scaffold.
 * =========================================================================== */
#define GTE_DIV_CAP   (131071.0 / 65536.0)   /* 0x1FFFF in 1.16 (near-plane cap) */
#define GTE_SXY_BOUND 1024.0
static int clamp16i(long v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : (int)v); }

/* load the camera rotation matrix (g_Camera+0x00..0x10, 5 words -> 9 int16) */
static void load_cam_matrix(CPUState* cpu, int R[3][3]) {
    uint32_t w0=cpu->read_word(G_CAMERA+0), w1=cpu->read_word(G_CAMERA+4);
    uint32_t w2=cpu->read_word(G_CAMERA+8), w3=cpu->read_word(G_CAMERA+12);
    uint32_t w4=cpu->read_word(G_CAMERA+16);
    R[0][0]=s16(w0&0xFFFF); R[0][1]=s16(w0>>16); R[0][2]=s16(w1&0xFFFF);
    R[1][0]=s16(w1>>16);    R[1][1]=s16(w2&0xFFFF); R[1][2]=s16(w2>>16);
    R[2][0]=s16(w3&0xFFFF); R[2][1]=s16(w3>>16); R[2][2]=s16(w4&0xFFFF);
}

/* MVMVA(sf=1, mx=rotation, v=V0, cv=none, lm=0): IR = clamp16((R·v)>>12). */
static void mvmva_ir(const int R[3][3], int vx, int vy, int vz, int ir[3]) {
    for (int i = 0; i < 3; i++) {
        int64_t m = (int64_t)R[i][0]*vx + (int64_t)R[i][1]*vy + (int64_t)R[i][2]*vz;
        ir[i] = clamp16i((long)(m >> 12));
    }
}

/* integer ground TR (the GTE's MAC after RTPS of the swizzled vector, TR=0). */
static void ground_tr(const int C[3][3], int vx, int vy, int vz, int TR[3]) {
    for (int i = 0; i < 3; i++) {
        int64_t m = (int64_t)C[i][0]*vx + (int64_t)C[i][1]*vy + (int64_t)C[i][2]*vz;
        TR[i] = (int)(m >> 12);
    }
}

/* RTPS one local vertex through R + integer TR (shift 12) -> float screen + view-Z */
typedef struct { float sx, sy, sz; } spt;
static void sh_proj(const int R[3][3], const int TR[3], double ofx, double ofy, double H,
                    double vx, double vy, double vz, spt* o) {
    double mac1 = (double)TR[0] + (R[0][0]*vx + R[0][1]*vy + R[0][2]*vz) / 4096.0;
    double mac2 = (double)TR[1] + (R[1][0]*vx + R[1][1]*vy + R[1][2]*vz) / 4096.0;
    double mac3 = (double)TR[2] + (R[2][0]*vx + R[2][1]*vy + R[2][2]*vz) / 4096.0;
    double ir1 = mac1 < -32768.0 ? -32768.0 : (mac1 > 32767.0 ? 32767.0 : mac1);
    double ir2 = mac2 < -32768.0 ? -32768.0 : (mac2 > 32767.0 ? 32767.0 : mac2);
    double sz3 = mac3 < 0.0 ? 0.0 : (mac3 > 65535.0 ? 65535.0 : mac3);
    double q = (sz3 <= 0.0 || H >= 2.0 * sz3) ? GTE_DIV_CAP : (H / sz3);
    double x = ofx + ir1 * q, y = ofy + ir2 * q;
    if (x < -GTE_SXY_BOUND) x = -GTE_SXY_BOUND; else if (x > GTE_SXY_BOUND-1.0) x = GTE_SXY_BOUND-1.0;
    if (y < -GTE_SXY_BOUND) y = -GTE_SXY_BOUND; else if (y > GTE_SXY_BOUND-1.0) y = GTE_SXY_BOUND-1.0;
    o->sx = (float)x; o->sy = (float)y; o->sz = (float)mac3;
}

/* signed 2*area on the int16-rounded SXY (== GTE NCLIP MAC0 sign). */
static double sh_nclip(const spt* a, const spt* b, const spt* c) {
    double ax = lrintf(a->sx), ay = lrintf(a->sy);
    double bx = lrintf(b->sx), by = lrintf(b->sy);
    double cx = lrintf(c->sx), cy = lrintf(c->sy);
    return ax*(by - cy) + bx*(cy - ay) + cx*(ay - by);
}

/* assemble the oriented-quad matrix R' from the two packed orientation indices.
 * idxB rotates the camera matrix's cols 1,2 by angle B; idxA then rotates cols
 * 0,1 by angle A (operating on the idxB result). Faithfully reproduces the .s
 * including the idxB-block quirk: R'11 = (C.R11 & 0xFFFF) + (idxA<<3), R'12 = 0
 * (the handwritten asm discards the P1<<16 term and folds idxA<<3 into R11). */
static void assemble_matrix(CPUState* cpu, const int C[3][3], int idxA, int idxB, int Rp[3][3]) {
    memcpy(Rp, C, sizeof(int)*9);
    if (idxB != 0) {
        int sinB = rd_s16(cpu, D_SIN_TABLE + (uint32_t)idxB*8);
        int cosB = rd_s16(cpu, D_SIN_TABLE + (uint32_t)idxB*8 + 0x80);
        int P[3], Q[3];
        mvmva_ir(Rp, 0,  cosB, sinB, P);     /* C·(0, cosB, sinB) */
        mvmva_ir(Rp, 0, -sinB, cosB, Q);     /* C·(0, -sinB, cosB) */
        int packed11 = (Rp[0][0] & 0xFFFF) + (idxA << 3);   /* the quirk */
        int n[3][3];
        n[0][0]=s16(packed11 & 0xFFFF); n[0][1]=s16((packed11>>16)&0xFFFF); n[0][2]=Q[0];
        n[1][0]=Rp[1][0];               n[1][1]=P[1];                       n[1][2]=Q[1];
        n[2][0]=Rp[2][0];               n[2][1]=P[2];                       n[2][2]=Q[2];
        memcpy(Rp, n, sizeof n);
    }
    if (idxA != 0) {
        int sinA = rd_s16(cpu, D_SIN_TABLE + (uint32_t)idxA*8);
        int cosA = rd_s16(cpu, D_SIN_TABLE + (uint32_t)idxA*8 + 0x80);
        int P[3], Q[3];
        mvmva_ir(Rp,  cosA, sinA, 0, P);     /* Rp·(cosA, sinA, 0) */
        mvmva_ir(Rp, -sinA, cosA, 0, Q);     /* Rp·(-sinA, cosA, 0) */
        int n[3][3];
        n[0][0]=P[0];     n[0][1]=Q[0];     n[0][2]=Rp[0][2];
        n[1][0]=P[1];     n[1][1]=Q[1];     n[1][2]=Rp[1][2];
        n[2][0]=P[2];     n[2][1]=Q[2];     n[2][2]=Rp[2][2];
        memcpy(Rp, n, sizeof n);
    }
}

/* native moby-shadow tris (4 FT3 fan tris per moby) */
typedef struct { float sx[3], sy[3], sz[3]; int u[3], v[3]; uint32_t rgb;
                 int tpx, tpy, clx, cly, bpp; } moshtri;
#define MAX_MOSH 1024
static moshtri s_mosh[MAX_MOSH]; static int s_mosh_n;

static void shadow_moby_build(CPUState* cpu) {
    s_mosh_n = 0;
    int C[3][3]; load_cam_matrix(cpu, C);
    uint32_t list_end = cpu->read_word(G_MOBYSHADOWS + 0x08);
    uint32_t uv0 = cpu->read_word(G_MOBYSHADOWS + 0x00);
    uint32_t uv1 = cpu->read_word(G_MOBYSHADOWS + 0x04);
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFF);
    int cam0 = (int)((uint32_t)cpu->read_word(G_CAMERA+0x28) >> 2);
    int cam1 = (int)((uint32_t)cpu->read_word(G_CAMERA+0x2C) >> 2);
    int cam2 = (int)((uint32_t)cpu->read_word(G_CAMERA+0x30) >> 2);
    /* FT3 material (constant across the fan): UV0=uv0, UV1=uv1, UV2=uv0+0x1F00 */
    int u0=(int)(uv0&0xFF), v0t=(int)((uv0>>8)&0xFF);
    int u1=(int)(uv1&0xFF), v1t=(int)((uv1>>8)&0xFF);
    int u2=u0, v2t=(int)(((uv0>>8)&0xFF)+0x1F);
    int clut=(int)(uv0>>16), tpage=(int)(uv1>>16);
    int tpx=(tpage&0xF)*64, tpy=((tpage>>4)&1)*256;
    int mode=(tpage>>7)&3, bpp=(mode==0)?4:(mode==1)?8:16;
    int clx=(clut&0x3F)*16, cly=(clut>>6)&0x1FF;
    int ZERO[3] = {0,0,0};

    for (uint32_t e = DRAWN_LIST; e < list_end; e += 8) {
        uint32_t mob = cpu->read_word(e + 0);
        int radius = (int)cpu->read_word(e + 4);
        if (mob < 0x80000000u || mob >= 0x80800000u) continue;
        uint32_t posX = cpu->read_word(mob + MOBY_POS + 0);
        uint32_t posY = cpu->read_word(mob + MOBY_POS + 4);
        uint32_t sd   = cpu->read_word(mob + MOBY_SHADOWDIST);
        int sdlo = (int)(sd & 0xFFFF);
        if (sdlo == 0) continue;
        int vx = s16(cam1 - (int)((uint32_t)posY >> 2));
        int vy = s16(cam2 - sdlo);
        int vz = s16((int)((uint32_t)posX >> 2) - cam0);
        int TR[3]; ground_tr(C, vx, vy, vz, TR);
        if (TR[2] - 0x1000 >= 0) continue;                  /* Z cull */
        spt anchor; sh_proj(C, ZERO, ofx, ofy, H, vx, vy, vz, &anchor);

        int idxB = (int)((sd >> 16) & 0x3F);
        int idxA = (int)((sd >> 22) & 0x3F);
        int Rp[3][3]; assemble_matrix(cpu, C, idxA, idxB, Rp);

        int rlo = s16(radius & 0xFFFF), rhi = s16((radius >> 16) & 0xFFFF);
        spt c[4];
        sh_proj(Rp, TR, ofx, ofy, H,  0,    0,    rlo, &c[0]);   /* RTPT V0 */
        sh_proj(Rp, TR, ofx, ofy, H,  rlo,  rhi,  0,   &c[1]);   /* RTPT V1 */
        sh_proj(Rp, TR, ofx, ofy, H,  0,    0,   -rlo, &c[2]);   /* RTPT V2 */
        sh_proj(Rp, TR, ofx, ofy, H, -rlo,  0,    0,   &c[3]);   /* RTPS 4th */

        if (sh_nclip(&c[0], &c[1], &c[2]) < 0.0) continue;       /* backface */
        /* anchor extent culls (match the .s prim count) */
        int axi = (int)lrintf(anchor.sx), ayi = (int)lrintf(anchor.sy);
        int32_t packed = (ayi << 16) | (axi & 0xFFFF);
        if (packed <= 0) continue;
        if (packed - 0xF00000 >= 0) continue;
        int xext = s16(axi & 0xFFFF);
        if (xext + 8 <= 0) continue;
        if (xext - 0x208 >= 0) continue;

        int trz = TR[2];
        int g = (trz < 0xC00) ? 0x80 : ((0x1000 - trz) >> 3);
        if (g < 0) g = 0;
        uint32_t grey = ((uint32_t)g) | ((uint32_t)g << 8) | ((uint32_t)g << 16);

        /* 4 fan tris: (anchor, c[i], c[i+1]) */
        for (int k = 0; k < 4; k++) {
            if (s_mosh_n >= MAX_MOSH) break;
            spt* p1 = &c[k]; spt* p2 = &c[(k+1)&3];
            moshtri* t = &s_mosh[s_mosh_n++];
            t->sx[0]=anchor.sx; t->sy[0]=anchor.sy; t->sz[0]=anchor.sz; t->u[0]=u0; t->v[0]=v0t;
            t->sx[1]=p1->sx;    t->sy[1]=p1->sy;    t->sz[1]=p1->sz;    t->u[1]=u1; t->v[1]=v1t;
            t->sx[2]=p2->sx;    t->sy[2]=p2->sy;    t->sz[2]=p2->sz;    t->u[2]=u2; t->v[2]=v2t;
            t->rgb = grey;
            t->tpx=tpx; t->tpy=tpy; t->clx=clx; t->cly=cly; t->bpp=bpp;
        }
    }
}

/* feed the native moby shadows (textured FT3, ABR mode 0 -> blend=1) */
static void shadow_moby_feed_native(CPUState* cpu) {
    shadow_moby_build(cpu);
    if (s_mosh_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_mosh_n; i++) {
        moshtri* m = &s_mosh[i];
        openpete_scene_tri st;
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = m->sx[k]; st.v[k].sy = m->sy[k];
            st.v[k].z  = openpete_native_depth_from_viewz(m->sz[k]);
            st.v[k].u = (float)m->u[k]; st.v[k].v = (float)m->v[k];
            st.v[k].rgba = (m->rgb & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 1; st.tpage_x = m->tpx; st.tpage_y = m->tpy;
        st.clut_x = m->clx; st.clut_y = m->cly; st.bpp = m->bpp; st.blend = 3; /* ABR mode 2 (subtract) */
        openpete_native_scene_add_tri(&st);
    }
}

/* A/B: native moby-shadow tris vs the captured FT3 prims (.s int16 SXY oracle).
 * Both produce 4 tris/moby in the same fan order, so compare element-wise. */
static void shadow_moby_compare(CPUState* cpu) {
    shadow_moby_build(cpu);
    int ft3 = 0; for (int p = 0; p < s_gte_n; p++) if (s_gte[p].textured && s_gte[p].nv == 3) ft3++;
    int n = s_mosh_n < ft3 ? s_mosh_n : ft3;
    double worst = 0, sum = 0; int over2 = 0, nv = 0, qi = 0;
    for (int p = 0; p < s_gte_n && qi < n; p++) {
        prim_t* pr = &s_gte[p];
        if (!(pr->textured && pr->nv == 3)) continue;
        moshtri* m = &s_mosh[qi];
        double tw = 0;
        for (int k = 0; k < 3; k++) {
            double dx = m->sx[k]-pr->x[k], dy = m->sy[k]-pr->y[k];
            double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
            sum += d; nv++; if (d > tw) tw = d;
        }
        if (tw > worst) worst = tw;
        if (tw > 2.0) over2++;
        qi++;
    }
    fprintf(stderr, "[shadow-moby f=%u] native=%d FT3=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx over2px=%d/%d\n",
            openpete_frame, s_mosh_n, ft3, n, nv?sum/nv:0, worst, over2, n);
}

/* ===========================================================================
 *  SCAFFOLD FEED — feed the captured .s prims with the correct PSX semantics
 *  (mode-0 semi-transparent). Geometry (SXY) comes straight from the .s prims
 *  — byte-exact — so this proves the mode-0 pipeline (textured + the new
 *  untextured-alpha path) end-to-end before the native geometry walk replaces
 *  the prim coupling. Depth is a single representative ground depth (the native
 *  walk gives per-vertex Z).
 * =========================================================================== */
static float scaffold_depth(void) {
    return openpete_native_depth_from_viewz(2048.0f);   /* mid ground depth */
}

/* moby shadows: each captured prim is a textured FT3 (code 0x26), ABR mode 0. */
static void shadow_moby_feed_scaffold(CPUState* cpu) {
    (void)cpu;
    if (s_gte_n <= 0) return;
    float depth = scaffold_depth();
    openpete_native_scene_begin_frame(openpete_frame);
    for (int p = 0; p < s_gte_n; p++) {
        prim_t* t = &s_gte[p];
        if (t->nv < 3 || !t->textured) continue;
        int tpx = (t->tpage & 0xF) * 64, tpy = ((t->tpage >> 4) & 1) * 256;
        int mode = (t->tpage >> 7) & 3, bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
        int clx = (t->clut & 0x3F) * 16, cly = (t->clut >> 6) & 0x1FF;
        openpete_scene_tri st;
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->x[k]; st.v[k].sy = t->y[k]; st.v[k].z = depth;
            st.v[k].u = (float)t->u[k]; st.v[k].v = (float)t->v[k];
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 1; st.tpage_x = tpx; st.tpage_y = tpy;
        st.clut_x = clx; st.clut_y = cly; st.bpp = bpp; st.blend = 3; /* ABR mode 2 (subtract) */
        openpete_native_scene_add_tri(&st);
    }
}

/* Spyro shadow: each captured G3 (code 0x32) is an untextured gouraud tri, ABR
 * mode 2 (B-F, subtractive: the shadow grey darkens the ground). Feed untextured
 * blend=3; the subtract blend reverse-subtracts the vertex colour (srcF=ONE, so
 * alpha is irrelevant — the grey at the anchor darkens, black at the rim no-ops). */
static void shadow_spyro_feed_scaffold(CPUState* cpu) {
    (void)cpu;
    if (s_gte_n <= 0) return;
    float depth = scaffold_depth();
    openpete_native_scene_begin_frame(openpete_frame);
    for (int p = 0; p < s_gte_n; p++) {
        prim_t* t = &s_gte[p];
        if (t->nv < 3 || t->textured) continue;       /* skip the E1 mini-packet */
        openpete_scene_tri st;
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->x[k]; st.v[k].sy = t->y[k]; st.v[k].z = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0; st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 3;                                  /* untextured subtract (ABR mode 2) */
        openpete_native_scene_add_tri(&st);
    }
}

/* ---- gating --------------------------------------------------------------- */
static int shadow_verify_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SHADOW_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static unsigned shadow_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_SHADOW_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

static int on_80059F8C_shadow_moby(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = shadow_verify_enabled();
    int feed   = openpete_native_render_enabled();
    if (!verify && !feed && !render_paths_active()) return 0;  /* inert: normal dispatch */
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_80059F8C(cpu);                          /* run the real renderer */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_SHADOW, startp, endp);

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_SHADOW)) {
        if (getenv("OPENPETE_SHADOW_SCAFFOLD"))
            shadow_moby_feed_scaffold(cpu);          /* fallback: .s-prim scaffold */
        else
            shadow_moby_feed_native(cpu);            /* native gamestate walk (default) */
    }

    if (verify && endp > startp && openpete_frame >= shadow_verify_frame()) {
        static int s_dumped = 0, s_reports = 0;
        if (!s_dumped) { shadow_moby_dump(cpu); s_dumped = 1; }
        if (s_reports < 60) { shadow_moby_compare(cpu); s_reports++; }
    }
    s_in = 0;
    return 1;
}

static int on_80059A48_shadow_spyro(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = shadow_verify_enabled();
    int feed   = openpete_native_render_enabled();
    if (!verify && !feed && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_80059A48(cpu);
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_SHADOW, startp, endp);

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_SHADOW))
        shadow_spyro_feed_scaffold(cpu);

    if (verify && endp > startp && openpete_frame >= shadow_verify_frame()) {
        static int s_dumped = 0, s_reports = 0;
        if (!s_dumped) { shadow_spyro_dump(cpu); s_dumped = 1; }
        if (s_reports < 60) {
            int g3 = 0; for (int p = 0; p < s_gte_n; p++) if (!s_gte[p].textured && s_gte[p].nv == 3) g3++;
            fprintf(stderr, "[shadow-spyro f=%u] prims=%d G3=%d\n", openpete_frame, s_gte_n, g3);
            s_reports++;
        }
    }
    s_in = 0;
    return 1;
}

static const feature_dispatch_entry_t shadow_dispatch[] = {
    { 0x80059F8Cu, on_80059F8C_shadow_moby },
    { 0x80059A48u, on_80059A48_shadow_spyro },
    { 0u, NULL },
};

const feature_module_t shadow_native_module = {
    .name           = "shadow_native",
    .apply_options  = NULL,
    .dispatch_table = shadow_dispatch,
};
