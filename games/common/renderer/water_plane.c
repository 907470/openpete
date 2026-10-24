/* water_plane.c - the water-plane split's shade and cut, shared by the player
 * and moby walks. See water_plane.h. */
#include "renderer/water_plane.h"

uint32_t wplane_shade(const int32_t fc[3], int32_t ir0, uint32_t rgb) {
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        int32_t c  = (int32_t)((rgb >> (8*i)) & 0xFFu);
        int32_t ir = fc[i] - (c << 4);
        int32_t v  = ((ir * ir0 + (c << 16)) >> 12) >> 4;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        out |= (uint32_t)v << (8*i);
    }
    return out;
}

/** @brief The point where one triangle edge crosses the plane.
 *
 * The crossing sits at the model-space parameter t = pd_a / (pd_a - pd_b),
 * clamped to [0, 1]: the affine attributes and the colour (rounded per
 * channel) take t; the screen position takes the perspective-correct
 * parameter when both corners carry a perspective divisor, else t. The
 * crossing keeps the first corner's identity and has distance 0.
 * @param A The edge's first corner.
 * @param B The edge's second corner.
 * @param nat The number of `at` entries in use.
 * @return The crossing. */
static wplane_vert_t wplane_cross(const wplane_vert_t* A, const wplane_vert_t* B, int nat) {
    double den = A->pd - B->pd;
    double t = den != 0.0 ? A->pd / den : 0.0;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    double wd = (1.0 - t) * A->w + t * B->w;
    double s = (A->w > 0.0 && B->w > 0.0 && wd > 0.0) ? t * B->w / wd : t;
    wplane_vert_t M = *A;
    M.sx = A->sx + s*(B->sx - A->sx);
    M.sy = A->sy + s*(B->sy - A->sy);
    M.w  = A->w  + t*(B->w  - A->w);
    for (int i = 0; i < nat; i++) M.at[i] = A->at[i] + t*(B->at[i] - A->at[i]);
    M.pd = 0.0; M.rgb = 0;
    for (int c = 0; c < 3; c++) {
        double ca = (double)((A->rgb >> (8*c)) & 0xFFu);
        double cb = (double)((B->rgb >> (8*c)) & 0xFFu);
        int cv = (int)(ca + t*(cb - ca) + 0.5);
        cv = cv < 0 ? 0 : (cv > 255 ? 255 : cv);
        M.rgb |= (uint32_t)cv << (8*c);
    }
    return M;
}

void wplane_cut(const wplane_vert_t in[3], int nat,
                wplane_vert_t pos[4], int* np, wplane_vert_t neg[4], int* nn) {
    if (nat > WPLANE_ATTRS) nat = WPLANE_ATTRS;
    *np = 0; *nn = 0;
    for (int k = 0; k < 3; k++) {
        const wplane_vert_t* A = &in[k];
        const wplane_vert_t* B = &in[(k + 1) % 3];
        const int a_up = A->pd >= 0.0, b_up = B->pd >= 0.0;
        if (a_up) pos[(*np)++] = *A; else neg[(*nn)++] = *A;
        if (a_up == b_up) continue;
        const wplane_vert_t M = wplane_cross(A, B, nat);
        pos[(*np)++] = M;
        neg[(*nn)++] = M;
    }
    if (*np < 3) *np = 0;
    if (*nn < 3) *nn = 0;
}
