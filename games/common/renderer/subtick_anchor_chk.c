// games/common/renderer/subtick_anchor_chk.c - temporary sub-tick anchor probe.
//
// OPENPETE_SUBTICK_ANCHOR_CHK=1 (summary at exit) or 2 (also one line per
// sample). On each sub-tick present a feeder hands every fed anchor (a glow
// fan's centre, a tracer point before its width offset) with the world points
// it paired. The reference is built apart from the feeder's own view:
//   * the camera is the camera ring's prev/curr pair at the present clock,
//     matrix and position lerped in float at alpha_ref = (now - curr) / (curr
//     - prev), clamped to [-0.35, 1] as the shared camera bracket does;
//   * R_cam: the world point lerped at alpha_ref, projected under that camera;
//   * R_obj: the world point lerped at the object's own clock, (now -
//     curr_ns) / (curr_ns - prev_ns) clamped to [0, 1], the moby feeder's alpha;
//   * R_scr: the screen-space lerp at alpha_ref of the prev point projected
//     under the prev camera and the curr point under the curr camera, the
//     reference a centroid-lerp harness judges against; D is |E_cam - S|.
// Camera motion per present: the ring pair's position step and rotation
// angle. Never writes PSX RAM.

#include "renderer/subtick_anchor_chk.h"
#include "renderer/interp_regions_gen.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "game_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

typedef struct {
    int    ok;
    double Mp[3][3], Mc[3][3], Pp[3], Pc[3];
    double a_ref, a_fed, dpos, drot;
    int64_t now;
} sac_cam;

static sac_cam s_cam;

typedef struct {
    double r_cam, r_obj, r_scr, d, a_ref, a_fed, a_obj, dpos, drot, step_px;
    unsigned frame;
    uint32_t key;
} sac_case;

typedef struct {
    unsigned long n, held, presents;
    double sum[4], mx[4];
    sac_case worst[3];
} sac_stat;

static sac_stat s_st[SAC_N];

/* Presents served without a camera pair, and the residual of their anchors
 * against the capture; `over1` counts anchors more than 1 px off. */
typedef struct { unsigned long presents, n, over1; double sum, mx; unsigned mx_frame; } sac_unb;
static sac_unb s_unb[SAC_N];
static unsigned long s_bracketed[SAC_N];

/* Sub-tick depth samples per path: the stale (tick) depth against the
 * present's true depth, and the fed depth against the same. `near` counts
 * samples whose stale depth is nearer than the true one by at least one
 * ordering-table slot (128 view units); `rate` is the true depth's change
 * toward the camera per tick, (stale - truth) / (1 - alpha) negated. */
typedef struct {
    unsigned long n, near1, far1;
    double near_mx, far_mx, sum_abs, fed_mx, rate_mx;
    unsigned near_frame;
    double near_alpha;
} sac_dstat;
static sac_dstat s_dep[SAC_N];
static const char* const k_name[SAC_N] = { "glow", "ribbon", "mesh" };

/** @brief Print the per-path summary and the three worst cases; registered with atexit. */
static void sac_report(void) {
    static const char* const m[4] = { "R_cam", "R_obj", "R_scr", "D_cam_scr" };
    for (int p = 0; p < SAC_N; p++) {
        const sac_stat* s = &s_st[p];
        fprintf(stderr, "[sac %s] judged=%lu held=%lu", k_name[p], s->n, s->held);
        for (int k = 0; k < 4; k++)
            fprintf(stderr, " %s mean=%.3f max=%.3f", m[k], s->n ? s->sum[k] / (double)s->n : 0.0, s->mx[k]);
        fprintf(stderr, "\n");
        const sac_unb* u = &s_unb[p];
        fprintf(stderr, "[sac %s unbracketed] presents=%lu bracketed_presents=%lu anchors=%lu over1px=%lu"
                        " resid mean=%.3f max=%.3f max_vbl=%u\n",
                k_name[p], u->presents, s_bracketed[p], u->n, u->over1, u->n ? u->sum / (double)u->n : 0.0,
                u->mx, u->mx_frame);
        const sac_dstat* d = &s_dep[p];
        fprintf(stderr, "[sac %s depth] samples=%lu stale_nearer_1slot=%lu stale_farther_1slot=%lu"
                        " stale_nearer_max=%.1f (vbl=%u a=%.3f) stale_farther_max=%.1f stale_abs_mean=%.2f"
                        " approach_max=%.1f/tick fed_resid_max=%.3f\n",
                k_name[p], d->n, d->near1, d->far1, d->near_mx, d->near_frame, d->near_alpha, d->far_mx,
                d->n ? d->sum_abs / (double)d->n : 0.0, d->rate_mx, d->fed_mx);
        for (int w = 0; w < 3; w++) {
            const sac_case* c = &s->worst[w];
            if (c->frame == 0u) continue;
            fprintf(stderr, "[sac %s worst%d] vbl=%u key=%08X R_cam=%.3f R_obj=%.3f R_scr=%.3f D=%.3f"
                            " a_ref=%.3f a_fed=%.3f a_obj=%.3f step_px=%.2f cam_dpos=%.1f cam_drot=%.3fdeg\n",
                    k_name[p], w, c->frame, c->key, c->r_cam, c->r_obj, c->r_scr, c->d, c->a_ref, c->a_fed,
                    c->a_obj, c->step_px, c->dpos, c->drot);
        }
    }
}

int sac_level(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_SUBTICK_ANCHOR_CHK");
        v = (e && *e) ? atoi(e) : 0;
        if (v > 0) atexit(sac_report);
    }
    return v;
}

/** @brief Decode a ring camera row's matrix (+0x00, five packed words) and position (+0x28).
 * @param row The camera row's bytes.
 * @param M Receives the matrix, Q12.
 * @param P Receives the position. */
static void sac_cam_decode(const uint8_t* row, double M[3][3], double P[3]) {
    uint32_t w[5];
    memcpy(w, row, sizeof w);
    int Mi[3][3];
    ngte_unpack_matrix(w[0], w[1], w[2], w[3], w[4], Mi);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) M[r][c] = Mi[r][c];
    int32_t p[3];
    memcpy(p, row + 0x28, sizeof p);
    for (int k = 0; k < 3; k++) P[k] = p[k];
}

/** @brief The rotation angle between two camera matrices, rows normalised, in degrees.
 * @param A One matrix.
 * @param B The other.
 * @return The angle. */
static double sac_rot_deg(const double A[3][3], const double B[3][3]) {
    double a[3][3], b[3][3];
    for (int r = 0; r < 3; r++) {
        double na = 0.0, nb = 0.0;
        for (int c = 0; c < 3; c++) { na += A[r][c] * A[r][c]; nb += B[r][c] * B[r][c]; }
        na = na > 0.0 ? sqrt(na) : 1.0; nb = nb > 0.0 ? sqrt(nb) : 1.0;
        for (int c = 0; c < 3; c++) { a[r][c] = A[r][c] / na; b[r][c] = B[r][c] / nb; }
    }
    double tr = 0.0;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) tr += a[r][c] * b[r][c];
    double x = (tr - 1.0) / 2.0;
    if (x > 1.0) x = 1.0; else if (x < -1.0) x = -1.0;
    return acos(x) * 180.0 / M_PI;
}

void sac_present_begin(int path, double alpha_fed) {
    s_cam.ok = 0;
    if (sac_level() > 0 && path >= 0 && path < SAC_N) s_bracketed[path]++;
    if (sac_level() <= 0) return;
    static int region = -2;
    if (region == -2) region = game_interp_row_index(GAME_INTERP_CAM_ROW);
    if (region < 0) return;
    double qa = 0.0;
    const openpete_interp_snapshot_t *p2 = NULL, *pv = NULL, *cu = NULL;
    const op_time_t now = openpete_interp_present_now();
    if (!openpete_interp_query3(now, g_openpete_tick_period_ns, &qa, &p2, &pv, &cu) || !pv || !cu) return;
    sac_cam_decode(pv->data + pv->region_offsets[region], s_cam.Mp, s_cam.Pp);
    sac_cam_decode(cu->data + cu->region_offsets[region], s_cam.Mc, s_cam.Pc);
    const double period = cu->tick_ns > pv->tick_ns ? (double)(cu->tick_ns - pv->tick_ns)
                                                    : (double)g_openpete_tick_period_ns;
    double a = (double)(now - cu->tick_ns) / period;
    if (a < -0.35) a = -0.35; else if (a > 1.0) a = 1.0;
    s_cam.a_ref = a;
    s_cam.a_fed = alpha_fed;
    s_cam.now = (int64_t)now;
    double d2 = 0.0;
    for (int k = 0; k < 3; k++) d2 += (s_cam.Pc[k] - s_cam.Pp[k]) * (s_cam.Pc[k] - s_cam.Pp[k]);
    s_cam.dpos = sqrt(d2);
    s_cam.drot = sac_rot_deg(s_cam.Mp, s_cam.Mc);
    s_cam.ok = 1;
}

void sac_unbracketed_present(int path) {
    if (sac_level() > 0 && path >= 0 && path < SAC_N) s_unb[path].presents++;
}

void sac_unbracketed_anchor(int path, double resid) {
    if (sac_level() <= 0 || path < 0 || path >= SAC_N) return;
    sac_unb* u = &s_unb[path];
    u->n++;
    u->sum += resid;
    if (resid > 1.0) u->over1++;
    if (resid > u->mx) { u->mx = resid; u->mx_frame = openpete_frame; }
    if (sac_level() >= 2)
        fprintf(stderr, "[sac-unb %s vbl=%u] resid=%.3f\n", k_name[path], openpete_frame, resid);
}

/** @brief Project a world point under a float camera, as the feeders' offset convention does.
 * @param M The camera matrix, Q12.
 * @param P The camera position.
 * @param w The world point.
 * @param v The view (offsets, H and --fov scale).
 * @param sx Receives the screen X.
 * @param sy Receives the screen Y. */
static void sac_project(const double M[3][3], const double P[3], const double w[3], const ngte_view* v,
                        double* sx, double* sy) {
    const double o0 = P[1] - w[1], o1 = P[2] - w[2], o2 = w[0] - P[0];
    double m[3];
    for (int r = 0; r < 3; r++) m[r] = (M[r][0] * o0 + M[r][1] * o1 + M[r][2] * o2) / 4096.0;
    ngte_proj o;
    ngte_screen_gte(m[0], m[1], m[2], v->ofx, v->ofy, v, 0, &o);
    *sx = o.sx; *sy = o.sy;
}

/** @brief Project a world point under the reference camera lerped at an alpha.
 * @param a The alpha.
 * @param w The world point.
 * @param v The view.
 * @param sx Receives the screen X.
 * @param sy Receives the screen Y. */
static void sac_project_at(double a, const double w[3], const ngte_view* v, double* sx, double* sy) {
    double M[3][3], P[3];
    for (int r = 0; r < 3; r++) {
        P[r] = s_cam.Pp[r] + (s_cam.Pc[r] - s_cam.Pp[r]) * a;
        for (int c = 0; c < 3; c++) M[r][c] = s_cam.Mp[r][c] + (s_cam.Mc[r][c] - s_cam.Mp[r][c]) * a;
    }
    sac_project(M, P, w, v, sx, sy);
}

/** @brief Insert a case into a path's worst-three list by R_cam.
 * @param s The path's statistics.
 * @param c The case. */
static void sac_worst_insert(sac_stat* s, const sac_case* c) {
    for (int i = 0; i < 3; i++) {
        if (s->worst[i].frame != 0u && c->r_cam <= s->worst[i].r_cam) continue;
        if (s->worst[i].frame == c->frame && s->worst[i].key == c->key) return;
        for (int j = 2; j > i; j--) s->worst[j] = s->worst[j - 1];
        s->worst[i] = *c;
        return;
    }
}

void sac_judge(int path, uint32_t key, const double* prev_w, const double curr_w[3],
               int64_t prev_ns, int64_t curr_ns, double fed_sx, double fed_sy, const ngte_view* v) {
    if (!s_cam.ok || path < 0 || path >= SAC_N) return;
    sac_stat* s = &s_st[path];
    if (!prev_w) { s->held++; return; }
    const double a = s_cam.a_ref;
    double w[3];
    for (int k = 0; k < 3; k++) w[k] = curr_w[k] + (prev_w[k] - curr_w[k]) * (1.0 - a);
    double ex, ey;
    sac_project_at(a, w, v, &ex, &ey);
    double ao = 1.0;
    if (curr_ns > prev_ns && prev_ns > 0) {
        ao = (double)(s_cam.now - curr_ns) / (double)(curr_ns - prev_ns);
        if (ao < 0.0) ao = 0.0; else if (ao > 1.0) ao = 1.0;
    }
    double wo[3];
    for (int k = 0; k < 3; k++) wo[k] = curr_w[k] + (prev_w[k] - curr_w[k]) * (1.0 - ao);
    double ox, oy;
    sac_project_at(a, wo, v, &ox, &oy);
    double px, py, cx, cy;
    sac_project(s_cam.Mp, s_cam.Pp, prev_w, v, &px, &py);
    sac_project(s_cam.Mc, s_cam.Pc, curr_w, v, &cx, &cy);
    const double sx = px + (cx - px) * a, sy = py + (cy - py) * a;
    sac_case c;
    memset(&c, 0, sizeof c);
    c.r_cam = hypot(fed_sx - ex, fed_sy - ey);
    c.r_obj = hypot(fed_sx - ox, fed_sy - oy);
    c.r_scr = hypot(fed_sx - sx, fed_sy - sy);
    c.d     = hypot(ex - sx, ey - sy);
    c.a_ref = a; c.a_fed = s_cam.a_fed; c.a_obj = ao;
    c.dpos = s_cam.dpos; c.drot = s_cam.drot;
    c.step_px = hypot(cx - px, cy - py);
    c.frame = openpete_frame; c.key = key;
    const double r[4] = { c.r_cam, c.r_obj, c.r_scr, c.d };
    for (int k = 0; k < 4; k++) { s->sum[k] += r[k]; if (r[k] > s->mx[k]) s->mx[k] = r[k]; }
    s->n++;
    sac_worst_insert(s, &c);
    if (sac_level() >= 2)
        fprintf(stderr, "[sac-s %s vbl=%u] key=%08X fed=(%.2f,%.2f) e_cam=(%.2f,%.2f) R_cam=%.3f R_obj=%.3f"
                        " R_scr=%.3f D=%.3f a_ref=%.3f a_fed=%.3f a_obj=%.3f step_px=%.2f cam_dpos=%.1f"
                        " cam_drot=%.3f\n",
                k_name[path], openpete_frame, key, fed_sx, fed_sy, ex, ey, c.r_cam, c.r_obj, c.r_scr, c.d,
                a, s_cam.a_fed, ao, c.step_px, s_cam.dpos, s_cam.drot);
}

void sac_depth(int path, double stale, double fed, double truth, double alpha) {
    if (!s_cam.ok || path < 0 || path >= SAC_N) return;
    sac_dstat* d = &s_dep[path];
    const double gap = truth - stale;          /* > 0: the stale depth is nearer than the truth */
    d->n++;
    d->sum_abs += fabs(gap);
    if (gap >= 128.0) d->near1++;
    if (gap <= -128.0) d->far1++;
    if (gap > d->near_mx) { d->near_mx = gap; d->near_frame = openpete_frame; d->near_alpha = alpha; }
    if (-gap > d->far_mx) d->far_mx = -gap;
    if (fabs(fed - truth) > d->fed_mx) d->fed_mx = fabs(fed - truth);
    if (alpha < 0.999 && gap / (1.0 - alpha) > d->rate_mx) d->rate_mx = gap / (1.0 - alpha);
    if (sac_level() >= 2)
        fprintf(stderr, "[sac-d %s vbl=%u] a=%.3f stale=%.1f fed=%.1f true=%.1f gap=%.1f\n",
                k_name[path], openpete_frame, alpha, stale, fed, truth, gap);
}

int sac_tick_depth(int tracer) {
    static int v = -1;   /* bit 0 the glow, bit 1 the tracers */
    if (v < 0) {
        const char* e = getenv("OPENPETE_SUBTICK_TICK_DEPTH");
        v = !e || !*e || *e == '0' ? 0 : *e == 'g' ? 1 : *e == 't' ? 2 : 3;
    }
    return (v >> (tracer ? 1 : 0)) & 1;
}
