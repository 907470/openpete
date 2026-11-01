/* openpete_launcher_fit.h - the launcher window's first size against a display.
 *
 * Two steps, both pure: the design size scaled by the display's content
 * scale where window coordinates are pixels (Windows), and the fit of a
 * request, with its window decorations, to the display's full and usable
 * bounds. All sizes are in the coordinates SDL creates the window in:
 * window coordinates on Wayland and macOS, pixels on X11 and Windows. Header-only, no state. */
#ifndef OPENPETE_LAUNCHER_FIT_H
#define OPENPETE_LAUNCHER_FIT_H

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How a launcher window request fits a display. */
typedef enum openpete_launcher_fit_how {
    OPENPETE_LAUNCHER_FIT_AS_REQUESTED = 0, /**< Fits the usable bounds. */
    OPENPETE_LAUNCHER_FIT_MAXIMIZED = 1,    /**< Fits the full bounds only. */
    OPENPETE_LAUNCHER_FIT_CLAMPED = 2       /**< Larger than the full bounds. */
} openpete_launcher_fit_how;

/** @brief Scale a size by a display content scale, rounding each side to the nearest pixel.
 *
 * A scale that is not positive (SDL_GetDisplayContentScale's failure
 * result, 0) leaves the size as it is.
 * @param w The width; receives the scaled width.
 * @param h The height; receives the scaled height.
 * @param scale The display's content scale (1.0 at 100%, 1.5 at 150%). */
static inline void openpete_launcher_scale_size(int *w, int *h, float scale)
{
    if (!(scale > 0.0f)) return;
    *w = (int)lroundf((float)*w * scale);
    *h = (int)lroundf((float)*h * scale);
}

/** @brief Fit a requested client size with its window decorations to a display: as requested when the outer size fits the usable bounds, maximized when it fits only the full bounds, else clamped so the outer size fits 90% of the usable bounds at the request's aspect.
 *
 * The request is the client area; the outer size the display holds is the
 * request plus the decorations (title bar, frame). The request, the
 * decorations and the bounds are in one coordinate space. The clamp scales
 * the client by the smaller of (0.9 * usable - decoration) / request per
 * axis, so the request's aspect is kept and the outer size fits 90% of the
 * usable bounds. Decorations of 0 compare the client size alone.
 * @param rw The requested client width.
 * @param rh The requested client height.
 * @param dw The decorations' total width (left plus right frame).
 * @param dh The decorations' total height (title bar and frame, top plus bottom).
 * @param full_w The display's full width.
 * @param full_h The display's full height.
 * @param use_w The display's usable width (without panels).
 * @param use_h The display's usable height.
 * @param w Receives the client width the window opens at (the request unless clamped).
 * @param h Receives the client height.
 * @return How the request fits. */
static inline openpete_launcher_fit_how openpete_launcher_fit(int rw, int rh, int dw, int dh, int full_w,
                                                              int full_h, int use_w, int use_h, int *w,
                                                              int *h)
{
    *w = rw;
    *h = rh;
    if (rw + dw <= use_w && rh + dh <= use_h) return OPENPETE_LAUNCHER_FIT_AS_REQUESTED;
    if (rw + dw <= full_w && rh + dh <= full_h) return OPENPETE_LAUNCHER_FIT_MAXIMIZED;
    const float kw = (0.9f * (float)use_w - (float)dw) / (float)rw;
    const float kh = (0.9f * (float)use_h - (float)dh) / (float)rh;
    const float k = kw < kh ? kw : kh;
    *w = (int)lroundf((float)rw * k);
    *h = (int)lroundf((float)rh * k);
    return OPENPETE_LAUNCHER_FIT_CLAMPED;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_LAUNCHER_FIT_H */
