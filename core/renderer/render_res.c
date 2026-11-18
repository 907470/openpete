/* Internal render resolution: value grammar and base-image arithmetic.
 * See openpete_render_res.h. No device, no renderer state. */
#include "openpete_render_res.h"
#include "openpete.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/** @brief Parse a run of decimal digits into a saturating value.
 * @param s Start of the run.
 * @param end Receives the first character after the run.
 * @return The value, saturated at 1000000; -1 when the run is empty. */
static long rr_digits(const char *s, const char **end)
{
    long v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        if (v < 1000000) v = v * 10 + (*p - '0');
        p++;
    }
    *end = p;
    return p == s ? -1 : v;
}

int openpete_render_res_parse(const char *s, openpete_render_res_t *out, char *msg, size_t cap)
{
    char dummy[1];
    if (!msg || cap == 0) { msg = dummy; cap = sizeof dummy; }
    msg[0] = '\0';
    if (!s || !*s) { snprintf(msg, cap, "--resolution expects window, N or WxH, got an empty value"); return -1; }
    if (!strcmp(s, "window")) { *out = (openpete_render_res_t){ OPENPETE_RR_WINDOW, 0, 0, 0 }; return 0; }
    const char *e = NULL;
    const long a = rr_digits(s, &e);
    if (a >= 0 && *e == '\0') {
        if (a < 1) { snprintf(msg, cap, "--resolution N expects 1..%d, got: %s", OPENPETE_RR_N_MAX, s); return -1; }
        if (a > OPENPETE_RR_N_MAX) {
            snprintf(msg, cap, "--resolution %s exceeds the maximum multiple %d; using %d",
                     s, OPENPETE_RR_N_MAX, OPENPETE_RR_N_MAX);
            *out = (openpete_render_res_t){ OPENPETE_RR_MULT, OPENPETE_RR_N_MAX, 0, 0 };
            return 1;
        }
        *out = (openpete_render_res_t){ OPENPETE_RR_MULT, (int)a, 0, 0 };
        return 0;
    }
    if (a >= 0 && *e == 'x') {
        const char *e2 = NULL;
        const long b = rr_digits(e + 1, &e2);
        if (b >= 0 && *e2 == '\0') {
            if (a < OPENPETE_RR_DIM_MIN || a > OPENPETE_RR_DIM_MAX ||
                b < OPENPETE_RR_DIM_MIN || b > OPENPETE_RR_DIM_MAX) {
                snprintf(msg, cap, "--resolution WxH expects each side %d..%d, got: %s",
                         OPENPETE_RR_DIM_MIN, OPENPETE_RR_DIM_MAX, s);
                return -1;
            }
            *out = (openpete_render_res_t){ OPENPETE_RR_EXACT, 0, (int)a, (int)b };
            return 0;
        }
    }
    snprintf(msg, cap, "--resolution expects window, N (1..%d) or WxH, got: %s", OPENPETE_RR_N_MAX, s);
    return -1;
}

void openpete_render_res_format(const openpete_render_res_t *v, char *buf, size_t cap)
{
    if (v->kind == OPENPETE_RR_MULT) snprintf(buf, cap, "%d", v->n);
    else if (v->kind == OPENPETE_RR_EXACT) snprintf(buf, cap, "%dx%d", v->w, v->h);
    else snprintf(buf, cap, "window");
}

int openpete_render_res_pack(const openpete_render_res_t *v)
{
    uint32_t a = 0, b = 0;
    if (v->kind == OPENPETE_RR_MULT) a = (uint32_t)v->n;
    else if (v->kind == OPENPETE_RR_EXACT) { a = (uint32_t)v->w; b = (uint32_t)v->h; }
    const uint32_t bits = ((uint32_t)v->kind & 3u) | ((a & 0x7FFFu) << 2) | ((b & 0x7FFFu) << 17);
    int r;
    memcpy(&r, &bits, sizeof r);
    return r;
}

openpete_render_res_t openpete_render_res_unpack(int packed)
{
    uint32_t bits;
    memcpy(&bits, &packed, sizeof bits);
    openpete_render_res_t v = { (int)(bits & 3u), 0, 0, 0 };
    const int a = (int)((bits >> 2) & 0x7FFFu), b = (int)((bits >> 17) & 0x7FFFu);
    if (v.kind == OPENPETE_RR_MULT) v.n = a;
    else if (v.kind == OPENPETE_RR_EXACT) { v.w = a; v.h = b; }
    else v.kind = OPENPETE_RR_WINDOW;
    return v;
}

const char *openpete_render_res_filter_name(int f)
{
    static const char *const k_names[] = { "copy", "nearest", "area", "sharp", "areaf" };
    return (f >= 0 && f <= OPENPETE_RR_F_AREAF) ? k_names[f] : "?";
}

/** @brief Choose one axis's filter from its source span and destination length.
 *
 * The integer-fetch filters (copy, nearest, area) need a whole-texel origin
 * and an integer span; every other case filters in float.
 * @param x0 Source origin in texels.
 * @param len Source span in texels.
 * @param d Destination length in pixels.
 * @param ratio Receives the integer ratio, 1 for the float filters.
 * @return The OPENPETE_RR_F_* filter. */
static int rr_axis_filter(float x0, float len, int d, int *ratio)
{
    *ratio = 1;
    const int whole = fabsf(x0 - floorf(x0 + 0.5f)) < 1e-6f && fabsf(len - floorf(len + 0.5f)) < 1e-6f;
    const int l = (int)floorf(len + 0.5f);
    if (whole && l > 0 && d > 0) {
        if (l == d) return OPENPETE_RR_F_COPY;
        if (d % l == 0) { *ratio = d / l; return OPENPETE_RR_F_NEAREST; }
        if (l % d == 0) { *ratio = l / d; return OPENPETE_RR_F_AREA; }
    }
    return len < (float)d ? OPENPETE_RR_F_SHARP : OPENPETE_RR_F_AREAF;
}

/** @brief Fill B, wf_render and the source rect from the value and the current n.
 * @param p The plan; kind, n (or bw, bh under _EXACT) and wf are read.
 * @param disp_w The game's display raster width.
 * @param clip_h The game's drawn lines. */
static void rr_size_base(openpete_render_res_plan_t *p, int disp_w, int clip_h)
{
    if (p->kind == OPENPETE_RR_MULT) {
        const double half = (double)disp_w * 0.5;
        int k = (int)ceil(half * ((double)p->wf - 1.0) - 1e-4);
        if (k < 0) k = 0;
        p->bw = p->n * (disp_w + 2 * k);
        p->bh = p->n * clip_h;
        p->wf_render = (float)(disp_w + 2 * k) / (float)disp_w;
        const float x0 = ((float)p->bw - (float)p->n * (float)disp_w * p->wf) * 0.5f;
        p->src[0] = x0 > 0.0f ? x0 : 0.0f;
        p->src[2] = (float)p->bw - p->src[0];
    } else {
        p->wf_render = p->wf;
        p->src[0] = 0.0f;
        p->src[2] = (float)p->bw;
    }
    p->src[1] = 0.0f;
    p->src[3] = (float)p->bh;
}

/** @brief Fill T and the per-axis filters from B and s.
 * @param p The plan. */
static void rr_finish(openpete_render_res_plan_t *p)
{
    p->tw = p->bw * p->s;
    p->th = p->bh * p->s;
    p->filter[0] = rr_axis_filter(p->src[0], p->src[2] - p->src[0], p->dst[2], &p->ratio[0]);
    p->filter[1] = rr_axis_filter(p->src[1], p->src[3] - p->src[1], p->dst[3], &p->ratio[1]);
}

/** @brief Report whether the scene target at factor s exceeds the bound on either axis.
 * @param p The plan, B filled.
 * @param s The factor.
 * @param lw Width bound.
 * @param lh Height bound.
 * @return 1 when over. */
static int rr_over(const openpete_render_res_plan_t *p, int s, long lw, long lh)
{
    return (long)p->bw * s > lw || (long)p->bh * s > lh;
}

int openpete_render_res_plan(const openpete_render_res_t *v, int ssaa, int win_w, int win_h,
                             const int dst[4], float wf, int disp_w, int clip_h,
                             uint32_t maxdim, openpete_render_res_plan_t *out)
{
    memset(out, 0, sizeof *out);
    out->s = 1;
    out->kind = v->kind;
    if (v->kind == OPENPETE_RR_WINDOW || win_w <= 0 || win_h <= 0 || dst[2] <= 0 || dst[3] <= 0 ||
        disp_w <= 0 || clip_h <= 0)
        return 0;
    memcpy(out->dst, dst, sizeof out->dst);
    out->wf = wf < 1.0f ? 1.0f : wf;
    out->n = v->kind == OPENPETE_RR_MULT ? v->n : 0;
    out->bw = v->w; out->bh = v->h;
    rr_size_base(out, disp_w, clip_h);
    const long lw = (long)win_w * OPENPETE_SSAA_MAX < (long)maxdim ? (long)win_w * OPENPETE_SSAA_MAX : (long)maxdim;
    const long lh = (long)win_h * OPENPETE_SSAA_MAX < (long)maxdim ? (long)win_h * OPENPETE_SSAA_MAX : (long)maxdim;
    int s = ssaa < 1 ? 1 : (ssaa > OPENPETE_SSAA_MAX ? OPENPETE_SSAA_MAX : ssaa);
    while (s > 1 && rr_over(out, s, lw, lh)) { s--; out->steps++; }
    if (out->kind == OPENPETE_RR_MULT) {
        while (out->n > 1 && rr_over(out, 1, lw, lh)) { out->n--; out->steps++; rr_size_base(out, disp_w, clip_h); }
    } else if (rr_over(out, 1, lw, lh)) {
        const double fw = (double)lw / out->bw, fh = (double)lh / out->bh;
        const double f = fw < fh ? fw : fh;
        out->bw = (int)floor(out->bw * f); out->bh = (int)floor(out->bh * f);
        if (out->bw < 1) out->bw = 1;
        if (out->bh < 1) out->bh = 1;
        out->steps++;
        rr_size_base(out, disp_w, clip_h);
    }
    out->s = s;
    out->active = 1;
    rr_finish(out);
    return 1;
}

int openpete_render_res_step_down(openpete_render_res_plan_t *p, int disp_w, int clip_h)
{
    if (!p->active) return 0;
    if (p->s > 1) p->s--;
    else if (p->kind == OPENPETE_RR_MULT && p->n > 1) { p->n--; rr_size_base(p, disp_w, clip_h); }
    else if (p->kind == OPENPETE_RR_EXACT && p->bw > 1 && p->bh > 1) {
        p->bw = p->bw * 3 / 4; p->bh = p->bh * 3 / 4;
        if (p->bw < 1) p->bw = 1;
        if (p->bh < 1) p->bh = 1;
        rr_size_base(p, disp_w, clip_h);
    } else return 0;
    p->steps++;
    rr_finish(p);
    return 1;
}
