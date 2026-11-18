/* tests/unit/psx_gpu_line.h - the pixels the PSX GPU draws for a line. */
#ifndef PSX_GPU_LINE_H
#define PSX_GPU_LINE_H

/** @brief Receives one drawn pixel.
 * @param ctx The caller's context.
 * @param x The pixel column.
 * @param y The pixel row. */
typedef void (*psx_gpu_plot_fn)(void* ctx, int x, int y);

/** @brief Calls plot once for every pixel the PSX GPU draws for a monochrome line between two integer vertices.
 *
 * The vertices are in drawing coordinates after the drawing offset, with no
 * drawing-area clip and no interlace skip. The vertex order is the order
 * of the GP0 command. The expected output is the measured table
 * tests/unit/data/gpu-line/line.pixels (gpu_line_table_test).
 * @param x0 First vertex x.
 * @param y0 First vertex y.
 * @param x1 Second vertex x.
 * @param y1 Second vertex y.
 * @param plot Called once per drawn pixel.
 * @param ctx Passed to plot.
 * @return The number of pixels drawn. */
int psx_gpu_line_pixels(int x0, int y0, int x1, int y1, psx_gpu_plot_fn plot, void* ctx);

#endif
