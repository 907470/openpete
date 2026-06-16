// games/spyro-1/features/spyro_native.c — Phase 3b.4 of
// notes/plans/openpete-native-renderer.md: the SPYRO (player-avatar)
// renderer re-derived from gamestate in float + its numerical A/B harness.
//
// Spyro (func_80023AC4 / r_pete.s) is a self-contained ~2 KLOC handwritten GTE
// routine, distinct from the moby path in three ways the format-spike doc
// calls out:
//   1. DELTA-ACCUMULATED vertices — each vertex is a signed (dx,dy,dz) added to
//      a running accumulator, dual 1-byte / 2-byte encoding with a wide escape
//      (NOT moby's random-access packed XYZ).
//   2. THREE skeletal segments (body/head/tail), each its own view×rotation
//      matrix + seam trig.
//   3. Progress is u8 0..255 (NOT moby's 0..4096).
//
// The doc carries several UNCERTAINs (exact 2-byte field widths, the IR0
// progress scale, which LUT carries colour) that it explicitly says to pin by
// live A/B, not by transcribing the atan2-style asm. So this module starts as a
// CAPTURE + ONE-SHOT DUMP harness: hook the renderer, bracket the polybuf
// cursor, capture the prims it emits as ground truth, and dump g_Spyro state +
// the model/animation headers + the vertex streams + the prim packets. The
// float decoder is built and A/B-verified against this ground truth before any
// scene feed.
//
// Determinism: never writes PSX RAM. Calls the real renderer exactly once
// (behaviour-identical to the normal dispatch fall-through) and only READS
// state afterwards. Gated by OPENPETE_SPYRO_VERIFY (dump/A/B) or a native
// renderer mode (--renderer vk|ab|diff; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "psx_runtime.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "render_paths.h"  /* --render-paths isolation */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void     func_80023AC4(CPUState* cpu);  /* the real SPYRO renderer */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map) --------------------------------------- */
#define G_SPYRO       0x80078A58u   /* the player struct (r_pete.s $ra base)   */
#define G_SPYROFLAME  0x800786C8u   /* flame-attach matrix cache @ +0xB8       */
#define G_MODELS      0x80076378u   /* Model* g_Models[]; [0] = SPYRO_MODEL     */
#define G_CAMERA      0x80076DD0u   /* m_ProjectionMatrix +0x00, m_Position +0x28 */
#define D_VERT_LUT    0x8006D614u   /* 128-entry x4B packed-delta / colour LUT  */
#define D_COL_LUT2    0x8006D808u   /* adjacent LUT (colour? — pin via A/B)     */
#define D_SIN_TABLE   0x8006CBF8u   /* sin[idx]; cos at D_8006CC78 (s16 1.12)   */
#define D_COS_TABLE   0x8006CC78u
#define D_SCRATCH     0x8006FCF4u   /* g_SonyImage scratch (seg matrices +0x1900)*/
#define POLYBUF_CURSOR 0x800757B0u

/* ---- captured prims (ground truth) --------------------------------------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (prim[0x7])  */
    uint8_t  len;           /* OT tag length (prim[0]>>24)     */
    int      nv;            /* 3 or 4                          */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];
    int      nraw;
} prim_t;

#define MAX_PRIMS 8192
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
static int s_code_hist[256];

/* Walk the polybuf [start,end) by OT tag length (stride=((w0>>24)+1)*4); decode
 * each prim by its code byte (prim[0x7] == word[1]>>24) with the general PSX
 * polygon decode (bit4 gouraud / bit3 quad / bit2 textured). Same scheme the
 * moby harness validated — Spyro emits the same POLY_* family (F3/FT3/G3/GT3,
 * see the format notes). */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_gte_n = 0;
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

/* ---- one-shot dump (falsify-before-ship): pin the format notes vs live ---- */
static void dump_words(CPUState* cpu, const char* label, uint32_t base, int nwords) {
    fprintf(stderr, "  %s @%08X:\n   ", label, base);
    for (int i = 0; i < nwords; i++) {
        fprintf(stderr, "%08X ", cpu->read_word(base + (uint32_t)i*4));
        if ((i & 7) == 7 && i != nwords - 1) fprintf(stderr, "\n   ");
    }
    fprintf(stderr, "\n");
}

static const char* k_seg_name[3] = { "body", "head", "tail" };

static void spyro_dump(CPUState* cpu, uint32_t startp, uint32_t endp) {
    fprintf(stderr, "\n[spyro-dump f=%u] prims [%08X,%08X) %u bytes  OFX=%d OFY=%d H=%u\n",
            openpete_frame, startp, endp, endp - startp,
            (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
            cpu->gte_ctrl[26] & 0xFFFF);

    /* g_Spyro: position, anim/frame/progress bytes, colorFilter, flag, rotmat. */
    dump_words(cpu, "g_Spyro[0x00..0x50]", G_SPYRO, 0x14);
    fprintf(stderr,
        "  anim:  body=%02X/%02X head=%02X/%02X tail=%02X/%02X  (cur/next)\n"
        "  kf:    body A=%02X B=%02X  head A=%02X B=%02X  tail A=%02X B=%02X\n"
        "  prog:  body=%02X head=%02X tail=%02X  sortDepth=%02X\n"
        "  colorFilter@0x28=%08X  flag@0x30=%08X\n",
        cpu->read_byte(G_SPYRO+0x18), cpu->read_byte(G_SPYRO+0x19),
        cpu->read_byte(G_SPYRO+0x1A), cpu->read_byte(G_SPYRO+0x1B),
        cpu->read_byte(G_SPYRO+0x1C), cpu->read_byte(G_SPYRO+0x1D),
        cpu->read_byte(G_SPYRO+0x1E), cpu->read_byte(G_SPYRO+0x1F),
        cpu->read_byte(G_SPYRO+0x20), cpu->read_byte(G_SPYRO+0x21),
        cpu->read_byte(G_SPYRO+0x22), cpu->read_byte(G_SPYRO+0x23),
        cpu->read_byte(G_SPYRO+0x24), cpu->read_byte(G_SPYRO+0x25),
        cpu->read_byte(G_SPYRO+0x26), cpu->read_byte(G_SPYRO+0x27),
        cpu->read_word(G_SPYRO+0x28), cpu->read_word(G_SPYRO+0x30));

    /* SPYRO_MODEL = g_Models[0] | KUSEG. */
    uint32_t model = cpu->read_word(G_MODELS) | 0x80000000u;
    fprintf(stderr, "  SPYRO_MODEL=%08X  numAnims=%u\n", model, cpu->read_word(model) & 0xFFFF);
    dump_words(cpu, "model[0x00..0x40]", model, 0x10);

    /* Per-segment: anim header + frame A/B SpyroAnimationFrame + vertex stream. */
    static const int seg_anim_off[3] = { 0x18, 0x1A, 0x1C };  /* g_Spyro anim idx byte */
    static const int seg_frA_off[3]  = { 0x1E, 0x20, 0x22 };  /* keyframe A byte        */
    static const int seg_frB_off[3]  = { 0x1F, 0x21, 0x23 };  /* keyframe B byte        */
    static const int seg_prog_off[3] = { 0x24, 0x25, 0x26 };
    for (int s = 0; s < 3; s++) {
        uint32_t animIdx = cpu->read_byte(G_SPYRO + (uint32_t)seg_anim_off[s]);
        uint32_t frA     = cpu->read_byte(G_SPYRO + (uint32_t)seg_frA_off[s]);
        uint32_t frB     = cpu->read_byte(G_SPYRO + (uint32_t)seg_frB_off[s]);
        uint32_t prog    = cpu->read_byte(G_SPYRO + (uint32_t)seg_prog_off[s]);
        uint32_t animHdr = cpu->read_word(model + 0x38 + animIdx * 4);
        fprintf(stderr, "  --- seg %d (%s): animIdx=%u frA=%u frB=%u prog=%u animHdr=%08X\n",
                s, k_seg_name[s], animIdx, frA, frB, prog, animHdr);
        if (animHdr < 0x80000000u || animHdr >= 0x80800000u) { fprintf(stderr, "    (bad hdr ptr)\n"); continue; }
        dump_words(cpu, "animHdr[0x00..0x28]", animHdr, 0x0A);
        uint32_t numFrames = cpu->read_word(animHdr) & 0xFFFF;
        uint32_t vbase = cpu->read_word(animHdr + 0x10);  /* m_AnimationVertices */
        fprintf(stderr, "    numFrames=%u vertCntHi=%u vertCntLo=%u shift=%u depthScale=%u "
                "isSpyroAnim=%u scale=%u  m_AnimationVertices=%08X faces=%08X colors=%08X\n",
                numFrames,
                cpu->read_byte(animHdr + 0x08), cpu->read_byte(animHdr + 0x09),
                cpu->read_byte(animHdr + 0x06), cpu->read_byte(animHdr + 0x0B),
                cpu->read_byte(animHdr + 0x04), cpu->read_byte(animHdr + 0x05),
                vbase, cpu->read_word(animHdr + 0x14), cpu->read_word(animHdr + 0x18));
        /* SpyroAnimationFrame for A/B — try BOTH strides (4B and 8B) so the dump
         * resolves the doc's "Frames[0] 8B but lw 0x24(hdr+frame*4)" ambiguity. */
        fprintf(stderr, "    frame words @0x24 stride4: A=%08X B=%08X | stride8: A=%08X,%08X B=%08X,%08X\n",
                cpu->read_word(animHdr + 0x24 + frA * 4), cpu->read_word(animHdr + 0x24 + frB * 4),
                cpu->read_word(animHdr + 0x24 + frA * 8), cpu->read_word(animHdr + 0x24 + frA * 8 + 4),
                cpu->read_word(animHdr + 0x24 + frB * 8), cpu->read_word(animHdr + 0x24 + frB * 8 + 4));
        if (vbase >= 0x80000000u && vbase < 0x80800000u)
            dump_words(cpu, "m_AnimationVertices[0x00..0x20]", vbase, 0x08);
    }

    /* Live GTE rotation matrix (post-call) vs each segment's +0x10 slot — which
     * matrix does the body actually use? (R11R12/R13R21/R22R23/R31R32/R33.) */
    fprintf(stderr, "  gte_ctrl R[0..4] (post-call): %08X %08X %08X %08X %08X\n",
            cpu->gte_ctrl[0], cpu->gte_ctrl[1], cpu->gte_ctrl[2], cpu->gte_ctrl[3], cpu->gte_ctrl[4]);

    /* Segment matrix scratch block (D_SCRATCH+0x1900, stride 0x44). */
    for (int s = 0; s < 3; s++) {
        uint32_t e = D_SCRATCH + 0x1900u + (uint32_t)s * 0x44u;
        fprintf(stderr, "  seg-scratch[%d] @%08X:\n   ", s, e);
        for (int w = 0; w < 0x11; w++) fprintf(stderr, "%08X ", cpu->read_word(e + (uint32_t)w*4));
        fprintf(stderr, "\n");
    }

    /* Prim histogram + first prims. */
    char hb[300]; int hp = 0; hb[0] = 0;
    for (int c = 0; c < 256; c++) if (s_code_hist[c] && hp < (int)sizeof(hb)-16)
        hp += snprintf(hb+hp, sizeof(hb)-hp, " %02X=%d", c, s_code_hist[c]);
    fprintf(stderr, "  prim codes:%s  (total=%d)\n", hb, s_gte_n);
    int show = s_gte_n < 4 ? s_gte_n : 4;
    for (int i = 0; i < show; i++) {
        prim_t* t = &s_gte[i];
        fprintf(stderr, "   prim[%d] code=%02X len=%u nv=%d g=%d t=%d q=%d  raw:",
                i, t->code, t->len, t->nv, t->gouraud, t->textured, t->quad);
        for (int w = 0; w < t->nraw; w++) fprintf(stderr, " %08X", t->raw[w]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[spyro-dump] end\n\n");
}

/* ======================================================================== *
 *  NATIVE VERTEX DECODE + PROJECTION (the format notes)                *
 * ======================================================================== *
 * Spyro's geometry is delta-accumulated, dual-width, two-keyframe-blended.
 * Phase A (the .s) pre-builds, per segment, a scratch entry at
 * D_SCRATCH+0x1900 + seg*0x44:
 *   +0x00 frameData  (sign=matrix-present, 0x1000=A 2-byte, 0x2000=B 2-byte,
 *                     [11:0]=INTPL blend (1.12), [23:14]=vertex count)
 *   +0x04..0x0C TRX/TRY/TRZ          +0x10..0x20 R11R12..R33 (SHORTMATRIX)
 *   +0x24/0x28/0x2C  A: animVtx-word / 1-byte / 2-byte cursors
 *   +0x34/0x38/0x3C  B: same for the next keyframe
 * The matrix is sticky: only reloaded when frameData<0 (else reuse previous
 * segment's). We read these GTE inputs straight from the scratch block (the
 * apples-to-apples analog of moby 3a.1 reading the cull record) and reproduce
 * the running-accumulator decode + INTPL lerp + RTPS in float. Re-deriving the
 * segment matrices from gamestate (Phase A) is a later step. */

#define SEG_SCRATCH (D_SCRATCH + 0x1900u)
#define SEG_STRIDE  0x44u

static void read_matrix(CPUState* cpu, uint32_t p, int M[3][3]) {
    uint32_t w0 = cpu->read_word(p), w1 = cpu->read_word(p + 4);
    uint32_t w2 = cpu->read_word(p + 8), w3 = cpu->read_word(p + 12);
    uint32_t w4 = cpu->read_word(p + 16);
    M[0][0] = (int16_t)(w0 & 0xFFFF); M[0][1] = (int16_t)(w0 >> 16); M[0][2] = (int16_t)(w1 & 0xFFFF);
    M[1][0] = (int16_t)(w1 >> 16);    M[1][1] = (int16_t)(w2 & 0xFFFF); M[1][2] = (int16_t)(w2 >> 16);
    M[2][0] = (int16_t)(w3 & 0xFFFF); M[2][1] = (int16_t)(w3 >> 16);  M[2][2] = (int16_t)(w4 & 0xFFFF);
}

/* Float RTPS mirror WITH translation (gte.cpp gte_rtps_internal), reproducing
 * the GTE's near-plane behaviour in float. For a vertex close to the camera the
 * hardware caps the perspective divide: push_sz clamps SZ3 to [0,0xFFFF], and
 * gte_divide returns 0x1FFFF (≈2.0 in the 1.16 quotient) whenever H >= 2*SZ3
 * (or SZ3==0) instead of the true H/SZ3. The .s therefore pins a near vertex
 * near the screen centre+2*IR; using the *true* (much larger) 1/z instead made
 * such verts shoot off — the Spyro tail-tip "stretch to the screen edge" (demo
 * f19196-19200, the only residual in the 20k sweep: 18/12110 frames, all the
 * tail). Mirroring the cap (and the IR16 / ±0x400 SXY clamps) removes it while
 * leaving the 99.85% of in-range verts byte-identical (there q = H/SZ3 exactly,
 * the path that already matched to ~0.8px). The UNR reciprocal's sub-pixel
 * wobble is not reproduced (our true divide is strictly closer to ideal). */
#define GTE_DIV_CAP   (131071.0 / 65536.0)   /* 0x1FFFF as a 1.16 quotient (~2.0) */
#define GTE_SXY_BOUND 1024.0                  /* push_sxy signed-11-bit clamp (4:3) */
static double project(const int M[3][3], const int TR[3], double vx, double vy, double vz,
                      double ofx_px, double ofy_px, double H,
                      double* out_sx, double* out_sy, int* out_sat) {
    double mac1 = (double)TR[0] + (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    double mac2 = (double)TR[1] + (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    double mac3 = (double)TR[2] + (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
    /* IR1/IR2 = MAC saturated to int16 (the GTE projects from saturated IR). */
    double ir1 = mac1 < -32768.0 ? -32768.0 : (mac1 > 32767.0 ? 32767.0 : mac1);
    double ir2 = mac2 < -32768.0 ? -32768.0 : (mac2 > 32767.0 ? 32767.0 : mac2);
    /* SZ3 = MAC3 saturated to [0,0xFFFF]; divide caps to 0x1FFFF when near. */
    double sz3 = mac3 < 0.0 ? 0.0 : (mac3 > 65535.0 ? 65535.0 : mac3);
    int sat = (sz3 <= 0.0 || H >= 2.0 * sz3);
    double q = sat ? GTE_DIV_CAP : (H / sz3);
    double sx = ofx_px + ir1 * q;
    double sy = ofy_px + ir2 * q;
    if (sx < -GTE_SXY_BOUND) sx = -GTE_SXY_BOUND; else if (sx > GTE_SXY_BOUND - 1.0) sx = GTE_SXY_BOUND - 1.0;
    if (sy < -GTE_SXY_BOUND) sy = -GTE_SXY_BOUND; else if (sy > GTE_SXY_BOUND - 1.0) sy = GTE_SXY_BOUND - 1.0;
    *out_sx = sx;
    *out_sy = sy;
    if (out_sat) *out_sat = sat;
    return mac3;
}

/* MIPS `(v << lsh) >> rsh` (sll then arithmetic sra) reproduced exactly. */
static inline int32_t sra_field(int32_t v, int lsh, int rsh) {
    return (int32_t)((uint32_t)v << lsh) >> rsh;
}

typedef struct { float sx, sy, vz; int sat, seg; } spxy;
#define MAX_SPROJ 2048
static spxy s_sproj[MAX_SPROJ]; static int s_sproj_n;
static int s_lastseg_start, s_lastseg_n, s_lastseg_id;  /* last segment's s_sproj range */

/* Decode ONE vertex of ONE keyframe stream into the running accumulator
 * (*az/*ax/*ay), advancing whichever packed cursor (v0 1-byte / v1 2-byte) this
 * vertex uses. `corr` is the per-vertex m_AnimationVertices correction word
 * (fetched + advanced by the caller). Returns the NEXT vertex's width as a
 * boolean (1 = 2-byte): the width is PER-VERTEX, carried in a flag bit of the
 * current vertex — bit0 of the byte (1-byte form, .L800246C8 `andi $t0,$s1,0x1`)
 * or bit14 (0x4000) of the halfword (2-byte form, .L800247F0 `andi $t0,$s1,0x4000`,
 * set before the escape branch so it applies to escapes too). The first vertex's
 * width comes from frameData bit 0x1000 (A) / 0x2000 (B). This is the headline
 * fix: the stream interleaves 1-/2-byte verts and a fixed per-segment width
 * desyncs the prefix-sum at the first switch. X/Y deltas subtract, Z adds. */
static int spyro_decode_vertex(CPUState* cpu, int is2, int32_t corr,
                               uint32_t* v0, uint32_t* v1, int* az, int* ax, int* ay) {
    if (!is2) {                                      /* 1-byte form */
        uint32_t b = cpu->read_byte(*v0); *v0 += 1;
        int32_t d = (int32_t)cpu->read_word(D_VERT_LUT + ((b << 1) & 0x1FCu)) + corr;
        *az += sra_field(d,0,21); *ax -= sra_field(d,11,21); *ay -= sra_field(d,22,21);
        return (b & 1u) ? 1 : 0;                     /* bit0 -> next vertex width */
    }
    int16_t h = (int16_t)cpu->read_half(*v1); *v1 += 2;  /* 2-byte form */
    int next2 = ((uint16_t)h & 0x4000u) ? 1 : 0;     /* bit14 -> next vertex width */
    if (h < 0) {                                     /* wide escape (absolute set, no corr) */
        uint32_t s2 = cpu->read_half(*v1); *v1 += 2;
        *az = sra_field((int32_t)h,18,21);
        *ax = -sra_field((int32_t)((s2 << 12) | (((uint32_t)(int32_t)h) << 28)),0,21);
        *ay = -sra_field((int32_t)(s2 << 22),0,21);
    } else {                                         /* h-split + corr-split */
        *az += sra_field((int32_t)h,18,25); *ax -= sra_field((int32_t)h,23,25); *ay -= sra_field((int32_t)h,28,25);
        *az += sra_field(corr,0,21); *ax -= sra_field(corr,11,21); *ay -= sra_field(corr,22,21);
    }
    return next2;
}

/* Decode one segment's model-space verts (running accumulator + INTPL lerp),
 * project each, append to s_sproj. Returns verts emitted. Mirrors the four
 * width combos of .L800244D8/.L80024580/.L800246B4/.L800247D8 + wide escapes,
 * with per-vertex width switching (see spyro_decode_vertex). */
static int spyro_project_segment(CPUState* cpu, uint32_t entry, const int M[3][3], const int TR[3],
                                 double ofx, double ofy, double H, int seg, int count) {
    uint32_t fd = cpu->read_word(entry + 0x00);
    int a2 = (fd & 0x1000u) != 0;           /* current frame-A width (per-vertex) */
    int b2 = (fd & 0x2000u) != 0;           /* current frame-B width (per-vertex) */
    int dual = (fd & 0xFFFu) != 0;          /* s7 != 0 -> two-keyframe INTPL */
    double blend = (double)(fd & 0xFFFu) / 4096.0;
    uint32_t atA = cpu->read_word(entry + 0x24), v0A = cpu->read_word(entry + 0x28), v1A = cpu->read_word(entry + 0x2C);
    uint32_t atB = cpu->read_word(entry + 0x34), v0B = cpu->read_word(entry + 0x38), v1B = cpu->read_word(entry + 0x3C);
    int aZ=0,aX=0,aY=0;   /* frame-A accumulator (Z added, X/Y subtracted) */
    int bZ=0,bX=0,bY=0;   /* frame-B accumulator */
    int emitted = 0;
    for (int i = 0; i < count && s_sproj_n < MAX_SPROJ; i++) {
        /* ---- frame A vertex (corr + cursor advance happen every vertex) ---- */
        int32_t corrA = (int32_t)cpu->read_word(atA); atA += 4;
        a2 = spyro_decode_vertex(cpu, a2, corrA, &v0A, &v1A, &aZ, &aX, &aY);
        double VX = aX, VY = aY, VZ = aZ;
        /* ---- frame B vertex (only if dual) ---- */
        if (dual) {
            int32_t corrB = (int32_t)cpu->read_word(atB); atB += 4;
            b2 = spyro_decode_vertex(cpu, b2, corrB, &v0B, &v1B, &bZ, &bX, &bY);
            VX = (double)aX + blend * (bX - aX);
            VY = (double)aY + blend * (bY - aY);
            VZ = (double)aZ + blend * (bZ - aZ);
        }
        double sx, sy; int sat;
        double vz = project(M, TR, VX, VY, VZ, ofx, ofy, H, &sx, &sy, &sat);
        s_sproj[s_sproj_n].sx = (float)sx; s_sproj[s_sproj_n].sy = (float)sy;
        s_sproj[s_sproj_n].vz = (float)vz; s_sproj[s_sproj_n].sat = sat; s_sproj[s_sproj_n].seg = seg;
        s_sproj_n++; emitted++;
    }
    return emitted;
}

/* Walk the 3 segment scratch entries, decode + project all verts to s_sproj. */
static int spyro_project_all(CPUState* cpu) {
    s_sproj_n = 0;
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFF);
    int M[3][3]; int TR[3]; int have_mat = 0;
    int nseg = 0;
    int prev_end = 0;                                /* cumulative scratchpad byte offset */
    for (int s = 0; s < 3; s++) {
        uint32_t entry = SEG_SCRATCH + (uint32_t)s * SEG_STRIDE;
        uint32_t fd = cpu->read_word(entry + 0x00);
        if (fd == 0) break;                          /* terminator */
        /* [23:14] is the CUMULATIVE scratchpad end byte-offset (fp runs
         * continuously across segments — it does NOT reset). This segment's
         * vertex count = (this_end - prev_end) / 4. */
        int cur_end = (int)((fd >> 14) & 0x3FFu);
        int count = (cur_end - prev_end) >> 2; if (count < 0) count = 0;
        prev_end = cur_end;
        /* Matrix reload keys on the frameData SIGN BIT, exactly like the .s:
         * `bgez $s1, .L80024478` SKIPS the ctc2 reload when the sign bit is
         * CLEAR, so the matrix is STICKY — the segment reuses the previous one.
         * The first segment always takes the body matrix: the .s decode loop
         * inherits whatever Phase A left in the GTE, which is the body's matrix
         * == seg0's +0x10 slot (confirmed: post-call gte_ctrl R[0..4] == seg0
         * +0x10 on every captured frame). NOTE: keying on "+0x10 != 0" is WRONG
         * — Phase A can leave non-zero scratch matrices in the head/tail +0x10
         * slots with a CLEAR sign bit (the .s ignores them, staying sticky);
         * adopting them projected head/tail with a stale matrix (f4549: head
         * vanishes, tail lands at the front of the body). */
        if (s == 0 || (fd & 0x80000000u)) { read_matrix(cpu, entry + 0x10, M); have_mat = 1; }
        TR[0] = (int32_t)cpu->read_word(entry + 0x04);
        TR[1] = (int32_t)cpu->read_word(entry + 0x08);
        TR[2] = (int32_t)cpu->read_word(entry + 0x0C);
        if (!have_mat) continue;                     /* no matrix yet (shouldn't happen for seg0) */
        s_lastseg_start = s_sproj_n;
        s_lastseg_n = spyro_project_segment(cpu, entry, M, TR, ofx, ofy, H, s, count);
        s_lastseg_id = s;
        nseg++;
    }
    return nseg;
}

/* ---- geometry A/B: native projected verts vs the captured prim verts ----
 * NN-match each prim vertex to the nearest native s_sproj (the moby-3a.1
 * instrument): a tight match proves the decode + projection reproduce the GTE.
 * prim->native direction so an over-decode (count too high) can't inflate. */
static void spyro_compare_geometry(void) {
    if (!s_sproj_n || !s_gte_n) { fprintf(stderr, "[spyro-verify f=%u] no data (proj=%d prim=%d)\n",
                                          openpete_frame, s_sproj_n, s_gte_n); return; }
    int total = 0, hit4 = 0, hit2 = 0; double errsum = 0; float emax = 0;
    for (int i = 0; i < s_gte_n; i++) {
        prim_t* p = &s_gte[i];
        for (int k = 0; k < p->nv; k++) {
            float bestd = 1e9f;
            for (int j = 0; j < s_sproj_n; j++) {
                float dx = p->x[k] - s_sproj[j].sx, dy = p->y[k] - s_sproj[j].sy;
                float d = dx*dx + dy*dy;
                if (d < bestd) bestd = d;
            }
            float dist = sqrtf(bestd);
            total++; errsum += dist; if (dist > emax) emax = dist;
            if (dist <= 4.0f) hit4++; if (dist <= 2.0f) hit2++;
        }
    }
    fprintf(stderr, "[spyro-verify f=%u] proj=%d primVerts=%d  match<=4px=%.1f%% <=2px=%.1f%%  mean=%.2fpx max=%.1fpx\n",
            openpete_frame, s_sproj_n, total,
            100.0*hit4/total, 100.0*hit2/total, errsum/total, emax);
}

/* ---- CLEAN per-vertex A/B vs the .s scratchpad SXY (no NN) ----------------
 * fp runs continuously across segments, so post-call scratchpad 0x1F800000
 * holds ALL verts in order: body[0..], head[..], tail[..] (238*4 < 1KB). Our
 * s_sproj is in the same order, so s_sproj[j] <-> scratchpad[j]. Direct compare
 * isolates a per-axis decode error with zero NN ambiguity / back-face noise. */
#define SCRATCHPAD 0x1F800000u
/* match% (<=4px) of native vert j vs scratchpad[j+off], per segment + overall */
static void spyro_scratchpad_at(CPUState* cpu, int off) {
    int n = s_sproj_n, hit4 = 0, tot = 0; double errsum = 0;
    int seg_hit[3]={0,0,0}, seg_tot[3]={0,0,0};
    int worst_j = -1; float worst_d = 0; float worst_gx=0, worst_gy=0;
    for (int j = 0; j < n; j++) {
        int k = j + off; if (k < 0 || k >= n) continue;
        uint32_t w = cpu->read_word(SCRATCHPAD + (uint32_t)k * 4);
        float gx = (float)(int16_t)(w & 0xFFFF), gy = (float)(int16_t)(w >> 16);
        float dx = s_sproj[j].sx - gx, dy = s_sproj[j].sy - gy, d = sqrtf(dx*dx + dy*dy);
        tot++; errsum += d; if (d <= 4.0f) hit4++;
        int sg = s_sproj[j].seg; if (sg>=0&&sg<3){ seg_tot[sg]++; if(d<=4.0f) seg_hit[sg]++; }
        if (d > worst_d) { worst_d = d; worst_j = j; worst_gx = gx; worst_gy = gy; }
    }
    if (worst_j >= 0 && worst_d > 8.0f)  /* outlier: native vs .s, with view-Z */
        fprintf(stderr, "  [outlier f=%u] j=%d seg=%d err=%.1fpx  native=(%.1f,%.1f) .s=(%.0f,%.0f) vz=%.1f sat=%d\n",
                openpete_frame, worst_j, s_sproj[worst_j].seg, worst_d,
                s_sproj[worst_j].sx, s_sproj[worst_j].sy, worst_gx, worst_gy,
                s_sproj[worst_j].vz, s_sproj[worst_j].sat);
    fprintf(stderr, "[spyro-scratch f=%u off=%+d] <=4px=%.1f%% mean=%.2fpx | body=%.0f%% head=%.0f%% tail=%.0f%%\n",
            openpete_frame, off, tot?100.0*hit4/tot:0, tot?errsum/tot:0,
            seg_tot[0]?100.0*seg_hit[0]/seg_tot[0]:0, seg_tot[1]?100.0*seg_hit[1]/seg_tot[1]:0,
            seg_tot[2]?100.0*seg_hit[2]/seg_tot[2]:0);
}
static void spyro_scratchpad_ab(CPUState* cpu) {
    if (s_sproj_n <= 0) return;
    /* Per-segment frameData + distinct-matrix flag (non-zero +0x10 slot). The
     * head "turning" => its own seam matrix => +0x10 non-zero / sign bit set;
     * surface it so head-merge frames are identifiable. */
    for (int s = 0; s < 3; s++) {
        uint32_t e = SEG_SCRATCH + (uint32_t)s * SEG_STRIDE;
        uint32_t fd = cpu->read_word(e);
        if (fd == 0) break;
        uint32_t m0 = cpu->read_word(e+0x10)|cpu->read_word(e+0x14)|cpu->read_word(e+0x18)
                     |cpu->read_word(e+0x1C)|cpu->read_word(e+0x20);
        fprintf(stderr, "  (%s fd=%08X blend=%u dual=%d A%s B%s mat=%s sign=%d)\n",
                k_seg_name[s], fd, fd & 0xFFF, (fd & 0xFFF) != 0,
                (fd & 0x1000) ? "2" : "1", (fd & 0x2000) ? "2" : "1",
                m0 ? "own" : "sticky", (int)(fd >> 31));
    }
    spyro_scratchpad_at(cpu, 0);
}

/* ---- full-run aggregate (20k verdict) ------------------------------------
 * Cheap per-frame (O(verts)) per-index match vs the .s scratchpad, accumulated
 * across the WHOLE run so a 20k/4-level demo gets a single verdict without 80k
 * log lines or the O(n^2) NN/idx cost. Tracks total Spyro-render frames, frames
 * with any segment <100% (≤4px), the worst single segment-match and its frame,
 * and the max per-vertex error. Prints a running line every 2000 frames + the
 * cumulative verdict, so the last line near the run end is the answer. */
static struct {
    long frames, bad_frames;
    double worst_match; unsigned worst_frame; int worst_seg;
    double max_err; unsigned max_err_frame;
} s_agg = { 0, 0, 100.0, 0, -1, 0.0, 0 };

static void spyro_agg_final(void) {
    fprintf(stderr, "[spyro-agg FINAL] frames=%ld imperfect=%ld (%.2f%%)  worst=%.1f%% (seg %d @f%u)  maxerr=%.1fpx @f%u  => %s\n",
            s_agg.frames, s_agg.bad_frames,
            s_agg.frames ? 100.0 * s_agg.bad_frames / s_agg.frames : 0.0,
            s_agg.worst_match, s_agg.worst_seg, s_agg.worst_frame,
            s_agg.max_err, s_agg.max_err_frame,
            s_agg.bad_frames == 0 ? "PASS (100% all segments, all frames)" : "RESIDUAL");
}

static void spyro_aggregate(CPUState* cpu) {
    static int s_reg = 0;
    if (!s_reg) { s_reg = 1; atexit(spyro_agg_final); }
    int seg_hit[3] = {0,0,0}, seg_tot[3] = {0,0,0};
    double fmax = 0.0; int worst_j = -1; float wgx = 0, wgy = 0;
    for (int j = 0; j < s_sproj_n; j++) {
        uint32_t w = cpu->read_word(SCRATCHPAD + (uint32_t)j * 4);
        float gx = (float)(int16_t)(w & 0xFFFF), gy = (float)(int16_t)(w >> 16);
        float dx = s_sproj[j].sx - gx, dy = s_sproj[j].sy - gy, d = sqrtf(dx*dx + dy*dy);
        int sg = s_sproj[j].seg; if (sg>=0&&sg<3){ seg_tot[sg]++; if(d<=4.0f) seg_hit[sg]++; }
        if (d > fmax) { fmax = d; worst_j = j; wgx = gx; wgy = gy; }
    }
    if (fmax > 8.0f && worst_j >= 0)  /* outlier: capture vert + view-Z wherever it occurs in the full run */
        fprintf(stderr, "  [outlier f=%u] j=%d seg=%d err=%.1fpx native=(%.1f,%.1f) .s=(%.0f,%.0f) vz=%.1f sat=%d "
                "(n=%d)\n", openpete_frame, worst_j, s_sproj[worst_j].seg, fmax,
                s_sproj[worst_j].sx, s_sproj[worst_j].sy, wgx, wgy,
                s_sproj[worst_j].vz, s_sproj[worst_j].sat, s_sproj_n);
    s_agg.frames++;
    int bad = 0;
    for (int sg = 0; sg < 3; sg++) {
        if (!seg_tot[sg]) continue;
        double m = 100.0 * seg_hit[sg] / seg_tot[sg];
        if (m < 100.0) bad = 1;
        if (m < s_agg.worst_match) { s_agg.worst_match = m; s_agg.worst_frame = openpete_frame; s_agg.worst_seg = sg; }
    }
    if (bad) s_agg.bad_frames++;
    if (fmax > s_agg.max_err) { s_agg.max_err = fmax; s_agg.max_err_frame = openpete_frame; }
    if ((s_agg.frames % 2000) == 0)
        fprintf(stderr, "[spyro-agg f=%u] frames=%ld imperfect=%ld  worst=%.1f%% (seg %d @f%u)  maxerr=%.1fpx @f%u\n",
                openpete_frame, s_agg.frames, s_agg.bad_frames, s_agg.worst_match,
                s_agg.worst_seg, s_agg.worst_frame, s_agg.max_err, s_agg.max_err_frame);
}

/* ---- scene feed (3b.4 scaffold) ------------------------------------------
 * Native float verts (s_sproj) carry the geometry; the captured prims carry
 * topology + per-vertex colour + texture (Tiledef). Each prim vertex is mapped
 * to its native vertex by EXACT INDEX RECOVERY: the .s wrote every vertex's
 * screen SXY to scratchpad 0x1F800000 in stream order (== s_sproj order), and
 * the prim packet's SXY is a byte-identical copy of that same RTPS output, so
 * `prim SXY == scratchpad[index]` recovers the .s's own vertex index exactly.
 * We then feed s_sproj[index] (native sub-pixel float). This replaces the old
 * screen-distance nearest-neighbour match, which grabbed ACROSS segments when
 * the head overlapped the body on screen (head triangles attaching to body
 * verts -> the "head merges with body" artifact): NN picks the closest vertex,
 * which need not be the one the face references. Index recovery is segment-safe
 * by construction. (The independent face-stream walk — the moby-3a.2b analog —
 * supersedes this scaffold entirely later.) */
static int spyro_nn(float px, float py) {
    int best = -1; float bd = 1e18f;
    for (int j = 0; j < s_sproj_n; j++) {
        float dx = px - s_sproj[j].sx, dy = py - s_sproj[j].sy, d = dx*dx + dy*dy;
        if (d < bd) { bd = d; best = j; }
    }
    return best;
}
/* Recover a prim vertex's stream index via exact SXY match vs the .s scratchpad.
 * Returns -1 if no exact match (caller falls back to NN). */
static int spyro_index_of(CPUState* cpu, int px, int py) {
    uint32_t want = ((uint32_t)(py & 0xFFFF) << 16) | (uint32_t)(px & 0xFFFF);
    for (int j = 0; j < s_sproj_n; j++)
        if (cpu->read_word(SCRATCHPAD + (uint32_t)j * 4) == want) return j;
    return -1;
}
/* Headless falsification: how often would screen-distance NN pick a DIFFERENT
 * vertex than exact index recovery? (Runs under OPENPETE_SPYRO_VERIFY, where the
 * scene feed is unavailable.) A high % means NN was mis-attaching faces — the
 * head-merge cause. */
static void spyro_idx_audit(CPUState* cpu) {
    long exact = 0, disagree = 0, fallback = 0, total = 0;
    for (int i = 0; i < s_gte_n; i++) {
        prim_t* p = &s_gte[i];
        if (p->nv < 3) continue;
        for (int k = 0; k < p->nv; k++) {
            int ex = spyro_index_of(cpu, (int)p->x[k], (int)p->y[k]);
            total++;
            if (ex < 0) { fallback++; continue; }
            exact++;
            if (ex != spyro_nn(p->x[k], p->y[k])) disagree++;
        }
    }
    fprintf(stderr, "[spyro-idx f=%u] exact=%ld/%ld fallback=%ld  NN-would-disagree=%ld (%.1f%%)\n",
            openpete_frame, exact, total, fallback, disagree,
            total ? 100.0 * disagree / total : 0.0);
}
static void spyro_feed_scaffold(CPUState* cpu) {
    static int s_logged = 0;
    if (s_sproj_n <= 0 || s_gte_n <= 0) return;
    /* frame-keyed: no-op if the cyclorama/mobys already began this frame (we
     * append after them), clears if Spyro is somehow the first feeder. */
    openpete_native_scene_begin_frame(openpete_frame);
    int fed = 0;
    long exact = 0, nn_disagree = 0, fallback = 0, total = 0;  /* falsification */
    for (int i = 0; i < s_gte_n; i++) {
        prim_t* p = &s_gte[i];
        if (p->nv < 3) continue;
        int tpx=0,tpy=0,clx=0,cly=0,bpp=16;
        if (p->textured) {
            unsigned tp=(unsigned)p->tpage, cl=(unsigned)p->clut;
            tpx=(int)(tp&0xFu)*64; tpy=(int)((tp>>4)&1u)*256;
            int mode=(int)((tp>>7)&3u); bpp=(mode==0)?4:(mode==1)?8:16;
            clx=(int)(cl&0x3Fu)*16; cly=(int)((cl>>6)&0x1FFu);
        }
        int blend = (p->code & 0x2) ? 1 : 0;   /* ABE (semi-transparent) bit */
        int idx[4];
        for (int k = 0; k < p->nv; k++) {
            int ex = spyro_index_of(cpu, (int)p->x[k], (int)p->y[k]);
            total++;
            if (ex >= 0) {
                exact++;
                if (ex != spyro_nn(p->x[k], p->y[k])) nn_disagree++;  /* NN would have erred */
                idx[k] = ex;
            } else {
                fallback++;
                idx[k] = spyro_nn(p->x[k], p->y[k]);
            }
        }
        int tris[2][3], nt;
        if (p->nv == 3) { tris[0][0]=0;tris[0][1]=1;tris[0][2]=2; nt=1; }
        else            { tris[0][0]=0;tris[0][1]=1;tris[0][2]=2;
                          tris[1][0]=1;tris[1][1]=3;tris[1][2]=2; nt=2; }  /* PSX quad split */
        for (int t = 0; t < nt; t++) {
            openpete_scene_tri st;
            int ok = 1;
            for (int k = 0; k < 3; k++) {
                int vk = tris[t][k], sj = idx[vk];
                if (sj < 0) { ok = 0; break; }
                st.v[k].sx = s_sproj[sj].sx; st.v[k].sy = s_sproj[sj].sy;
                st.v[k].z  = openpete_native_depth_from_viewz(s_sproj[sj].vz);
                st.v[k].u  = (float)p->u[vk]; st.v[k].v = (float)p->v[vk];
                st.v[k].rgba = p->rgb[vk] | 0xFF000000u;
            }
            if (!ok) continue;
            st.textured = p->textured; st.tpage_x = tpx; st.tpage_y = tpy;
            st.clut_x = clx; st.clut_y = cly; st.bpp = bpp; st.blend = blend;
            openpete_native_scene_add_tri(&st); fed++;
        }
    }
    if (s_logged < 6 && fed > 0) {
        s_logged++;
        fprintf(stderr, "[spyro-native f=%u] %d tris  idx-recover: exact=%ld/%ld fallback=%ld  "
                "NN-would-disagree=%ld (%.1f%%)\n",
                openpete_frame, fed, exact, total, fallback,
                nn_disagree, total ? 100.0 * nn_disagree / total : 0.0);
    }
}

/* ---- gating --------------------------------------------------------------- */
static int spyro_verify_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SPYRO_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static unsigned spyro_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_SPYRO_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

static int on_80023AC4_spyro_native(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = spyro_verify_enabled();
    int feed   = openpete_native_render_enabled();
    /* --render-paths also needs the bracket so the OT filter knows this path's
     * polybuf range, even when not feeding/verifying. */
    if (!verify && !feed && !render_paths_active()) return 0;  /* inert: normal dispatch runs it */
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_80023AC4(cpu);                          /* run the real renderer */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    render_path_record(RP_SPYRO, startp, endp);   /* polybuf range for the OT filter */

    if (feed && endp > startp && render_path_enabled(RP_SPYRO)) {  /* native feed (if selected) */
        capture_prims(cpu, startp, endp);         /* topology + colour + texture */
        spyro_project_all(cpu);                   /* native float verts -> s_sproj */
        spyro_feed_scaffold(cpu);                 /* index-recovered native verts + prim topology */
    }

    if (verify) {
        static int s_dumped = 0, s_reports = 0;
        if (openpete_frame >= spyro_verify_frame() && endp > startp) {
            capture_prims(cpu, startp, endp);
            if (!s_dumped) { spyro_dump(cpu, startp, endp); s_dumped = 1; }
            spyro_project_all(cpu);                  /* native decode + project -> s_sproj */
            if (s_reports < 200) {                   /* detailed spot-check (first 200 frames) */
                spyro_compare_geometry();            /* prim-NN A/B (geometry verdict) */
                spyro_scratchpad_ab(cpu);            /* clean per-vertex A/B vs scratchpad */
                spyro_idx_audit(cpu);                /* index-recovery vs NN disagreement */
                s_reports++;
            }
            spyro_aggregate(cpu);                    /* cheap per-frame full-run (20k) verdict */
        }
    }
    s_in = 0;
    return 1;
}

static const feature_dispatch_entry_t spyro_dispatch[] = {
    { 0x80023AC4u, on_80023AC4_spyro_native },
    { 0u, NULL },
};

const feature_module_t spyro_native_module = {
    .name           = "spyro_native",
    .apply_options  = NULL,
    .dispatch_table = spyro_dispatch,
};
