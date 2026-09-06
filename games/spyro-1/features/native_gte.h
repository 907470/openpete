// games/spyro-1/features/native_gte.h — W2.3: the ONE copy of the GTE
// float-mirror math every native feeder used to re-static-define.
//
// Two projection families exist on purpose (see the W2.3 survey in
// notes/plans/openpete-native-renderer.md):
//
//   * ngte_project_raw — unclamped float H/z (moby, cyclorama). More
//     correct than the GTE for widescreen reveal; callers own their
//     near-plane policy on the returned view-Z.
//   * ngte_project_gte / ngte_screen_gte — the GTE-faithful pipeline
//     (IR16 saturation, SZ3 clamp, 0x1FFFF divide cap, optional ±1024
//     SXY bound) used where PSX screen positions must be matched to the
//     unit (spyro, flame, shadow, particle — particle drops the SXY
//     bound: widescreen reveal needs the true X).
//
// Near-plane z-floors deliberately stay in the CALLERS: they encode
// per-feeder policy (moby !=0→1.0, portal >1.0, terrain 1e-9, tracer
// reject ≤1.0) and unifying them is a behaviour change, not consolidation.

#ifndef OPENPETE_NATIVE_GTE_H
#define OPENPETE_NATIVE_GTE_H

#include <stdint.h>
#include "psx_runtime_internal.h"

/* GTE UNR divide cap (0x1FFFF in 16.16) and SXY output saturation. */
#define NGTE_DIV_CAP   (131071.0 / 65536.0)
#define NGTE_SXY_BOUND 1024.0

/* Unpack a GTE SHORTMATRIX (5 words = 9 packed int16) from raw words —
 * for callers whose source is the GTE control regs, not RAM. */
void ngte_unpack_matrix(uint32_t w0, uint32_t w1, uint32_t w2,
                        uint32_t w3, uint32_t w4, int M[3][3]);

/* Unpack a GTE SHORTMATRIX (5 words = 9 packed int16) from PSX RAM. */
void ngte_read_matrix(CPUState* cpu, uint32_t p, int M[3][3]);

/* MAC1/2/3 = (M · v) / 4096 — the rotate step alone, no TR, no divide. */
void ngte_mac(const int M[3][3], double vx, double vy, double vz,
              double* m1, double* m2, double* m3);

/* Raw-float projection family. TR may be NULL (treated as zero).
 * z-floor: mac3 != 0 ? mac3 : 1.0 (exactly-zero guard only — callers
 * cull/clip on the RETURNED mac3, which is unfloored). out_sat mirrors
 * the GTE's would-have-saturated flags. Returns mac3 (view-Z). */
double ngte_project_raw(const int M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx_px, double ofy_px, double H,
                        double* out_sx, double* out_sy, int* out_sat);

/* GTE-faithful projection family output. m1/m2/m3 are the UNsaturated
 * MACs (particle stores them); sx/sy are post-IR16-clamp screen coords. */
typedef struct {
    double sx, sy;
    double m1, m2, m3;
    int sat;              /* SZ3 <= 0 or H >= 2*SZ3 (divide capped) */
} ngte_proj;

/* The divide half alone, for callers that already have view-space MACs. */
void ngte_screen_gte(double m1, double m2, double m3,
                     double ofx, double ofy, double H,
                     int sxy_clamp, ngte_proj* o);

/* Full GTE-faithful projection. TR may be NULL (treated as zero). */
void ngte_project_gte(const int M[3][3], const double TR[3],
                      double vx, double vy, double vz,
                      double ofx, double ofy, double H,
                      int sxy_clamp, ngte_proj* o);

/* GTE DPCS single channel, bit-exact integer mirror: c8 = 8-bit colour,
 * fc12 = 12-bit-expanded far colour, ir0 = 1.3.12 blend factor. */
int ngte_dpcs_ch(int c8, int fc12, int ir0);

/* O = (A · B) >> 12 on Q12 3x3 matrices, the column-rotate the game does
 * via MVMVA. Accumulates in 64 bits: the GTE's MAC is 44 bits wide, and
 * a scaled model matrix times a rotation exceeds int32 before the shift. */
void ngte_mat_mul3(const int A[3][3], const int B[3][3], int O[3][3]);

/* The game's in-RAM sin/cos table (D_SIN_TABLE): 256 entries per circle,
 * int16 Q12, cos at +0x80 bytes. A byte angle indexes it at byte<<1. */

/* sin/cos at fractional byte-angle resolution: lerp adjacent table
 * entries. Exact at integer angles, smooth between, so a rotation renders
 * below the 1.4°/step byte grid. fb wraps into [0,256). */
void ngte_sincos_f(CPUState* cpu, double fb, double* s_out, double* c_out);

/* ngte_sincos_f rounded to the nearest int for Q12 matrix composition. */
void ngte_sincos_interp(CPUState* cpu, double fb, int* s_out, int* c_out);

/* Byte-exact table lookups for a packed euler word (Z=byte0, X=byte1,
 * Y=byte2; m_Rotation@+0x44). rot==0 gives sin 0 / cos 4096 on every axis. */
typedef struct { int sy, cy, sx, cx, sz, cz; } ngte_euler_sc;
void ngte_euler_sincos(CPUState* cpu, uint32_t rot, ngte_euler_sc* e);

/* out = start·Ry·Rx·Rz in Q12, the order the game's MVMVA sequence applies
 * (func_80022A2C block_80022DBC..80022F34). */
void ngte_compose_euler(const int start[3][3], const ngte_euler_sc* e, int out[3][3]);

#endif /* OPENPETE_NATIVE_GTE_H */
