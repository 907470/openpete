#include "gte.h"
#include "psx_runtime.h"
#include "automation.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "psx/gtereg.h"   // PsyCross GTERegisters / GTE_operator (used as
                          // ground-truth reference for the GTE trace logger)

/* FOV + verdict GTE hooks (eff_H, canonical-SX FIFO).
 * static inline — codegen byte-identical to the open-coded math this
 * replaced; verified via objdump-diff. See
 * .claude/plans/openpete-feature-modules.md Phase 3. */
#include "features/widescreen_gte_inline.h"

/* Renderer-foundation Phase 0 (world-space cache). gte_rtps_internal
 * emits per-vertex entries here; PsyCross's OT walker consumes them. */

/* Day 4 Phase 1.1: PC-of-issuing-instruction for the current GTE op.
 * Recompiled code writes this immediately before each gte_execute()
 * call (emitted by code_generator.cpp). Stays at 0 from pre-regen
 * generated code, hand-written thunks, and the interpreter path —
 * those paths look like "unknown caller" downstream. */
extern "C" uint32_t openpete_gte_caller_pc = 0;
extern "C" unsigned openpete_frame;

extern "C" {
    extern unsigned openpete_frame; /* core/include/openpete_fiber.h */
    extern uint32_t g_ps1_frame;    /* core/runtime/dispatch.c */
}

namespace PSXRecomp {
namespace GTE {

// ---------------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------------

// Leading-zero count for the perspective-division reciprocal lookup.
static uint32_t gte_leadingzerocount(uint32_t lzcs) {
    if (!lzcs) return 32;
    uint32_t lzcr = lzcs;
    static const uint8_t debruijn32[32] = {
        0, 31,  9, 30,  3,  8, 13, 29,  2,  5,  7, 21, 12, 24, 28, 19,
        1, 10,  4, 14,  6, 22, 25, 20, 11, 15, 23, 26, 16, 27, 17, 18
    };
    lzcr |= lzcr >> 1;
    lzcr |= lzcr >> 2;
    lzcr |= lzcr >> 4;
    lzcr |= lzcr >> 8;
    lzcr |= lzcr >> 16;
    lzcr++;
    return debruijn32[(uint32_t)(lzcr * 0x076be629u) >> 27];
}

// Perspective division using the PSX GTE's Newton-Raphson reciprocal table.
// Bit-exact with PsyCross / real hardware, replacing a naive `H*0x20000/SZ3`
// that drifts ±1 LSB and made silhouette-edge vertices flicker as Z changes.
//
// Returns the unsigned divide result (max 0x1FFFF). Sets FLAG_DIV_OVF when
// `numerator >= 2*denominator` (the hardware's "saturate to 0x1FFFF" path).
static int32_t gte_divide(uint16_t H, uint16_t SZ3, uint32_t& FLAG) {
    if (H >= (uint32_t)SZ3 * 2u) {
        FLAG |= FLAG_DIV_OVF;
        return 0x1FFFF;
    }

    static const uint8_t table[] = {
        0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1, 0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3,
        0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5, 0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8,
        0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb, 0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
        0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4, 0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a,
        0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86,
        0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
        0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
        0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d, 0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55,
        0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
        0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b,
        0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f,
        0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
        0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
        0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
        0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
        0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
        0x00,
    };

    int shift = (int)gte_leadingzerocount(SZ3) - 16;
    if (shift < 0) shift = 0;

    int r1 = ((int)SZ3 << shift) & 0x7fff;
    int r2 = (int)table[(r1 + 0x40) >> 7] + 0x101;
    int r3 = ((0x80 - (r2 * (r1 + 0x8000))) >> 8) & 0x1ffff;
    uint32_t reciprocal = (uint32_t)((r2 * r3) + 0x80) >> 8;

    uint32_t result = (uint32_t)(((uint64_t)reciprocal * ((uint32_t)H << shift) + 0x8000ULL) >> 16);
    if (result > 0x1FFFFu) {
        FLAG |= FLAG_DIV_OVF;
        return 0x1FFFF;
    }
    return (int32_t)result;
}

// Transform a vertex by Light matrix → IR1/IR2/IR3 (step 1 of lighting)
//
// `lm` comes from bit 10 of the issuing instruction word: 1 clamps IR to
// [0, 0x7FFF], 0 to [-0x8000, 0x7FFF]. Hardware honours it on every IR
// write-back, including the lighting family — a hardcoded lm=1 here
// silently zeroes every negative component, which breaks the off-label
// signed-vector uses of these ops (e.g. INTPL as a vertex lerp).
static void light_transform(GTEState* gte, int16_t* V, bool lm) {
    int64_t mac1 = ((int64_t)gte->L[0][0] * V[0] +
                    (int64_t)gte->L[0][1] * V[1] +
                    (int64_t)gte->L[0][2] * V[2]) >> 12;
    int64_t mac2 = ((int64_t)gte->L[1][0] * V[0] +
                    (int64_t)gte->L[1][1] * V[1] +
                    (int64_t)gte->L[1][2] * V[2]) >> 12;
    int64_t mac3 = ((int64_t)gte->L[2][0] * V[0] +
                    (int64_t)gte->L[2][1] * V[1] +
                    (int64_t)gte->L[2][2] * V[2]) >> 12;
    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);
}

// Apply light color matrix + background color to IR → IR (step 2 of lighting)
static void light_color(GTEState* gte, bool lm) {
    int64_t mac1 = ((int64_t)gte->BK[0] << 12) +
                   (int64_t)gte->LC[0][0] * gte->IR1 +
                   (int64_t)gte->LC[0][1] * gte->IR2 +
                   (int64_t)gte->LC[0][2] * gte->IR3;
    int64_t mac2 = ((int64_t)gte->BK[1] << 12) +
                   (int64_t)gte->LC[1][0] * gte->IR1 +
                   (int64_t)gte->LC[1][1] * gte->IR2 +
                   (int64_t)gte->LC[1][2] * gte->IR3;
    int64_t mac3 = ((int64_t)gte->BK[2] << 12) +
                   (int64_t)gte->LC[2][0] * gte->IR1 +
                   (int64_t)gte->LC[2][1] * gte->IR2 +
                   (int64_t)gte->LC[2][2] * gte->IR3;
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);
}

// Multiply IR by RGBC color and push to RGB FIFO (step 3 of color output)
/* §13.16 audit (OPENPETE_OFX_CENSUS=1): full-trace census of RTPS calls
 * whose native OFX ≠ canonical 256. The widescreen fix treats
 * non-canonical OFX as the authoritative UI signal — this census proves
 * (or breaks) the converse claim that no WORLD projection anywhere in a
 * run carries OFX≠256. Keyed per caller PC; [ofx-census] NEW on first
 * sight per PC, full table at process exit. */
namespace {
struct OfxCensusEnt { uint32_t pc; int32_t lo, hi; uint64_t n_rp, n_off; };
OfxCensusEnt g_ofx_census_tab[64];
unsigned     g_ofx_census_n = 0;

void ofx_census_dump() {
    for (unsigned k = 0; k < g_ofx_census_n; k++)
        fprintf(stderr,
                "[ofx-census] pc=%08X ofx=[%d..%d] rp=%llu off=%llu\n",
                g_ofx_census_tab[k].pc,
                (int)(g_ofx_census_tab[k].lo >> 16),
                (int)(g_ofx_census_tab[k].hi >> 16),
                (unsigned long long)g_ofx_census_tab[k].n_rp,
                (unsigned long long)g_ofx_census_tab[k].n_off);
}

void ofx_census_record(uint32_t pc, int32_t ofx, int rp, unsigned rot) {
    unsigned i;
    for (i = 0; i < g_ofx_census_n; i++)
        if (g_ofx_census_tab[i].pc == pc) break;
    if (i == g_ofx_census_n) {
        if (g_ofx_census_n >= 64) return;   /* overflow: keep first 64 PCs */
        if (g_ofx_census_n == 0) atexit(ofx_census_dump);
        g_ofx_census_tab[i].pc = pc;
        g_ofx_census_tab[i].lo = g_ofx_census_tab[i].hi = ofx;
        g_ofx_census_tab[i].n_rp = g_ofx_census_tab[i].n_off = 0;
        g_ofx_census_n++;
        fprintf(stderr, "[ofx-census] NEW pc=%08X ofx=%d rp=%d rot=%u\n",
                pc, (int)(ofx >> 16), rp, rot);
    }
    if (ofx < g_ofx_census_tab[i].lo) g_ofx_census_tab[i].lo = ofx;
    if (ofx > g_ofx_census_tab[i].hi) g_ofx_census_tab[i].hi = ofx;
    if (rp) g_ofx_census_tab[i].n_rp++;
    else    g_ofx_census_tab[i].n_off++;
}
} // namespace

static void color_output(GTEState* gte, bool lm) {
    uint8_t r0 = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g0 = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b0 = (gte->RGBC >> 16) & 0xFF;
    int64_t mac1 = ((int64_t)r0 * gte->IR1) << 4;
    int64_t mac2 = ((int64_t)g0 * gte->IR2) << 4;
    int64_t mac3 = ((int64_t)b0 * gte->IR3) << 4;
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
}

// Apply depth cueing in *color* space, exactly mirroring PsyCross's reference
// (which itself follows the Nocash GTE spec / hardware sequence). Callers compute
// the pre-cue MAC values (RGBC*IR for NCDS/CDP/DPCL, RGBC<<16 for DPCS/DPCT,
// IR<<12 for INTPL) and pass them in unshifted; this helper finishes:
//
//   diff_ir   = saturate_ir((FC<<12 - color_MAC) >> 12, lm=0)   ; intermediate
//   final_MAC = (color_MAC + IR0 * diff_ir) >> 12
//   IR        = saturate_ir(final_MAC, lm=true)
//   push_rgb(saturate_color(final_MAC >> 4))
//
// The intermediate IR-saturation step is significant: very large color_MAC
// values vs FC saturate the delta to ±0x7FFF *before* the IR0 multiply, which
// is what real hardware does and which the previous implementation skipped.
static void color_depth_cue_finish(GTEState* gte,
                                    int64_t mac1, int64_t mac2, int64_t mac3,
                                    bool lm) {
    int32_t fc_diff1 = static_cast<int32_t>((((int64_t)gte->FC[0] << 12) - mac1) >> 12);
    int32_t fc_diff2 = static_cast<int32_t>((((int64_t)gte->FC[1] << 12) - mac2) >> 12);
    int32_t fc_diff3 = static_cast<int32_t>((((int64_t)gte->FC[2] << 12) - mac3) >> 12);
    int32_t ir_d1 = gte->saturate_ir(fc_diff1, 1, false);
    int32_t ir_d2 = gte->saturate_ir(fc_diff2, 2, false);
    int32_t ir_d3 = gte->saturate_ir(fc_diff3, 3, false);
    int64_t total1 = mac1 + (int64_t)gte->IR0 * ir_d1;
    int64_t total2 = mac2 + (int64_t)gte->IR0 * ir_d2;
    int64_t total3 = mac3 + (int64_t)gte->IR0 * ir_d3;
    gte->MAC1 = static_cast<int32_t>(total1 >> 12);
    gte->MAC2 = static_cast<int32_t>(total2 >> 12);
    gte->MAC3 = static_cast<int32_t>(total3 >> 12);
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
}

// NCDS / NCDT / CDP variant: post-lighting (IR holds BK+LCM*L*V) and the
// pre-cue MAC is RGBC * IR scaled left by 4 (matching the spec's
// `[R*IR1, G*IR2, B*IR3] SHL 4` step).
static void color_depth_cue_from_lit_ir(GTEState* gte, bool lm) {
    uint8_t r0 = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g0 = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b0 = (gte->RGBC >> 16) & 0xFF;
    int64_t mac1 = (int64_t)(r0 << 4) * gte->IR1;
    int64_t mac2 = (int64_t)(g0 << 4) * gte->IR2;
    int64_t mac3 = (int64_t)(b0 << 4) * gte->IR3;
    color_depth_cue_finish(gte, mac1, mac2, mac3, lm);
}

// ---------------------------------------------------------------------------
// RTPS — Perspective Transformation (internal, operates on given vertex V)
// ---------------------------------------------------------------------------
void gte_rtps_internal(GTEState* gte, int16_t* V, bool setMac0) {
    // Step 1: Matrix multiplication + translation
    int64_t mac1 = ((int64_t)gte->TR[0] << 12) +
                   (int64_t)gte->RT[0][0] * V[0] +
                   (int64_t)gte->RT[0][1] * V[1] +
                   (int64_t)gte->RT[0][2] * V[2];
    int64_t mac2 = ((int64_t)gte->TR[1] << 12) +
                   (int64_t)gte->RT[1][0] * V[0] +
                   (int64_t)gte->RT[1][1] * V[1] +
                   (int64_t)gte->RT[1][2] * V[2];
    int64_t mac3 = ((int64_t)gte->TR[2] << 12) +
                   (int64_t)gte->RT[2][0] * V[0] +
                   (int64_t)gte->RT[2][1] * V[1] +
                   (int64_t)gte->RT[2][2] * V[2];

    gte->check_mac_overflow(mac1 >> 12, 1);
    gte->check_mac_overflow(mac2 >> 12, 2);
    gte->check_mac_overflow(mac3 >> 12, 3);
    gte->MAC1 = static_cast<int32_t>(mac1 >> 12);
    gte->MAC2 = static_cast<int32_t>(mac2 >> 12);
    gte->MAC3 = static_cast<int32_t>(mac3 >> 12);

    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, false);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, false);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, false);

    // Step 2: Push SZ FIFO
    gte->push_sz(gte->MAC3);

    /* --fov GTE H override — see core/features/widescreen_gte_inline.h.
     * 0 sentinel reduces to native PSX GTE math. (The Hor+ OFX widening
     * that used to sit next to this was retired 2026-07-06 with the
     * PsyCross-mode --aspect substitution stack; projection is always
     * canonical now.) */
    int32_t eff_H   = widescreen_eff_h((int32_t)gte->H);

    // Step 3: Perspective division
    int32_t h_div_sz = gte_divide((uint16_t)eff_H, gte->SZ[3], gte->FLAG);

    // Step 4: Project to screen coordinates
    int64_t sx = ((int64_t)gte->OFX + (int64_t)gte->IR1 * h_div_sz) >> 16;
    int64_t sy = (gte->OFY + (int64_t)gte->IR2 * h_div_sz) >> 16;
    gte->push_sxy(sx, sy);

    /* OPENPETE_RTPS_BOXLOG="x0,y0,x1,y1,frame": temp forensics — log the caller
     * PC of every RTPS whose screen point lands in the box on that frame. */
    {
        static int s_bl = -2; static int bx0, by0, bx1, by1, bfr;
        if (s_bl == -2) {
            const char* e = getenv("OPENPETE_RTPS_BOXLOG");
            s_bl = (e && sscanf(e, "%d,%d,%d,%d,%d", &bx0,&by0,&bx1,&by1,&bfr) == 5) ? 1 : -1;
        }
        if (s_bl == 1) {
            if ((int)openpete_frame == bfr && sx >= bx0 && sx <= bx1 && sy >= by0 && sy <= by1) {
                static unsigned s_bl_n = 0;
                if (s_bl_n < 4000) { s_bl_n++;
                    fprintf(stderr, "[rtps-box] pc=%08X sx=%d sy=%d sz3=%u\n",
                            openpete_gte_caller_pc, (int)sx, (int)sy, (unsigned)gte->SZ[3]);
                }
            }
        }
    }

    /* §13.16 audit — see ofx_census_record above. */
    {
        static int s_ofxc = -1;
        if (s_ofxc < 0) { const char* e = getenv("OPENPETE_OFX_CENSUS");
                          s_ofxc = (e && *e) ? 1 : 0; }
        if (s_ofxc && gte->OFX != (256 << 16))
            ofx_census_record(openpete_gte_caller_pc, (int32_t)gte->OFX,
                              /*render_pass=*/0, openpete_frame);
    }

    /* Side-channel canonical SX for openpete render→gameplay channel
     * decoupling. The mfc2 read of C2_SXY2 in handwritten asm truncates
     * SX to int16, losing the q16 fractional bits — so any host-side
     * "canonical 4:3 verdict" computed from delta_OFX has irreducible
     * 1-pixel ambiguity at boundaries (canonical SX=512 vs 511.x both
     * round to widened SX=597 with delta_OFX≈85.33). Stash the
     * pre-truncation canonical SX in q16 here, in a 3-deep FIFO matching
     * push_sxy semantics. Substitutions in r_particles.s read
     * `openpete_canonical_sx2_q16` (top of FIFO = latest projection) and
     * test it directly against the canonical 4:3 thresholds (0 and
     * 0x02000000) — exact verdict, no rounding. */
    int64_t canonical_sx_q16 = (int64_t)gte->OFX + (int64_t)gte->IR1 * h_div_sz;
    widescreen_push_canonical_sx_q16(canonical_sx_q16);

    // Step 5: Depth cueing (MAC0/IR0) — only for last vertex of RTPT or RTPS
    if (setMac0) {
        int64_t mac0 = (int64_t)gte->DQA * h_div_sz + gte->DQB;
        gte->check_mac0_overflow(mac0);
        gte->MAC0 = static_cast<int32_t>(mac0);
        gte->IR0 = gte->saturate_ir0(static_cast<int32_t>(mac0 >> 12));
    }
}

// RTPS (0x01) — single vertex, always sets MAC0
void gte_rtps(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_rtps_internal(gte, gte->V0, true);
    gte->set_error_flag();
}

// RTPT (0x30) — triple vertex, only last sets MAC0
void gte_rtpt(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_rtps_internal(gte, gte->V0, false);
    gte_rtps_internal(gte, gte->V1, false);
    gte_rtps_internal(gte, gte->V2, true);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCLIP (0x06) — Normal Clipping (2D cross product for backface culling)
// ---------------------------------------------------------------------------
void gte_nclip(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int16_t sx0 = static_cast<int16_t>(gte->SXY[0] & 0xFFFF);
    int16_t sy0 = static_cast<int16_t>(gte->SXY[0] >> 16);
    int16_t sx1 = static_cast<int16_t>(gte->SXY[1] & 0xFFFF);
    int16_t sy1 = static_cast<int16_t>(gte->SXY[1] >> 16);
    int16_t sx2 = static_cast<int16_t>(gte->SXY[2] & 0xFFFF);
    int16_t sy2 = static_cast<int16_t>(gte->SXY[2] >> 16);
    int64_t mac0 = (int64_t)sx0 * (sy1 - sy2) +
                   (int64_t)sx1 * (sy2 - sy0) +
                   (int64_t)sx2 * (sy0 - sy1);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// AVSZ3 (0x2D) — Average Z (3 points)
// ---------------------------------------------------------------------------
void gte_avsz3(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int64_t mac0 = (int64_t)gte->ZSF3 * (gte->SZ[1] + gte->SZ[2] + gte->SZ[3]);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0 >> 12);
    gte->OTZ = gte->saturate_sz(gte->MAC0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// AVSZ4 (0x2E) — Average Z (4 points)
// ---------------------------------------------------------------------------
void gte_avsz4(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int64_t mac0 = (int64_t)gte->ZSF4 * (gte->SZ[0] + gte->SZ[1] + gte->SZ[2] + gte->SZ[3]);
    gte->check_mac0_overflow(mac0);
    gte->MAC0 = static_cast<int32_t>(mac0 >> 12);
    gte->OTZ = gte->saturate_sz(gte->MAC0);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCCS (0x1B) — Normal Color Color Single
// ---------------------------------------------------------------------------
void gte_nccs_internal(GTEState* gte, int16_t* V, bool lm) {
    light_transform(gte, V, lm);
    light_color(gte, lm);
    color_output(gte, lm);
}

void gte_nccs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_nccs_internal(gte, gte->V0, (instr >> 10) & 1);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCCT (0x3F) — Normal Color Color Triple
// ---------------------------------------------------------------------------
void gte_ncct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    gte_nccs_internal(gte, gte->V0, lm);
    gte_nccs_internal(gte, gte->V1, lm);
    gte_nccs_internal(gte, gte->V2, lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCDS (0x13) — Normal Color Depth Cue Single
//
// Per Nocash spec / PsyCross reference:
//   IR  = LLM * V  SAR sf*12, lm
//   IR  = (BK*0x1000 + LCM * IR)  SAR sf*12, lm
//   MAC = (RGBC<<4) * IR + IR0 * sat_ir((FC*0x1000 - (RGBC<<4)*IR) SAR 12, lm=0)
//   MAC = MAC SAR sf*12
//   IR  = sat_ir(MAC, lm)
//   push RGB FIFO with sat_color(MAC>>4)
//
// The previous implementation interpolated IR (the lighting result) toward FC
// *before* multiplying by RGBC, which produced `(1-IR0)*RGBC*L + IR0*RGBC*FC`
// instead of the correct `(1-IR0)*RGBC*L + IR0*FC`. Visible in Spyro's intro
// cutscene as a vertex-color-tinted fog (snow mountains turning purple/cyan
// instead of reading as a clean blue atmospheric haze).
// ---------------------------------------------------------------------------
void gte_ncds_internal(GTEState* gte, int16_t* V, bool lm) {
    light_transform(gte, V, lm);
    light_color(gte, lm);
    color_depth_cue_from_lit_ir(gte, lm);
}

void gte_ncds(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    gte_ncds_internal(gte, gte->V0, lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCDT (0x16) — Normal Color Depth Cue Triple
// ---------------------------------------------------------------------------
void gte_ncdt(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    gte_ncds_internal(gte, gte->V0, lm);
    gte_ncds_internal(gte, gte->V1, lm);
    gte_ncds_internal(gte, gte->V2, lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCS (0x1E) — Normal Color Single (no vertex color multiply)
// ---------------------------------------------------------------------------
void gte_ncs_internal(GTEState* gte, int16_t* V, bool lm) {
    light_transform(gte, V, lm);
    light_color(gte, lm);
    // Output directly (no RGBC multiply)
    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));
}

void gte_ncs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    gte_ncs_internal(gte, gte->V0, (instr >> 10) & 1);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// NCT (0x20) — Normal Color Triple
// ---------------------------------------------------------------------------
void gte_nct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    gte_ncs_internal(gte, gte->V0, lm);
    gte_ncs_internal(gte, gte->V1, lm);
    gte_ncs_internal(gte, gte->V2, lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCS (0x10) — Depth Cueing Single (from RGBC color, no lighting)
// pre-cue MAC = RGBC << 16 (matches PsyCross's `(R << 16)`)
// ---------------------------------------------------------------------------
void gte_dpcs(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    uint8_t r = (gte->RGBC >> 0)  & 0xFF;
    uint8_t g = (gte->RGBC >> 8)  & 0xFF;
    uint8_t b = (gte->RGBC >> 16) & 0xFF;
    color_depth_cue_finish(gte,
                           (int64_t)r << 16, (int64_t)g << 16, (int64_t)b << 16,
                           lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCT (0x2A) — Depth Cueing Triple (RGB FIFO walk; new value is pushed each
// iteration, so RGB[0] *after* the prior push is the next input).
// ---------------------------------------------------------------------------
void gte_dpct(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    for (int i = 0; i < 3; i++) {
        uint8_t r = (gte->RGB[0] >> 0)  & 0xFF;
        uint8_t g = (gte->RGB[0] >> 8)  & 0xFF;
        uint8_t b = (gte->RGB[0] >> 16) & 0xFF;
        color_depth_cue_finish(gte,
                               (int64_t)r << 16, (int64_t)g << 16, (int64_t)b << 16,
                               lm);
    }
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// DPCL (0x29) — Depth Cueing Light (uses pre-loaded IR for lighting; NCDS
// without the LLM*V and BK+LCM*IR prelude).
// ---------------------------------------------------------------------------
void gte_dpcl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    color_depth_cue_from_lit_ir(gte, lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// INTPL (0x11) — Interpolation: pre-cue MAC = IR << 12.
// ---------------------------------------------------------------------------
void gte_intpl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    color_depth_cue_finish(gte,
                           (int64_t)gte->IR1 << 12,
                           (int64_t)gte->IR2 << 12,
                           (int64_t)gte->IR3 << 12,
                           lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// CDP (0x14) — Color Depth Cue (skip LLM*V; IR1/2/3 come in pre-loaded).
// Equivalent to NCDS minus the light_transform step.
// ---------------------------------------------------------------------------
void gte_cdp(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    light_color(gte, lm);
    color_depth_cue_from_lit_ir(gte, lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// CC (0x1C) — Color Color (light color + vertex color, no depth cue)
// ---------------------------------------------------------------------------
void gte_cc(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    bool lm = (instr >> 10) & 1;
    light_color(gte, lm);
    color_output(gte, lm);
    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// MVMVA (0x12) — Matrix Vector Multiply Add
// Highly configurable: matrix, vector, and translation selected by bits
// ---------------------------------------------------------------------------
void gte_mvmva(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    uint32_t mx = (instr >> 17) & 3;  // Matrix: 0=RT, 1=Light, 2=LightColor, 3=reserved
    uint32_t vv = (instr >> 15) & 3;  // Vector: 0=V0, 1=V1, 2=V2, 3=IR
    uint32_t tv = (instr >> 13) & 3;  // Translation: 0=TR, 1=BK, 2=FC/bugged, 3=none
    int sf = (instr >> 19) & 1;        // Shift: 0=no shift, 1=shift right 12

    // Select matrix
    int16_t M[3][3];
    switch (mx) {
        case 0: std::memcpy(M, gte->RT, sizeof(M)); break;
        case 1: std::memcpy(M, gte->L, sizeof(M)); break;
        case 2: std::memcpy(M, gte->LC, sizeof(M)); break;
        default: std::memset(M, 0, sizeof(M)); break; // Garbage on real HW
    }

    // Select vector
    int16_t V[3];
    switch (vv) {
        case 0: V[0] = gte->V0[0]; V[1] = gte->V0[1]; V[2] = gte->V0[2]; break;
        case 1: V[0] = gte->V1[0]; V[1] = gte->V1[1]; V[2] = gte->V1[2]; break;
        case 2: V[0] = gte->V2[0]; V[1] = gte->V2[1]; V[2] = gte->V2[2]; break;
        case 3: V[0] = gte->IR1;   V[1] = gte->IR2;   V[2] = gte->IR3;   break;
    }

    // Select translation vector
    int64_t T[3];
    switch (tv) {
        case 0: T[0] = (int64_t)gte->TR[0] << 12; T[1] = (int64_t)gte->TR[1] << 12; T[2] = (int64_t)gte->TR[2] << 12; break;
        case 1: T[0] = (int64_t)gte->BK[0] << 12; T[1] = (int64_t)gte->BK[1] << 12; T[2] = (int64_t)gte->BK[2] << 12; break;
        case 2: T[0] = (int64_t)gte->FC[0] << 12; T[1] = (int64_t)gte->FC[1] << 12; T[2] = (int64_t)gte->FC[2] << 12; break;
        case 3: T[0] = 0; T[1] = 0; T[2] = 0; break;
    }

    // Multiply
    int64_t mac1 = T[0] + (int64_t)M[0][0] * V[0] + (int64_t)M[0][1] * V[1] + (int64_t)M[0][2] * V[2];
    int64_t mac2 = T[1] + (int64_t)M[1][0] * V[0] + (int64_t)M[1][1] * V[1] + (int64_t)M[1][2] * V[2];
    int64_t mac3 = T[2] + (int64_t)M[2][0] * V[0] + (int64_t)M[2][1] * V[1] + (int64_t)M[2][2] * V[2];

    if (sf) {
        mac1 >>= 12; mac2 >>= 12; mac3 >>= 12;
    }

    gte->check_mac_overflow(mac1, 1);
    gte->check_mac_overflow(mac2, 2);
    gte->check_mac_overflow(mac3, 3);

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// OP (0x0C) — Outer Product of two vectors (cross product)
// ---------------------------------------------------------------------------
void gte_op(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;
    // D1=RT[0][0], D2=RT[1][1], D3=RT[2][2] (rotation matrix diagonal)
    int16_t d1 = gte->RT[0][0];
    int16_t d2 = gte->RT[1][1];
    int16_t d3 = gte->RT[2][2];

    int64_t mac1 = (int64_t)d2 * gte->IR3 - (int64_t)d3 * gte->IR2;
    int64_t mac2 = (int64_t)d3 * gte->IR1 - (int64_t)d1 * gte->IR3;
    int64_t mac3 = (int64_t)d1 * gte->IR2 - (int64_t)d2 * gte->IR1;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// SQR (0x28) — Square of IR vector
// ---------------------------------------------------------------------------
void gte_sqr(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = (int64_t)gte->IR1 * gte->IR1;
    int64_t mac2 = (int64_t)gte->IR2 * gte->IR2;
    int64_t mac3 = (int64_t)gte->IR3 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// GPF (0x3D) — General Purpose Interpolation (IR0 * IR)
// ---------------------------------------------------------------------------
void gte_gpf(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = (int64_t)gte->IR0 * gte->IR1;
    int64_t mac2 = (int64_t)gte->IR0 * gte->IR2;
    int64_t mac3 = (int64_t)gte->IR0 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// GPL (0x3E) — General Purpose Interpolation (MAC + IR0 * IR)
// ---------------------------------------------------------------------------
void gte_gpl(GTEState* gte, uint32_t instr) {
    gte->FLAG = 0;
    int sf = (instr >> 19) & 1;

    int64_t mac1 = ((int64_t)gte->MAC1 << (sf ? 12 : 0)) + (int64_t)gte->IR0 * gte->IR1;
    int64_t mac2 = ((int64_t)gte->MAC2 << (sf ? 12 : 0)) + (int64_t)gte->IR0 * gte->IR2;
    int64_t mac3 = ((int64_t)gte->MAC3 << (sf ? 12 : 0)) + (int64_t)gte->IR0 * gte->IR3;

    if (sf) { mac1 >>= 12; mac2 >>= 12; mac3 >>= 12; }

    gte->MAC1 = static_cast<int32_t>(mac1);
    gte->MAC2 = static_cast<int32_t>(mac2);
    gte->MAC3 = static_cast<int32_t>(mac3);

    bool lm = (instr >> 10) & 1;
    gte->IR1 = gte->saturate_ir(gte->MAC1, 1, lm);
    gte->IR2 = gte->saturate_ir(gte->MAC2, 2, lm);
    gte->IR3 = gte->saturate_ir(gte->MAC3, 3, lm);

    gte->push_rgb(
        gte->saturate_color(gte->MAC1 >> 4, 0),
        gte->saturate_color(gte->MAC2 >> 4, 1),
        gte->saturate_color(gte->MAC3 >> 4, 2));

    gte->set_error_flag();
}

// ---------------------------------------------------------------------------
// MTC2 — Move To Coprocessor 2 (write GTE data register)
// ---------------------------------------------------------------------------
void gte_mtc2(GTEState* gte, uint8_t reg, uint32_t value) {
    switch (reg) {
        case 0:  gte->V0[0] = value & 0xFFFF; gte->V0[1] = value >> 16; break;
        case 1:  gte->V0[2] = value & 0xFFFF; break;
        case 2:  gte->V1[0] = value & 0xFFFF; gte->V1[1] = value >> 16; break;
        case 3:  gte->V1[2] = value & 0xFFFF; break;
        case 4:  gte->V2[0] = value & 0xFFFF; gte->V2[1] = value >> 16; break;
        case 5:  gte->V2[2] = value & 0xFFFF; break;
        case 6:  gte->RGBC = value; break;
        case 7:  gte->OTZ = value & 0xFFFF; break;
        case 8:  gte->IR0 = static_cast<int16_t>(value & 0xFFFF); break;
        case 9:  gte->IR1 = static_cast<int16_t>(value & 0xFFFF); break;
        case 10: gte->IR2 = static_cast<int16_t>(value & 0xFFFF); break;
        case 11: gte->IR3 = static_cast<int16_t>(value & 0xFFFF); break;
        case 12: gte->SXY[0] = value; break;
        case 13: gte->SXY[1] = value; break;
        case 14: gte->SXY[2] = value; gte->SXY[3] = value; break;
        case 15: // SXYP — push to FIFO
            gte->SXY[0] = gte->SXY[1]; gte->SXY[1] = gte->SXY[2];
            gte->SXY[2] = value; gte->SXY[3] = value; break;
        case 16: gte->SZ[0] = value & 0xFFFF; break;
        case 17: gte->SZ[1] = value & 0xFFFF; break;
        case 18: gte->SZ[2] = value & 0xFFFF; break;
        case 19: gte->SZ[3] = value & 0xFFFF; break;
        case 20: gte->RGB[0] = value; break;
        case 21: gte->RGB[1] = value; break;
        case 22: gte->RGB[2] = value; break;
        case 23: break; // RES1 reserved
        case 24: gte->MAC0 = value; break;
        case 25: gte->MAC1 = value; break;
        case 26: gte->MAC2 = value; break;
        case 27: gte->MAC3 = value; break;
        case 28: // IRGB — packed 5-bit RGB → IR1/IR2/IR3
            gte->IR1 = (value & 0x1F) << 7;
            gte->IR2 = ((value >> 5) & 0x1F) << 7;
            gte->IR3 = ((value >> 10) & 0x1F) << 7;
            break;
        case 29: break; // ORGB read-only
        case 30: // LZCS — set source and compute LZCR
            gte->LZCS = static_cast<int32_t>(value);
            {
                uint32_t v = value;
                if ((int32_t)value < 0) v = ~v;
                int cnt = 0;
                if (v == 0) { cnt = 32; }
                else { while (!(v & 0x80000000u)) { v <<= 1; cnt++; } }
                gte->LZCR = cnt;
            }
            break;
        case 31: break; // LZCR read-only
        default: break;
    }
}

// ---------------------------------------------------------------------------
// MFC2 — Move From Coprocessor 2 (read GTE data register)
// ---------------------------------------------------------------------------
uint32_t gte_mfc2(GTEState* gte, uint8_t reg) {
    switch (reg) {
        case 0:  return (static_cast<uint16_t>(gte->V0[1]) << 16) | static_cast<uint16_t>(gte->V0[0]);
        case 1:  return static_cast<uint16_t>(gte->V0[2]);
        case 2:  return (static_cast<uint16_t>(gte->V1[1]) << 16) | static_cast<uint16_t>(gte->V1[0]);
        case 3:  return static_cast<uint16_t>(gte->V1[2]);
        case 4:  return (static_cast<uint16_t>(gte->V2[1]) << 16) | static_cast<uint16_t>(gte->V2[0]);
        case 5:  return static_cast<uint16_t>(gte->V2[2]);
        case 6:  return gte->RGBC;
        case 7:  return gte->OTZ;
        case 8:  return static_cast<int32_t>(gte->IR0);  // sign-extend
        case 9:  return static_cast<int32_t>(gte->IR1);
        case 10: return static_cast<int32_t>(gte->IR2);
        case 11: return static_cast<int32_t>(gte->IR3);
        case 12: return gte->SXY[0];
        case 13: return gte->SXY[1];
        case 14: return gte->SXY[2];
        case 15: return gte->SXY[3];
        case 16: return gte->SZ[0];
        case 17: return gte->SZ[1];
        case 18: return gte->SZ[2];
        case 19: return gte->SZ[3];
        case 20: return gte->RGB[0];
        case 21: return gte->RGB[1];
        case 22: return gte->RGB[2];
        case 23: return 0; // RES1
        case 24: return gte->MAC0;
        case 25: return gte->MAC1;
        case 26: return gte->MAC2;
        case 27: return gte->MAC3;
        case 28: case 29: { // IRGB/ORGB
            uint32_t r = std::clamp((int)(gte->IR1 >> 7), 0, 0x1F);
            uint32_t g = std::clamp((int)(gte->IR2 >> 7), 0, 0x1F);
            uint32_t b = std::clamp((int)(gte->IR3 >> 7), 0, 0x1F);
            return (b << 10) | (g << 5) | r;
        }
        case 30: return gte->LZCS;
        case 31: return gte->LZCR;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// CTC2 — Move To Coprocessor 2 Control (write GTE control register)
// ---------------------------------------------------------------------------
void gte_ctc2(GTEState* gte, uint8_t reg, uint32_t value) {
    switch (reg) {
        case 0:  gte->RT[0][0] = value & 0xFFFF; gte->RT[0][1] = value >> 16; break;
        case 1:  gte->RT[0][2] = value & 0xFFFF; gte->RT[1][0] = value >> 16; break;
        case 2:  gte->RT[1][1] = value & 0xFFFF; gte->RT[1][2] = value >> 16; break;
        case 3:  gte->RT[2][0] = value & 0xFFFF; gte->RT[2][1] = value >> 16; break;
        case 4:  gte->RT[2][2] = value & 0xFFFF; break;
        case 5:  gte->TR[0] = value; break;
        case 6:  gte->TR[1] = value; break;
        case 7:  gte->TR[2] = value; break;
        case 8:  gte->L[0][0] = value & 0xFFFF; gte->L[0][1] = value >> 16; break;
        case 9:  gte->L[0][2] = value & 0xFFFF; gte->L[1][0] = value >> 16; break;
        case 10: gte->L[1][1] = value & 0xFFFF; gte->L[1][2] = value >> 16; break;
        case 11: gte->L[2][0] = value & 0xFFFF; gte->L[2][1] = value >> 16; break;
        case 12: gte->L[2][2] = value & 0xFFFF; break;
        case 13: gte->BK[0] = value; break;
        case 14: gte->BK[1] = value; break;
        case 15: gte->BK[2] = value; break;
        case 16: gte->LC[0][0] = value & 0xFFFF; gte->LC[0][1] = value >> 16; break;
        case 17: gte->LC[0][2] = value & 0xFFFF; gte->LC[1][0] = value >> 16; break;
        case 18: gte->LC[1][1] = value & 0xFFFF; gte->LC[1][2] = value >> 16; break;
        case 19: gte->LC[2][0] = value & 0xFFFF; gte->LC[2][1] = value >> 16; break;
        case 20: gte->LC[2][2] = value & 0xFFFF; break;
        case 21: gte->FC[0] = value; break;
        case 22: gte->FC[1] = value; break;
        case 23: gte->FC[2] = value; break;
        case 24: gte->OFX = value; break;
        case 26: gte->H = value & 0xFFFF; break;
        case 25: gte->OFY = value; break;
        case 27: gte->DQA = static_cast<int16_t>(value & 0xFFFF); break;
        case 28: gte->DQB = value; break;
        case 29: gte->ZSF3 = static_cast<int16_t>(value & 0xFFFF); break;
        case 30: gte->ZSF4 = static_cast<int16_t>(value & 0xFFFF); break;
        case 31: gte->FLAG = value & 0x7FFFF000u; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// CFC2 — Move From Coprocessor 2 Control (read GTE control register)
// ---------------------------------------------------------------------------
uint32_t gte_cfc2(GTEState* gte, uint8_t reg) {
    switch (reg) {
        case 0:  return (static_cast<uint16_t>(gte->RT[0][1]) << 16) | static_cast<uint16_t>(gte->RT[0][0]);
        case 1:  return (static_cast<uint16_t>(gte->RT[1][0]) << 16) | static_cast<uint16_t>(gte->RT[0][2]);
        case 2:  return (static_cast<uint16_t>(gte->RT[1][2]) << 16) | static_cast<uint16_t>(gte->RT[1][1]);
        case 3:  return (static_cast<uint16_t>(gte->RT[2][1]) << 16) | static_cast<uint16_t>(gte->RT[2][0]);
        case 4:  return static_cast<uint16_t>(gte->RT[2][2]);
        case 5:  return gte->TR[0];
        case 6:  return gte->TR[1];
        case 7:  return gte->TR[2];
        case 8:  return (static_cast<uint16_t>(gte->L[0][1]) << 16) | static_cast<uint16_t>(gte->L[0][0]);
        case 9:  return (static_cast<uint16_t>(gte->L[1][0]) << 16) | static_cast<uint16_t>(gte->L[0][2]);
        case 10: return (static_cast<uint16_t>(gte->L[1][2]) << 16) | static_cast<uint16_t>(gte->L[1][1]);
        case 11: return (static_cast<uint16_t>(gte->L[2][1]) << 16) | static_cast<uint16_t>(gte->L[2][0]);
        case 12: return static_cast<uint16_t>(gte->L[2][2]);
        case 13: return gte->BK[0];
        case 14: return gte->BK[1];
        case 15: return gte->BK[2];
        case 16: return (static_cast<uint16_t>(gte->LC[0][1]) << 16) | static_cast<uint16_t>(gte->LC[0][0]);
        case 17: return (static_cast<uint16_t>(gte->LC[1][0]) << 16) | static_cast<uint16_t>(gte->LC[0][2]);
        case 18: return (static_cast<uint16_t>(gte->LC[1][2]) << 16) | static_cast<uint16_t>(gte->LC[1][1]);
        case 19: return (static_cast<uint16_t>(gte->LC[2][1]) << 16) | static_cast<uint16_t>(gte->LC[2][0]);
        case 20: return static_cast<uint16_t>(gte->LC[2][2]);
        case 21: return gte->FC[0];
        case 22: return gte->FC[1];
        case 23: return gte->FC[2];
        case 24: return gte->OFX;
        case 25: return gte->OFY;
        case 26: return gte->H;
        case 27: return static_cast<int32_t>(gte->DQA); // sign-extend
        case 28: return gte->DQB;
        case 29: return static_cast<int32_t>(gte->ZSF3);
        case 30: return static_cast<int32_t>(gte->ZSF4);
        case 31: return gte->FLAG;
        default: return 0;
    }
}

} // namespace GTE
} // namespace PSXRecomp

// ---------------------------------------------------------------------------
// GTE trace logger — emits a CSV with three rows per logged op: psxrecomp's
// own GTE ("rec"), PsyCross's GTE ("psy"), and pcsx-redux's GTE ("pcsx") run
// on identical pre-state. Three independent codebases on the same canonical
// CP2D/CP2C register layout — divergence between any pair flags a bug in
// one engine.
//
// Used to validate the GTE depth-cue + bit-exact divide rewrite (commit
// 9467e0e) against the user-reported titlescreen-cutscene mountain-peak
// flicker. Keep the logger around as a regression check for any future GTE
// math change.
//
// Activate with env vars (all read once on first call):
//   OPENPETE_GTE_TRACE     path to output CSV (no env = disabled)
//   OPENPETE_GTE_TRACE_LO  inclusive lower openpete_frame (default 1100)
//   OPENPETE_GTE_TRACE_HI  exclusive upper openpete_frame (default 1111)
//   OPENPETE_GTE_TRACE_OPS comma-sep hex opcodes, default RTPS/NCLIP/RTPT/
//                          AVSZ3/AVSZ4 ("01,06,30,2D,2E")
//
// Diff with: awk -F, '$2=="rec"' trace.csv > rec; '$2=="psy"' > psy;
//            '$2=="pcsx"' > pcsx; diff rec psy; diff rec pcsx.
// ---------------------------------------------------------------------------
// PsyCross GTERegisters / GTE_operator pulled in via psx/gtereg.h above.

extern "C" void pcsxr_gte_run(uint32_t cmd,
                              const uint32_t pre_data[32],
                              const uint32_t pre_ctrl[32],
                              uint32_t       post_data[32],
                              uint32_t       post_ctrl[32]);

namespace {

struct GTETraceCfg {
    FILE*    fp        = nullptr;
    int      enabled   = -1; // -1 = uninit, 0 = disabled, 1 = enabled
    unsigned frame_lo  = 1100u;
    unsigned frame_hi  = 1111u;
    uint64_t op_mask   = 0; // bit i set = log opcode i
};
GTETraceCfg g_gte_trace;

uint64_t parse_op_mask(const char* s) {
    uint64_t m = 0;
    if (!s) return 0;
    while (*s) {
        char* end;
        unsigned long v = std::strtoul(s, &end, 16);
        if (v < 64) m |= (1ull << v);
        if (*end == ',') s = end + 1;
        else break;
    }
    return m;
}

FILE* gte_trace_init() {
    if (g_gte_trace.enabled == 0) return nullptr;
    if (g_gte_trace.enabled == 1) return g_gte_trace.fp;
    const char* path = std::getenv("OPENPETE_GTE_TRACE");
    if (!path || !*path) { g_gte_trace.enabled = 0; return nullptr; }
    if (const char* lo = std::getenv("OPENPETE_GTE_TRACE_LO"))
        g_gte_trace.frame_lo = (unsigned)std::strtoul(lo, nullptr, 0);
    if (const char* hi = std::getenv("OPENPETE_GTE_TRACE_HI"))
        g_gte_trace.frame_hi = (unsigned)std::strtoul(hi, nullptr, 0);
    if (const char* ops = std::getenv("OPENPETE_GTE_TRACE_OPS"))
        g_gte_trace.op_mask = parse_op_mask(ops);
    if (!g_gte_trace.op_mask) {
        g_gte_trace.op_mask = (1ull << 0x01) | (1ull << 0x06) |
                              (1ull << 0x30) | (1ull << 0x2D) | (1ull << 0x2E);
    }
    g_gte_trace.fp = std::fopen(path, "w");
    if (!g_gte_trace.fp) { g_gte_trace.enabled = 0; return nullptr; }
    std::fprintf(g_gte_trace.fp,
        "frame,engine,op,vidx,"
        "Vx,Vy,Vz,"
        "TRx,TRy,TRz,"
        "RT00,RT01,RT02,RT10,RT11,RT12,RT20,RT21,RT22,"
        "OFX,OFY,H,DQA,DQB,"
        "MAC0,MAC1,MAC2,MAC3,IR0,IR1,IR2,IR3,"
        "SZ0,SZ1,SZ2,SZ3,"
        "SXY0,SXY1,SXY2,"
        "RGB0,RGB1,RGB2,"
        "FLAG\n");
    std::fprintf(stdout, "[GTE-TRACE] writing to %s frames [%u,%u) ops=0x%llX\n",
                 path, g_gte_trace.frame_lo, g_gte_trace.frame_hi,
                 (unsigned long long)g_gte_trace.op_mask);
    std::fflush(stdout);
    g_gte_trace.enabled = 1;
    return g_gte_trace.fp;
}

// Decode one register file (CP2D[32] + CP2C[32] in raw mtc2 form) into the
// individual fields we want to log. This is the canonical PSX register
// layout, identical between psxrecomp and PsyCross.
struct DecodedGTE {
    int16_t  V[3][3];
    int32_t  TR[3];
    int16_t  RT[3][3];
    int32_t  OFX, OFY;
    uint16_t H;
    int16_t  DQA;
    int32_t  DQB;
    int32_t  MAC[4];
    int16_t  IR[4];
    uint16_t SZ[4];
    uint32_t SXY[3];
    uint32_t RGB[3];
    uint32_t FLAG;
};

void decode_gte(DecodedGTE& d, const uint32_t* CP2D, const uint32_t* CP2C) {
    auto lo16 = [](uint32_t w) { return (int16_t)(w & 0xFFFFu); };
    auto hi16 = [](uint32_t w) { return (int16_t)((w >> 16) & 0xFFFFu); };
    d.V[0][0] = lo16(CP2D[0]); d.V[0][1] = hi16(CP2D[0]); d.V[0][2] = lo16(CP2D[1]);
    d.V[1][0] = lo16(CP2D[2]); d.V[1][1] = hi16(CP2D[2]); d.V[1][2] = lo16(CP2D[3]);
    d.V[2][0] = lo16(CP2D[4]); d.V[2][1] = hi16(CP2D[4]); d.V[2][2] = lo16(CP2D[5]);
    d.RT[0][0] = lo16(CP2C[0]); d.RT[0][1] = hi16(CP2C[0]); d.RT[0][2] = lo16(CP2C[1]);
    d.RT[1][0] = hi16(CP2C[1]); d.RT[1][1] = lo16(CP2C[2]); d.RT[1][2] = hi16(CP2C[2]);
    d.RT[2][0] = lo16(CP2C[3]); d.RT[2][1] = hi16(CP2C[3]); d.RT[2][2] = lo16(CP2C[4]);
    d.TR[0] = (int32_t)CP2C[5]; d.TR[1] = (int32_t)CP2C[6]; d.TR[2] = (int32_t)CP2C[7];
    d.OFX = (int32_t)CP2C[24]; d.OFY = (int32_t)CP2C[25];
    d.H   = (uint16_t)(CP2C[26] & 0xFFFFu);
    d.DQA = lo16(CP2C[27]); d.DQB = (int32_t)CP2C[28];
    d.MAC[0] = (int32_t)CP2D[24]; d.MAC[1] = (int32_t)CP2D[25];
    d.MAC[2] = (int32_t)CP2D[26]; d.MAC[3] = (int32_t)CP2D[27];
    d.IR[0] = lo16(CP2D[8]);  d.IR[1] = lo16(CP2D[9]);
    d.IR[2] = lo16(CP2D[10]); d.IR[3] = lo16(CP2D[11]);
    d.SZ[0] = (uint16_t)(CP2D[16] & 0xFFFFu);
    d.SZ[1] = (uint16_t)(CP2D[17] & 0xFFFFu);
    d.SZ[2] = (uint16_t)(CP2D[18] & 0xFFFFu);
    d.SZ[3] = (uint16_t)(CP2D[19] & 0xFFFFu);
    d.SXY[0] = CP2D[12]; d.SXY[1] = CP2D[13]; d.SXY[2] = CP2D[14];
    d.RGB[0] = CP2D[20]; d.RGB[1] = CP2D[21]; d.RGB[2] = CP2D[22];
    d.FLAG = CP2C[31];
}

void gte_trace_emit(FILE* fp, const DecodedGTE& d, const char* engine,
                    const char* op_label, int vidx) {
    int vi = (vidx < 0 || vidx > 2) ? 0 : vidx;
    std::fprintf(fp,
        "%u,%s,%s,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,%u,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,"
        "%u,%u,%u,%u,"
        "0x%08X,0x%08X,0x%08X,"
        "0x%08X,0x%08X,0x%08X,"
        "0x%08X\n",
        openpete_frame, engine, op_label, vidx,
        d.V[vi][0], d.V[vi][1], d.V[vi][2],
        d.TR[0], d.TR[1], d.TR[2],
        d.RT[0][0], d.RT[0][1], d.RT[0][2],
        d.RT[1][0], d.RT[1][1], d.RT[1][2],
        d.RT[2][0], d.RT[2][1], d.RT[2][2],
        d.OFX, d.OFY, (unsigned)d.H, d.DQA, d.DQB,
        d.MAC[0], d.MAC[1], d.MAC[2], d.MAC[3],
        d.IR[0], d.IR[1], d.IR[2], d.IR[3],
        (unsigned)d.SZ[0], (unsigned)d.SZ[1], (unsigned)d.SZ[2], (unsigned)d.SZ[3],
        d.SXY[0], d.SXY[1], d.SXY[2],
        d.RGB[0], d.RGB[1], d.RGB[2],
        d.FLAG);
}

void emit_op_rows(FILE* fp, const DecodedGTE& d_rec, const DecodedGTE& d_psy,
                  const DecodedGTE& d_pcsx, uint8_t func) {
    auto triple = [&](const char* op, int vidx) {
        gte_trace_emit(fp, d_rec,  "rec",  op, vidx);
        gte_trace_emit(fp, d_psy,  "psy",  op, vidx);
        gte_trace_emit(fp, d_pcsx, "pcsx", op, vidx);
    };
    switch (func) {
        case 0x01: triple("RTPS",    0); break;
        case 0x30: triple("RTPT_v0", 0);
                   triple("RTPT_v1", 1);
                   triple("RTPT_v2", 2); break;
        case 0x06: triple("NCLIP",  -1); break;
        case 0x2D: triple("AVSZ3",  -1); break;
        case 0x2E: triple("AVSZ4",  -1); break;
        case 0x0C: triple("OP",     -1); break;
        case 0x10: triple("DPCS",   -1); break;
        case 0x11: triple("INTPL",  -1); break;
        case 0x12: triple("MVMVA",  -1); break;
        case 0x13: triple("NCDS",    0); break;
        case 0x14: triple("CDP",    -1); break;
        case 0x16: triple("NCDT_v0", 0);
                   triple("NCDT_v1", 1);
                   triple("NCDT_v2", 2); break;
        case 0x1B: triple("NCCS",    0); break;
        case 0x1C: triple("CC",     -1); break;
        case 0x1E: triple("NCS",     0); break;
        case 0x20: triple("NCT_v0",  0);
                   triple("NCT_v1",  1);
                   triple("NCT_v2",  2); break;
        case 0x28: triple("SQR",    -1); break;
        case 0x29: triple("DPCL",   -1); break;
        case 0x2A: triple("DPCT",   -1); break;
        case 0x3D: triple("GPF",    -1); break;
        case 0x3E: triple("GPL",    -1); break;
        case 0x3F: triple("NCCT_v0", 0);
                   triple("NCCT_v1", 1);
                   triple("NCCT_v2", 2); break;
        default:
            triple("UNK",  -1); break;
    }
}

void gte_trace_log_post(uint8_t func, uint32_t cmd,
                        const uint32_t pre_data[32], const uint32_t pre_ctrl[32],
                        const uint32_t post_data[32], const uint32_t post_ctrl[32]) {
    FILE* fp = gte_trace_init();
    if (!fp) return;
    if (openpete_frame < g_gte_trace.frame_lo) return;
    if (openpete_frame >= g_gte_trace.frame_hi) {
        if (g_gte_trace.enabled == 1) {
            std::fflush(fp);
            std::fclose(fp);
            g_gte_trace.fp = nullptr;
            g_gte_trace.enabled = 0;
            std::fprintf(stdout, "[GTE-TRACE] window closed at frame %u\n", openpete_frame);
            std::fflush(stdout);
        }
        return;
    }
    if (func >= 64 || !((g_gte_trace.op_mask >> func) & 1ull)) return;

    DecodedGTE d_rec;
    decode_gte(d_rec, post_data, post_ctrl);

    // Run PsyCross's GTE on the same pre-state. gteRegs is global; save +
    // restore the full copy so we don't perturb any other PsyCross consumer.
    // (At time of writing, openpete doesn't actually use PsyCross's GTE for
    // transform — only its GPU; so the save/restore is defensive.)
    DecodedGTE d_psy{};
    {
        unsigned int saved_d[32], saved_c[32];
        std::memcpy(saved_d, gteRegs.CP2D.r, sizeof(saved_d));
        std::memcpy(saved_c, gteRegs.CP2C.r, sizeof(saved_c));
        std::memcpy(gteRegs.CP2D.r, pre_data, sizeof(saved_d));
        std::memcpy(gteRegs.CP2C.r, pre_ctrl, sizeof(saved_c));
        GTE_operator((int)cmd);
        decode_gte(d_psy, gteRegs.CP2D.r, gteRegs.CP2C.r);
        std::memcpy(gteRegs.CP2D.r, saved_d, sizeof(saved_d));
        std::memcpy(gteRegs.CP2C.r, saved_c, sizeof(saved_c));
    }

    // Run pcsx-redux's GTE on the same pre-state. The vendored runner.cpp
    // owns a private register file, so no save/restore needed.
    DecodedGTE d_pcsx{};
    {
        uint32_t pcsx_data[32], pcsx_ctrl[32];
        pcsxr_gte_run(cmd, pre_data, pre_ctrl, pcsx_data, pcsx_ctrl);
        decode_gte(d_pcsx, pcsx_data, pcsx_ctrl);
    }

    emit_op_rows(fp, d_rec, d_psy, d_pcsx, func);
}
} // namespace

// ---------------------------------------------------------------------------
// gte_execute — C-linkage bridge called from generated code
// ---------------------------------------------------------------------------

/* fps-unlock Phase 2 / PGXP-fix diagnostic: counters used to verify
 * what the recomp's gte_execute sees vs what PsyCross's OT walker
 * sees. Gated by OPENPETE_RENDER_PROBE=1. The expensive per-frame
 * RTPS/RTPT sample dump was dropped after we confirmed PsyCross's
 * OT-walker counts disagree with the AddPrim function-call counts
 * (because the game uses the inline addPrim() macro, not the
 * function). Counters reset per frame from PsyX_EndScene. */
extern "C" {
    int      g_openpete_render_probe_active = -1;  /* lazy-init from env */
    unsigned g_openpete_render_probe_count_rtps  = 0u;
    unsigned g_openpete_render_probe_count_rtpt  = 0u;
    unsigned g_openpete_render_probe_count_mvmva = 0u;
    unsigned g_openpete_render_probe_count_other = 0u;
    unsigned g_openpete_render_probe_count_matrix_changes = 0u;
    int16_t  g_openpete_render_probe_last_rt[3][3] = {{0}};
    int32_t  g_openpete_render_probe_last_tr[3] = {0};
}

static inline int render_probe_enabled() {
    if (g_openpete_render_probe_active < 0) {
        const char* e = std::getenv("OPENPETE_RENDER_PROBE");
        g_openpete_render_probe_active = (e && *e == '1') ? 1 : 0;
    }
    return g_openpete_render_probe_active;
}

extern "C" void gte_execute(CPUState* cpu, uint32_t cmd) {
    using namespace PSXRecomp::GTE;

    /* OPENPETE_GTE_PCHIST=<frame>: one-frame caller-PC histogram (temp forensics). */
    {
        static int s_hist_frame = -2;
        if (s_hist_frame == -2) {
            const char* e = getenv("OPENPETE_GTE_PCHIST");
            s_hist_frame = (e && *e) ? atoi(e) : -1;
        }
        if (s_hist_frame >= 0) {
            static uint32_t pcs[4096]; static uint32_t cnt[4096]; static int n = 0; static int dumped = 0;
            if ((int)openpete_frame == s_hist_frame) {
                uint32_t pc = openpete_gte_caller_pc;
                int i; for (i = 0; i < n; i++) if (pcs[i] == pc) { cnt[i]++; break; }
                if (i == n && n < 4096) { pcs[n] = pc; cnt[n] = 1; n++; }
            } else if ((int)openpete_frame > s_hist_frame && !dumped && n) {
                dumped = 1;
                for (int i = 0; i < n; i++)
                    fprintf(stderr, "[gtehist] pc=%08X n=%u cmd?\n", pcs[i], cnt[i]);
            }
        }
    }

    /* Snapshot raw pre-state for trace logger before any decode side-effects */
    uint32_t pre_data[32], pre_ctrl[32];
    std::memcpy(pre_data, cpu->gte_data, sizeof(pre_data));
    std::memcpy(pre_ctrl, cpu->gte_ctrl, sizeof(pre_ctrl));

    GTEState gte;
    // Skip reg 15 (SXYP: push-write, would corrupt SXY FIFO) and
    // reg 28 (IRGB: overwrites IR1/2/3 with lossy 5-bit values; use regs 9-11 instead)
    for (int i = 0; i < 32; i++) {
        if (i == 15 || i == 28) continue;
        gte_mtc2(&gte, i, cpu->gte_data[i]);
    }
    for (int i = 0; i < 32; i++) gte_ctc2(&gte, i, cpu->gte_ctrl[i]);

    uint8_t func = cmd & 0x3F;
    switch (func) {
        case 0x01: gte_rtps(&gte, cmd); break;
        case 0x06: gte_nclip(&gte, cmd); break;
        case 0x0C: gte_op(&gte, cmd); break;
        case 0x10: gte_dpcs(&gte, cmd); break;
        case 0x11: gte_intpl(&gte, cmd); break;
        case 0x12: gte_mvmva(&gte, cmd); break;
        case 0x13: gte_ncds(&gte, cmd); break;
        case 0x14: gte_cdp(&gte, cmd); break;
        case 0x16: gte_ncdt(&gte, cmd); break;
        case 0x1B: gte_nccs(&gte, cmd); break;
        case 0x1C: gte_cc(&gte, cmd); break;
        case 0x1E: gte_ncs(&gte, cmd); break;
        case 0x20: gte_nct(&gte, cmd); break;
        case 0x28: gte_sqr(&gte, cmd); break;
        case 0x29: gte_dpcl(&gte, cmd); break;
        case 0x2A: gte_dpct(&gte, cmd); break;
        case 0x2D: gte_avsz3(&gte, cmd); break;
        case 0x2E: gte_avsz4(&gte, cmd); break;
        case 0x30: gte_rtpt(&gte, cmd); break;
        case 0x3D: gte_gpf(&gte, cmd); break;
        case 0x3E: gte_gpl(&gte, cmd); break;
        case 0x3F: gte_ncct(&gte, cmd); break;
        default:
            static int s_unk = 0;
            if (++s_unk <= 20)
                printf("[GTE] unimplemented cmd=0x%02X (full=0x%07X)\n", func, cmd);
            break;
    }

    for (int i = 0; i < 32; i++) cpu->gte_data[i] = gte_mfc2(&gte, i);
    for (int i = 0; i < 32; i++) cpu->gte_ctrl[i] = gte_cfc2(&gte, i);

    gte_trace_log_post(func, cmd, pre_data, pre_ctrl,
                       cpu->gte_data, cpu->gte_ctrl);

    /* fps-unlock Phase 2 probe — see comment block above. */
    if (render_probe_enabled()) {
        if      (func == 0x01) g_openpete_render_probe_count_rtps++;
        else if (func == 0x30) g_openpete_render_probe_count_rtpt++;
        else if (func == 0x12) g_openpete_render_probe_count_mvmva++;
        else                   g_openpete_render_probe_count_other++;
        bool matrix_change =
            (std::memcmp(gte.RT, g_openpete_render_probe_last_rt, sizeof(gte.RT)) != 0) ||
            (gte.TR[0] != g_openpete_render_probe_last_tr[0]) ||
            (gte.TR[1] != g_openpete_render_probe_last_tr[1]) ||
            (gte.TR[2] != g_openpete_render_probe_last_tr[2]);
        if (matrix_change && (func == 0x01 || func == 0x30 || func == 0x12)) {
            g_openpete_render_probe_count_matrix_changes++;
            std::memcpy(g_openpete_render_probe_last_rt, gte.RT, sizeof(gte.RT));
            g_openpete_render_probe_last_tr[0] = gte.TR[0];
            g_openpete_render_probe_last_tr[1] = gte.TR[1];
            g_openpete_render_probe_last_tr[2] = gte.TR[2];
        }
    }

    /* Diagnostic: log when RTPS/RTPT produces FLAG error bit (bit 31).
     * Print at most one sample per frame so a single early-boot burst can't
     * starve the rate limit before later-frame rendering happens. */
    extern uint32_t g_ps1_frame;
    if ((func == 0x01 || func == 0x30) && (cpu->gte_ctrl[31] & 0x80000000u)) {
        static int s_flag_err = 0;
        static uint32_t s_last_frame = 0xFFFFFFFFu;
        if (g_ps1_frame != s_last_frame && ++s_flag_err <= 60) {
            s_last_frame = g_ps1_frame;
            printf("[GTE-FLAG] RTPS err #%d f%u FLAG=0x%08X "
                   "V0=(%d,%d,%d) TR=(%d,%d,%d) H=%u SZ3=%u "
                   "MAC=(%d,%d,%d) IR=(%d,%d,%d) SXY=0x%08X\n",
                   s_flag_err, g_ps1_frame, cpu->gte_ctrl[31],
                   gte.V0[0], gte.V0[1], gte.V0[2],
                   gte.TR[0], gte.TR[1], gte.TR[2],
                   gte.H, gte.SZ[3],
                   gte.MAC1, gte.MAC2, gte.MAC3,
                   gte.IR1, gte.IR2, gte.IR3,
                   cpu->gte_data[14]);
            fflush(stdout);
        }
    }
}

/* C-callable wrappers for GTE register transfers (used by overlay interpreter) */
// Helper: load GTEState from CPUState, skipping push-write and lossy registers
static void gte_load_data(PSXRecomp::GTE::GTEState& gte, CPUState* cpu) {
    using namespace PSXRecomp::GTE;
    for (int i = 0; i < 32; i++) {
        if (i == 15 || i == 28) continue;  // SXYP (push) and IRGB (lossy overwrite)
        gte_mtc2(&gte, i, cpu->gte_data[i]);
    }
    for (int i = 0; i < 32; i++) gte_ctc2(&gte, i, cpu->gte_ctrl[i]);
}

extern "C" uint32_t gte_read_data(CPUState* cpu, uint8_t reg) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    gte_load_data(gte, cpu);
    return gte_mfc2(&gte, reg);
}

extern "C" uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    gte_load_data(gte, cpu);
    return gte_cfc2(&gte, reg);
}

extern "C" void gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    gte_load_data(gte, cpu);
    gte_mtc2(&gte, reg, val);
    for (int i = 0; i < 32; i++) cpu->gte_data[i] = gte_mfc2(&gte, i);
    for (int i = 0; i < 32; i++) cpu->gte_ctrl[i] = gte_cfc2(&gte, i);
}

extern "C" void gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val) {
    using namespace PSXRecomp::GTE;
    GTEState gte;
    gte_load_data(gte, cpu);
    gte_ctc2(&gte, reg, val);
    for (int i = 0; i < 32; i++) cpu->gte_data[i] = gte_mfc2(&gte, i);
    for (int i = 0; i < 32; i++) cpu->gte_ctrl[i] = gte_cfc2(&gte, i);
}
