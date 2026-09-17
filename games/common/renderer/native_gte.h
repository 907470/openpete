// games/common/renderer/native_gte.h — the ONE copy of the GTE
// float-mirror math shared by every native feeder of every game.
//
// Two projection families exist on purpose:
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

/* The game's camera matrix scales its screen-Y row by this (row norms
 * 4096/2560/4096): the vertical FOV of a clip_h-row band is
 * 2·atan((clip_h/2) / (NGTE_Y_ROW · H)). */
#define NGTE_Y_ROW 0.625

/* One projection view per producer pass: every input a feeder needs to
 * project into PSX screen space, read once from the CPU state the pass
 * runs under (the staged g_Camera, which a held producer camera swaps) and
 * from the renderer's display policy. Built inside the producer-camera
 * bracket where one exists, so the bracket's integer-camera rule
 * (openpete_native_interp_pos returns 0 for the camera) reaches cam[].
 *
 * H_canon is the GTE's H: every hardware-limit mirror (IR16 saturation,
 * divide cap, near-fixup regime, UNR overflow regime) reads it. H_eff =
 * H_canon · s is the projection distance the feeder draws with; s is the
 * --fov scale (1.0 exactly at the default, so H_eff == H_canon
 * bit-for-bit). F / xlo / xhi are the Hor+ factor and its screen-X range;
 * extended is 1 when the view shows more than the canonical 4:3 frame
 * (F > 1 or s < 1) and the feeders must admit what the game's verdicts
 * dropped. */
typedef struct {
    int    cam_M[3][3];   /* g_Camera projection matrix (+0x00), Q12 */
    double cam_ram[3];    /* camera position, integer staged RAM (+0x28..0x30) */
    double cam[3];        /* camera position, sub-tick interpolated when
                           * available, else cam_ram */
    int    cam_interp;    /* 1 = cam[] came from the interp spine */
    double ofx, ofy;      /* gte_ctrl[24], [25] / 65536 */
    double H_canon;       /* gte_ctrl[26] */
    double s;             /* --fov scale, latched per present */
    double H_eff;         /* H_canon · s */
    float  F, xlo, xhi;   /* Hor+ factor and screen-X bounds */
    int    extended;      /* view wider than the canonical 4:3 frame */
} ngte_view;

void ngte_view_build(CPUState* cpu, ngte_view* v);

/* Engagement census of the --fov policy (OPENPETE_FOV_CENSUS=1 arms it and
 * prints at exit). Every GTE hardware-limit mirror keys on H_canon; each
 * site also evaluates the same test at H_eff and both verdicts are counted,
 * so one run shows per site that the regime engaged (hit > 0), that the
 * display H would have moved it (eff != hit when s != 1) and that it did
 * not (hit is the same code at every FOV; at s = 1 the two are equal).
 * sxy_bound counts the SXY pin on the scaled output, which moves with s by
 * design. bb_untagged counts shaded-walk billboard records with no HUD tag
 * (expected 0: every billboard anchor is a game-authored screen
 * coordinate). reveal_admitted counts mobys the reveal admitted host-side
 * (0 at the canonical view). */
enum { NGTE_SITE_RAW_SAT, NGTE_SITE_GTE_SAT, NGTE_SITE_PART_CAP,
       NGTE_SITE_MOBY_NEAR, NGTE_SITE_TERRAIN_UNR, NGTE_SITE_N };
typedef struct {
    unsigned long hit[NGTE_SITE_N];   /* the limit test at H_canon */
    unsigned long eff[NGTE_SITE_N];   /* the same test at H_eff */
    unsigned long sxy_bound;
    unsigned long bb_untagged;
    unsigned long reveal_admitted;
} ngte_census_t;
extern ngte_census_t ngte_census;
extern int ngte_census_on;
#define NGTE_CENSUS_HIT(field) \
    ((void)(ngte_census_on ? __atomic_fetch_add(&ngte_census.field, 1ul, __ATOMIC_RELAXED) : 0ul))
/* One limit site: canon is the verdict the code acts on, eff the verdict the
 * display H would have given. */
#define NGTE_LIMIT(site, canon, eff_) \
    ((void)(ngte_census_on ? ((canon) ? __atomic_fetch_add(&ngte_census.hit[site], 1ul, __ATOMIC_RELAXED) : 0ul, \
                              (eff_)  ? __atomic_fetch_add(&ngte_census.eff[site], 1ul, __ATOMIC_RELAXED) : 0ul) : 0ul))
/* *c = *v with s = 1.0, H_eff = H_canon: the view for records whose screen
 * position the game authored (HUD, billboards) and for instruments that
 * compare against canonical prims. */
void ngte_view_canon(const ngte_view* v, ngte_view* c);

/* Unpack a GTE SHORTMATRIX (5 words = 9 packed int16) from raw words —
 * for callers whose source is the GTE control regs, not RAM. */
void ngte_unpack_matrix(uint32_t w0, uint32_t w1, uint32_t w2,
                        uint32_t w3, uint32_t w4, int M[3][3]);

/* Unpack a GTE SHORTMATRIX (5 words = 9 packed int16) from PSX RAM. */
void ngte_read_matrix(CPUState* cpu, uint32_t p, int M[3][3]);

/* MAC1/2/3 = (M · v) / 4096 — the rotate step alone, no TR, no divide. */
void ngte_mac(const int M[3][3], double vx, double vy, double vz,
              double* m1, double* m2, double* m3);

/* IR1/2/3 as the hardware hands them back with lm = 0: MAC saturated to signed
 * 16 bits. A walk that mirrors a draw reading IR after MVMVA needs the
 * saturation, since the unsaturated MAC differs wherever the sector is far
 * enough for the product to overflow. */
static inline double ngte_ir_clamp(double mac) {
    return mac < -32768.0 ? -32768.0 : (mac > 32767.0 ? 32767.0 : mac);
}

/* Raw-float projection family. TR may be NULL (treated as zero). ofx/ofy
 * are per-call (the shaded walk's billboards project about their own
 * anchor); H comes from the view.
 * z-floor: mac3 != 0 ? mac3 : 1.0 (exactly-zero guard only — callers
 * cull/clip on the RETURNED mac3, which is unfloored). out_sat mirrors
 * the GTE's would-have-saturated flags. Returns mac3 (view-Z). */
double ngte_project_raw(const int M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx_px, double ofy_px, const ngte_view* v,
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
                     double ofx, double ofy, const ngte_view* v,
                     int sxy_clamp, ngte_proj* o);

/* Full GTE-faithful projection. TR may be NULL (treated as zero). */
void ngte_project_gte(const int M[3][3], const double TR[3],
                      double vx, double vy, double vz,
                      double ofx, double ofy, const ngte_view* v,
                      int sxy_clamp, ngte_proj* o);

/* GTE DPCS single channel, bit-exact integer mirror: c8 = 8-bit colour,
 * fc12 = 12-bit-expanded far colour, ir0 = 1.3.12 blend factor. */
int ngte_dpcs_ch(int c8, int fc12, int ir0);

/* GTE DPCS in float: lerp every 8-bit channel of rgb toward the matching
 * channel of fc by ir0/4096, rounded and clamped to 0..255. Both are packed
 * 0x00BBGGRR. The sky and portal-preview draws run the real op (cmd 0x10,
 * sf=1) over a colour table with FC = the fog far colour's components SHL 4
 * and IR0 = the fog factor; FC SHL 12 then lines up with the colour's SHL 16,
 * so the op reduces to this per-channel lerp. The float form is what the rest
 * of the native path carries; ngte_dpcs_ch is the bit-exact integer mirror
 * for the walks that must match the GTE to the unit. */
uint32_t ngte_depth_cue(uint32_t rgb, uint32_t fc, int ir0);

/* The fog far colour (game_layout.fog.far_colour) as packed 0x00BBGGRR: one
 * word holding the three 8-bit components, or, when fog.far_colour_words is
 * set, three words R, G, B at the GTE's RFC/GFC/BFC scale (16 times the
 * 8-bit value). 0 when the game leaves the address unset. */
uint32_t ngte_fog_far_colour(CPUState* cpu);

/* O = (A · B) >> 12 on Q12 3x3 matrices, the column-rotate the game does
 * via MVMVA. Accumulates in 64 bits: the GTE's MAC is 44 bits wide, and
 * a scaled model matrix times a rotation exceeds int32 before the shift. */
void ngte_mat_mul3(const int A[3][3], const int B[3][3], int O[3][3]);

/* The game's in-RAM sin/cos table (game_layout.sin_table): 256 entries per
 * circle, int16 Q12, cos at +0x80 bytes. A byte angle indexes it at byte<<1.
 * A game whose table is unlocated (the field is 0) gets sin 0 / cos 4096 from
 * every helper below instead of a read through address 0. */

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
