/* core/psx/gte.c - the PSX Geometry Transformation Engine (coprocessor 2).
 *
 * Implements the 22 documented GTE commands over the register file held in
 * CPUState (gte_data[32] = cop2r0-31, gte_ctrl[32] = cop2r32-63), from the
 * register-level description of the hardware: 44-bit MAC1-3 accumulation
 * with an overflow check on every addition, 32-bit MAC0 checks on the final
 * value, IR/colour/screen/Z saturation with their FLAG bits, the SXY, SZ and
 * colour FIFOs, and the unsigned Newton-Raphson perspective division.
 *
 * The recompiled code reads and writes the register arrays directly for
 * mfc2/cfc2/mtc2/ctc2 (with the IRGB and LZCS side effects inlined at the
 * store), so every command ends by writing the whole file back in its
 * hardware read form: 16-bit registers extended, SXYP mirroring SXY2, IRGB
 * and ORGB collapsed from IR1-3, LZCR counted from LZCS, FLAG bit 31
 * summarising its error bits. A store the game makes between commands is
 * not reinterpreted here; the OPENPETE_GTE_CENSUS instrument counts such
 * stores (gte_instruments.c).
 */
#include <stdint.h>
#include <string.h>

#include "openpete_gte.h"
#include "openpete_psx.h"
#include "openpete_psxtrace.h"

uint32_t openpete_gte_caller_pc = 0;

/* Data register indices (cop2r0-31). */
enum {
    GD_VXY0 = 0, GD_VZ0 = 1, GD_RGBC = 6, GD_OTZ = 7, GD_IR0 = 8, GD_IR1 = 9,
    GD_SXY0 = 12, GD_SXY1 = 13, GD_SXY2 = 14, GD_SXYP = 15,
    GD_SZ0 = 16, GD_SZ1 = 17, GD_SZ2 = 18, GD_SZ3 = 19,
    GD_RGB0 = 20, GD_RGB1 = 21, GD_RGB2 = 22,
    GD_MAC0 = 24, GD_IRGB = 28, GD_ORGB = 29, GD_LZCS = 30, GD_LZCR = 31,
};

/* Control register indices (cop2r32-63). */
enum {
    GC_RT = 0, GC_TR = 5, GC_LLM = 8, GC_BK = 13, GC_LCM = 16, GC_FC = 21,
    GC_OFX = 24, GC_OFY = 25, GC_H = 26, GC_DQA = 27, GC_DQB = 28,
    GC_ZSF3 = 29, GC_ZSF4 = 30, GC_FLAG = 31,
};

/* FLAG bits. MAC1-3 positive overflow is bit 31-i, negative 28-i; IR1-3
 * saturation bit 25-i; colour R/G/B saturation bit 22-i (i = 1..3). */
#define GF_SZ_OTZ   (1u << 18)
#define GF_DIV      (1u << 17)
#define GF_MAC0_POS (1u << 16)
#define GF_MAC0_NEG (1u << 15)
#define GF_SX2      (1u << 14)
#define GF_SY2      (1u << 13)
#define GF_IR0      (1u << 12)
#define GF_ERROR    (1u << 31)
/* The signed 44-bit range of the MAC1-3 accumulator. */
#define GTE_MAC44_MAX ((INT64_C(1) << 43) - 1)
#define GTE_MAC44_MIN (-(INT64_C(1) << 43))
/* Bits 30-23 and 18-13: the bits FLAG bit 31 summarises. */
#define GF_ERROR_SRC 0x7F87E000u
/* Bits 30-12: the stored, software-visible part of FLAG. */
#define GF_STORED    0x7FFFF000u

/** One command in flight: the register file, the FLAG bits raised so far and the decoded sf/lm fields. */
typedef struct gte_op {
    uint32_t* d;    /**< cpu->gte_data. */
    uint32_t* c;    /**< cpu->gte_ctrl. */
    uint32_t  flag; /**< FLAG bits raised by this command (reset at command start). */
    int       sh;   /**< sf * 12: the fraction shift applied to MAC results. */
    int       lm;   /**< 1 = IR1-3 saturate to 0..7FFFh, 0 = -8000h..7FFFh. */
} gte_op_t;

/* Reciprocal seed table of the perspective division, indexed by
 * (d - 7FC0h) >> 7 for a normalised divisor d in 8000h..FFFFh; entry i is
 * max(0, (40000h / (i + 100h) + 1) / 2 - 101h). */
static const uint8_t k_unr_table[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8,
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B,
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F,
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A,
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
    0x00,
};

/** @brief Return the low halfword of a register word as a signed 16-bit value. */
static int32_t gte_lo16(uint32_t r) { return (int16_t)(uint16_t)(r & 0xFFFFu); }

/** @brief Return the high halfword of a register word as a signed 16-bit value. */
static int32_t gte_hi16(uint32_t r) { return (int16_t)(uint16_t)(r >> 16); }

/** @brief Return the low 32 bits of a 64-bit value as a register word. */
static uint32_t gte_word(int64_t v) { return (uint32_t)(uint64_t)v; }

/** @brief Clamp v to [lo, hi], raising bit in *flag when it was outside. */
static int32_t gte_clamp(int64_t v, int32_t lo, int32_t hi, uint32_t* flag, uint32_t bit) {
    if (v < lo) { *flag |= bit; return lo; }
    if (v > hi) { *flag |= bit; return hi; }
    return (int32_t)v;
}

/** @brief Sign-extend a value from bit 43: the wrap of the 44-bit MAC1-3 accumulator. */
static int64_t gte_sext44(int64_t v) { return (int64_t)((uint64_t)v << 20) >> 20; }

/** @brief Add term to the 44-bit accumulator of MACi, raising the overflow flag of that addition, and return the wrapped sum.
 *
 * A sum above +7FFFFFFFFFFh raises bit 31-i, below -80000000000h bit 28-i;
 * the accumulator keeps the 44-bit wrap, so an overflow undone by a later
 * addition leaves a correct final value with the flag still set.
 * @param g The command in flight.
 * @param i MAC index 1..3.
 * @param acc The accumulator before the addition.
 * @param term The product or term added.
 * @return The 44-bit wrapped sum, sign-extended. */
static int64_t gte_acc(gte_op_t* g, int i, int64_t acc, int64_t term) {
    const int64_t v = acc + term;
    if (v > GTE_MAC44_MAX) g->flag |= 1u << (31 - i);
    else if (v < GTE_MAC44_MIN) g->flag |= 1u << (28 - i);
    return gte_sext44(v);
}

/** @brief Store MACi = acc SAR (sf*12), truncated to 32 bits, and return it. */
static int32_t gte_set_mac(gte_op_t* g, int i, int64_t acc) {
    const uint32_t m = gte_word(acc >> g->sh);
    g->d[GD_MAC0 + i] = m;
    return (int32_t)m;
}

/** @brief Saturate a MAC value into IRi's range (0..7FFFh under lm, else -8000h..7FFFh), raising bit 25-i. */
static int32_t gte_ir_sat(gte_op_t* g, int i, int32_t v, int lm) {
    return gte_clamp(v, lm ? 0 : -0x8000, 0x7FFF, &g->flag, 1u << (25 - i));
}

/** @brief Store IRi saturated from a MAC value under the given lm. */
static void gte_set_ir(gte_op_t* g, int i, int32_t mac, int lm) {
    g->d[GD_IR0 + i] = (uint32_t)gte_ir_sat(g, i, mac, lm);
}

/** @brief Store MAC0 from its internal value, raising bit 16/15 when the value leaves the signed 32-bit range, and return the internal value. */
static int64_t gte_set_mac0(gte_op_t* g, int64_t v) {
    if (v > INT32_MAX) g->flag |= GF_MAC0_POS;
    else if (v < INT32_MIN) g->flag |= GF_MAC0_NEG;
    g->d[GD_MAC0] = gte_word(v);
    return v;
}

/** @brief Load the 3x3 matrix whose nine 16-bit elements start at control register base (two per word, low half first). */
static void gte_load_matrix(const uint32_t* c, int base, int32_t m[9]) {
    for (int k = 0; k < 9; k++) {
        const uint32_t r = c[base + k / 2];
        m[k] = (k & 1) ? gte_hi16(r) : gte_lo16(r);
    }
}

/** @brief Load vector V0, V1 or V2 (v = 0..2), or [IR1, IR2, IR3] (v = 3). */
static void gte_load_vector(const uint32_t* d, int v, int32_t out[3]) {
    if (v == 3) {
        for (int i = 0; i < 3; i++) out[i] = gte_lo16(d[GD_IR1 + i]);
        return;
    }
    out[0] = gte_lo16(d[2 * v]);
    out[1] = gte_hi16(d[2 * v]);
    out[2] = gte_lo16(d[2 * v + 1]);
}

/** @brief Load three consecutive 32-bit control registers (TR, BK or FC). */
static void gte_load_triple(const uint32_t* c, int base, int32_t out[3]) {
    for (int i = 0; i < 3; i++) out[i] = (int32_t)c[base + i];
}

/** @brief Compute [MAC1..3] = (T*1000h + M*V) SAR (sf*12) and [IR1..3] = MAC saturated under lm.
 * @param g The command in flight.
 * @param m The matrix, row major.
 * @param v The vector (copied before IR is written).
 * @param t The translation, or zeros.
 * @param lm The IR saturation mode. */
static void gte_mat_vec(gte_op_t* g, const int32_t m[9], const int32_t v[3], const int32_t t[3], int lm) {
    for (int i = 0; i < 3; i++) {
        int64_t acc = gte_acc(g, i + 1, 0, (int64_t)t[i] * 0x1000);
        for (int j = 0; j < 3; j++) acc = gte_acc(g, i + 1, acc, (int64_t)m[3 * i + j] * v[j]);
        gte_set_ir(g, i + 1, gte_set_mac(g, i + 1, acc), lm);
    }
}

/** @brief MVMVA with the far-colour translation (cv = 2): the translation and first column only raise IR flags, the result is the last two columns.
 *
 * Part one computes (T*1000h + M_i1*V1) SAR (sf*12) and saturates it as if
 * lm = 0, keeping only the IR saturation flag; part two restarts the sum at
 * zero with M_i2*V2 + M_i3*V3, which becomes MACi and IRi under lm.
 * @param g The command in flight.
 * @param m The matrix.
 * @param v The vector.
 * @param t The far colour. */
static void gte_mat_vec_fc(gte_op_t* g, const int32_t m[9], const int32_t v[3], const int32_t t[3]) {
    for (int i = 0; i < 3; i++) {
        const int64_t part1 = gte_sext44((int64_t)t[i] * 0x1000 + (int64_t)m[3 * i] * v[0]);
        (void)gte_ir_sat(g, i + 1, (int32_t)gte_word(part1 >> g->sh), 0);
        int64_t acc = gte_acc(g, i + 1, 0, (int64_t)m[3 * i + 1] * v[1]);
        acc = gte_acc(g, i + 1, acc, (int64_t)m[3 * i + 2] * v[2]);
        gte_set_ir(g, i + 1, gte_set_mac(g, i + 1, acc), g->lm);
    }
}

/** @brief Push [MAC1/16, MAC2/16, MAC3/16, CODE] onto the colour FIFO, each colour saturated to 0..FFh (bits 21-19). */
static void gte_push_color(gte_op_t* g) {
    uint32_t rgb = g->d[GD_RGBC] & 0xFF000000u;
    for (int i = 0; i < 3; i++) {
        const int32_t mac = (int32_t)g->d[GD_MAC0 + 1 + i];
        rgb |= (uint32_t)gte_clamp(mac >> 4, 0, 0xFF, &g->flag, 1u << (21 - i)) << (8 * i);
    }
    g->d[GD_RGB0] = g->d[GD_RGB1];
    g->d[GD_RGB1] = g->d[GD_RGB2];
    g->d[GD_RGB2] = rgb;
}

/** @brief Compute "MAC + (FC - MAC) * IR0" on the three accumulators.
 *
 * IRi = ((FCi SHL 12) - MACi) SAR (sf*12), saturated as if lm = 0; then
 * MACi += IRi * IR0. Both additions are 44-bit checked.
 * @param g The command in flight.
 * @param acc The three accumulators, updated in place. */
static void gte_depth_cue(gte_op_t* g, int64_t acc[3]) {
    int32_t fc[3];
    gte_load_triple(g->c, GC_FC, fc);
    const int32_t ir0 = gte_lo16(g->d[GD_IR0]);
    for (int i = 0; i < 3; i++) {
        const int64_t diff = gte_acc(g, i + 1, (int64_t)fc[i] * 0x1000, -acc[i]);
        const int32_t ir = gte_ir_sat(g, i + 1, (int32_t)gte_word(diff >> g->sh), 0);
        g->d[GD_IR1 + i] = (uint32_t)ir;
        acc[i] = gte_acc(g, i + 1, acc[i], (int64_t)ir * ir0);
    }
}

/** @brief Finish a colour command: MAC = acc SAR (sf*12), push the colour FIFO, IR = MAC under lm. */
static void gte_color_finish(gte_op_t* g, const int64_t acc[3]) {
    for (int i = 0; i < 3; i++) (void)gte_set_mac(g, i + 1, acc[i]);
    gte_push_color(g);
    for (int i = 0; i < 3; i++) gte_set_ir(g, i + 1, (int32_t)g->d[GD_MAC0 + 1 + i], g->lm);
}

/** @brief Read the R, G, B bytes of a colour register word. */
static void gte_rgb_bytes(uint32_t w, int32_t rgb[3]) {
    for (int i = 0; i < 3; i++) rgb[i] = (int32_t)((w >> (8 * i)) & 0xFFu);
}

/** @brief Set acc = [R*IR1, G*IR2, B*IR3] SHL 4 from the RGBC colour and the current IR1-3. */
static void gte_color_mul(gte_op_t* g, int64_t acc[3]) {
    int32_t rgb[3];
    gte_rgb_bytes(g->d[GD_RGBC], rgb);
    for (int i = 0; i < 3; i++)
        acc[i] = gte_acc(g, i + 1, 0, ((int64_t)rgb[i] * gte_lo16(g->d[GD_IR1 + i])) * 16);
}

/* Colour-stage modes after the BK + LCM*IR light stage. */
enum { GTE_NC_PLAIN = 0, GTE_NC_COLOR = 1, GTE_NC_DEPTH = 2 };

/** @brief Run the light-colour stage [IR] = [MAC] = (BK*1000h + LCM*IR) SAR (sf*12) and the colour tail of NCx, CC and CDP.
 *
 * GTE_NC_PLAIN pushes the light result (NCS/NCT); GTE_NC_COLOR multiplies
 * by RGBC (NCCx, CC); GTE_NC_DEPTH also depth-cues toward FC (NCDx, CDP).
 * @param g The command in flight.
 * @param mode One of GTE_NC_PLAIN, GTE_NC_COLOR, GTE_NC_DEPTH. */
static void gte_light_color(gte_op_t* g, int mode) {
    int32_t lcm[9], ir[3], bk[3];
    int64_t acc[3];
    gte_load_matrix(g->c, GC_LCM, lcm);
    gte_load_vector(g->d, 3, ir);
    gte_load_triple(g->c, GC_BK, bk);
    gte_mat_vec(g, lcm, ir, bk, g->lm);
    if (mode == GTE_NC_PLAIN) {
        gte_push_color(g);
        return;
    }
    gte_color_mul(g, acc);
    if (mode == GTE_NC_DEPTH) gte_depth_cue(g, acc);
    gte_color_finish(g, acc);
}

/** @brief One vertex of NCS/NCT, NCCS/NCCT or NCDS/NCDT: [IR] = [MAC] = (LLM*Vv) SAR (sf*12), then the light-colour stage. */
static void gte_normal_color(gte_op_t* g, int v, int mode) {
    static const int32_t k_zero[3] = {0, 0, 0};
    int32_t llm[9], vec[3];
    gte_load_matrix(g->c, GC_LLM, llm);
    gte_load_vector(g->d, v, vec);
    gte_mat_vec(g, llm, vec, k_zero, g->lm);
    gte_light_color(g, mode);
}

/** @brief Divide H by SZ3 the way the hardware does (unsigned Newton-Raphson), returning 0..1FFFFh.
 *
 * When H >= SZ3*2 the result is 1FFFFh with FLAG bit 17. Otherwise the
 * divisor is normalised to 8000h..FFFFh, a reciprocal seed is read from
 * k_unr_table and refined twice, and the rounded product is limited to
 * 1FFFFh without a flag.
 * @param g The command in flight.
 * @param h H, unsigned 16-bit.
 * @param sz3 SZ3 after saturation, unsigned 16-bit.
 * @return The projection factor. */
static uint32_t gte_divide(gte_op_t* g, uint32_t h, uint32_t sz3) {
    if (h >= sz3 * 2u) {
        g->flag |= GF_DIV;
        return 0x1FFFFu;
    }
    int z = 0;
    while (!(sz3 & (0x8000u >> z))) z++;
    const uint64_t n = (uint64_t)h << z;
    uint64_t d = (uint64_t)sz3 << z;
    const uint64_t u = (uint64_t)k_unr_table[(d - 0x7FC0u) >> 7] + 0x101u;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    const uint64_t q = (n * d + 0x8000u) >> 16;
    return q > 0x1FFFFu ? 0x1FFFFu : (uint32_t)q;
}

/** @brief Push a screen Z onto the SZ FIFO (SZ0 <- SZ1 <- SZ2 <- SZ3 <- sz). */
static void gte_push_sz(uint32_t* d, uint32_t sz) {
    d[GD_SZ0] = d[GD_SZ1];
    d[GD_SZ1] = d[GD_SZ2];
    d[GD_SZ2] = d[GD_SZ3];
    d[GD_SZ3] = sz;
}

/** @brief Push a screen XY onto the SXY FIFO (SXY0 <- SXY1 <- SXY2 <- sxy). */
static void gte_push_sxy(uint32_t* d, uint32_t sxy) {
    d[GD_SXY0] = d[GD_SXY1];
    d[GD_SXY1] = d[GD_SXY2];
    d[GD_SXY2] = sxy;
}

/** @brief Push the projected X in q16 (the MAC0 value SX2 is taken from, before saturation) onto the canonical-SX FIFO the particle and sparkle cull verdicts read. */
static void gte_push_canonical_sx(int64_t sx_q16) {
    openpete_canonical_sx0_q16 = openpete_canonical_sx1_q16;
    openpete_canonical_sx1_q16 = openpete_canonical_sx2_q16;
    openpete_canonical_sx2_q16 = sx_q16;
}

/** @brief Rotate and translate Vv into MAC1-3/IR1-3 for RTPS/RTPT and return the saturated screen Z.
 *
 * IR1-3 saturate to -8000h..7FFFh regardless of lm. IR3's flag (bit 22) is
 * raised from the 44-bit sum SAR 12, the pre-saturation SZ3, not from the
 * value IR3 is clamped from, which differs when sf = 0. SZ3 saturates to
 * 0..FFFFh with bit 18.
 * @param g The command in flight.
 * @param v Vertex index 0..2.
 * @return SZ3 for this vertex. */
static uint32_t gte_rtp_transform(gte_op_t* g, int v) {
    int32_t rt[9], vec[3], tr[3];
    gte_load_matrix(g->c, GC_RT, rt);
    gte_load_vector(g->d, v, vec);
    gte_load_triple(g->c, GC_TR, tr);
    int32_t mac[3];
    int64_t acc = 0;
    for (int i = 0; i < 3; i++) {
        acc = gte_acc(g, i + 1, 0, (int64_t)tr[i] * 0x1000);
        for (int j = 0; j < 3; j++) acc = gte_acc(g, i + 1, acc, (int64_t)rt[3 * i + j] * vec[j]);
        mac[i] = gte_set_mac(g, i + 1, acc);
    }
    gte_set_ir(g, 1, mac[0], 0);
    gte_set_ir(g, 2, mac[1], 0);
    uint32_t ir3_flag = 0;
    g->d[GD_IR1 + 2] = (uint32_t)gte_clamp(mac[2], -0x8000, 0x7FFF, &ir3_flag, 1u);
    /* MAC3 is 44 bits wide inside the GTE; the register keeps its low 32
     * bits (which IR3 saturates from), while SZ3 and the IR3 flag take the
     * full sum SAR 12 for either sf. */
    const int64_t z = acc >> 12;
    (void)gte_clamp(z, -0x8000, 0x7FFF, &g->flag, 1u << 22);
    return (uint32_t)gte_clamp(z, 0, 0xFFFF, &g->flag, GF_SZ_OTZ);
}

/** @brief One vertex of RTPS/RTPT: transform, push SZ, divide, project to SX2/SY2 and push SXY; on the last vertex also depth-cue IR0.
 *
 * MAC0 = n*IR1 + OFX gives SX2 = MAC0 SAR 16 saturated to -400h..3FFh
 * (bit 14); MAC0 = n*IR2 + OFY gives SY2 (bit 13); on the last vertex
 * MAC0 = n*DQA + DQB gives IR0 = MAC0 SAR 12 saturated to 0..1000h (bit
 * 12). Each MAC0 is checked for 32-bit overflow on its internal value, and
 * the screen and IR0 values derive from the internal value.
 * @param g The command in flight.
 * @param v Vertex index 0..2.
 * @param last Nonzero for the vertex that computes IR0 (RTPS, RTPT's V2). */
static void gte_rtp(gte_op_t* g, int v, int last) {
    const uint32_t sz3 = gte_rtp_transform(g, v);
    gte_push_sz(g->d, sz3);
    const int64_t n = gte_divide(g, g->c[GC_H] & 0xFFFFu, sz3);
    const int64_t mx = gte_set_mac0(g, n * gte_lo16(g->d[GD_IR1]) + (int32_t)g->c[GC_OFX]);
    const int32_t sx = gte_clamp(mx >> 16, -0x400, 0x3FF, &g->flag, GF_SX2);
    const int64_t my = gte_set_mac0(g, n * gte_lo16(g->d[GD_IR1 + 1]) + (int32_t)g->c[GC_OFY]);
    const int32_t sy = gte_clamp(my >> 16, -0x400, 0x3FF, &g->flag, GF_SY2);
    gte_push_sxy(g->d, ((uint32_t)sx & 0xFFFFu) | ((uint32_t)sy << 16));
    gte_push_canonical_sx(mx);
    if (!last) return;
    const int64_t mq = gte_set_mac0(g, n * gte_lo16(g->c[GC_DQA]) + (int32_t)g->c[GC_DQB]);
    g->d[GD_IR0] = (uint32_t)gte_clamp(mq >> 12, 0, 0x1000, &g->flag, GF_IR0);
}

/** @brief NCLIP: MAC0 = SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1, checked for 32-bit overflow. */
static void gte_nclip(gte_op_t* g) {
    int64_t x[3], y[3];
    for (int i = 0; i < 3; i++) {
        x[i] = gte_lo16(g->d[GD_SXY0 + i]);
        y[i] = gte_hi16(g->d[GD_SXY0 + i]);
    }
    (void)gte_set_mac0(g, x[0] * y[1] + x[1] * y[2] + x[2] * y[0] - x[0] * y[2] - x[1] * y[0] - x[2] * y[1]);
}

/** @brief OP: [MAC1..3] = [IR3*D2 - IR2*D3, IR1*D3 - IR3*D1, IR2*D1 - IR1*D2] SAR (sf*12), D = RT11, RT22, RT33; IR = MAC under lm. */
static void gte_op(gte_op_t* g) {
    int32_t ir[3];
    gte_load_vector(g->d, 3, ir);
    const int32_t dd[3] = {gte_lo16(g->c[GC_RT]), gte_lo16(g->c[GC_RT + 2]), gte_lo16(g->c[GC_RT + 4])};
    for (int i = 0; i < 3; i++) {
        const int a = (i + 1) % 3, b = (i + 2) % 3;
        int64_t acc = gte_acc(g, i + 1, 0, (int64_t)ir[b] * dd[a]);
        acc = gte_acc(g, i + 1, acc, -((int64_t)ir[a] * dd[b]));
        gte_set_ir(g, i + 1, gte_set_mac(g, i + 1, acc), g->lm);
    }
}

/** @brief SQR: [MAC1..3] = [IR1^2, IR2^2, IR3^2] SAR (sf*12); IR = MAC under lm. */
static void gte_sqr(gte_op_t* g) {
    int32_t ir[3];
    gte_load_vector(g->d, 3, ir);
    for (int i = 0; i < 3; i++)
        gte_set_ir(g, i + 1, gte_set_mac(g, i + 1, gte_acc(g, i + 1, 0, (int64_t)ir[i] * ir[i])), g->lm);
}

/** @brief AVSZ3 (count 3) or AVSZ4 (count 4): MAC0 = ZSF * (sum of the last count SZ entries); OTZ = MAC0 SAR 12 saturated to 0..FFFFh (bit 18). */
static void gte_avsz(gte_op_t* g, int count) {
    const int32_t zsf = gte_lo16(g->c[count == 3 ? GC_ZSF3 : GC_ZSF4]);
    int64_t sum = 0;
    for (int i = 4 - count; i < 4; i++) sum += g->d[GD_SZ0 + i] & 0xFFFFu;
    const int64_t m = gte_set_mac0(g, sum * zsf);
    g->d[GD_OTZ] = (uint32_t)gte_clamp(m >> 12, 0, 0xFFFF, &g->flag, GF_SZ_OTZ);
}

/** @brief MVMVA: [MAC1..3] = (Tx*1000h + Mx*Vx) SAR (sf*12), IR = MAC under lm, with mx, v and cv from the command word.
 *
 * mx = 3 selects the matrix [-R*10h, R*10h, IR0, RT13, RT13, RT13, RT22,
 * RT22, RT22] (R = RGBC bits 0-7); cv = 2 (FC) takes the split path of
 * gte_mat_vec_fc; cv = 3 adds no translation.
 * @param g The command in flight.
 * @param cmd The command word. */
static void gte_mvmva(gte_op_t* g, uint32_t cmd) {
    static const int k_mat_base[3] = {GC_RT, GC_LLM, GC_LCM};
    static const int k_tr_base[3] = {GC_TR, GC_BK, GC_FC};
    const int mx = (int)((cmd >> 17) & 3u), vx = (int)((cmd >> 15) & 3u), cv = (int)((cmd >> 13) & 3u);
    int32_t m[9], v[3], t[3] = {0, 0, 0};
    if (mx == 3) {
        const int32_t r = (int32_t)(g->d[GD_RGBC] & 0xFFu);
        const int32_t rt13 = gte_lo16(g->c[GC_RT + 1]), rt22 = gte_lo16(g->c[GC_RT + 2]);
        const int32_t garbage[9] = {-r * 16, r * 16, gte_lo16(g->d[GD_IR0]), rt13, rt13, rt13, rt22, rt22, rt22};
        memcpy(m, garbage, sizeof(m));
    } else {
        gte_load_matrix(g->c, k_mat_base[mx], m);
    }
    gte_load_vector(g->d, vx, v);
    if (cv != 3) gte_load_triple(g->c, k_tr_base[cv], t);
    if (cv == 2) gte_mat_vec_fc(g, m, v, t);
    else gte_mat_vec(g, m, v, t, g->lm);
}

/** @brief Depth-cue colour commands DPCS/DPCT, INTPL and DCPL: build the accumulators, cue toward FC, finish.
 *
 * DPCS/DPCT start from [R, G, B] SHL 16 (src = the colour word; DPCT passes
 * RGB0), INTPL from [IR1..3] SHL 12, DCPL from [R*IR1, G*IR2, B*IR3] SHL 4.
 * @param g The command in flight.
 * @param fn The function code (0x10, 0x2A, 0x11 or 0x29).
 * @param src The colour word DPCS/DPCT read. */
static void gte_depth_color(gte_op_t* g, uint32_t fn, uint32_t src) {
    int64_t acc[3];
    if (fn == 0x11) {
        for (int i = 0; i < 3; i++) acc[i] = gte_acc(g, i + 1, 0, (int64_t)gte_lo16(g->d[GD_IR1 + i]) * 0x1000);
    } else if (fn == 0x29) {
        gte_color_mul(g, acc);
    } else {
        int32_t rgb[3];
        gte_rgb_bytes(src, rgb);
        for (int i = 0; i < 3; i++) acc[i] = gte_acc(g, i + 1, 0, (int64_t)rgb[i] * 0x10000);
    }
    gte_depth_cue(g, acc);
    gte_color_finish(g, acc);
}

/** @brief GPF (base 0) or GPL (base = MAC SHL (sf*12)): [MAC] = ([IR] * IR0 + base) SAR (sf*12); push the colour FIFO; IR = MAC under lm. */
static void gte_gp(gte_op_t* g, int with_base) {
    const int32_t ir0 = gte_lo16(g->d[GD_IR0]);
    int64_t acc[3];
    for (int i = 0; i < 3; i++) {
        const int64_t base = with_base ? (int64_t)(int32_t)g->d[GD_MAC0 + 1 + i] * ((int64_t)1 << g->sh) : 0;
        acc[i] = gte_acc(g, i + 1, base, (int64_t)gte_lo16(g->d[GD_IR1 + i]) * ir0);
    }
    gte_color_finish(g, acc);
}

/** @brief Run the command whose function code is cmd bits 0-5 and return 1, or return 0 for a code with no documented command (no state changes). */
static int gte_dispatch(gte_op_t* g, uint32_t cmd) { // complexity-ratchet: one arm per GTE function code, the guest instruction set's interface
    switch (cmd & 0x3Fu) {
    case 0x01: gte_rtp(g, 0, 1); return 1;                                    /* RTPS */
    case 0x06: gte_nclip(g); return 1;                                        /* NCLIP */
    case 0x0C: gte_op(g); return 1;                                           /* OP */
    case 0x10: gte_depth_color(g, 0x10, g->d[GD_RGBC]); return 1;             /* DPCS */
    case 0x11: gte_depth_color(g, 0x11, 0); return 1;                         /* INTPL */
    case 0x12: gte_mvmva(g, cmd); return 1;                                   /* MVMVA */
    case 0x13: gte_normal_color(g, 0, GTE_NC_DEPTH); return 1;                /* NCDS */
    case 0x14: gte_light_color(g, GTE_NC_DEPTH); return 1;                    /* CDP */
    case 0x16: for (int v = 0; v < 3; v++) gte_normal_color(g, v, GTE_NC_DEPTH); return 1; /* NCDT */
    case 0x1B: gte_normal_color(g, 0, GTE_NC_COLOR); return 1;                /* NCCS */
    case 0x1C: gte_light_color(g, GTE_NC_COLOR); return 1;                    /* CC */
    case 0x1E: gte_normal_color(g, 0, GTE_NC_PLAIN); return 1;                /* NCS */
    case 0x20: for (int v = 0; v < 3; v++) gte_normal_color(g, v, GTE_NC_PLAIN); return 1; /* NCT */
    case 0x28: gte_sqr(g); return 1;                                          /* SQR */
    case 0x29: gte_depth_color(g, 0x29, 0); return 1;                         /* DCPL */
    case 0x2A: for (int k = 0; k < 3; k++) gte_depth_color(g, 0x10, g->d[GD_RGB0]); return 1; /* DPCT */
    case 0x2D: gte_avsz(g, 3); return 1;                                      /* AVSZ3 */
    case 0x2E: gte_avsz(g, 4); return 1;                                      /* AVSZ4 */
    case 0x30: for (int v = 0; v < 3; v++) gte_rtp(g, v, v == 2); return 1;   /* RTPT */
    case 0x3D: gte_gp(g, 0); return 1;                                        /* GPF */
    case 0x3E: gte_gp(g, 1); return 1;                                        /* GPL */
    case 0x3F: for (int v = 0; v < 3; v++) gte_normal_color(g, v, GTE_NC_COLOR); return 1; /* NCCT */
    default: return 0;
    }
}

/** @brief Return the IRGB/ORGB field of one IR register: IR SAR 7 saturated to 0..1Fh, no flag. */
static uint32_t gte_orgb_field(uint32_t ir) {
    const int32_t v = gte_lo16(ir) >> 7;
    return (uint32_t)(v < 0 ? 0 : (v > 0x1F ? 0x1F : v));
}

/** @brief Return the count of leading bits equal to bit 31 of v (LZCR from LZCS), 1..32. */
static uint32_t gte_leading_bits(uint32_t v) {
    const uint32_t sign = UINT32_C(1) << 31;
    uint32_t x = (v & sign) ? ~v : v;
    uint32_t n = 0;
    while (n < 32u && !(x & sign)) {
        x <<= 1;
        n++;
    }
    return n;
}

/* Registers kept in their hardware read form after every command.
 * Sign-extended from 16 bits: data 1, 3, 5 (VZ0-2), 8 (IR0), 9-11 (IR1-3);
 * control 4 (RT33), 12 (L33), 20 (LB3), 26 (H, which the hardware
 * sign-extends on read although the division uses it unsigned), 27 (DQA),
 * 29 (ZSF3), 30 (ZSF4). Zero-extended from 16 bits: data 7 (OTZ), 16-19
 * (SZ0-3). Data 15 (SXYP) mirrors data 14 (SXY2); data 28 (IRGB) and 29
 * (ORGB) read IR1-3 collapsed to 5:5:5; data 31 (LZCR) counts the leading
 * bits of data 30 (LZCS); control 31 (FLAG) holds bits 30-12 with bit 31
 * the OR of bits 30-23 and 18-13. Data 23 (RES1) and every 32-bit register
 * keep the stored word. */
static const uint8_t k_sext_data[] = {1, 3, 5, 8, 9, 10, 11};
static const uint8_t k_sext_ctrl[] = {4, 12, 20, 26, 27, 29, 30};
static const uint8_t k_zext_data[] = {7, 16, 17, 18, 19};

/** @brief Rewrite the register file in its hardware read form after a command (the table above) and store FLAG from the command's bits. */
static void gte_writeback(uint32_t* d, uint32_t* c, uint32_t flag) {
    for (size_t i = 0; i < sizeof(k_sext_data); i++) d[k_sext_data[i]] = (uint32_t)gte_lo16(d[k_sext_data[i]]);
    for (size_t i = 0; i < sizeof(k_sext_ctrl); i++) c[k_sext_ctrl[i]] = (uint32_t)gte_lo16(c[k_sext_ctrl[i]]);
    for (size_t i = 0; i < sizeof(k_zext_data); i++) d[k_zext_data[i]] &= 0xFFFFu;
    d[GD_SXYP] = d[GD_SXY2];
    d[GD_IRGB] = d[GD_ORGB] = gte_orgb_field(d[GD_IR1]) | (gte_orgb_field(d[GD_IR1 + 1]) << 5) |
                              (gte_orgb_field(d[GD_IR1 + 2]) << 10);
    d[GD_LZCR] = gte_leading_bits(d[GD_LZCS]);
    flag &= GF_STORED;
    c[GC_FLAG] = flag | ((flag & GF_ERROR_SRC) ? GF_ERROR : 0u);
}

int openpete_gte_run(uint32_t* data, uint32_t* ctrl, uint32_t cmd) {
    gte_op_t g = {data, ctrl, 0u, ((cmd >> 19) & 1u) ? 12 : 0, (int)((cmd >> 10) & 1u)};
    if (!gte_dispatch(&g, cmd)) return 0;
    gte_writeback(data, ctrl, g.flag);
    return 1;
}

void gte_execute(CPUState* cpu, uint32_t cmd) {
    uint32_t pre_d[32], pre_c[32];
    const int hash = openpete_gte_hash_on;
    const int instr = openpete_gte_instruments_on;
    if (hash || instr) {
        memcpy(pre_d, cpu->gte_data, sizeof(pre_d));
        memcpy(pre_c, cpu->gte_ctrl, sizeof(pre_c));
    }
    const int ran = openpete_gte_run(cpu->gte_data, cpu->gte_ctrl, cmd);
    if (instr) openpete_gte_instruments_op(cpu, cmd, ran, pre_d, pre_c);
    if (hash)
        openpete_gte_hash_op(cmd, openpete_gte_caller_pc, pre_d, pre_c, cpu->gte_data, cpu->gte_ctrl);
}
