/* tests/unit/gpu_line_table_test.c - psx_gpu_line_pixels against the measured
 * table of the pixels the PSX GPU draws for a line.
 *
 * The table (data/gpu-line/line.pixels, written by
 * tools/gpu_blackbox/measure_lines.py) holds, for every end offset (dx, dy)
 * in [-64, 64]^2 of a line from (100, 100), the drawn pixels as a start
 * pixel followed by k = max(|dx|, |dy|) steps: each step moves one pixel
 * along the major axis (x when |dx| >= |dy|) toward the end, and also one
 * pixel along the minor axis toward the end when its bit is set. The test
 * expands each row, runs psx_gpu_line_pixels on the same vertices and
 * fails on any case whose pixel set differs, that plots a pixel twice or
 * whose return value is not the number of pixels plotted. */
#include "psx_gpu_line.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X0 100
#define Y0 100
#define GRID 256

typedef struct { unsigned char px[GRID][GRID]; long plotted, outside, twice; } grid_t;

/** @brief Marks one plotted pixel in the grid.
 * @param ctx The grid_t.
 * @param x The pixel column.
 * @param y The pixel row. */
static void plot(void* ctx, int x, int y)
{
    grid_t* g = ctx;
    g->plotted++;
    if (x < 0 || x >= GRID || y < 0 || y >= GRID) { g->outside++; return; }
    if (g->px[y][x]) g->twice++;
    g->px[y][x] = 1;
}

/** @brief Returns bit i of a hexadecimal number written as text.
 * @param bits The number, lowercase hexadecimal, most significant digit first.
 * @param n The number of digits.
 * @param i The bit index.
 * @return 0 or 1, or -1 when the digit holding bit i is not a hex digit. */
static int hex_bit(const char* bits, int n, int i)
{
    const int nib = i / 4;
    if (nib >= n) return 0;
    const char c = bits[n - 1 - nib];
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    return v < 0 ? -1 : (v >> (i % 4)) & 1;
}

/** @brief Expands one table row into the grid.
 * @param dx The end offset x.
 * @param dy The end offset y.
 * @param bits The row's step bits, hexadecimal.
 * @param g Receives the pixels.
 * @return 0, or -1 when bits holds more than k bits or a non-hex digit. */
static int expand(int dx, int dy, const char* bits, grid_t* g)
{
    const int adx = abs(dx), ady = abs(dy), k = adx > ady ? adx : ady;
    /* Per step: the major-axis move and the minor-axis move a set bit adds. */
    const int mx = adx >= ady ? (dx < 0 ? -1 : 1) : 0, my = adx >= ady ? 0 : (dy < 0 ? -1 : 1);
    const int nx = mx ? 0 : (dx < 0 ? -1 : 1), ny = mx ? (dy < 0 ? -1 : 1) : 0;
    const int n = (int)strlen(bits);
    int x = X0, y = Y0;
    plot(g, x, y);
    for (int i = 0; i < k; i++) {
        const int bit = hex_bit(bits, n, i);
        if (bit < 0) return -1;
        x += mx + bit * nx;
        y += my + bit * ny;
        plot(g, x, y);
    }
    return (n > (k + 3) / 4 + (k == 0)) ? -1 : 0;
}

typedef struct { long cases, bad_cases, bad_px, twice, table_px; int shown; } tally_t;

/** @brief Runs psx_gpu_line_pixels on one line and compares it with the table's pixels.
 * @param dx The end offset x.
 * @param dy The end offset y.
 * @param want The table's pixels.
 * @param t The tally. */
static void check_line(int dx, int dy, const grid_t* want, tally_t* t)
{
    static grid_t got;
    memset(&got, 0, sizeof got);
    const int r = psx_gpu_line_pixels(X0, Y0, X0 + dx, Y0 + dy, plot, &got);
    long diff = got.outside;
    for (int y = 0; y < GRID; y++)
        for (int x = 0; x < GRID; x++) diff += want->px[y][x] != got.px[y][x];
    t->cases++;
    t->twice += got.twice;
    if (!diff && !got.twice && r == got.plotted) return;
    t->bad_cases++; t->bad_px += diff;
    if (t->shown++ < 8)
        fprintf(stderr, "mismatch: line (%d,%d)-(%d,%d): %ld pixels differ, %ld plotted twice, returned %d of %ld plotted\n",
                X0, Y0, X0 + dx, Y0 + dy, diff, got.twice, r, got.plotted);
}

/** @brief Runs every table row against psx_gpu_line_pixels.
 * @param argc Argument count.
 * @param argv argv[1] is the table path.
 * @return 0 on pass, 1 on a mismatch or a bad table. */
int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s line.pixels\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    static grid_t want;
    char line[256], bits[64];
    tally_t t;
    memset(&t, 0, sizeof t);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int dx, dy;
        memset(&want, 0, sizeof want);
        if (sscanf(line, "%d %d %63s", &dx, &dy, bits) != 3 || expand(dx, dy, bits, &want) || want.outside) {
            fprintf(stderr, "bad row: %s", line);
            fclose(f);
            return 1;
        }
        t.table_px += want.plotted;
        check_line(dx, dy, &want, &t);
    }
    fclose(f);
    printf("gpu_line_table_test: %ld cases, %ld table pixels, %ld cases mismatch (%ld pixels, %ld plotted twice)\n",
           t.cases, t.table_px, t.bad_cases, t.bad_px, t.twice);
    if (t.cases == 0) { fprintf(stderr, "FAIL: the table holds no case\n"); return 1; }
    printf("%s\n", t.bad_cases ? "FAIL" : "PASS");
    return t.bad_cases ? 1 : 0;
}
