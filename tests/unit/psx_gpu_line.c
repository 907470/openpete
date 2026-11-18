/* tests/unit/psx_gpu_line.c - the pixels the PSX GPU draws for a line.
 *
 * Measured rule (tests/unit/data/gpu-line/line.pixels, every end offset in
 * [-64, 64]^2): the line draws one pixel per major-axis coordinate from the
 * first vertex to the second, both endpoints included; the major axis is x
 * when |dx| >= |dy|, else y. At major-axis step i of k, the minor-axis offset
 * is i * dminor / k rounded to the nearest integer. When that value lies
 * exactly halfway between two pixels, the pixel taken depends on the line's
 * screen geometry only, never on vertex order: on an x-major line the row
 * nearer the endpoint with the larger x; on a y-major line the column nearer
 * the endpoint with the smaller x. */
#include "psx_gpu_line.h"

#include <stdlib.h>

int psx_gpu_line_pixels(int x0, int y0, int x1, int y1, psx_gpu_plot_fn plot, void* ctx)
{
    const int dx = x1 - x0, dy = y1 - y0;
    const int adx = abs(dx), ady = abs(dy);
    const int x_major = adx >= ady;
    const int k = x_major ? adx : ady;   /* major-axis steps */
    const int n = x_major ? ady : adx;   /* minor-axis distance */
    const int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (k == 0) {
        plot(ctx, x0, y0);
        return 1;
    }
    /* A halfway minor offset rounds away from the first vertex (bias 0) or
     * toward it (bias 1). Away means toward the larger-x end on an x-major
     * line and toward the smaller-x end on a y-major line. */
    const long bias = x_major ? (dx < 0) : (dx > 0);
    for (int i = 0; i <= k; i++) {
        const int m = (int)((2L * i * n + k - bias) / (2L * k));
        if (x_major)
            plot(ctx, x0 + sx * i, y0 + sy * m);
        else
            plot(ctx, x0 + sx * m, y0 + sy * i);
    }
    return k + 1;
}
