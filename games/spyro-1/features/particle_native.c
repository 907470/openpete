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

/* the default class's 4-corner Sutherland reject (block_8005768C..80057718):
 * cull only when ALL 4 corners lie outside the SAME screen edge (s4=top,
 * s5=bottom, s6=right; left = SX<=0), on the packed int32 SXY like the .s. */
static int clip4_cull(const float* sx, const float* sy) {
    int top = 1, bot = 1, left = 1, right = 1;
    for (int k = 0; k < 4; k++) {
        int SX = (int)lrintf(sx[k]), SY = (int)lrintf(sy[k]);
        int32_t v   = (int32_t)(((uint32_t)(SY & 0xFFFF) << 16) | (uint32_t)(SX & 0xFFFF));
        int32_t sxs = (int32_t)((uint32_t)(SX & 0xFFFF) << 16);
        if ((v - 0x10000) > 0)     top = 0;
        if ((v - 0x1000000) < 0)   bot = 0;
        if (sxs > 0)               left = 0;
        if ((sxs - 0x2000000) < 0) right = 0;
    }
    return top | bot | left | right;
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

/* sel-1 gouraud motion-streak LINE_G (code 0x50): a line between two
 * camera-relative world positions with a per-endpoint colour. */
typedef struct {
    float ax, ay, bx, by;        /* projected endpoints */
    uint32_t rgb0, rgb1;         /* per-endpoint colour (gouraud) */
    float cz;                    /* EP0 view-Z (depth) */
    int blend;
} pline;
#define MAX_PLINE 1024
static pline s_pline[MAX_PLINE]; static int s_pline_n;

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
    s_fq_n = 0; s_pline_n = 0;
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
            if (zsum < 512.0 || zsum >= 32768.0) continue;          /* depth-sum window */
            if ((((int)zsum) >> 7) - texsub_hi < 0) continue;       /* LOD cull */
            if (clip4_cull(q.sx, q.sy)) continue;                   /* 4-corner clip */
            q.cz = (float)(zsum / 4.0);
            decode_material(cpu, pp, mclass, &q);
            s_fq[s_fq_n++] = q;
        } else if (sel == 0) {                          /* TILE / 1px dot (block_80057F98) */
            projres c; proj2(R, ofx, ofy, H, (double)VYb, (double)VZb, (double)VXb, &c);
            int sz3 = (int)(c.m3 < 0 ? 0 : (c.m3 > 65535 ? 65535 : c.m3));
            if (sz3 == 0 || sz3 >= 8192) continue;      /* SZ window (SZ3!=0 .. 8192) */
            if (screen_rect_cull(c.sx, c.sy)) continue;
            int lod = (int)((B >> 16) & 0xFF);
            if (((sz3 >> 5) - lod - 2) <= 0) continue;  /* depth-cue LOD cull (s3=2) */
            /* The .s emits this as a LINE_F from (SX,SY) to (SX+1,SY) — a
             * 2px-wide x 1px-tall horizontal footprint (PSX plots both line
             * endpoints). A 1px-wide quad renders as a tall sliver because PSX
             * pixels are tall (512x224 framebuffer shown at 4:3 ~ 0.583:1 px);
             * matching the 2px line width makes it ~square like the .s. */
            q.sx[0]=(float)c.sx;       q.sy[0]=(float)c.sy;        /* 2x1 line footprint */
            q.sx[1]=(float)(c.sx+2.0); q.sy[1]=(float)c.sy;
            q.sx[2]=(float)c.sx;       q.sy[2]=(float)(c.sy+1.0);
            q.sx[3]=(float)(c.sx+2.0); q.sy[3]=(float)(c.sy+1.0);
            for (int k = 0; k < 4; k++) q.sz[k] = (float)c.m3;
            q.cz = (float)c.m3;
            uint32_t cmd = cpu->read_word(pp + 0x0C);    /* flat colour, no texture */
            q.rgb = cmd & 0xFFFFFF; q.textured = 0;
            q.blend = ((cmd >> 24) & 0x02) ? 1 : 0;
            for (int k = 0; k < 4; k++) { q.u[k] = q.v[k] = 0; }
            q.tpx = q.tpy = q.clx = q.cly = q.bpp = 0;
            s_fq[s_fq_n++] = q;
        } else if (sel == 1 && s_pline_n < MAX_PLINE) {  /* gouraud motion-streak LINE_G (block_80057E7C) */
            /* EP0 = (X0=A&0xFFFF, Y0=A>>16, Z0=B&0xFFFF); the loop's VXb/VYb/VZb
             * already hold EP0's swizzle (GTE-Z/X/Y). EP1 = (X1=B>>16,
             * Y1=C&0xFFFF, Z1=C>>16). Same camera-rel /4 swizzle. */
            uint32_t C = cpu->read_word(pp + 0x0C);
            int X1 = (int)(B >> 16), Y1 = (int)(C & 0xFFFF), Z1 = (int)(C >> 16);
            projres e0, e1;
            proj2(R, ofx, ofy, H, (double)VYb, (double)VZb, (double)VXb, &e0);   /* EP0 */
            proj2(R, ofx, ofy, H, (double)(t8 - Y1), (double)(t9 - Z1), (double)(X1 - s7), &e1); /* EP1 */
            int sz3 = (int)(e0.m3 < 0 ? 0 : (e0.m3 > 65535 ? 65535 : e0.m3));
            if (sz3 == 0 || sz3 >= 8192) continue;       /* EP0 SZ window */
            if (screen_rect_cull(e0.sx, e0.sy)) continue; /* EP0 screen-rect */
            uint32_t col0 = cpu->read_word(pp + 0x10), col1 = cpu->read_word(pp + 0x14);
            int lod = (int)(col1 >> 24);
            if (((sz3 >> 5) - lod - 2) <= 0) continue;    /* EP0 depth-cue LOD (s3=2) */
            pline* l = &s_pline[s_pline_n++];
            l->ax=(float)e0.sx; l->ay=(float)e0.sy; l->bx=(float)e1.sx; l->by=(float)e1.sy;
            l->rgb0 = col0 & 0xFFFFFF; l->rgb1 = col1 & 0xFFFFFF;
            l->cz = (float)e0.m3;
            l->blend = ((col0 >> 24) & 0x02) ? 1 : 0;     /* code 0x50 → opaque */
        }
        /* sel 4 (LINE_G), 5 (affine matrix): absent across the whole 20k demo
         * (census: s4=s5=0) — no test target, deferred. */
    }
}

static void particle_compare(CPUState* cpu) {
    particle_build(cpu);
    int ft4 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x2C || s_prim[p].code == 0x2E) ft4++;
    int natft4 = 0; for (int q = 0; q < s_fq_n; q++) if (s_fq[q].sel != 0) natft4++;  /* sel 0 = non-FT4 */
    int n = natft4 < ft4 ? natft4 : ft4;
    double worst = 0, sum = 0; int nv = 0, matched = 0, over4 = 0, fqi = 0;
    for (int p = 0; p < s_prim_n && matched < n; p++) {
        prim_t* pr = &s_prim[p];
        if (!(pr->code == 0x2C || pr->code == 0x2E)) continue;
        while (fqi < s_fq_n && s_fq[fqi].sel == 0) fqi++;     /* skip sel-0 tile quads */
        if (fqi >= s_fq_n) break;
        fquad* q = &s_fq[fqi++];
        double qw = 0;
        for (int k = 0; k < 4; k++) {
            double dx = q->sx[k] - pr->x[k], dy = q->sy[k] - pr->y[k];
            double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
            sum += d; nv++; if (d > qw) qw = d;
        }
        if (qw > worst) worst = qw;
        if (qw > 4.0) over4++;
        matched++;
    }
    /* order-independent NN: each native FT4 quad -> nearest prim FT4 by centroid
     * (robust to a cull-boundary count mismatch that cascades the ordered A/B). */
    double nnsum = 0; int nnn = 0;
    for (int q = 0; q < s_fq_n; q++) {
        if (s_fq[q].sel == 0) continue;
        double qcx = (s_fq[q].sx[0]+s_fq[q].sx[1]+s_fq[q].sx[2]+s_fq[q].sx[3])/4.0;
        double qcy = (s_fq[q].sy[0]+s_fq[q].sy[1]+s_fq[q].sy[2]+s_fq[q].sy[3])/4.0;
        double best = 1e9;
        for (int p = 0; p < s_prim_n; p++) {
            if (!(s_prim[p].code==0x2C||s_prim[p].code==0x2E)) continue;
            double pcx=(s_prim[p].x[0]+s_prim[p].x[1]+s_prim[p].x[2]+s_prim[p].x[3])/4.0;
            double pcy=(s_prim[p].y[0]+s_prim[p].y[1]+s_prim[p].y[2]+s_prim[p].y[3])/4.0;
            double d=(qcx-pcx<0?pcx-qcx:qcx-pcx)+(qcy-pcy<0?pcy-qcy:qcy-pcy);
            if (d<best) best=d;
        }
        if (best<1e8) { nnsum+=best; nnn++; }
    }
    int ns2=0,ns3=0,ns6=0; for (int q=0;q<s_fq_n;q++){if(s_fq[q].sel==2)ns2++;else if(s_fq[q].sel==3)ns3++;else if(s_fq[q].sel>=6)ns6++;}
    if (getenv("OPENPETE_PARTICLE_DEBUG")) fprintf(stderr, "   [sel] nat sel2=%d sel3=%d sel6=%d\n", ns2, ns3, ns6);
    int nat_sel0 = 0; for (int q = 0; q < s_fq_n; q++) if (s_fq[q].sel == 0) nat_sel0++;
    /* sel-0 GEOMETRY (closes the count-only blind spot that hid the 1px-sliver
     * bug): the .s emits each sel-0 as a LINE_F (code 0x40), kept raw — vertex0
     * is raw[2] packed (Y<<16)|X. NN-match each native sel-0 quad anchor
     * (sx[0],sy[0]) to the nearest .s line vertex0 and report the distance. */
    int prim_line = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x40) prim_line++;
    double t0sum = 0, t0worst = 0; int t0n = 0, t0fpmis = 0;
    for (int q = 0; q < s_fq_n; q++) {
        if (s_fq[q].sel != 0) continue;
        double ax = s_fq[q].sx[0], ay = s_fq[q].sy[0], best = 1e9; int bp = -1;
        for (int p = 0; p < s_prim_n; p++) {
            if (s_prim[p].code != 0x40) continue;
            double lx = (double)(int16_t)(s_prim[p].raw[2] & 0xFFFF);
            double ly = (double)(int16_t)(s_prim[p].raw[2] >> 16);
            double d = (ax-lx<0?lx-ax:ax-lx) + (ay-ly<0?ly-ay:ay-ly);
            if (d < best) { best = d; bp = p; }
        }
        if (bp >= 0) {
            t0sum += best; if (best > t0worst) t0worst = best; t0n++;
            /* FOOTPRINT check — the 1px-sliver bug was a WIDTH error at the
             * correct position, which a position NN alone misses. Compare the
             * native quad's covered-pixel extent to the .s line's: line covers
             * pixels v0..v1 inclusive = |Δ|+1; the native quad covers sx[1]-sx[0]
             * (built as +2 to match the 2px line). */
            int v0x=(int16_t)(s_prim[bp].raw[2]&0xFFFF), v1x=(int16_t)(s_prim[bp].raw[3]&0xFFFF);
            int v0y=(int16_t)(s_prim[bp].raw[2]>>16),    v1y=(int16_t)(s_prim[bp].raw[3]>>16);
            int line_w=(v1x>v0x?v1x-v0x:v0x-v1x)+1, line_h=(v1y>v0y?v1y-v0y:v0y-v1y)+1;
            int nat_w=(int)(s_fq[q].sx[1]-s_fq[q].sx[0]+0.5), nat_h=(int)(s_fq[q].sy[2]-s_fq[q].sy[0]+0.5);
            if (nat_w != line_w || nat_h != line_h) t0fpmis++;
        }
    }
    /* sel-1 gouraud LINE_G (code 0x50) A/B: NN native streak endpoints -> nearest
     * .s 0x50 line endpoint (raw[2]=xy0, raw[4]=xy1, packed (Y<<16)|X). */
    int prim_gline = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x50) prim_gline++;
    double l1sum = 0, l1worst = 0; int l1n = 0;
    for (int t = 0; t < s_pline_n; t++) {
        double e[2][2] = { {s_pline[t].ax, s_pline[t].ay}, {s_pline[t].bx, s_pline[t].by} };
        for (int k = 0; k < 2; k++) {
            double best = 1e9;
            for (int p = 0; p < s_prim_n; p++) {
                if (s_prim[p].code != 0x50) continue;
                int idx[2] = {2, 4};
                for (int m = 0; m < 2; m++) {
                    double lx = (double)(int16_t)(s_prim[p].raw[idx[m]] & 0xFFFF);
                    double ly = (double)(int16_t)(s_prim[p].raw[idx[m]] >> 16);
                    double d = (e[k][0]-lx<0?lx-e[k][0]:e[k][0]-lx) + (e[k][1]-ly<0?ly-e[k][1]:e[k][1]-ly);
                    if (d < best) best = d;
                }
            }
            if (best < 1e8) { l1sum += best; if (best > l1worst) l1worst = best; l1n++; }
        }
    }
    fprintf(stderr, "[part-ab f=%u] FT4 native=%d prim=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx over4=%d/%d nnCtr=%.1f  sel0:nat=%d line=%d nn=%.1f/%.1f fpMis=%d  sel1:nat=%d gline=%d nn=%.1f/%.1f\n",
            openpete_frame, natft4, ft4, n, nv?sum/nv:0, worst, over4, n, nnn?nnsum/nnn:0,
            nat_sel0, prim_line, t0n?t0sum/t0n:0, t0worst, t0fpmis,
            s_pline_n, prim_gline, l1n?l1sum/l1n:0, l1worst);
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
    if (s_fq_n <= 0 && s_pline_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);   /* append after world + actors */
    int split[2][3] = { {0,1,2}, {1,3,2} };              /* FT4 -> 2 tris */
    /* sel-1 gouraud streak lines -> thin (1px-perp) gouraud quads */
    for (int i = 0; i < s_pline_n; i++) {
        pline* l = &s_pline[i];
        float dx = l->bx - l->ax, dy = l->by - l->ay;
        float len = sqrtf(dx*dx + dy*dy);
        float px = 0.5f, py = 0.0f;
        if (len > 1e-3f) { px = -dy/len*0.5f; py = dx/len*0.5f; }
        float cx[4] = { l->ax-px, l->ax+px, l->bx-px, l->bx+px };
        float cy[4] = { l->ay-py, l->ay+py, l->by-py, l->by+py };
        uint32_t cc[4] = { l->rgb0, l->rgb0, l->rgb1, l->rgb1 };  /* gouraud along the line */
        float depth = openpete_native_depth_from_viewz(l->cz);
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st;
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = cx[j]; st.v[k].sy = cy[j]; st.v[k].z = depth;
                st.v[k].u = 0.0f; st.v[k].v = 0.0f;
                st.v[k].rgba = (cc[j] & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = 0;
            st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
            st.blend = l->blend;
            openpete_native_scene_add_tri(&st);
        }
    }
    if (s_fq_n <= 0) return;
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

/* ---- glow rings (func_800580F4) — native float walk + feed + A/B ----------- *
 * Decoded from the recompiled C (generated/PSX.EXE_full.c block_800580F4). Each
 * active glow (D_80078800[16], stride 0x24) is a gouraud TRIANGLE FAN: a bright
 * centre (the glow colour) fading to BLACK at a perspective-scaled ring, drawn
 * ADDITIVE (the .s emits a 0xE1000220 draw-mode packet = ABR mode 1, then N-1
 * G3-STP tris, code 0x32). The ring template (unk_0x04, default D_8006E308) is a
 * static 17-point ellipse (96 wide x 60 tall — pre-compensated for tall PSX
 * pixels, so ~circular on the 4:3 display); unk_0x00 is the ring point count.
 *
 * Differences from the particle/sparkle swizzle: the glow uses RAW camera world
 * coords (NO >>2 — MobyPos is full-scale world units), and a per-glow LOD shift
 * that cancels out of SX/SY and persize, so the float walk skips it (the int16
 * floor it introduces is exactly what we're improving on). All gamestate-derived
 * (pool + static ring template + colour); determinism-safe by construction (the
 * .s ran headless; glows write no verdict byte). */
typedef struct {
    float sx[3], sy[3];      /* fan tri: v0=centre, v1=prev ring, v2=this ring */
    uint32_t rgb[3];         /* v0=glow colour, v1/v2=black (the fade) */
    float cz;                /* centre view-Z (depth) */
} gtri;
#define MAX_GTRI 4096
static gtri s_gtri[MAX_GTRI]; static int s_gtri_n;

static void glow_build(CPUState* cpu) {
    s_gtri_n = 0;
    int R[3][3]; load_cam_matrix(cpu, R);
    int camX = (int)cpu->read_word(G_CAMERA+0x28);    /* RAW world units (no >>2) */
    int camY = (int)cpu->read_word(G_CAMERA+0x2C);
    int camZ = (int)cpu->read_word(G_CAMERA+0x30);
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFF);

    for (int i = 0; i < 16 && s_gtri_n < MAX_GTRI; i++) {
        uint32_t g = D_GLOWS + (uint32_t)i*0x24;
        int count = (int)cpu->read_word(g + 0x00);    /* unk_0x00 = ring point count */
        if (count <= 0) continue;                     /* inactive slot */
        uint32_t mpp = cpu->read_word(g + 0x08);      /* MobyPos* */
        if (!mpp) continue;
        uint32_t tmpl = cpu->read_word(g + 0x04);     /* ring template (int32 x,y pairs) */
        if (!tmpl) continue;
        int px = (int)cpu->read_word(mpp+0) + (int)cpu->read_word(g + 0x14);  /* + PosOffset */
        int py = (int)cpu->read_word(mpp+4) + (int)cpu->read_word(g + 0x18);
        int pz = (int)cpu->read_word(mpp+8) + (int)cpu->read_word(g + 0x1C);
        /* swizzle like particles but in raw world units: GTE-X=camY-py,
         * GTE-Y=camZ-pz, GTE-Z=px-camX; TR=0 through the camera matrix. */
        projres c; proj2(R, ofx, ofy, H, (double)(camY-py), (double)(camZ-pz), (double)(px-camX), &c);
        double mac3 = c.m3;
        if (mac3 <= 0.0) continue;                    /* behind camera */
        int size = (int)cpu->read_word(g + 0x10);
        int bias = (int)cpu->read_word(g + 0x20);
        double depthidx = mac3 / 128.0 + bias;        /* the .s OT-depth cull (a3<=0) */
        if (depthidx <= 0.0) continue;
        double persize = (double)size * 4096.0 / mac3;/* IR0 = size*4096/Z */
        uint32_t color = cpu->read_word(g + 0x0C) & 0xFFFFFF;

        int nc = count > 64 ? 64 : count;
        double rx[64], ry[64];
        for (int j = 0; j < nc; j++) {
            int tx = (int)cpu->read_word(tmpl + (uint32_t)j*8 + 0);  /* template offset (int32) */
            int ty = (int)cpu->read_word(tmpl + (uint32_t)j*8 + 4);
            rx[j] = c.sx + persize * tx / 4096.0;     /* GPF: centre + (IR0*tmpl)>>12 */
            ry[j] = c.sy + persize * ty / 4096.0;
        }
        for (int j = 1; j < nc && s_gtri_n < MAX_GTRI; j++) {
            gtri* t = &s_gtri[s_gtri_n++];
            t->sx[0]=(float)c.sx;   t->sy[0]=(float)c.sy;   t->rgb[0]=color;   /* centre */
            t->sx[1]=(float)rx[j-1];t->sy[1]=(float)ry[j-1];t->rgb[1]=0;       /* prev ring */
            t->sx[2]=(float)rx[j];  t->sy[2]=(float)ry[j];  t->rgb[2]=0;       /* this ring */
            t->cz = (float)mac3;
        }
    }
}

/* Feed the native glow fan tris into the scene — untextured gouraud, ADDITIVE
 * (blend 2). Display-only; the .s produced any verdict state headless. */
static void glow_feed_native(CPUState* cpu) {
    glow_build(cpu);
    if (s_gtri_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_gtri_n; i++) {
        gtri* t = &s_gtri[i];
        float depth = openpete_native_depth_from_viewz(t->cz);
        openpete_scene_tri st;
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->sx[k]; st.v[k].sy = t->sy[k]; st.v[k].z = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 2;                                 /* ABR mode 1 (additive) */
        openpete_native_scene_add_tri(&st);
    }
}

/* A/B the native glow fan vs the captured G3-STP prims (code 0x32). Per-vertex
 * NN (order-independent) over all native fan verts -> nearest .s G3 vertex;
 * validates the centre projection AND the ring corners (persize + template). */
static void glow_compare(CPUState* cpu) {
    glow_build(cpu);
    int prim_g3 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x32) prim_g3++;
    double sum = 0, worst = 0; int nn = 0;
    for (int t = 0; t < s_gtri_n; t++) for (int k = 0; k < 3; k++) {
        double ax = s_gtri[t].sx[k], ay = s_gtri[t].sy[k], best = 1e9;
        for (int p = 0; p < s_prim_n; p++) {
            if (s_prim[p].code != 0x32) continue;
            for (int m = 0; m < 3; m++) {
                double dx = ax - s_prim[p].x[m], dy = ay - s_prim[p].y[m];
                double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
                if (d < best) best = d;
            }
        }
        if (best < 1e8) { sum += best; if (best > worst) worst = best; nn++; }
    }
    fprintf(stderr, "[glow-ab f=%u] native_tri=%d prim_g3=%d nn=%.2f worst=%.1fpx\n",
            openpete_frame, s_gtri_n, prim_g3, nn?sum/nn:0, worst);
}

/* ---- sparkle twinkles (func_800584C4) — native float walk + feed + A/B ----- *
 * Decoded from the recompiled C (block_800584C4). Each live sparkle (g_Sparkles[8],
 * stride 0x18) is a "twinkle cross" = TWO LINE_F prims (code 0x40, opaque white):
 * T0→T3 and T1→T2, the two diagonals. Tips (view space, VZ=0):
 *   T0=(-ec,-es) T1=(es,-ec) T2=(-es,ec) T3=(ec,es),
 *   ec=(scaledFade·cos[fade8])>>19, es=(scaledFade·sin[fade8])>>19,
 *   scaledFade=(256−fade8)·m_Fade, fade8=|m_Speed−newLife|·256/m_Speed.
 * They are transformed by a uniform matrix diag(a3,a3,a3), a3=max(centreMAC3,4096),
 * TR = centre·2 (the centre's view-space MAC), then projected.
 *
 * VERDICT-SAFE by construction: the `.s` ran headless (realfn) — it decremented
 * m_Life and killed off-screen/expired sparkles by writing m_Life=0. So we read
 * the pool AFTER the `.s` and **render exactly the sparkles with post-`.s`
 * m_Life>0** (which is `newLife` — the value the `.s` used for the fade), no
 * separate cull needed. The native lines are emitted as thin quads (the scene is
 * triangles-only), like the sel-0 line footprint. */
typedef struct {
    float ax, ay, bx, by;    /* line endpoints (centerline) */
    float cz;                /* centre view-Z (depth) */
    uint32_t rgb;            /* m_Color (flat) */
} sline;
#define MAX_SLINE 64
static sline s_sline[MAX_SLINE]; static int s_sline_n;

static void sparkle_build(CPUState* cpu) {
    s_sline_n = 0;
    int R[3][3]; load_cam_matrix(cpu, R);
    int camX = (int)cpu->read_word(G_CAMERA+0x28);   /* RAW; the .s shifts the diff >>2 */
    int camY = (int)cpu->read_word(G_CAMERA+0x2C);
    int camZ = (int)cpu->read_word(G_CAMERA+0x30);
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFF);

    for (int i = 0; i < 8 && s_sline_n + 2 <= MAX_SLINE; i++) {
        uint32_t sp = G_SPARKLES + (uint32_t)i*0x18;
        uint32_t w3 = cpu->read_word(sp + 0x0C);
        int newLife = (int)(w3 & 0xFF);              /* post-.s m_Life (== newLife) */
        if (newLife <= 0) continue;                  /* killed/expired by the .s → skip */
        int speed = (int)((w3 >> 8) & 0xFF);
        if (speed <= 0) continue;
        uint32_t w5 = cpu->read_word(sp + 0x14);
        int fade = (int)(w5 & 0xFF);                 /* m_Fade (+0x14) */
        uint32_t color = cpu->read_word(sp + 0x10) & 0xFFFFFF;
        int px = (int)cpu->read_word(sp + 0), py = (int)cpu->read_word(sp + 4), pz = (int)cpu->read_word(sp + 8);
        /* centre RTPS: swizzle (camera-relative >>2), TR=0, camera matrix */
        double VXg = (double)((camY - py) >> 2);     /* GTE-X */
        double VYg = (double)((camZ - pz) >> 2);     /* GTE-Y */
        double VZg = (double)((px - camX) >> 2);     /* GTE-Z */
        projres c; proj2(R, ofx, ofy, H, VXg, VYg, VZg, &c);
        if (c.m3 <= 0.0) continue;

        /* SCALE: fade8 = |m_Speed − newLife|·256/m_Speed → scaledFade pulses the
         * twinkle. ANGLE: indexed by the animated m_0x0E seed (+0xE), which the
         * .s increments ~3·dt/frame for a smooth spin (NOT fade8 — that only
         * scales). Read post-.s m_0x0E (the value the .s used + wrote). */
        int df = speed - newLife; if (df < 0) df = -df;
        int fade8 = speed ? (df * 256) / speed : 0;
        if (fade8 < 0) fade8 = 0; else if (fade8 > 255) fade8 = 255;
        double scaledFade = (double)(256 - fade8) * fade;
        int angIdx = (int)((w3 >> 16) & 0xFF);       /* m_0x0E (spin angle) */
        int sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx*2);
        int cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx*2 + 128);  /* cos = +0x80 */
        double ec = scaledFade * cosv / 524288.0;    /* >>19 */
        double es = scaledFade * sinv / 524288.0;
        /* tips view-space (VZ=0); transform diag(a3)+TR=2*centre, then project */
        double a3 = c.m3 > 4096.0 ? c.m3 : 4096.0;
        double TR0 = c.m1*2.0, TR1 = c.m2*2.0, TR2 = c.m3*2.0;
        double sz3 = TR2 < 0 ? 0 : (TR2 > 65535 ? 65535 : TR2);
        double qd = (sz3 <= 0.0 || H >= 2.0*sz3) ? GTE_DIV_CAP : (double)H/sz3;
        double T[4][2] = { {-ec,-es}, {es,-ec}, {-es,ec}, {ec,es} };
        double sx[4], sy[4];
        for (int k = 0; k < 4; k++) {
            double m1 = TR0 + a3 * T[k][0] / 4096.0;
            double m2 = TR1 + a3 * T[k][1] / 4096.0;
            sx[k] = clampSXY(ofx + clamp16(m1)*qd);
            sy[k] = clampSXY(ofy + clamp16(m2)*qd);
        }
        /* line 1 = T0→T3, line 2 = T1→T2 */
        sline* l0 = &s_sline[s_sline_n++];
        l0->ax=(float)sx[0]; l0->ay=(float)sy[0]; l0->bx=(float)sx[3]; l0->by=(float)sy[3];
        l0->cz=(float)c.m3; l0->rgb=color;
        sline* l1 = &s_sline[s_sline_n++];
        l1->ax=(float)sx[1]; l1->ay=(float)sy[1]; l1->bx=(float)sx[2]; l1->by=(float)sy[2];
        l1->cz=(float)c.m3; l1->rgb=color;
    }
}

/* Feed each sparkle line as a thin (1px-perp) opaque quad — the scene is
 * triangles-only, so LINE_F becomes a 2-tri ribbon along the centerline. */
static void sparkle_feed_native(CPUState* cpu) {
    sparkle_build(cpu);
    if (s_sline_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    int split[2][3] = { {0,1,2}, {1,3,2} };
    for (int i = 0; i < s_sline_n; i++) {
        sline* l = &s_sline[i];
        float dx = l->bx - l->ax, dy = l->by - l->ay;
        float len = sqrtf(dx*dx + dy*dy);
        float px = 0.5f, py = 0.0f;                  /* perpendicular ·0.5 (1px wide) */
        if (len > 1e-3f) { px = -dy/len*0.5f; py = dx/len*0.5f; }
        float cx[4] = { l->ax-px, l->ax+px, l->bx-px, l->bx+px };
        float cy[4] = { l->ay-py, l->ay+py, l->by-py, l->by+py };
        float depth = openpete_native_depth_from_viewz(l->cz);
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st;
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = cx[j]; st.v[k].sy = cy[j]; st.v[k].z = depth;
                st.v[k].u = 0.0f; st.v[k].v = 0.0f;
                st.v[k].rgba = (l->rgb & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = 0;
            st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
            st.blend = 0;                            /* LINE_F 0x40 = opaque */
            openpete_native_scene_add_tri(&st);
        }
    }
}

/* A/B native sparkle lines vs the captured LINE_F prims (code 0x40): per-vertex
 * NN over native endpoints ↔ nearest .s line endpoint (raw[2]/raw[3]). */
static void sparkle_compare(CPUState* cpu) {
    sparkle_build(cpu);
    int prim_line = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x40) prim_line++;
    double sum = 0, worst = 0; int nn = 0;
    for (int t = 0; t < s_sline_n; t++) {
        double e[2][2] = { {s_sline[t].ax, s_sline[t].ay}, {s_sline[t].bx, s_sline[t].by} };
        for (int k = 0; k < 2; k++) {
            double best = 1e9;
            for (int p = 0; p < s_prim_n; p++) {
                if (s_prim[p].code != 0x40) continue;
                for (int m = 2; m <= 3; m++) {       /* raw[2]=v0, raw[3]=v1 packed (Y<<16)|X */
                    double lx = (double)(int16_t)(s_prim[p].raw[m] & 0xFFFF);
                    double ly = (double)(int16_t)(s_prim[p].raw[m] >> 16);
                    double d = (e[k][0]-lx<0?lx-e[k][0]:e[k][0]-lx) + (e[k][1]-ly<0?ly-e[k][1]:e[k][1]-ly);
                    if (d < best) best = d;
                }
            }
            if (best < 1e8) { sum += best; if (best > worst) worst = best; nn++; }
        }
    }
    fprintf(stderr, "[sparkle-ab f=%u] native_line=%d prim_line=%d nn=%.2f worst=%.1fpx\n",
            openpete_frame, s_sline_n, prim_line, nn?sum/nn:0, worst);
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

    /* sel-class census (uncapped) — locate the rare/unimplemented classes (1/4/5)
     * so we have real test frames before decoding them. tag 'p' only. */
    if (tag[0] == 'p' && getenv("OPENPETE_PARTICLE_CENSUS")) {
        int sh[8] = {0}; uint32_t pp = s_part_pool;
        for (int i = 0; i < 320; i++, pp += 0x20) {
            uint32_t hdr = cpu->read_word(pp);
            int sel = (int)((hdr >> 8) & 0xFF);
            if (sel == 0xFF) break;
            if (sel & 0x80) continue;
            sh[sel < 7 ? sel : 7]++;
        }
        if (sh[1] || sh[4] || sh[5])
            fprintf(stderr, "[selhist f=%u] s0=%d s1=%d s2=%d s3=%d s4=%d s5=%d s6+=%d\n",
                    openpete_frame, sh[0], sh[1], sh[2], sh[3], sh[4], sh[5], sh[7]);
    }

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

    /* feed the native geometry into the scene: FT4 particles (tag 'p'), glow
     * rings (tag 'g'), or sparkle twinkles (tag 's'). */
    if (feed && endp > startp && render_path_enabled(RP_PARTICLES)) {
        if (tag[0] == 'p')      particle_feed_native(cpu);
        else if (tag[0] == 'g') glow_feed_native(cpu);
        else if (tag[0] == 's') sparkle_feed_native(cpu);
    }

    if (verify && endp > startp && openpete_frame >= verify_frame()) {
        static int s_dumped[3] = {0,0,0};
        int slot = (tag[0]=='p') ? 0 : (tag[0]=='g') ? 1 : 2;
        if (!s_dumped[slot]) {
            if (dump_pools_too) dump_pools(cpu);
            dump_prims(cpu, tag, startp, endp);
            s_dumped[slot] = 1;
        }
        if (slot == 0) {                                 /* particles: FT4 A/B */
            static int s_reports = 0;
            if (s_reports < 2000) { particle_compare(cpu); s_reports++; }
        } else if (slot == 1) {                          /* glows: ring fan A/B */
            static int s_greports = 0;
            if (s_greports < 2000) { glow_compare(cpu); s_greports++; }
        } else if (slot == 2) {                          /* sparkles: line A/B */
            static int s_sreports = 0;
            if (s_sreports < 2000) { sparkle_compare(cpu); s_sreports++; }
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
