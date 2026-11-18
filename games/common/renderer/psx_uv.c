/* games/common/renderer/psx_uv.c - the texel the PSX GPU reads at each pixel
 * of a textured triangle.
 *
 * Measured rule (tests/unit/data/gpu-uv/uv.texels: census triangles of the
 * three games, sprites, plain, mirrored, scaled and skewed quads, random
 * triangles and their reorderings):
 *
 * - Each of u and v has two gradients, per pixel in x and in y, in fixed
 *   point with 12 fraction bits: the exact gradient of the plane through
 *   the three vertices, scaled by 4096 and truncated toward zero.
 * - The plane starts at one vertex, the base: the vertex with the smallest
 *   x. When two vertices share the smallest x, the base is the one that
 *   follows the other in the command's cyclic vertex order (v0 -> v1 ->
 *   v2 -> v0). This tie is the only way vertex order changes the texels.
 * - At pixel (x, y) the value is base.u * 4096 + 2048 + Gx * (x - base.x) +
 *   Gy * (y - base.y), and the texel is that value shifted right by 12
 *   (floor), mod 256. The 2048 rounds the base texel to the nearest.
 *
 * The hardware documentation states only that the GPU interpolates texture
 * coordinates linearly in screen space; the precision, the base vertex and
 * the rounding are taken from the table.
 *
 * The plane is stored as doubles holding integers divided by 4096. For
 * integer x and y, and for every value an on-screen triangle produces, each
 * product and sum in psx_uv_plane_at is exact in double precision, so the
 * floor of the result equals the integer computation regardless of the
 * floating-point rounding mode. */
#include "renderer/psx_uv.h"

#define PSX_UV_FRAC_BITS 12
#define PSX_UV_ONE (1LL << PSX_UV_FRAC_BITS)

/** @brief Returns the index of the vertex the texel plane starts from.
 *
 * The vertex with the smallest x; of two sharing it, the one that follows
 * the other in cyclic vertex order.
 * @param iv The triangle's vertices, each {x, y, u, v}.
 * @return 0, 1 or 2. */
static int psx_uv_base_vertex(const int iv[3][4])
{
    int b = 0;
    for (int i = 1; i < 3; i++)
        if (iv[i][0] < iv[b][0]) b = i;
    for (int i = 0; i < 3; i++)
        if (i != b && iv[i][0] == iv[b][0] && i == (b + 1) % 3) return i;
    return b;
}

int psx_uv_plane_setup(const int iv[3][4], psx_uv_plane* p)
{
    const long long ex1 = iv[1][0] - iv[0][0], ey1 = iv[1][1] - iv[0][1];
    const long long ex2 = iv[2][0] - iv[0][0], ey2 = iv[2][1] - iv[0][1];
    const long long area = ex1 * ey2 - ex2 * ey1;   /* twice the signed area */
    p->u_org = p->v_org = p->dudx = p->dudy = p->dvdx = p->dvdy = 0.0;
    if (area == 0) return 0;
    const int b = psx_uv_base_vertex(iv);
    long long g[2][2];   /* [u or v][x or y], 12 fraction bits */
    for (int w = 0; w < 2; w++) {
        const long long d1 = iv[1][2 + w] - iv[0][2 + w], d2 = iv[2][2 + w] - iv[0][2 + w];
        /* C division truncates toward zero, as the measured gradients do. */
        g[w][0] = (d1 * ey2 - d2 * ey1) * PSX_UV_ONE / area;
        g[w][1] = (ex1 * d2 - ex2 * d1) * PSX_UV_ONE / area;
    }
    const long long bx = iv[b][0], by = iv[b][1];
    const long long u0 = iv[b][2] * PSX_UV_ONE + PSX_UV_ONE / 2 - g[0][0] * bx - g[0][1] * by;
    const long long v0 = iv[b][3] * PSX_UV_ONE + PSX_UV_ONE / 2 - g[1][0] * bx - g[1][1] * by;
    const double one = (double)PSX_UV_ONE;
    p->u_org = (double)u0 / one;
    p->v_org = (double)v0 / one;
    p->dudx = (double)g[0][0] / one;
    p->dudy = (double)g[0][1] / one;
    p->dvdx = (double)g[1][0] / one;
    p->dvdy = (double)g[1][1] / one;
    return 1;
}

double psx_uv_plane_at(const psx_uv_plane* p, double x, double y, int which_v)
{
    return which_v ? p->v_org + p->dvdx * x + p->dvdy * y
                   : p->u_org + p->dudx * x + p->dudy * y;
}
