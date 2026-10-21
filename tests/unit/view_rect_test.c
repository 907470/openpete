/* tests/unit/view_rect_test.c - the native view scissor covers exactly the
 * pixels the view's quads cover.
 *
 * Over a grid of window sizes (widths 640..2560 at eight heights, AUTO
 * aspect, plus fixed 4:3 and 16:9), the layout of openpete_view_layout is
 * turned into a pixel rect by openpete_view_px_rect and compared with the
 * pixel-centre set of the view rect's window-pixel edges, found by scanning
 * every pixel: a pixel is inside when l <= x + 0.5 < r (likewise in y), the
 * rasteriser's coverage rule for the background quad and the 2D bars drawn
 * to those edges. The previous arithmetic, which rounded the origin and the
 * size separately, is run on the same grid as the negative control: it must
 * disagree somewhere, or the sweep did not reach the case it guards. */

#include "openpete_view_rect.h"

#include <stdio.h>

/** @brief Find the covered span [lo, hi) of pixels 0..n-1 by scanning their centres.
 * @param a Low edge in pixels.
 * @param b High edge in pixels.
 * @param n Pixel count of the axis.
 * @param lo Receives the first covered pixel (n when none).
 * @param hi Receives one past the last covered pixel. */
static void scan_span(float a, float b, int n, int* lo, int* hi)
{
    *lo = n; *hi = 0;
    for (int x = 0; x < n; x++) {
        const float c = (float)x + 0.5f;
        if (c >= a && c < b) { if (*lo == n) *lo = x; *hi = x + 1; }
    }
    if (*lo == n) *lo = *hi = 0;
}

/** @brief The scissor arithmetic before the fix: origin and size rounded separately.
 * @param cx Centre x in clip space.
 * @param fhx Half-extent x in clip space.
 * @param hy Half-extent y in clip space.
 * @param W Window width.
 * @param H Window height.
 * @return The rect. */
static openpete_px_rect_t old_rect(float cx, float fhx, float hy, int W, int H)
{
    openpete_px_rect_t p;
    p.x = (int)(((cx - fhx) * 0.5f + 0.5f) * (float)W + 0.5f);
    p.w = (int)(fhx * (float)W + 0.5f);
    p.y = (int)((0.5f - 0.5f * hy) * (float)H + 0.5f);
    p.h = (int)(hy * (float)H + 0.5f);
    return p;
}

/** @brief Report whether a rect reaches a pixel outside the scanned pixel-centre set of the view edges.
 * @param p The rect.
 * @param e The view edges.
 * @param W Window width.
 * @param H Window height.
 * @return 1 when a scissor of this rect passes a pixel no view quad covers, else 0. */
static int overshoots(openpete_px_rect_t p, openpete_view_edges_t e, int W, int H)
{
    int x0, x1, y0, y1;
    scan_span(e.l, e.r, W, &x0, &x1);
    scan_span(e.t, e.b, H, &y0, &y1);
    return p.x < x0 || p.x + p.w > x1 || p.y < y0 || p.y + p.h > y1;
}

/** @brief Report whether a rect equals the scanned pixel-centre set of the view edges.
 * @param p The rect.
 * @param e The view edges.
 * @param W Window width.
 * @param H Window height.
 * @return 1 on a match, else 0. */
static int matches(openpete_px_rect_t p, openpete_view_edges_t e, int W, int H)
{
    int x0, x1, y0, y1;
    scan_span(e.l, e.r, W, &x0, &x1);
    scan_span(e.t, e.b, H, &y0, &y1);
    return p.x == x0 && p.w == x1 - x0 && p.y == y0 && p.h == y1 - y0;
}

/** @brief Sweep the window grid at one aspect and count the sizes each arithmetic gets wrong.
 * @param aspect Fixed aspect, or 0 for AUTO.
 * @param label Name printed with the counts.
 * @param old_bad Receives the sizes the old arithmetic gets wrong.
 * @param old_over Receives the sizes where the old arithmetic's scissor
 *        passes a pixel outside the view: the line the bars leave lit.
 * @return The sizes the helper gets wrong. */
static int sweep(float aspect, const char* label, int* old_bad, int* old_over)
{
    static const int heights[] = { 432, 600, 720, 768, 900, 1080, 1200, 1440 };
    int n = 0, bad = 0;
    *old_bad = 0; *old_over = 0;
    for (int W = 640; W <= 2560; W++)
        for (unsigned k = 0; k < sizeof heights / sizeof heights[0]; k++) {
            const int H = heights[k];
            const openpete_view_layout_t L = openpete_view_layout(W, H, 640, 432, aspect);
            /* The renderer stores the 4:3 sub-rect hx / wf and widens it back. */
            const float fhx = (L.hx / L.wf) * L.wf;
            const openpete_view_edges_t e = openpete_view_edges(L.cx, fhx, L.hy, W, H);
            n++;
            if (!matches(openpete_view_px_rect(L.cx, fhx, L.hy, W, H), e, W, H)) {
                if (bad < 4) fprintf(stderr, "FAIL %s %dx%d\n", label, W, H);
                bad++;
            }
            const openpete_px_rect_t o = old_rect(L.cx, fhx, L.hy, W, H);
            if (!matches(o, e, W, H)) (*old_bad)++;
            if (overshoots(o, e, W, H)) (*old_over)++;
        }
    printf("%s: %d sizes, helper wrong at %d, old arithmetic wrong at %d"
           " (scissor past the view at %d)\n", label, n, bad, *old_bad, *old_over);
    return bad;
}

/** @brief Run the sweeps.
 * @return 0 when the helper matches everywhere and the old arithmetic does not. */
int main(void)
{
    int fail = 0, old_bad = 0, old_over = 0, unused_bad = 0, unused_over = 0;
    fail |= sweep(0.0f, "AUTO", &old_bad, &old_over) != 0;
    fail |= sweep(4.0f / 3.0f, "4:3", &unused_bad, &unused_over) != 0;
    fail |= sweep(16.0f / 9.0f, "16:9", &unused_bad, &unused_over) != 0;
    if (old_over == 0) {
        fprintf(stderr, "FAIL negative control: the old arithmetic matched every AUTO size\n");
        fail = 1;
    }
    return fail;
}
