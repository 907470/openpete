/* tests/unit/render_res_test.c - the --resolution grammar and the base-image
 * arithmetic (core/renderer/render_res.c) against hand-computed sizes: the
 * strict parser, the pack round trip, the N width rule (B even, each Hor+
 * side whole cells, crop under one cell), the bound steps (s, then N; WxH by
 * one factor) and the per-axis filter choice. */
#include "openpete_render_res.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail = 0, s_checks = 0;

/** @brief Count one check and print it when it fails.
 * @param ok The check's verdict.
 * @param what What was checked. */
static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) { s_fail++; fprintf(stderr, "FAIL: %s\n", what); }
}

/** @brief Check the parser's verdict and value for one text.
 * @param s The text.
 * @param rc The expected return.
 * @param kind The expected kind when accepted.
 * @param a The expected n or w.
 * @param b The expected h. */
static void parse_case(const char *s, int rc, int kind, int a, int b)
{
    openpete_render_res_t v = { -1, -1, -1, -1 };
    char msg[160];
    const int r = openpete_render_res_parse(s, &v, msg, sizeof msg);
    char what[200];
    snprintf(what, sizeof what, "parse '%s' -> %d (%s)", s ? s : "(null)", r, msg);
    check(r == rc, what);
    if (rc < 0 || r < 0) return;
    check(v.kind == kind, what);
    if (kind == OPENPETE_RR_MULT) check(v.n == a, what);
    if (kind == OPENPETE_RR_EXACT) check(v.w == a && v.h == b, what);
    openpete_render_res_t u = openpete_render_res_unpack(openpete_render_res_pack(&v));
    check(memcmp(&u, &v, sizeof u) == 0, "pack round trip");
}

/** @brief Plan one N or WxH case and check B, s and the crop.
 * @param v The value.
 * @param ssaa The requested factor.
 * @param win_w Bound width.
 * @param win_h Bound height.
 * @param wf The widen.
 * @param clip_h Drawn lines.
 * @param bw Expected base width.
 * @param bh Expected base height.
 * @param s Expected factor.
 * @param crop_cells Expected crop per side in PSX columns. */
static void plan_case(openpete_render_res_t v, int ssaa, int win_w, int win_h, float wf, int clip_h,
                      int bw, int bh, int s, double crop_cells)
{
    const int dst[4] = { 0, 0, win_w, win_h };
    openpete_render_res_plan_t p;
    openpete_render_res_plan(&v, ssaa, win_w, win_h, dst, wf, 512, clip_h, 16384u, &p);
    char what[200];
    snprintf(what, sizeof what, "plan kind=%d n=%d %dx%d wf=%.4f -> B=%dx%d s=%d x0=%.3f (want %dx%d s=%d crop %.2f)",
             v.kind, v.n, win_w, win_h, (double)wf, p.bw, p.bh, p.s, (double)p.src[0], bw, bh, s, crop_cells);
    check(p.active && p.bw == bw && p.bh == bh && p.s == s, what);
    if (v.kind == OPENPETE_RR_MULT && p.n > 0) {
        check(p.bw % 2 == 0, what);
        check(fabs(p.src[0] / p.n - crop_cells) < 0.01, what);
        check(p.src[0] < (float)p.n, what);
    }
}

int main(void)
{
    parse_case("window", 0, OPENPETE_RR_WINDOW, 0, 0);
    parse_case("1", 0, OPENPETE_RR_MULT, 1, 0);
    parse_case("8", 0, OPENPETE_RR_MULT, 8, 0);
    parse_case("9", 1, OPENPETE_RR_MULT, 8, 0);
    parse_case("99999999999", 1, OPENPETE_RR_MULT, 8, 0);
    parse_case("0", -1, 0, 0, 0);
    parse_case("1280x720", 0, OPENPETE_RR_EXACT, 1280, 720);
    parse_case("64x16384", 0, OPENPETE_RR_EXACT, 64, 16384);
    parse_case("63x100", -1, 0, 0, 0);
    parse_case("100000x1", -1, 0, 0, 0);
    parse_case("1280x720junk", -1, 0, 0, 0);
    parse_case("2x", -1, 0, 0, 0);
    parse_case("x2", -1, 0, 0, 0);
    parse_case("-2", -1, 0, 0, 0);
    parse_case("maximized", -1, 0, 0, 0);
    parse_case("Window", -1, 0, 0, 0);
    parse_case("", -1, 0, 0, 0);
    parse_case(NULL, -1, 0, 0, 0);

    const openpete_render_res_t n1 = { OPENPETE_RR_MULT, 1, 0, 0 };
    const openpete_render_res_t n2 = { OPENPETE_RR_MULT, 2, 0, 0 };
    const openpete_render_res_t n4 = { OPENPETE_RR_MULT, 4, 0, 0 };
    const openpete_render_res_t n8 = { OPENPETE_RR_MULT, 8, 0, 0 };
    /* Example sizes: Spyro 1 (224 lines), Spyro 2/3 (216). */
    plan_case(n1, 1, 1920, 1080, 1.0f, 224, 512, 224, 1, 0.0);
    plan_case(n2, 1, 1920, 1080, 1.0f, 224, 1024, 448, 1, 0.0);
    plan_case(n4, 1, 1920, 1080, 1.0f, 216, 2048, 864, 1, 0.0);
    const float wf_s1_169 = (16.0f / 9.0f) / (640.0f / 448.0f);
    const float wf_s2_169 = (16.0f / 9.0f) / (640.0f / 432.0f);
    plan_case(n1, 1, 1920, 1080, wf_s1_169, 224, 638, 224, 1, 0.42);
    plan_case(n2, 1, 1920, 1080, wf_s1_169, 224, 1276, 448, 1, 0.42);
    plan_case(n4, 1, 1920, 1080, wf_s1_169, 224, 2552, 896, 1, 0.42);
    plan_case(n2, 1, 1920, 1080, wf_s2_169, 216, 1232, 432, 1, 0.8);
    plan_case(n2, 1, 1920, 1080, 4.0f / 3.0f, 224, 1368, 448, 1, 0.67);
    plan_case(n4, 1, 1920, 1080, 4.0f / 3.0f, 216, 2736, 864, 1, 0.67);
    /* Bounds: headless Spyro 1 4:3 C = 640x448 steps N = 8 to 5; Hor+ 16:9
     * C = 853x448 steps N = 8 to 4; a 1920x1080 window takes N = 8. */
    plan_case(n8, 1, 640, 448, 1.0f, 224, 2560, 1120, 1, 0.0);
    plan_case(n8, 1, 853, 448, 4.0f / 3.0f, 224, 2736, 896, 1, 0.67);
    plan_case(n8, 1, 1920, 1080, wf_s1_169, 224, 5104, 1792, 1, 0.42);
    /* s steps before N: N = 2 at 640x448 with ssaa 4 -> T 4096 wide, s 2. */
    plan_case(n2, 4, 640, 448, 1.0f, 224, 1024, 448, 2, 0.0);
    /* WxH scaled by one factor, rounded down. */
    const openpete_render_res_t big = { OPENPETE_RR_EXACT, 0, 8000, 2000 };
    plan_case(big, 1, 640, 448, 1.0f, 224, 2560, 640, 1, 0.0);

    /* Filters: copy, nearest, area at integer ratios; sharp and areaf otherwise. */
    {
        const openpete_render_res_t e = { OPENPETE_RR_EXACT, 0, 640, 448 };
        const int d1[4] = { 0, 0, 640, 448 }, d2[4] = { 0, 0, 1280, 896 }, d3[4] = { 0, 0, 320, 224 },
                  d4[4] = { 0, 0, 1000, 700 }, d5[4] = { 0, 0, 500, 400 };
        openpete_render_res_plan_t p;
        openpete_render_res_plan(&e, 1, 640, 448, d1, 1.0f, 512, 224, 16384u, &p);
        check(p.filter[0] == OPENPETE_RR_F_COPY && p.filter[1] == OPENPETE_RR_F_COPY, "copy");
        openpete_render_res_plan(&e, 1, 1280, 896, d2, 1.0f, 512, 224, 16384u, &p);
        check(p.filter[0] == OPENPETE_RR_F_NEAREST && p.ratio[0] == 2, "nearest 2");
        openpete_render_res_plan(&e, 1, 320, 224, d3, 1.0f, 512, 224, 16384u, &p);
        check(p.filter[0] == OPENPETE_RR_F_AREA && p.ratio[1] == 2, "area 2");
        openpete_render_res_plan(&e, 1, 1000, 700, d4, 1.0f, 512, 224, 16384u, &p);
        check(p.filter[0] == OPENPETE_RR_F_SHARP && p.filter[1] == OPENPETE_RR_F_SHARP, "sharp");
        openpete_render_res_plan(&e, 1, 500, 400, d5, 1.0f, 512, 224, 16384u, &p);
        check(p.filter[0] == OPENPETE_RR_F_AREAF && p.filter[1] == OPENPETE_RR_F_AREAF, "areaf");
        openpete_render_res_plan(&n2, 1, 1920, 1080, (const int[4]){ 0, 0, 1920, 1080 }, wf_s1_169, 512, 224, 16384u, &p);
        check(p.filter[0] == OPENPETE_RR_F_SHARP, "N crop is filtered");
        const int st = openpete_render_res_step_down(&p, 512, 224);
        check(st == 1 && p.n == 1 && p.bw == 638, "step down N");
        check(openpete_render_res_step_down(&p, 512, 224) == 0, "no step below N = 1");
    }
    {
        const openpete_render_res_t w = { OPENPETE_RR_WINDOW, 0, 0, 0 };
        openpete_render_res_plan_t p;
        check(openpete_render_res_plan(&w, 2, 1920, 1080, (const int[4]){ 0, 0, 1920, 1080 }, 1.0f, 512, 224,
                                       16384u, &p) == 0, "window is no base");
    }

    printf("render_res_test: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
