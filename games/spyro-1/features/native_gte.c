// games/spyro-1/features/native_gte.c — see native_gte.h. Bodies are the
// byte-exact union of the per-feeder copies this module replaced (W2.3);
// any change here moves EVERY feeder and must re-pass the bit-exact
// visual gate.

#include "native_gte.h"

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

double ngte_project_raw(const int M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx_px, double ofy_px, double H,
                        double* out_sx, double* out_sy, int* out_sat) {
    double t0 = TR ? TR[0] : 0.0, t1 = TR ? TR[1] : 0.0, t2 = TR ? TR[2] : 0.0;
    double mac1 = t0 + (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    double mac2 = t1 + (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    double mac3 = t2 + (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
    double z = (mac3 != 0.0) ? mac3 : 1.0;
    *out_sx = ofx_px + mac1 * H / z;
    *out_sy = ofy_px + mac2 * H / z;
    if (out_sat) *out_sat = (mac1 > 32767.0 || mac1 < -32768.0 ||
                             mac2 > 32767.0 || mac2 < -32768.0 ||
                             mac3 <= 0.0 || H >= 2.0 * mac3);
    return mac3;
}

void ngte_screen_gte(double m1, double m2, double m3,
                     double ofx, double ofy, double H,
                     int sxy_clamp, ngte_proj* o) {
    /* IR1/IR2 = MAC saturated to int16 (the GTE projects from saturated IR). */
    double ir1 = m1 < -32768.0 ? -32768.0 : (m1 > 32767.0 ? 32767.0 : m1);
    double ir2 = m2 < -32768.0 ? -32768.0 : (m2 > 32767.0 ? 32767.0 : m2);
    /* SZ3 = MAC3 saturated to [0,0xFFFF]; divide caps to 0x1FFFF when near. */
    double sz3 = m3 < 0.0 ? 0.0 : (m3 > 65535.0 ? 65535.0 : m3);
    int sat = (sz3 <= 0.0 || H >= 2.0 * sz3);
    double q = sat ? NGTE_DIV_CAP : (H / sz3);
    double sx = ofx + ir1 * q;
    double sy = ofy + ir2 * q;
    if (sxy_clamp) {
        if (sx < -NGTE_SXY_BOUND) sx = -NGTE_SXY_BOUND; else if (sx > NGTE_SXY_BOUND - 1.0) sx = NGTE_SXY_BOUND - 1.0;
        if (sy < -NGTE_SXY_BOUND) sy = -NGTE_SXY_BOUND; else if (sy > NGTE_SXY_BOUND - 1.0) sy = NGTE_SXY_BOUND - 1.0;
    }
    o->sx = sx; o->sy = sy;
    o->m1 = m1; o->m2 = m2; o->m3 = m3;
    o->sat = sat;
}

void ngte_project_gte(const int M[3][3], const double TR[3],
                      double vx, double vy, double vz,
                      double ofx, double ofy, double H,
                      int sxy_clamp, ngte_proj* o) {
    double t0 = TR ? TR[0] : 0.0, t1 = TR ? TR[1] : 0.0, t2 = TR ? TR[2] : 0.0;
    double mac1 = t0 + (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    double mac2 = t1 + (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    double mac3 = t2 + (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
    ngte_screen_gte(mac1, mac2, mac3, ofx, ofy, H, sxy_clamp, o);
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
