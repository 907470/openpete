/* Internal render resolution (--resolution window | N | WxH): the value
 * grammar and the size arithmetic of the base image, free of any device so
 * the width rule and the bounds are testable on the CPU. */
#ifndef OPENPETE_RENDER_RES_H
#define OPENPETE_RENDER_RES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Value kinds. */
#define OPENPETE_RR_WINDOW 0   /* the window, today's path */
#define OPENPETE_RR_MULT   1   /* N times the game's drawn raster */
#define OPENPETE_RR_EXACT  2   /* exact W x H pixels */

#define OPENPETE_RR_N_MAX   8
#define OPENPETE_RR_DIM_MIN 64
#define OPENPETE_RR_DIM_MAX 16384

/* Per-axis resample filters. */
#define OPENPETE_RR_F_COPY    0   /* equal size, whole-texel origin */
#define OPENPETE_RR_F_NEAREST 1   /* integer upscale, whole-texel origin */
#define OPENPETE_RR_F_AREA    2   /* integer downscale, whole-texel origin */
#define OPENPETE_RR_F_SHARP   3   /* sharp-bilinear upscale */
#define OPENPETE_RR_F_AREAF   4   /* area average at a fractional ratio or origin */

/** A --resolution value. */
typedef struct {
    int kind;   /**< OPENPETE_RR_WINDOW, _MULT or _EXACT */
    int n;      /**< the multiplier under _MULT */
    int w, h;   /**< the size under _EXACT */
} openpete_render_res_t;

/** The base image and its resample for one value, window and widen. */
typedef struct {
    int active;            /**< 0 under window: no base */
    int kind;              /**< the value's kind */
    int n;                 /**< effective multiplier under _MULT, after the bounds */
    int bw, bh;            /**< base size B */
    int s;                 /**< effective supersampling factor */
    int tw, th;            /**< scene target T = B x s */
    float wf;              /**< the view's widen */
    float wf_render;       /**< the widen B is composed at: wf' under _MULT, wf under _EXACT */
    float src[4];          /**< resample source rect in B texels: x0, y0, x1, y1 */
    int dst[4];            /**< resample destination rect: x, y, w, h */
    int filter[2];         /**< per-axis OPENPETE_RR_F_* */
    int ratio[2];          /**< per-axis integer ratio of the nearest and area filters, else 1 */
    int steps;             /**< bound steps taken (s, then N, or the WxH scale) */
} openpete_render_res_plan_t;

/** @brief Parse a --resolution value: window, digits only (N), or digits x digits (W x H).
 *
 * N = 0 is refused; N above OPENPETE_RR_N_MAX is clamped to it with a
 * message. W and H outside OPENPETE_RR_DIM_MIN..OPENPETE_RR_DIM_MAX, any
 * other text (maximized, trailing characters, a sign) are refused.
 * @param s The text.
 * @param out Receives the value when the return is not negative.
 * @param msg Receives the refusal or clamp message; may be NULL.
 * @param cap Capacity of msg.
 * @return 0 accepted, 1 accepted after clamping N, -1 refused. */
int openpete_render_res_parse(const char *s, openpete_render_res_t *out, char *msg, size_t cap);

/** @brief Write a value in the flag's spelling: window, N or WxH.
 * @param v The value.
 * @param buf Receives the text.
 * @param cap Capacity of buf. */
void openpete_render_res_format(const openpete_render_res_t *v, char *buf, size_t cap);

/** @brief Pack a value into one int for a live-latched store.
 * @param v The value.
 * @return The packed bits; window packs to 0. */
int openpete_render_res_pack(const openpete_render_res_t *v);

/** @brief Unpack a value packed by openpete_render_res_pack.
 * @param bits The packed bits.
 * @return The value. */
openpete_render_res_t openpete_render_res_unpack(int bits);

/** @brief Size the base image, its scene target and its resample for a value.
 *
 * Under _MULT, B = N (disp_w + 2k) x N clip_h with
 * k = ceil(disp_w/2 (wf - 1) - 1e-4), composed at wf' = (disp_w + 2k) / disp_w,
 * and the source rect is B's centre N disp_w wf columns. Under _EXACT, B is
 * W x H at wf and the source rect is all of B. Each axis of T = B x s is
 * bounded by maxdim and by OPENPETE_SSAA_MAX x the window's axis: s steps
 * down first, then N; an _EXACT size is scaled by one factor on both axes.
 * @param v The value.
 * @param ssaa The requested supersampling factor.
 * @param win_w Width the bound is taken against (the window, or the capture extent).
 * @param win_h Height the bound is taken against.
 * @param dst The picture area V in the output: x, y, w, h.
 * @param wf The view's widen, at least 1.
 * @param disp_w The game's display raster width.
 * @param clip_h The game's drawn lines.
 * @param maxdim The device's largest 2D image dimension.
 * @param out Receives the plan; active 0 under window or for an unusable input.
 * @return out->active. */
int openpete_render_res_plan(const openpete_render_res_t *v, int ssaa, int win_w, int win_h,
                             const int dst[4], float wf, int disp_w, int clip_h,
                             uint32_t maxdim, openpete_render_res_plan_t *out);

/** @brief Take one allocation step down: s first, then N, or an _EXACT size by 3/4.
 *
 * Recomputes B, T and the resample of the plan for the new size.
 * @param p The plan to step; its inputs are re-read from it.
 * @param disp_w The game's display raster width.
 * @param clip_h The game's drawn lines.
 * @return 1 when a step was taken, 0 when none is left. */
int openpete_render_res_step_down(openpete_render_res_plan_t *p, int disp_w, int clip_h);

/** @brief Name a per-axis filter for the engagement line.
 * @param f An OPENPETE_RR_F_* value.
 * @return copy, nearest, area, sharp or areaf. */
const char *openpete_render_res_filter_name(int f);

#ifdef __cplusplus
}
#endif

#endif
