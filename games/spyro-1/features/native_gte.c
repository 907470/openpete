// games/spyro-1/features/native_gte.c — see native_gte.h. Bodies are the
// byte-exact union of the per-feeder copies this module replaced (W2.3);
// any change here moves EVERY feeder and must re-pass the bit-exact
// visual gate.

#include "native_gte.h"
#include "spyro1_symbols.gen.h"   /* D_SIN_TABLE, G_CAMERA */
#include "openpete_native_render.h"
#include "spyro_native_reextract.h"  /* openpete_native_interp_pos */

#include <stdio.h>
#include <stdlib.h>

ngte_census_t ngte_census;
int ngte_census_on;
static void ngte_census_print(void) {
    static const char* site[NGTE_SITE_N] = { "raw_sat", "gte_sat", "part_cap", "moby_near", "terrain_unr" };
    fprintf(stderr, "[fov-census]");
    for (int i = 0; i < NGTE_SITE_N; i++)
        fprintf(stderr, " %s=%lu/eff%lu", site[i], ngte_census.hit[i], ngte_census.eff[i]);
    fprintf(stderr, " sxy_bound=%lu bb_untagged=%lu reveal_admitted=%lu\n",
            ngte_census.sxy_bound, ngte_census.bb_untagged, ngte_census.reveal_admitted);
}
static void ngte_census_arm(void) {
    static int armed = 0;
    if (armed) return;
    armed = 1;
    const char* e = getenv("OPENPETE_FOV_CENSUS");
    ngte_census_on = (e && *e && *e != '0') ? 1 : 0;
    if (ngte_census_on) atexit(ngte_census_print);
}

void ngte_unpack_matrix(uint32_t w0, uint32_t w1, uint32_t w2,
                        uint32_t w3, uint32_t w4, int M[3][3]) {
    M[0][0] = (int16_t)(w0 & 0xFFFF); M[0][1] = (int16_t)(w0 >> 16); M[0][2] = (int16_t)(w1 & 0xFFFF);
    M[1][0] = (int16_t)(w1 >> 16);    M[1][1] = (int16_t)(w2 & 0xFFFF); M[1][2] = (int16_t)(w2 >> 16);
    M[2][0] = (int16_t)(w3 & 0xFFFF); M[2][1] = (int16_t)(w3 >> 16);  M[2][2] = (int16_t)(w4 & 0xFFFF);
}

void ngte_read_matrix(CPUState* cpu, uint32_t p, int M[3][3]) {
    ngte_unpack_matrix(cpu->read_word(p), cpu->read_word(p + 4),
                       cpu->read_word(p + 8), cpu->read_word(p + 12),
                       cpu->read_word(p + 16), M);
}

void ngte_mac(const int M[3][3], double vx, double vy, double vz,
              double* m1, double* m2, double* m3) {
    *m1 = (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    *m2 = (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    *m3 = (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
}

void ngte_view_build(CPUState* cpu, ngte_view* v) {
    ngte_census_arm();
    ngte_read_matrix(cpu, G_CAMERA + 0x00, v->cam_M);
    v->cam_ram[0] = (double)(int32_t)cpu->read_word(G_CAMERA + 0x28);
    v->cam_ram[1] = (double)(int32_t)cpu->read_word(G_CAMERA + 0x2C);
    v->cam_ram[2] = (double)(int32_t)cpu->read_word(G_CAMERA + 0x30);
    v->cam_interp = openpete_native_interp_pos(G_CAMERA, v->cam) ? 1 : 0;
    if (!v->cam_interp) { v->cam[0] = v->cam_ram[0]; v->cam[1] = v->cam_ram[1]; v->cam[2] = v->cam_ram[2]; }
    v->ofx     = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    v->ofy     = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    v->H_canon = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
    openpete_native_fov_note_canon(v->H_canon, NGTE_Y_ROW);
    v->s       = openpete_native_fov_h_scale(v->H_canon, NGTE_Y_ROW);
    v->H_eff   = v->H_canon * v->s;
    v->F       = openpete_native_widescreen_factor();
    openpete_native_widescreen_x_bounds(&v->xlo, &v->xhi);
    v->extended = (v->F > 1.0001f || v->s < 1.0);
}

void ngte_view_canon(const ngte_view* v, ngte_view* c) {
    *c = *v;
    c->s = 1.0;
    c->H_eff = c->H_canon;
}

double ngte_project_raw(const int M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx_px, double ofy_px, const ngte_view* v,
                        double* out_sx, double* out_sy, int* out_sat) {
    const double H = v->H_eff;
    double t0 = TR ? TR[0] : 0.0, t1 = TR ? TR[1] : 0.0, t2 = TR ? TR[2] : 0.0;
    double mac1 = t0 + (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    double mac2 = t1 + (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    double mac3 = t2 + (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
    double z = (mac3 != 0.0) ? mac3 : 1.0;
    *out_sx = ofx_px + mac1 * H / z;
    *out_sy = ofy_px + mac2 * H / z;
    /* Limit mirrors read the GTE's H: the regime is the hardware's, not the
     * display's (H_eff moves with --fov, H_canon does not). */
    if (out_sat) {
        int ir = (mac1 > 32767.0 || mac1 < -32768.0 || mac2 > 32767.0 || mac2 < -32768.0 || mac3 <= 0.0);
        *out_sat = ir || v->H_canon >= 2.0 * mac3;
        NGTE_LIMIT(NGTE_SITE_RAW_SAT, *out_sat, ir || H >= 2.0 * mac3);
    }
    return mac3;
}

void ngte_screen_gte(double m1, double m2, double m3,
                     double ofx, double ofy, const ngte_view* v,
                     int sxy_clamp, ngte_proj* o) {
    const double H = v->H_eff;
    /* IR1/IR2 = MAC saturated to int16 (the GTE projects from saturated IR). */
    double ir1 = m1 < -32768.0 ? -32768.0 : (m1 > 32767.0 ? 32767.0 : m1);
    double ir2 = m2 < -32768.0 ? -32768.0 : (m2 > 32767.0 ? 32767.0 : m2);
    /* SZ3 = MAC3 saturated to [0,0xFFFF]; the divide caps at 0x1FFFF when
     * SZ3 <= H/2 — the GTE's H (H_canon), so the regime does not move with
     * --fov. The capped quotient scales with the view (H_eff/sz3 =
     * s·H_canon/sz3), so the cap is NGTE_DIV_CAP·s: one arithmetic form
     * on both sides of the regime boundary. The SXY bound is applied to
     * the projected output: a vertex the hardware pinned at the edge stays
     * at the edge of the scaled view instead of inside it. */
    double sz3 = m3 < 0.0 ? 0.0 : (m3 > 65535.0 ? 65535.0 : m3);
    int sat = (sz3 <= 0.0 || v->H_canon >= 2.0 * sz3);
    double q = sat ? NGTE_DIV_CAP * v->s : (H / sz3);
    NGTE_LIMIT(NGTE_SITE_GTE_SAT, sat, sz3 <= 0.0 || H >= 2.0 * sz3);
    double sx = ofx + ir1 * q;
    double sy = ofy + ir2 * q;
    if (sxy_clamp) {
        int hit = 0;
        if (sx < -NGTE_SXY_BOUND) { sx = -NGTE_SXY_BOUND; hit = 1; } else if (sx > NGTE_SXY_BOUND - 1.0) { sx = NGTE_SXY_BOUND - 1.0; hit = 1; }
        if (sy < -NGTE_SXY_BOUND) { sy = -NGTE_SXY_BOUND; hit = 1; } else if (sy > NGTE_SXY_BOUND - 1.0) { sy = NGTE_SXY_BOUND - 1.0; hit = 1; }
        if (hit) NGTE_CENSUS_HIT(sxy_bound);
    }
    o->sx = sx; o->sy = sy;
    o->m1 = m1; o->m2 = m2; o->m3 = m3;
    o->sat = sat;
}

void ngte_project_gte(const int M[3][3], const double TR[3],
                      double vx, double vy, double vz,
                      double ofx, double ofy, const ngte_view* v,
                      int sxy_clamp, ngte_proj* o) {
    double t0 = TR ? TR[0] : 0.0, t1 = TR ? TR[1] : 0.0, t2 = TR ? TR[2] : 0.0;
    double mac1 = t0 + (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    double mac2 = t1 + (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    double mac3 = t2 + (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
    ngte_screen_gte(mac1, mac2, mac3, ofx, ofy, v, sxy_clamp, o);
}

int ngte_dpcs_ch(int c8, int fc12, int ir0) {
    long mac = (long)c8 << 16;
    long ird = (((long)fc12 << 12) - mac) >> 12;
    if (ird >  32767) ird =  32767;
    if (ird < -32768) ird = -32768;
    long out = ((mac + (long)ir0 * ird) >> 12) >> 4;
    if (out < 0)   out = 0;
    if (out > 255) out = 255;
    return (int)out;
}

void ngte_mat_mul3(const int A[3][3], const int B[3][3], int O[3][3]) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        O[i][j] = (int)(((int64_t)A[i][0]*B[0][j] + (int64_t)A[i][1]*B[1][j] +
                         (int64_t)A[i][2]*B[2][j]) >> 12);
}

void ngte_sincos_f(CPUState* cpu, double fb, double* s_out, double* c_out) {
    while (fb < 0.0) fb += 256.0;
    while (fb >= 256.0) fb -= 256.0;
    int b0 = (int)fb; double frac = fb - (double)b0;
    uint32_t i0 = (uint32_t)b0 << 1, i1 = (uint32_t)((b0 + 1) & 0xFF) << 1;
    double s0 = (int16_t)cpu->read_half(D_SIN_TABLE + i0),        s1 = (int16_t)cpu->read_half(D_SIN_TABLE + i1);
    double c0 = (int16_t)cpu->read_half(D_SIN_TABLE + 0x80u + i0), c1 = (int16_t)cpu->read_half(D_SIN_TABLE + 0x80u + i1);
    *s_out = s0 + (s1 - s0) * frac;
    *c_out = c0 + (c1 - c0) * frac;
}

static int ngte_rnd_i(double x) { return (int)(x >= 0.0 ? x + 0.5 : x - 0.5); }

void ngte_sincos_interp(CPUState* cpu, double fb, int* s_out, int* c_out) {
    double s, c;
    ngte_sincos_f(cpu, fb, &s, &c);
    *s_out = ngte_rnd_i(s);
    *c_out = ngte_rnd_i(c);
}

void ngte_euler_sincos(CPUState* cpu, uint32_t rot, ngte_euler_sc* e) {
    uint32_t iy = (rot >> 15) & 0x1FEu, ix = (rot & 0xFF00u) >> 7, iz = (rot & 0xFFu) << 1;
    e->sy = (int16_t)cpu->read_half(D_SIN_TABLE + iy); e->cy = (int16_t)cpu->read_half(D_SIN_TABLE + 0x80u + iy);
    e->sx = (int16_t)cpu->read_half(D_SIN_TABLE + ix); e->cx = (int16_t)cpu->read_half(D_SIN_TABLE + 0x80u + ix);
    e->sz = (int16_t)cpu->read_half(D_SIN_TABLE + iz); e->cz = (int16_t)cpu->read_half(D_SIN_TABLE + 0x80u + iz);
}

void ngte_compose_euler(const int start[3][3], const ngte_euler_sc* e, int out[3][3]) {
    int Ry[3][3] = {{e->cy, 0, -e->sy}, {0, 4096, 0}, {e->sy, 0, e->cy}};
    int Rx[3][3] = {{4096, 0, 0}, {0, e->cx, -e->sx}, {0, e->sx, e->cx}};
    int Rz[3][3] = {{e->cz, -e->sz, 0}, {e->sz, e->cz, 0}, {0, 0, 4096}};
    int t1[3][3], t2[3][3];
    ngte_mat_mul3(start, Ry, t1);
    ngte_mat_mul3(t1, Rx, t2);
    ngte_mat_mul3(t2, Rz, out);
}
