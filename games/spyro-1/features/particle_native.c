// games/spyro-1/features/particle_native.c — Phase 3b.3 of
// notes/plans/openpete-native-renderer.md: the PARTICLE renderer family
// (func_800573C8 particles + func_800580F4 glows + func_800584C4 sparkles /
// r_particles.s) re-derived from gamestate in float + its numerical A/B
// harness. This is the LAST 3b actor and the most complex: 7 particle sprite
// classes (FT4 billboards, dots/tiles, F2 lines), additive glow rings, and
// twinkle sparkle quads — a mix of GPU primitive types over one polybuf.
//
// VERDICT-CRITICAL (see the format notes): the .s writes Particle.m_03
// (drawn stamp, read next frame -> conditional rand() -> RNG seed) and
// Sparkle m_Life. We NEVER reproduce those — the real .s runs headless exactly
// once per call (behaviour-identical to normal dispatch) and we only READ pool
// state + capture its emitted prims afterwards, so determinism holds by
// construction. The native path only mirrors the GEOMETRY in float.
//
// Three sub-renderers hooked; all record RP_PARTICLES (retiring the thin
// render_paths.c bracket on func_800573C8). func_800573C8 is called ~9x/frame
// (one per occlusion/gamestate pass); glows+sparkles once/frame. Each call
// brackets the polybuf cursor and appends to the native scene.
//
// Gated by OPENPETE_PARTICLE_VERIFY (dump/A/B) or a native renderer mode
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

extern void     func_800573C8(CPUState* cpu);  /* RenderParticles  */
extern void     func_800580F4(CPUState* cpu);  /* RenderGlows      */
extern void     func_800584C4(CPUState* cpu);  /* RenderSparkles   */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map / the format notes) --------------- */
#define G_PARTICLES    0x80075824u   /* Particle[] base, stride 0x20, m_Type=0xFF term */
#define G_SPARKLES     0x80077108u   /* Sparkle[8], stride 0x18                        */
#define D_GLOWS        0x80078800u   /* Glow[16], stride 0x24                          */
#define D_PARTTEX      0x80076278u   /* ParticleTexture*[32] by m_Type                 */
#define G_CAMERA       0x80076DD0u   /* projMat +0x00 (5 words), world pos +0x28        */
#define D_SIN_TABLE    0x8006CBF8u   /* SINE_8 sin[idx]; cos at +0x80                   */
#define POLYBUF_CURSOR 0x800757B0u

/* ---- captured prims (ground truth) --------------------------------------- *
 * Particle FT4 quads (GPU code 0x2C/0x2E) + glow G3 rings (0x32) decode with
 * the standard PSX polygon layout (code = high byte of word[+0x04]; word[+0x00]
 * is the OT tag whose high byte is the *length*, low24 the link). Tiles (0x60),
 * lines (0x40/0x48) and the sparkle quads are NON-polygon prims: kept as raw
 * words for the dump, decoded specially in the feed once the dump confirms the
 * exact layout. */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1]>>24)  */
    uint8_t  taglen;        /* OT tag length (word[0]>>24)       */
    int      nv;            /* decoded polygon verts (3/4), else 0 */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad, stp;
    uint32_t raw[16];
    int      nraw;
} prim_t;

#define MAX_PRIMS 8192
static prim_t s_prim[MAX_PRIMS]; static int s_prim_n;
/* The g_Particles pool pointer is double-buffered and swapped between render
 * calls, so reading it AFTER the .s runs gets the wrong (other) buffer. The
 * hook stashes the pointer the .s actually walked at hook-entry. */
static uint32_t s_part_pool;
static int s_code_hist[256];     /* keyed on the GPU code byte (word[+4]>>24) */
static int s_tag_hist[256];      /* keyed on the OT tag length (word[+0]>>24) */

static int s16(int v) { return (int)(int16_t)(uint32_t)v; }

/* Walk a polybuf [start,end) by OT tag length (stride=((w0>>24)+1)*4); decode
 * polygon prims (code high byte of word[+4]) with the standard PSX layout. */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < MAX_PRIMS) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t taglen = (w0 >> 24) & 0xFF;
        if (taglen == 0 || taglen > 12) break;        /* not a tag / corrupt — stop */
        uint32_t stride = (taglen + 1) * 4;
        uint8_t code = (uint8_t)(cpu->read_word(p + 4) >> 24);
        if (s_prim_n < MAX_PRIMS) {
            prim_t* t = &s_prim[s_prim_n];
            memset(t, 0, sizeof(*t));
            t->code = code; t->taglen = (uint8_t)taglen;
            t->nraw = (int)(stride / 4); if (t->nraw > 16) t->nraw = 16;
            for (int i = 0; i < t->nraw; i++) t->raw[i] = cpu->read_word(p + (uint32_t)i*4);
            if ((code & 0xE0) == 0x20) {              /* a GPU polygon */
                int gouraud = (code & 0x10) != 0;
                int quad    = (code & 0x08) != 0;
                int tex     = (code & 0x04) != 0;
                int stp     = (code & 0x02) != 0;
                t->gouraud = gouraud; t->textured = tex; t->quad = quad; t->stp = stp;
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
            s_tag_hist[taglen & 0xFF]++;
            s_prim_n++;
        }
        p += stride;
    }
}

/* ---- one-shot dump (gamestate pools + raw polybuf + decoded prims) -------- */
static void dump_pools(CPUState* cpu) {
    fprintf(stderr, "  [particles] g_Particles walk (sel/m_Class Life m03 | +4 +8):");
    uint32_t pp = s_part_pool; int npart = 0;   /* pool the .s walked (hook-entry) */
    for (int i = 0; i < 320; i++, pp += 0x20) {
        uint32_t hdr = cpu->read_word(pp);
        int sel = (int)((hdr >> 8) & 0xFF), mclass = (int)(hdr & 0xFF);
        if (sel == 0xFF) break;                        /* end of pool */
        if (sel & 0x80) continue;                      /* inactive slot (skip) */
        int life = (int)((hdr >> 16) & 0xFF), m03 = (int)((hdr >> 24) & 0xFF);
        if (npart < 16) fprintf(stderr, " [%d]s%d/c%d L%d d%d(%08X,%08X)", i, sel, mclass, life, m03,
                                cpu->read_word(pp+4), cpu->read_word(pp+8));
        npart++;
    }
    fprintf(stderr, "  (total live=%d)\n", npart);

    fprintf(stderr, "  [sparkles] g_Sparkles[8] (pos | Life Speed type | color size):\n");
    for (int i = 0; i < 8; i++) {
        uint32_t sp = G_SPARKLES + (uint32_t)i*0x18;
        int x = (int)cpu->read_word(sp+0), y = (int)cpu->read_word(sp+4), z = (int)cpu->read_word(sp+8);
        uint32_t w3 = cpu->read_word(sp+0xC), col = cpu->read_word(sp+0x10), w5 = cpu->read_word(sp+0x14);
        fprintf(stderr, "    [%d] (%d,%d,%d) life=%d spd=%d t=%d col=%06X size=%d\n", i, x, y, z,
                (int)(w3&0xFF), (int)((w3>>8)&0xFF), (int)((w3>>24)&0xFF), col&0xFFFFFF, (int)((w5>>8)&0xFF));
    }

    fprintf(stderr, "  [glows] D_80078800[16] (active | MobyPos* color size | offset bias):\n");
    for (int i = 0; i < 16; i++) {
        uint32_t g = D_GLOWS + (uint32_t)i*0x24;
        uint32_t act = cpu->read_word(g+0);
        if (act == 0) continue;
        uint32_t mp = cpu->read_word(g+0x08), col = cpu->read_word(g+0x0C), sz = cpu->read_word(g+0x10);
        int ox = (int)cpu->read_word(g+0x14), oy = (int)cpu->read_word(g+0x18), oz = (int)cpu->read_word(g+0x1C);
        int bias = (int)cpu->read_word(g+0x20);
        fprintf(stderr, "    [%d] act=%X mobypos=%08X col=%06X size=%d off=(%d,%d,%d) bias=%d\n",
                i, act, mp, col&0xFFFFFF, (int)sz, ox, oy, oz, bias);
    }
    fprintf(stderr, "  camera projMat:");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(G_CAMERA+(uint32_t)i*4));
    fprintf(stderr, "  pos=(%d,%d,%d)\n", (int)cpu->read_word(G_CAMERA+0x28),
            (int)cpu->read_word(G_CAMERA+0x2C), (int)cpu->read_word(G_CAMERA+0x30));
    fprintf(stderr, "  OFX=%d OFY=%d H=%d DQA=%d DQB=%d\n",
            (int)cpu->gte_ctrl[24]>>16, (int)cpu->gte_ctrl[25]>>16, (int)(cpu->gte_ctrl[26]&0xFFFF),
            (int)cpu->gte_ctrl[27], (int)cpu->gte_ctrl[28]);
}

static void dump_prims(CPUState* cpu, const char* tag, uint32_t start, uint32_t end) {
    fprintf(stderr, "\n=== [%s-dump f=%u] polybuf [%08X,%08X) %u bytes, %d prims ===\n",
            tag, openpete_frame, start, end, end - start, s_prim_n);
    /* linear raw dump (4 words/row) — the only reliable view of non-polygon prims */
    fprintf(stderr, "  RAW words:\n");
    int rows = 0;
    for (uint32_t p = start; p < end && rows < 40; p += 16, rows++) {
        fprintf(stderr, "    +%04X:", p - start);
        for (int i = 0; i < 4 && p + (uint32_t)i*4 < end; i++)
            fprintf(stderr, " %08X", cpu->read_word(p + (uint32_t)i*4));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  code histogram (word[+4]>>24):");
    for (int c = 0; c < 256; c++) if (s_code_hist[c]) fprintf(stderr, " %02X:%d", c, s_code_hist[c]);
    fprintf(stderr, "\n  tag-len histogram (word[+0]>>24):");
    for (int c = 0; c < 256; c++) if (s_tag_hist[c]) fprintf(stderr, " %d:%d", c, s_tag_hist[c]);
    fprintf(stderr, "\n");
    int n = s_prim_n < 8 ? s_prim_n : 8;
    for (int i = 0; i < n; i++) {
        prim_t* t = &s_prim[i];
        fprintf(stderr, "  prim[%d] code=%02X taglen=%d nv=%d tex=%d gour=%d quad=%d stp=%d  raw:",
                i, t->code, t->taglen, t->nv, t->textured, t->gouraud, t->quad, t->stp);
        for (int k = 0; k < t->nraw; k++) fprintf(stderr, " %08X", t->raw[k]);
        if (t->nv) {
            fprintf(stderr, "\n      decoded:");
            for (int k = 0; k < t->nv; k++)
                fprintf(stderr, " v%d(%.0f,%.0f rgb=%06X uv=%d,%d)", k, t->x[k], t->y[k], t->rgb[k], t->u[k], t->v[k]);
            if (t->textured) fprintf(stderr, " clut=%04X tpage=%04X", t->clut, t->tpage);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "=== end %s-dump ===\n\n", tag);
}

/* ---- native float walk (3b.3) — WORK IN PROGRESS, verify-only ------------- *
 * Re-derive particle geometry from gamestate in float. NOT YET fed to the scene
 * (no native particle rendering) and NOT YET A/B-validated — this is the probe
 * that will become the walk. It runs only under OPENPETE_PARTICLE_VERIFY (an
 * A/B diagnostic) and is inert in the feed/oracle paths, so determinism is
 * unaffected.
 *
 * ESTABLISHED so far (see the format notes "Native walk progress"):
 *   - g_Particles (0x80075824) is a POINTER to the buffer (deref before walking).
 *   - dispatch class = the HIGH byte of the lh header (m_Type/"sel"); the LOW
 *     byte is m_Class (texture/clut index). The doc had these swapped.
 *   - position layout: X=lo16(+0x04), Y=hi16(+0x04), Z=lo16(+0x08), all in
 *     world/4 units; camera-relative VX=X-camX/4, VY=camY/4-Y, VZ=camZ/4-Z.
 *     Confirmed by sel=2/3 particles decoding to small near-camera offsets.
 *   - the FT4 (code 0x2C) prims in the demo come from sel=2 + sel=3 (affine /
 *     GPF-rect billboards), NOT sel=0 — so a clean ordered A/B needs ALL FT4
 *     emitters (0/2/3) implemented before the prim sequence aligns. TODO.
 *
 * Stage 1 below = the FT4 emitters present in the attract demo: sel 3
 * (block_80057A74, depth-cued screen rect), sel 2 (block_80057C08, affine
 * sin/cos billboard) + the sel>=6 fall-through rotated billboard
 * (block_800574F8). sel 0 (TILE), sel 1/4 (lines), sel 5 (affine matrix),
 * glows + sparkles still TODO. */
#define GTE_DIV_CAP   (131071.0 / 65536.0)
#define GTE_SXY_BOUND 1024.0

static int rd_s16(CPUState* cpu, uint32_t addr) {
    uint32_t w = cpu->read_word(addr & ~3u);
    return (int)(int16_t)((addr & 2u) ? (w >> 16) : (w & 0xFFFF));
}
static double clamp16(double v) { return v < -32768.0 ? -32768.0 : (v > 32767.0 ? 32767.0 : v); }
static double clampSXY(double x) {
    if (x < -GTE_SXY_BOUND) return -GTE_SXY_BOUND;
    if (x > GTE_SXY_BOUND-1.0) return GTE_SXY_BOUND-1.0;
    return x;
}

/* camera rotation/projection matrix (g_Camera+0x00, 5 words -> 9 int16) */
static void load_cam_matrix(CPUState* cpu, int R[3][3]) {
    uint32_t w0 = cpu->read_word(G_CAMERA+0), w1 = cpu->read_word(G_CAMERA+4);
    uint32_t w2 = cpu->read_word(G_CAMERA+8), w3 = cpu->read_word(G_CAMERA+12);
    uint32_t w4 = cpu->read_word(G_CAMERA+16);
    R[0][0]=s16(w0&0xFFFF); R[0][1]=s16(w0>>16); R[0][2]=s16(w1&0xFFFF);
    R[1][0]=s16(w1>>16);    R[1][1]=s16(w2&0xFFFF); R[1][2]=s16(w2>>16);
    R[2][0]=s16(w3&0xFFFF); R[2][1]=s16(w3>>16); R[2][2]=s16(w4&0xFFFF);
}

/* RTPS one view-space vertex (TR=0) through R -> screen + the view-space MAC */
typedef struct { double sx, sy, m1, m2, m3; } projres;
static void proj2(const int R[3][3], double ofx, double ofy, double H,
                  double vx, double vy, double vz, projres* o) {
    double mac1 = (R[0][0]*vx + R[0][1]*vy + R[0][2]*vz) / 4096.0;
    double mac2 = (R[1][0]*vx + R[1][1]*vy + R[1][2]*vz) / 4096.0;
    double mac3 = (R[2][0]*vx + R[2][1]*vy + R[2][2]*vz) / 4096.0;
    o->m1 = mac1; o->m2 = mac2; o->m3 = mac3;
    double sz3 = mac3 < 0.0 ? 0.0 : (mac3 > 65535.0 ? 65535.0 : mac3);
    double q = (sz3 <= 0.0 || H >= 2.0 * sz3) ? GTE_DIV_CAP : (H / sz3);
    o->sx = clampSXY(ofx + clamp16(mac1) * q);
    o->sy = clampSXY(ofy + clamp16(mac2) * q);
}

/* the .s screen-rect reject of the projected centre (s4=0x10000 s5=0x1000000
 * s6=0x2000000), faithfully on the packed int32 (SY<<16)|(SX&0xFFFF). */
static int screen_rect_cull(double sxf, double syf) {
    int SX = (int)lrint(sxf), SY = (int)lrint(syf);
    int32_t v0  = (int32_t)(((uint32_t)(SY & 0xFFFF) << 16) | (uint32_t)(SX & 0xFFFF));
    int32_t sxs = (int32_t)((uint32_t)(SX & 0xFFFF) << 16);
    if ((v0 - 0x10000) <= 0)    return 1;   /* above top / SY<=1   */
    if ((v0 - 0x1000000) >= 0)  return 1;   /* below bottom SY>=256 */
    if (sxs <= 0)               return 1;    /* left of screen SX<=0 */
    if ((sxs - 0x2000000) >= 0) return 1;    /* right of screen SX>=512 */
    return 0;
}

typedef struct {
    float sx[4], sy[4], sz[4];   /* corner screen XY + per-corner view-Z */
    float cz;                     /* representative centre view-Z (depth sort) */
    int sel;
    uint32_t rgb;                 /* modulation colour (particle +0x0C low24) */
    int u[4], v[4];               /* per-corner texel coords */
    int tpx, tpy, clx, cly, bpp;  /* VRAM material */
    int textured, blend;          /* 0 opaque / 1 alpha / 2 add / 3 sub */
} fquad;
#define MAX_FQUAD 2048
static fquad s_fq[MAX_FQUAD]; static int s_fq_n;

/* Decode the FT4 material from gamestate (static texture table + the particle's
 * colour word) — same idiom for all FT4 classes: cmd/colour at +0x0C (hi byte =
 * GPU code, STP=bit1), texture record = D_80076278[m_Class] + (byte +0x10)*8,
 * UV0/clut at texrec+4, UV3/tpage at texrec+8, the other two corners are the
 * (u3,v0)/(u0,v3) split. corner k <-> UVk (matches the .s vertex order). */
static void decode_material(CPUState* cpu, uint32_t pp, int mclass, fquad* q) {
    uint32_t cmd = cpu->read_word(pp + 0x0C);
    q->rgb = cmd & 0xFFFFFF;
    int stp = ((cmd >> 24) & 0x02) != 0;          /* GPU code STP bit */
    uint32_t texbase = cpu->read_word(D_PARTTEX + (uint32_t)mclass * 4);
    if (!texbase) {                               /* no texture record -> flat */
        q->textured = 0; q->blend = stp ? 1 : 0;
        for (int k = 0; k < 4; k++) { q->u[k] = q->v[k] = 0; }
        q->tpx = q->tpy = q->clx = q->cly = q->bpp = 0;
        return;
    }
    uint32_t texrec = texbase + (cpu->read_word(pp + 0x10) & 0xFF) * 8u;
    uint32_t w4 = cpu->read_word(texrec + 4), w8 = cpu->read_word(texrec + 8);
    int u0 = (int)(w4 & 0xFF), v0 = (int)((w4 >> 8) & 0xFF), clut = (int)((w4 >> 16) & 0xFFFF);
    int u3 = (int)(w8 & 0xFF), v3 = (int)((w8 >> 8) & 0xFF), tpage = (int)((w8 >> 16) & 0xFFFF);
    q->u[0]=u0; q->v[0]=v0; q->u[1]=u3; q->v[1]=v0; q->u[2]=u0; q->v[2]=v3; q->u[3]=u3; q->v[3]=v3;
    q->tpx = (tpage & 0xF) * 64; q->tpy = ((tpage >> 4) & 1) * 256;
    int mode = (tpage >> 7) & 3; q->bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
    q->clx = (clut & 0x3F) * 16; q->cly = (clut >> 6) & 0x1FF;
    q->textured = 1;
    int abr = (tpage >> 5) & 3;                    /* data-driven blend (ABR + STP) */
    q->blend = stp ? (abr == 0 ? 1 : abr == 1 ? 2 : abr == 2 ? 3 : 2) : 0;
}

/* SZ window 128..8192 (clamped SZ3) + depth-cue LOD cull (SZ3>>5 < texsub>>8). */
static int sz_and_lod_cull(double mac3, int texsub_hi) {
    int sz3 = (int)(mac3 < 0 ? 0 : (mac3 > 65535 ? 65535 : mac3));
    if (sz3 < 128 || sz3 >= 8192) return 1;
    if (((sz3 >> 5) - texsub_hi) < 0) return 1;
    return 0;
}

static void particle_build(CPUState* cpu) {
    s_fq_n = 0;
    int R[3][3]; load_cam_matrix(cpu, R);
    int s7 = (int)cpu->read_word(G_CAMERA+0x28) >> 2;   /* camX/4 */
    int t8 = (int)cpu->read_word(G_CAMERA+0x2C) >> 2;   /* camY/4 */
    int t9 = (int)cpu->read_word(G_CAMERA+0x30) >> 2;   /* camZ/4 */
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFF);

    uint32_t pp = s_part_pool;                          /* pool the .s walked (hook-entry) */
    for (int i = 0; i < 320 && s_fq_n < MAX_FQUAD; i++, pp += 0x20) {
        uint32_t hdr = cpu->read_word(pp);
        int mtype = (int)((hdr >> 8) & 0xFF);           /* dispatch byte (sel) */
        int mclass = (int)(hdr & 0xFF);                 /* texture/clut index */
        if (mtype == 0xFF) break;                       /* end of pool (gp>>8==-1) */
        if (mtype & 0x80) continue;                     /* inactive slot (skip, not stop) */
        int sel = mtype;
        uint32_t A = cpu->read_word(pp + 4), B = cpu->read_word(pp + 8);
        int VXb = (int)(A & 0xFFFF) - s7;               /* GTE-Z */
        int VYb = t8 - (int)(A >> 16);                  /* GTE-X */
        int VZb = t9 - (int)(B & 0xFFFF);               /* GTE-Y */
        int texsub_hi = (int)(cpu->read_word(pp + 0x10) & 0xFFFF) >> 8;
        fquad q; q.sel = sel;

        if (sel == 3) {                                 /* depth-cued screen rect */
            projres c; proj2(R, ofx, ofy, H, (double)VYb, (double)VZb, (double)VXb, &c);
            if (screen_rect_cull(c.sx, c.sy)) continue;
            if (sz_and_lod_cull(c.m3, texsub_hi)) continue;
            int sz3 = (int)(c.m3 < 0 ? 0 : (c.m3 > 65535 ? 65535 : c.m3));
            double qd = sz3 <= 0 ? 0 : (double)H / sz3;
            int IR0 = (int)(qd * 4096.0); if (IR0 < 0) IR0 = 0; else if (IR0 > 4096) IR0 = 4096;
            int sizeB = (int)((B >> 16) & 0xFF), alphaB = (int)((B >> 24) & 0xFF);
            int a2 = (sizeB * IR0) >> 12, a3 = (alphaB * IR0) >> 12;  /* halfW, halfH */
            int t0 = a2 >> 1, t1 = a3 >> 1;
            double cx = c.sx + t0, cy = c.sy + t1;       /* rect corners (screen) */
            double X0 = cx - a2, X1 = cx, Y0 = cy - a3, Y1 = cy;
            q.sx[0]=X0; q.sy[0]=Y0; q.sx[1]=X1; q.sy[1]=Y0;
            q.sx[2]=X0; q.sy[2]=Y1; q.sx[3]=X1; q.sy[3]=Y1;
            for (int k = 0; k < 4; k++) q.sz[k] = (float)c.m3;
            q.cz = (float)c.m3;
            decode_material(cpu, pp, mclass, &q);
            s_fq[s_fq_n++] = q;
        } else if (sel == 2) {                          /* affine sin/cos billboard */
            projres c; proj2(R, ofx, ofy, H, (double)VYb, (double)VZb, (double)VXb, &c);
            if (screen_rect_cull(c.sx, c.sy)) continue;
            if (sz_and_lod_cull(c.m3, texsub_hi)) continue;
            int sizeB = (int)((B >> 16) & 0xFF);
            int angIdx = (int)(((B >> 23) + 64) & 0x1FE);
            int sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx);
            int cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx + 0x80);
            int a2 = (sizeB * sinv) >> 10, a3 = (sizeB * cosv) >> 10;
            double C[4][2] = { {-a2,-a3}, {a3,-a2}, {-a3,a2}, {a2,a3} };
            double TR0 = c.m1*4.0, TR1 = c.m2*4.0, TR2 = c.m3*4.0;
            double sz3 = TR2 < 0 ? 0 : (TR2 > 65535 ? 65535 : TR2);
            double qd = (sz3 <= 0.0 || H >= 2.0*sz3) ? GTE_DIV_CAP : (double)H/sz3;
            for (int k = 0; k < 4; k++) {                /* R'=diag(4096,2560,4096)+TR */
                double m1 = C[k][0] + TR0;
                double m2 = C[k][1]*2560.0/4096.0 + TR1;
                q.sx[k] = (float)clampSXY(ofx + clamp16(m1)*qd);
                q.sy[k] = (float)clampSXY(ofy + clamp16(m2)*qd);
                q.sz[k] = (float)TR2;
            }
            q.cz = (float)c.m3;                          /* real centre depth (not TR2) */
            decode_material(cpu, pp, mclass, &q);
            s_fq[s_fq_n++] = q;
        } else if (sel >= 6) {                          /* rotated-billboard FT4 default */
            int sizeB = (int)((B >> 16) & 0xFF);
            int angIdx = (int)((B >> 23) & 0x1FE);
            int sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx);
            int cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx + 0x80);
            int eS = (sizeB * sinv) >> 12, eC = (sizeB * cosv) >> 12;
            double cx[4] = { (double)(VYb-eS), (double)(VYb+eC), (double)(VYb-eC), (double)(VYb+eS) };
            double cz[4] = { (double)(VXb+eC), (double)(VXb+eS), (double)(VXb-eS), (double)(VXb-eC) };
            projres pr[4]; double zsum = 0;
            for (int k = 0; k < 4; k++) {
                proj2(R, ofx, ofy, H, cx[k], (double)VZb, cz[k], &pr[k]);
                q.sx[k] = (float)pr[k].sx; q.sy[k] = (float)pr[k].sy; q.sz[k] = (float)pr[k].m3;
                zsum += pr[k].m3 < 0 ? 0 : (pr[k].m3 > 65535 ? 65535 : pr[k].m3);
            }
            if (zsum < 512.0 || zsum >= 32768.0) continue;
            q.cz = (float)(zsum / 4.0);
            decode_material(cpu, pp, mclass, &q);
            s_fq[s_fq_n++] = q;
        }
        /* sel 0 (TILE), 1/4 (lines), 5 (affine matrix): TODO, not FT4-comparable */
    }
}

static void particle_compare(CPUState* cpu) {
    particle_build(cpu);
    int ft4 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x2C || s_prim[p].code == 0x2E) ft4++;
    int n = s_fq_n < ft4 ? s_fq_n : ft4;
    double worst = 0, sum = 0; int nv = 0, qi = 0, over4 = 0;
    for (int p = 0; p < s_prim_n && qi < n; p++) {
        prim_t* pr = &s_prim[p];
        if (!(pr->code == 0x2C || pr->code == 0x2E)) continue;
        fquad* q = &s_fq[qi];
        double qw = 0;
        for (int k = 0; k < 4; k++) {
            double dx = q->sx[k] - pr->x[k], dy = q->sy[k] - pr->y[k];
            double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
            sum += d; nv++; if (d > qw) qw = d;
        }
        if (qw > worst) worst = qw;
        if (qw > 4.0) over4++;
        qi++;
    }
    fprintf(stderr, "[part-ab f=%u] native=%d prims(FT4)=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx over4=%d/%d\n",
            openpete_frame, s_fq_n, ft4, n, nv?sum/nv:0, worst, over4, n);
    if (getenv("OPENPETE_PARTICLE_DEBUG")) {
        for (int p = 0, c = 0; p < s_prim_n && c < 3 && c < s_fq_n; p++) {
            if (!(s_prim[p].code==0x2C||s_prim[p].code==0x2E)) continue;
            fprintf(stderr, "   sel%d prim:(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)  nat:(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                    s_fq[c].sel,
                    s_prim[p].x[0],s_prim[p].y[0],s_prim[p].x[1],s_prim[p].y[1],
                    s_prim[p].x[2],s_prim[p].y[2],s_prim[p].x[3],s_prim[p].y[3],
                    s_fq[c].sx[0],s_fq[c].sy[0],s_fq[c].sx[1],s_fq[c].sy[1],
                    s_fq[c].sx[2],s_fq[c].sy[2],s_fq[c].sx[3],s_fq[c].sy[3]);
            c++;
        }
    }
}

/* ---- native feed (3b.3 endpoint: float FT4 particles into the scene) -------
 * Push the native FT4 quads (sel 2/3/6) as textured tris with the data-driven
 * blend + a single representative centre depth (particles are small billboards;
 * the per-vertex z would over-separate the affine path's TR<<2). Determinism-
 * safe (display-only; the .s produced m_03 / Sparkle m_Life headless). NOTE:
 * sel 0 (tile), 1/4 (lines), 5 (affine matrix), sparkles + glow rings are not
 * yet walked, so those are absent in the native panel (TODO). */
static void particle_feed_native(CPUState* cpu) {
    particle_build(cpu);
    if (s_fq_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);   /* append after world + actors */
    int split[2][3] = { {0,1,2}, {1,3,2} };              /* FT4 -> 2 tris */
    for (int i = 0; i < s_fq_n; i++) {
        fquad* q = &s_fq[i];
        float depth = openpete_native_depth_from_viewz(q->cz);
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st;
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = q->sx[j]; st.v[k].sy = q->sy[j]; st.v[k].z = depth;
                st.v[k].u = (float)q->u[j]; st.v[k].v = (float)q->v[j];
                st.v[k].rgba = (q->rgb & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = q->textured;
            st.tpage_x = q->tpx; st.tpage_y = q->tpy;
            st.clut_x = q->clx; st.clut_y = q->cly; st.bpp = q->bpp;
            st.blend = q->blend;
            openpete_native_scene_add_tri(&st);
        }
    }
}

/* ---- gating --------------------------------------------------------------- */
static int verify_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PARTICLE_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static unsigned verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_PARTICLE_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

/* Shared hook body: run the real renderer once, capture its prims, record the
 * RP_PARTICLES polybuf range, (dump in verify mode). `tag` selects the dump
 * label + which gamestate pools to print. */
static int on_particle_renderer(CPUState* cpu, void (*realfn)(CPUState*), const char* tag, int dump_pools_too) {
    int verify = verify_enabled();
    int feed   = openpete_native_render_enabled();
    if (!verify && !feed && !render_paths_active()) return 0;  /* inert: normal dispatch */
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    s_part_pool = cpu->read_word(G_PARTICLES);     /* the pool the .s walks (g_Particles is a ptr) */
    realfn(cpu);                                   /* run the real renderer (verdict bytes) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_PARTICLES, startp, endp);

    if (verify) {
        static int s_trace[3] = {0,0,0};
        int slot = (tag[0]=='p') ? 0 : (tag[0]=='g') ? 1 : 2;
        if (s_trace[slot] < 30 && endp != startp) {
            fprintf(stderr, "[%s-call f=%u] polybuf %u bytes\n", tag, openpete_frame, endp - startp);
            s_trace[slot]++;
        }
    }

    if ((feed || verify) && endp > startp) {
        s_prim_n = 0;
        capture_prims(cpu, startp, endp);
    }

    /* feed the native FT4 particles (sel 2/3/6) into the scene. Only the
     * particles renderer (tag 'p'); glows/sparkles have no native walk yet. */
    if (feed && tag[0]=='p' && endp > startp && render_path_enabled(RP_PARTICLES))
        particle_feed_native(cpu);

    if (verify && endp > startp && openpete_frame >= verify_frame()) {
        static int s_dumped[3] = {0,0,0};
        int slot = (tag[0]=='p') ? 0 : (tag[0]=='g') ? 1 : 2;
        if (!s_dumped[slot]) {
            if (dump_pools_too) dump_pools(cpu);
            dump_prims(cpu, tag, startp, endp);
            s_dumped[slot] = 1;
        }
        if (slot == 0) {                                 /* particles: class-0 FT4 A/B */
            static int s_reports = 0;
            if (s_reports < 100) { particle_compare(cpu); s_reports++; }
        }
    }

    s_in = 0;
    return 1;
}

static int on_800573C8_particles(CPUState* cpu, uint32_t addr) { (void)addr; return on_particle_renderer(cpu, func_800573C8, "particles", 1); }
static int on_800580F4_glows(CPUState* cpu, uint32_t addr)     { (void)addr; return on_particle_renderer(cpu, func_800580F4, "glows", 0); }
static int on_800584C4_sparkles(CPUState* cpu, uint32_t addr)  { (void)addr; return on_particle_renderer(cpu, func_800584C4, "sparkles", 0); }

static const feature_dispatch_entry_t particle_dispatch[] = {
    { 0x800573C8u, on_800573C8_particles },
    { 0x800580F4u, on_800580F4_glows },
    { 0x800584C4u, on_800584C4_sparkles },
    { 0u, NULL },
};

const feature_module_t particle_native_module = {
    .name           = "particle_native",
    .apply_options  = NULL,
    .dispatch_table = particle_dispatch,
};
