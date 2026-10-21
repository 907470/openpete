/* openpete_view_rect.h - the native view's layout in a window and its pixel rect.
 *
 * The native scene is drawn into a sub-rect of the window: the 4:3 content
 * widened by the Hor+ factor, letterboxed or pillarboxed when the window's
 * shape differs from the render aspect. The background quad and the 2D bars
 * are drawn to that rect's clip-space edges, and the rasteriser covers a
 * pixel when its centre lies inside (left and top edges inclusive, right and
 * bottom exclusive). The scissor and the freeze-backdrop copy take the same
 * pixel set, so no pixel lies inside the scissor and outside every quad.
 * Header-only, no state. */
#ifndef OPENPETE_VIEW_RECT_H
#define OPENPETE_VIEW_RECT_H

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Aspect-fit content into a clip-space cell and return the fitted half-extents.
 *
 * The content is scaled uniformly by the smaller of the cell-to-content
 * width and height ratios, measured in window pixels.
 * @param chx Cell half-extent x in clip space.
 * @param chy Cell half-extent y in clip space.
 * @param content_w Content width in pixels.
 * @param content_h Content height in pixels.
 * @param win_w Window width in pixels.
 * @param win_h Window height in pixels.
 * @param out_hx Receives the fitted half-extent x in clip space.
 * @param out_hy Receives the fitted half-extent y in clip space. */
static inline void openpete_view_fit_cell(float chx, float chy, int content_w, int content_h,
                                          int win_w, int win_h, float *out_hx, float *out_hy)
{
    float cell_w = chx * (float)win_w;  /* cell size in px (full clip = win px) */
    float cell_h = chy * (float)win_h;
    float s = cell_w / (float)content_w;
    float sy = cell_h / (float)content_h;
    if (sy < s) s = sy;
    *out_hx = ((float)content_w * s) / (float)win_w;
    *out_hy = ((float)content_h * s) / (float)win_h;
}

/** @brief The native view's placement in one full-window cell, in clip space.
 *
 * cx is the centre x; hx and hy are the half-extents of the whole render
 * rect (the 4:3 content widened by wf); the 4:3 content sub-rect is hx / wf
 * wide about the same centre. */
typedef struct {
    float wf;   /* Hor+ widen factor, at least 1 */
    float cx;   /* centre x in clip space */
    float hx;   /* render-rect half-extent x in clip space */
    float hy;   /* render-rect half-extent y in clip space */
} openpete_view_layout_t;

/** @brief Compute the render rect of the native view in a full-window cell.
 *
 * A fixed aspect (aspect_wh > 0) gives wf = aspect_wh / (4/3) and fits that
 * rect into the window with bars. AUTO (aspect_wh 0) derives wf from the
 * window shape against the fit shape; when that is at least 1 the rect is
 * the window exactly, else wf is 1 and the 4:3 rect is fitted with
 * letterbox bars. wf is clamped to at least 1 in both modes.
 * @param win_w Window width in pixels.
 * @param win_h Window height in pixels.
 * @param fit_w Display-shape width the 4:3 content is fitted at.
 * @param fit_h Display-shape height the 4:3 content is fitted at.
 * @param aspect_wh Fixed render aspect, or 0 for AUTO.
 * @return The layout. */
static inline openpete_view_layout_t openpete_view_layout(int win_w, int win_h, int fit_w, int fit_h,
                                                          float aspect_wh)
{
    const float chx = 1.0f;   /* one full-window cell */
    openpete_view_layout_t L;
    float wf = 1.0f;
    int auto_fill = 0;   /* AUTO derived from the cell -> rect == cell */
    if (aspect_wh > 0.0f)
        wf = aspect_wh / (4.0f / 3.0f);
    else if (fit_w > 0 && fit_h > 0) {
        wf = ((float)win_w * (float)fit_h * chx) / ((float)win_h * (float)fit_w);
        auto_fill = (wf >= 1.0f);
    }
    if (wf < 1.0f) wf = 1.0f;
    L.wf = wf;
    L.cx = -1.0f + chx;
    if (auto_fill) {
        /* Set exactly instead of round-tripping through the fit's integer
         * target width, whose rounding leaves a sub-pixel pillar. */
        L.hx = chx; L.hy = 1.0f;
    } else {
        openpete_view_fit_cell(chx, 1.0f, (int)((float)fit_w * wf + 0.5f), fit_h,
                               win_w, win_h, &L.hx, &L.hy);
    }
    return L;
}

/** @brief Return the first pixel whose centre lies at or past a window-pixel edge.
 *
 * For a left or top edge this is the first covered pixel; for a right or
 * bottom edge it is one past the last covered pixel, so the covered span of
 * [lo, hi) is openpete_view_px_edge(lo) .. openpete_view_px_edge(hi).
 * @param e Edge position in window pixels.
 * @return The pixel index. */
static inline int openpete_view_px_edge(float e) { return (int)ceilf(e - 0.5f); }

/** @brief The view rect's edges in window pixels, unrounded. */
typedef struct { float l, t, r, b; } openpete_view_edges_t;

/** @brief Return the window-pixel edges of a clip-space rect centred at (cx, 0).
 *
 * x maps through (c * 0.5 + 0.5) * win_w and y through (0.5 - 0.5 * c) *
 * win_h (clip y up, pixels down); the right and bottom edges are the left
 * and top edges plus the rect's pixel size.
 * @param cx Centre x in clip space.
 * @param hx Half-extent x in clip space.
 * @param hy Half-extent y in clip space.
 * @param win_w Window width in pixels.
 * @param win_h Window height in pixels.
 * @return The edges. */
static inline openpete_view_edges_t openpete_view_edges(float cx, float hx, float hy,
                                                        int win_w, int win_h)
{
    openpete_view_edges_t e;
    e.l = ((cx - hx) * 0.5f + 0.5f) * (float)win_w;
    e.r = e.l + hx * (float)win_w;
    e.t = (0.5f - 0.5f * hy) * (float)win_h;
    e.b = e.t + hy * (float)win_h;
    return e;
}

/** @brief An integer pixel rect: origin and size. */
typedef struct { int x, y, w, h; } openpete_px_rect_t;

/** @brief Return the pixels whose centres lie inside a clip-space rect centred at (cx, 0).
 *
 * Both edges of each axis round through openpete_view_px_edge, so the rect
 * is the set the rasteriser covers for a quad at those edges. Rounding the
 * origin and the size separately instead puts the far edge one pixel past
 * the quad when both fractions round up.
 * @param cx Centre x in clip space.
 * @param hx Half-extent x in clip space.
 * @param hy Half-extent y in clip space.
 * @param win_w Window width in pixels.
 * @param win_h Window height in pixels.
 * @return The pixel rect; w or h is 0 for an empty span. */
static inline openpete_px_rect_t openpete_view_px_rect(float cx, float hx, float hy,
                                                       int win_w, int win_h)
{
    const openpete_view_edges_t e = openpete_view_edges(cx, hx, hy, win_w, win_h);
    openpete_px_rect_t p;
    p.x = openpete_view_px_edge(e.l);
    p.y = openpete_view_px_edge(e.t);
    const int x1 = openpete_view_px_edge(e.r), y1 = openpete_view_px_edge(e.b);
    p.w = x1 > p.x ? x1 - p.x : 0;
    p.h = y1 > p.y ? y1 - p.y : 0;
    return p;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_VIEW_RECT_H */
