/* games/common/renderer/psx_uv.h - the texel the PSX GPU reads at each pixel
 * of a textured triangle, as a plane over screen position. */
#ifndef PSX_UV_H
#define PSX_UV_H

/* The fields are the implementation's to choose; callers use
 * psx_uv_plane_setup and psx_uv_plane_at only. */
typedef struct psx_uv_plane {
    double u_org, v_org;   /* u and v at screen position (0, 0) */
    double dudx, dudy;     /* u per pixel in x and in y */
    double dvdx, dvdy;     /* v per pixel in x and in y */
} psx_uv_plane;

/** @brief Builds the plane of texel coordinates the PSX GPU reads across a textured triangle.
 *
 * The plane is affine in screen position. At every pixel (x, y) the GPU
 * draws for the triangle, floor(psx_uv_plane_at(p, x, y, 0)) mod 256 and
 * floor(psx_uv_plane_at(p, x, y, 1)) mod 256 are the u and v the GPU reads
 * (before the texture window). For a sprite (x, y, w, h, u, v) the same
 * holds at every pixel of the sprite on each of the two triangles it is fed
 * as: (x, y, u, v), (x + w, y, u + w, v), (x, y + h, u, v + h) and
 * (x + w, y, u + w, v), (x + w, y + h, u + w, v + h), (x, y + h, u, v + h).
 * The expected output is the measured table tests/unit/data/gpu-uv/uv.texels
 * (gpu_uv_table_test).
 * @param iv The triangle's integer vertices in the GP0 command's order, each {x, y, u, v}.
 * @param p Receives the plane.
 * @return 1 on success; 0 when the triangle's screen area is zero and the
 *         GPU draws nothing. */
int psx_uv_plane_setup(const int iv[3][4], psx_uv_plane* p);

/** @brief Evaluates the plane at a screen position, in texels, unwrapped.
 *
 * @param p The plane from psx_uv_plane_setup.
 * @param x The screen x: pixel (x, y) is the integer position, fractions interpolate.
 * @param y The screen y.
 * @param which_v Nonzero for v, zero for u.
 * @return The plane's value. */
double psx_uv_plane_at(const psx_uv_plane* p, double x, double y, int which_v);

#endif
