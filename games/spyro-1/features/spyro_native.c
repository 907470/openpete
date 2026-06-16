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
// state afterwards. Gated by OPENPETE_SPYRO_VERIFY (dump/A/B) or --native-ab
// (scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "psx_runtime.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"

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

/* Float RTPS mirror WITH translation (gte.cpp gte_rtps_internal). */
static double project(const int M[3][3], const int TR[3], double vx, double vy, double vz,
                      double ofx_px, double ofy_px, double H,
                      double* out_sx, double* out_sy, int* out_sat) {
    double mac1 = (double)TR[0] + (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    double mac2 = (double)TR[1] + (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    double mac3 = (double)TR[2] + (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
    double z = (mac3 != 0.0) ? mac3 : 1.0;
    *out_sx = ofx_px + mac1 * H / z;
    *out_sy = ofy_px + mac2 * H / z;
    if (out_sat) *out_sat = (mac3 <= 0.0 || H >= 2.0 * mac3);
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

/* Decode one segment's model-space verts (running accumulator + INTPL lerp),
 * project each, append to s_sproj. Returns verts emitted. Mirrors the four
 * width combos of .L800244D8/.L80024580/.L800246B4/.L800247D8 + wide escapes. */
static int spyro_project_segment(CPUState* cpu, uint32_t entry, const int M[3][3], const int TR[3],
                                 double ofx, double ofy, double H, int seg, int count) {
    uint32_t fd = cpu->read_word(entry + 0x00);
    int a2 = (fd & 0x1000u) != 0;
    int b2 = (fd & 0x2000u) != 0;
    int dual = (fd & 0xFFFu) != 0;          /* s7 != 0 -> two-keyframe INTPL */
    double blend = (double)(fd & 0xFFFu) / 4096.0;
    uint32_t atA = cpu->read_word(entry + 0x24), v0A = cpu->read_word(entry + 0x28), v1A = cpu->read_word(entry + 0x2C);
    uint32_t atB = cpu->read_word(entry + 0x34), v0B = cpu->read_word(entry + 0x38), v1B = cpu->read_word(entry + 0x3C);
    int aZ=0,aX=0,aY=0;   /* frame-A accumulator (Z added, X/Y subtracted) */
    int bZ=0,bX=0,bY=0;   /* frame-B accumulator */
    int emitted = 0;
    for (int i = 0; i < count && s_sproj_n < MAX_SPROJ; i++) {
        /* ---- frame A vertex ---- */
        int32_t corrA = (int32_t)cpu->read_word(atA); atA += 4;
        if (!a2) {                                   /* A 1-byte */
            uint32_t b = cpu->read_byte(v0A); v0A += 1;
            int32_t d = (int32_t)cpu->read_word(D_VERT_LUT + ((b << 1) & 0x1FCu)) + corrA;
            aZ += sra_field(d,0,21); aX -= sra_field(d,11,21); aY -= sra_field(d,22,21);
        } else {                                     /* A 2-byte */
            int16_t h = (int16_t)cpu->read_half(v1A); v1A += 2;
            if (h < 0) {                             /* wide escape (absolute) */
                uint32_t s2 = cpu->read_half(v1A); v1A += 2;
                aZ = sra_field((int32_t)h,18,21);
                aX = -sra_field((int32_t)((s2 << 12) | (((uint32_t)(int32_t)h) << 28)),0,21);
                aY = -sra_field((int32_t)(s2 << 22),0,21);
            } else {
                aZ += sra_field((int32_t)h,18,25); aX -= sra_field((int32_t)h,23,25); aY -= sra_field((int32_t)h,28,25);
                aZ += sra_field(corrA,0,21); aX -= sra_field(corrA,11,21); aY -= sra_field(corrA,22,21);
            }
        }
        double VX = aX, VY = aY, VZ = aZ;
        /* ---- frame B vertex (only if dual) ---- */
        if (dual) {
            int32_t corrB = (int32_t)cpu->read_word(atB); atB += 4;
            if (!b2) {                               /* B 1-byte */
                uint32_t b = cpu->read_byte(v0B); v0B += 1;
                int32_t d = (int32_t)cpu->read_word(D_VERT_LUT + ((b << 1) & 0x1FCu)) + corrB;
                bZ += sra_field(d,0,21); bX -= sra_field(d,11,21); bY -= sra_field(d,22,21);
            } else {                                 /* B 2-byte */
                int16_t h = (int16_t)cpu->read_half(v1B); v1B += 2;
                bZ += sra_field(corrB,0,21); bX -= sra_field(corrB,11,21); bY -= sra_field(corrB,22,21);
                if (h < 0) {                         /* wide escape (overwrites) */
                    uint32_t s2 = cpu->read_half(v1B); v1B += 2;
                    bZ = sra_field((int32_t)h,18,21);
                    bX = -sra_field((int32_t)((s2 << 12) | (((uint32_t)(int32_t)h) << 28)),0,21);
                    bY = -sra_field((int32_t)(s2 << 22),0,21);
                } else {
                    bZ += sra_field((int32_t)h,18,25); bX -= sra_field((int32_t)h,23,25); bY -= sra_field((int32_t)h,28,25);
                }
            }
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
        /* Matrix is STICKY across segments. Phase A stores a segment's own
         * matrix at +0x10 only when it has a distinct (seam-rotated) pose; an
         * all-zero slot means "reuse the previous segment's matrix" (the .s
         * skips the ctc2 reload via bgez). So load +0x10 only when non-zero. */
        uint32_t m0 = cpu->read_word(entry + 0x10), m1 = cpu->read_word(entry + 0x14);
        uint32_t m2 = cpu->read_word(entry + 0x18), m3 = cpu->read_word(entry + 0x1C);
        uint32_t m4 = cpu->read_word(entry + 0x20);
        if (m0|m1|m2|m3|m4) { read_matrix(cpu, entry + 0x10, M); have_mat = 1; }
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
    for (int j = 0; j < n; j++) {
        int k = j + off; if (k < 0 || k >= n) continue;
        uint32_t w = cpu->read_word(SCRATCHPAD + (uint32_t)k * 4);
        float gx = (float)(int16_t)(w & 0xFFFF), gy = (float)(int16_t)(w >> 16);
        float dx = s_sproj[j].sx - gx, dy = s_sproj[j].sy - gy, d = sqrtf(dx*dx + dy*dy);
        tot++; errsum += d; if (d <= 4.0f) hit4++;
        int sg = s_sproj[j].seg; if (sg>=0&&sg<3){ seg_tot[sg]++; if(d<=4.0f) seg_hit[sg]++; }
    }
    fprintf(stderr, "[spyro-scratch f=%u off=%+d] <=4px=%.1f%% mean=%.2fpx | body=%.0f%% head=%.0f%% tail=%.0f%%\n",
            openpete_frame, off, tot?100.0*hit4/tot:0, tot?errsum/tot:0,
            seg_tot[0]?100.0*seg_hit[0]/seg_tot[0]:0, seg_tot[1]?100.0*seg_hit[1]/seg_tot[1]:0,
            seg_tot[2]?100.0*seg_hit[2]/seg_tot[2]:0);
}
static void spyro_scratchpad_ab(CPUState* cpu) {
    if (s_sproj_n <= 0) return;
    uint32_t fd0 = cpu->read_word(SEG_SCRATCH + 0x00);  /* body frameData */
    fprintf(stderr, "  (body fd=%08X blend=%u dual=%d A%s B%s)\n", fd0, fd0 & 0xFFF,
            (fd0 & 0xFFF) != 0, (fd0 & 0x1000) ? "2" : "1", (fd0 & 0x2000) ? "2" : "1");
    spyro_scratchpad_at(cpu, 0);
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
    if (!verify && !feed) return 0;             /* inert: let normal dispatch run it */
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    func_80023AC4(cpu);                          /* run the real renderer */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    if (verify) {
        static int s_dumped = 0, s_reports = 0;
        if (openpete_frame >= spyro_verify_frame() && endp > startp) {
            capture_prims(cpu, startp, endp);
            if (!s_dumped) { spyro_dump(cpu, startp, endp); s_dumped = 1; }
            if (s_reports < 12) {
                spyro_project_all(cpu);              /* native decode + project -> s_sproj */
                spyro_compare_geometry();            /* prim-NN A/B (geometry verdict) */
                spyro_scratchpad_ab(cpu);            /* clean per-vertex A/B vs scratchpad */
                s_reports++;
            }
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
