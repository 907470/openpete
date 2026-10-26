/* openpete_moby_snap_rule.h - the sunk-pickup snap's plane side and
 * per-fragment depth rule on the CPU, the mirror of psx_moby_snap.glsl
 * op_snap_depth.
 *
 * Camera space is the PSX screen unprojected at view-Z: a point on the ray
 * (x, y, 1) at view-Z z is (x z, y z, z), the eye at the origin. A contact
 * plane n.p = d carries the side the moby stands on (side * (n.p - d) > 0)
 * and its slab, the moby's pull (128 m_DepthOffset). */
#ifndef OPENPETE_MOBY_SNAP_RULE_H
#define OPENPETE_MOBY_SNAP_RULE_H

#include <math.h>

/* One contact plane of a pulled moby in the snap's camera space. */
typedef struct {
    float n[3], d;     /* unit normal and offset: the plane is n.p = d          */
    float side;        /* +1 or -1: the side of the plane the moby stands on     */
    float slab;        /* the depth past the plane a fragment may lie and still
                        * snap: the moby's pull, 128 m_DepthOffset              */
    int   dside;       /* a contact face is double-sided                        */
} openpete_snap_plane;

/* cos 45 degrees: a double-sided plane whose normal lies this close to world
 * up takes the upper side as the moby's. */
#define OPENPETE_SNAP_UP_COS 0.70710678118654752

/** @brief The side of a contact plane the moby stands on.
 *
 * Single-sided faces: the eye's side, since a drawn single-sided face is
 * seen from its open side. Double-sided: the upper side when the normal lies
 * within 45 degrees of world up, else the side holding the moby's vertex
 * mass.
 * @param n The plane's unit normal.
 * @param d The plane's offset.
 * @param dside Nonzero when a contact face of the plane is double-sided.
 * @param up World up in camera space (unit), or NULL when unknown.
 * @param mass The mean signed distance (n.p - d) of the moby's vertices.
 * @return +1 or -1. */
static inline double openpete_snap_side(const double n[3], double d, int dside,
                                        const double* up, double mass)
{
    if (!dside) return -d > 0.0 ? 1.0 : -1.0;
    const double nu = up ? n[0] * up[0] + n[1] * up[1] + n[2] * up[2] : 0.0;
    if (up && fabs(nu) >= OPENPETE_SNAP_UP_COS) return nu > 0.0 ? 1.0 : -1.0;
    return mass > 0.0 ? 1.0 : -1.0;
}

/** @brief The snap's margin at view-Z z for a reversed (1/vz) depth attachment (op_snap_eps).
 * @param z The view-Z.
 * @return The margin, world units. */
static inline double openpete_snap_eps(double z)
{
    const double q = 4.0 * 6e-8 * z, e = 0.5 + 2e-5 * z;
    return q > e ? q : e;
}

/** @brief Whether a terrain key owner at view-Z zt on a ray with n.ray = nr lies in the plane n.p = d (op_snap_coplanar).
 * @param zt The owner's view-Z.
 * @param nr The plane normal's dot product with the ray (x, y, 1).
 * @param d The plane's offset.
 * @return 1 when it does, else 0. */
static inline int openpete_snap_coplanar(double zt, double nr, double d)
{
    const double q = (zt + 4096.0) * (zt + 4096.0) / (4096.0 * 16777215.0);
    return fabs(zt * nr - d) <= 4.0 + q * fabs(nr);
}

/** @brief The view-Z one plane moves a moby fragment to, on the side of the plane the eye looks from.
 *
 * With the eye on the moby's side the result is a snap in front of the
 * owner (*rev); otherwise, under a double-sided face, a push behind it
 * (*hid). Planes that move nothing leave both untouched.
 * @param pl The plane.
 * @param p The fragment's point in camera space.
 * @param zt The terrain key owner's view-Z at its pixel.
 * @param key Nonzero when the face's bucket is strictly nearer than the terrain key's.
 * @param S The largest slab of the present, at least 1.
 * @param rev The nearest snap so far, updated.
 * @param hid The farthest push so far, updated. */
static inline void openpete_snap_rule_plane(const openpete_snap_plane* pl, const double p[3], double zt,
                                            int key, double S, double* rev, double* hid)
{
    const double D = pl->d;
    const double nr = (pl->n[0] * p[0] + pl->n[1] * p[1]) / p[2] + pl->n[2];
    if (fabs(nr) < 1e-9) return;
    const double zc = D / nr;
    if (zc <= 1.0 || !openpete_snap_coplanar(zt, nr, D)) return;
    const double s = pl->side * (pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - D);
    if (s < -(double)pl->slab) return;
    const int eye_side = pl->side * -D > 0.0;
    if (eye_side && !key) return;
    const double ref = eye_side == (zt < zc) ? zt : zc, eps = openpete_snap_eps(ref);
    const double dir = eye_side ? -1.0 : 1.0;    /* in front of ref, or behind it */
    double w;
    if (s < 0.0) {
        w = ref + dir * eps * (2.0 + s / S);
    } else {
        /* Unsunk within 4 eps of ref along the ray: the band between ref and
         * the plane crossing maps onto the half away from the plane. */
        const double edge = ref + dir * 4.0 * eps, span = fabs(zc - edge) > 4.0 * eps ? fabs(zc - edge) : 4.0 * eps;
        if (!(dir * (edge - p[2]) > 0.0)) return;
        w = edge - dir * 2.0 * eps * fabs(p[2] - edge) / span;
    }
    if (eye_side && w < *rev) *rev = w;
    if (!eye_side && w > *hid) *hid = w;
}

/** @brief The view-Z a moby fragment writes after the snap (op_snap_depth with a reversed attachment).
 *
 * @param p The fragment's point in camera space (its key depth).
 * @param vz The view-Z it writes without the snap.
 * @param zt The terrain key owner's view-Z at its pixel (on its ray).
 * @param key Nonzero when the face's bucket is strictly nearer than the
 *        terrain key's at the pixel.
 * @param pl The moby's planes.
 * @param np Their count.
 * @param S The largest slab of the present.
 * @return The written view-Z. */
static inline double openpete_snap_rule_vz(const double p[3], double vz, double zt, int key,
                                           const openpete_snap_plane* pl, int np, double S)
{
    double rev = 3.0e38, hid = -1.0;
    for (int k = 0; k < np; k++)
        openpete_snap_rule_plane(&pl[k], p, zt, key, S < 1.0 ? 1.0 : S, &rev, &hid);
    if (!(rev < vz) && !(hid > vz)) return vz;
    if (rev < vz) vz = rev;
    if (hid > vz) vz = hid;
    return vz > 1.0 ? vz : 1.0;
}

#endif /* OPENPETE_MOBY_SNAP_RULE_H */
