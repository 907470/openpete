/* tests/unit/moby_snap_rule_test.c - the sunk-pickup snap's side term and
 * depth rule (openpete_moby_snap_rule.h, the CPU mirror of
 * psx_moby_snap.glsl) on synthetic contact planes.
 *
 * Camera space: the eye at the origin looking down +z, screen y down, so
 * world up is (0, -1, 0). A pickup rests on a floor and pokes 10 world
 * through it. Seen from above, the sunk tip snaps in front of the floor;
 * seen from beneath a double-sided floor, the upper side stays the moby's,
 * so the body is not pulled through the floor and the tip is pushed behind
 * it. A wall's side comes from the moby's vertex mass. An owner off the
 * plane, a tied bucket and a point past the slab never snap, and an unsunk
 * point within the band stays nearer than every snapped point behind it. */

#include "openpete_moby_snap_rule.h"

#include <stdio.h>

static const double k_up[3] = { 0.0, -1.0, 0.0 };

/** @brief Report one failed expectation.
 * @param ok The expectation.
 * @param what Its description.
 * @return 0 when it holds, else 1. */
static int expect(int ok, const char* what)
{
    if (!ok) fprintf(stderr, "FAIL %s\n", what);
    return !ok;
}

/** @brief Build one plane with the side openpete_snap_side gives it.
 * @param nx Normal x.
 * @param ny Normal y.
 * @param nz Normal z.
 * @param d Offset.
 * @param dside Double-sided flag.
 * @param mass The moby's mean signed distance from the plane.
 * @return The plane, slab 512 (m_DepthOffset 4). */
static openpete_snap_plane plane(double nx, double ny, double nz, double d, int dside, double mass)
{
    const double n[3] = { nx, ny, nz };
    openpete_snap_plane p = { { (float)nx, (float)ny, (float)nz }, (float)d,
                              (float)openpete_snap_side(n, d, dside, k_up, mass), 512.0f, dside };
    return p;
}

/** @brief The view-Z at which a ray through camera-space point q crosses the plane y = yf.
 * @param q The point.
 * @param yf The plane's camera-space y.
 * @return The crossing's view-Z. */
static double floor_z(const double q[3], double yf)
{
    return yf / (q[1] / q[2]);
}

/** @brief Run the cases.
 * @return 0 when every case passes. */
int main(void)
{
    int fail = 0;

    /* Floor 100 below the eye, seen from above: single-sided takes the eye's
     * side, double-sided the upper side, which is the same side here. */
    {
        const openpete_snap_plane ss = plane(0, 1, 0, 100, 0, -20);
        const openpete_snap_plane ds = plane(0, -1, 0, -100, 1, 20);
        fail |= expect(ss.side * -ss.d > 0.0f, "single-sided floor: eye on the moby's side");
        fail |= expect(ds.side * (ds.n[1] * 80.0f - ds.d) > 0.0f, "double-sided floor: upper side");
        const double tip[3] = { 0.0, 105.0, 1000.0 };       /* 5 below the floor */
        const double zt = floor_z(tip, 100.0);
        const openpete_snap_plane* pp[2] = { &ss, &ds };
        for (int k = 0; k < 2; k++) {
            const double w = openpete_snap_rule_vz(tip, tip[2], zt, 1, pp[k], 1, 512.0);
            fail |= expect(w < zt && w > zt - 2.0 * openpete_snap_eps(zt) - 1e-9,
                           k ? "double-sided floor from above: tip snaps in front" :
                               "single-sided floor from above: tip snaps in front");
            fail |= expect(openpete_snap_rule_vz(tip, tip[2], zt, 0, pp[k], 1, 512.0) == tip[2],
                           "tied bucket: no snap");
            fail |= expect(openpete_snap_rule_vz(tip, tip[2], zt - 40.0, 1, pp[k], 1, 512.0) == tip[2],
                           "owner off the plane: no snap");
        }
        const double deep[3] = { 0.0, 700.0, 1000.0 };      /* 600 below: past the slab */
        fail |= expect(openpete_snap_rule_vz(deep, deep[2], floor_z(deep, 100.0), 1, &ss, 1, 512.0) == deep[2],
                       "past the slab: no snap");
        /* Monotone band: an unsunk point just above the floor stays nearer
         * than a sunk point behind it on the same ray. */
        const double above[3] = { 0.0, 99.9, 999.0 }, below[3] = { 0.0, 100.3, 1003.0 };
        const double zta = floor_z(above, 100.0);
        const double wa = openpete_snap_rule_vz(above, above[2], zta, 1, &ss, 1, 512.0);
        const double wb = openpete_snap_rule_vz(below, below[2], zta, 1, &ss, 1, 512.0);
        fail |= expect(wb < below[2] && wa < wb, "band: the unsunk point stays in front of the sunk one");
    }

    /* Double-sided floor 100 above the eye, the pickup on its upper side,
     * seen from beneath. */
    {
        const openpete_snap_plane ds = plane(0, 1, 0, -100, 1, 20);
        fail |= expect(ds.side < 0.0f, "double-sided floor from beneath: the moby's side is the upper one");
        fail |= expect(!(ds.side * -ds.d > 0.0f), "double-sided floor from beneath: eye not on the moby's side");
        const double body[3] = { 0.0, -120.0, 1000.0 };
        const double zb = floor_z(body, -100.0);
        fail |= expect(openpete_snap_rule_vz(body, body[2], zb, 1, &ds, 1, 512.0) == body[2],
                       "from beneath: the body keeps its depth behind the floor");
        const double tip[3] = { 0.0, -95.0, 1000.0 };
        const double zt = floor_z(tip, -100.0);
        const double w = openpete_snap_rule_vz(tip, tip[2], zt, 1, &ds, 1, 512.0);
        fail |= expect(zt > tip[2] && w > zt, "from beneath: the tip is pushed behind the floor");
        /* The same floor single-sided would face the eye from its open side
         * only when the eye is above it: the side is the eye's. */
        const openpete_snap_plane ss = plane(0, 1, 0, -100, 0, 20);
        fail |= expect(ss.side * -ss.d > 0.0f, "single-sided: the eye's side");
    }

    /* Double-sided wall: its normal is horizontal, the vertex mass decides. */
    {
        const openpete_snap_plane a = plane(1, 0, 0, 50, 1, 30);
        const openpete_snap_plane b = plane(1, 0, 0, 50, 1, -30);
        fail |= expect(a.side > 0.0f && b.side < 0.0f, "double-sided wall: side of the vertex mass");
        const openpete_snap_plane c = plane(0.7, -0.714142842854285, 0, 50, 1, -30);
        fail |= expect(c.side > 0.0f, "double-sided slope within 45 degrees of up: upper side");
    }

    if (!fail) printf("moby_snap_rule_test: ok\n");
    return fail;
}
