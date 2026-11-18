/* tests/unit/gpu_uv_table_test.c - psx_uv_plane_setup against the measured
 * table of the texel the PSX GPU reads at each pixel of a textured triangle
 * and sprite.
 *
 * The table (data/gpu-uv/uv.texels, written by
 * tools/gpu_blackbox/measure_uv.py) holds triangle cases (T, the vertices in
 * GP0 order) and sprite cases (S x y w h u v), each followed by runs of drawn
 * pixels with their texel coordinates (R y x n U V; U and V are the first
 * value then the steps between neighbours mod 256, d*r for r equal steps).
 * A triangle case checks the plane of its own vertices at every listed
 * pixel; a sprite case checks the planes of both triangles the sprite is fed
 * as (psx_uv.h) at every pixel of the sprite. A pixel passes when
 * floor(psx_uv_plane_at) mod 256 equals the table's u and v. A triangle
 * whose setup reports zero area must list no pixel. Each plane is also
 * checked to be affine (the caller evaluates it between pixels), and a setup
 * returning anything but 0 or 1 fails. */
#include "renderer/psx_uv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { long cases, pixels, bad_cases, bad_pixels, zero_area_drawn, not_affine, bad_return; int shown; } tally_t;

/** @brief Expands a run-length coded list of n texel coordinates.
 * @param s The coded list.
 * @param n The run length.
 * @param out Receives n values in 0..255.
 * @return 0, or -1 when the list does not hold exactly n values. */
static int expand(const char* s, int n, int* out)
{
    char* end;
    long v = strtol(s, &end, 10);
    int k = 0;
    if (end == s) return -1;
    out[k++] = (int)(v & 0xFF);
    s = end;
    while (*s == ',') {
        const long d = strtol(s + 1, &end, 10);
        long r = 1;
        if (*end == '*') r = strtol(end + 1, &end, 10);
        s = end;
        for (long i = 0; i < r; i++) {
            if (k >= n) return -1;
            v += d;
            out[k++] = (int)(v & 0xFF);
        }
    }
    return k == n ? 0 : -1;
}

/** @brief Reads the texel coordinate a plane gives at a pixel.
 * @param p The plane.
 * @param x The pixel column.
 * @param y The pixel row.
 * @param which_v Nonzero for v.
 * @return floor of the plane's value, mod 256. */
static int texel(const psx_uv_plane* p, int x, int y, int which_v)
{
    return (int)((long long)floor(psx_uv_plane_at(p, x, y, which_v)) & 0xFF);
}

/** @brief Tests a plane for affinity at a few points.
 * @param p The plane.
 * @return 1 when the midpoint values equal the means of their ends within 1e-6. */
static int affine(const psx_uv_plane* p)
{
    static const double pts[3][4] = { { 0, 0, 37, 11 }, { -5, 300, 120, 2 }, { 17.25, 3.5, 18.75, 90 } };
    for (int w = 0; w < 2; w++)
        for (int i = 0; i < 3; i++) {
            const double a = psx_uv_plane_at(p, pts[i][0], pts[i][1], w);
            const double b = psx_uv_plane_at(p, pts[i][2], pts[i][3], w);
            const double m = psx_uv_plane_at(p, (pts[i][0] + pts[i][2]) / 2, (pts[i][1] + pts[i][3]) / 2, w);
            if (fabs(m - (a + b) / 2) > 1e-6) return 0;
        }
    return 1;
}

typedef struct { int kind, ok[2]; psx_uv_plane pl[2]; char text[160]; long bad, px; } case_t;

/** @brief Closes a case: counts it and reports it when any pixel failed.
 * @param c The case.
 * @param t The tally. */
static void close_case(case_t* c, tally_t* t)
{
    if (!c->kind) return;
    t->cases++;
    if (c->bad) {
        t->bad_cases++;
        if (t->shown++ < 8) fprintf(stderr, "mismatch: %s: %ld of %ld pixels\n", c->text, c->bad, c->px);
    }
    c->kind = 0;
}

/** @brief Opens a triangle or sprite case from its table row.
 * @param line The row.
 * @param c Receives the case.
 * @param t The tally.
 * @return 1 opened, 0 bad row. */
static int open_case(const char* line, case_t* c, tally_t* t)
{
    memset(c, 0, sizeof *c);
    snprintf(c->text, sizeof c->text, "%.150s", line);
    c->text[strcspn(c->text, "\n")] = 0;
    int iv[2][3][4], n = 0;
    if (line[0] == 'T') {
        int* q = &iv[0][0][0];
        if (sscanf(line + 1, "%d %d %d %d %d %d %d %d %d %d %d %d",
                   q, q + 1, q + 2, q + 3, q + 4, q + 5, q + 6, q + 7, q + 8, q + 9, q + 10, q + 11) != 12) return 0;
        n = 1;
    } else {
        int x, y, w, h, u, v;
        if (sscanf(line + 1, "%d %d %d %d %d %d", &x, &y, &w, &h, &u, &v) != 6) return 0;
        const int cr[4][4] = { { x, y, u, v }, { x + w, y, u + w, v }, { x, y + h, u, v + h }, { x + w, y + h, u + w, v + h } };
        static const int idx[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
        for (int k = 0; k < 2; k++)
            for (int j = 0; j < 3; j++) memcpy(iv[k][j], cr[idx[k][j]], sizeof iv[k][j]);
        n = 2;
    }
    for (int k = 0; k < n; k++) {
        const int r = psx_uv_plane_setup((const int (*)[4])iv[k], &c->pl[k]);
        if (r != 0 && r != 1) t->bad_return++;
        c->ok[k] = r == 1;
        if (c->ok[k] && !affine(&c->pl[k])) t->not_affine++;
    }
    c->kind = n;
    return 1;
}

/** @brief Checks one run of pixels against the open case.
 * @param line The R row.
 * @param c The open case.
 * @param t The tally.
 * @return 0, or -1 on a bad row. */
static int check_run(const char* line, case_t* c, tally_t* t)
{
    int y, x, n, off = 0;
    static char us[65536], vs[65536];
    static int u[4096], v[4096];
    if (!c->kind || sscanf(line + 1, "%d %d %d %65535s %65535s%n", &y, &x, &n, us, vs, &off) != 5 || n <= 0 || n > 4096)
        return -1;
    if (expand(us, n, u) || expand(vs, n, v)) return -1;
    for (int i = 0; i < n; i++) {
        int bad = 0;
        for (int k = 0; k < c->kind; k++) {
            if (!c->ok[k]) { t->zero_area_drawn++; bad = 1; continue; }
            bad |= texel(&c->pl[k], x + i, y, 0) != u[i] || texel(&c->pl[k], x + i, y, 1) != v[i];
        }
        c->px++; t->pixels++;
        if (bad) { c->bad++; t->bad_pixels++; }
    }
    return 0;
}

/** @brief Runs every table case against psx_uv_plane_setup.
 * @param argc Argument count.
 * @param argv argv[1] is the table path.
 * @return 0 on pass, 1 on a mismatch or a bad table. */
int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s uv.texels\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    static char line[140000];
    tally_t t;
    case_t c;
    memset(&t, 0, sizeof t);
    memset(&c, 0, sizeof c);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (line[0] == 'T' || line[0] == 'S') {
            close_case(&c, &t);
            if (!open_case(line, &c, &t)) { fprintf(stderr, "bad row: %s", line); return 1; }
        } else if (line[0] != 'R' || check_run(line, &c, &t)) {
            fprintf(stderr, "bad row: %.200s\n", line);
            return 1;
        }
    }
    close_case(&c, &t);
    fclose(f);
    printf("gpu_uv_table_test: %ld cases, %ld pixels; %ld cases mismatch (%ld pixels), %ld pixels drawn on a zero-area setup, %ld planes not affine, %ld setups returned neither 0 nor 1\n",
           t.cases, t.pixels, t.bad_cases, t.bad_pixels, t.zero_area_drawn, t.not_affine, t.bad_return);
    const int fail = t.cases == 0 || t.pixels == 0 || t.bad_cases || t.not_affine || t.bad_return;
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
