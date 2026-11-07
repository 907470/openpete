/* moby_snap.c - contact planes of pulled mobys against the drawn terrain;
 * see moby_snap.h. Camera space throughout is the PSX screen unprojected at
 * view-Z with the terrain build's projection: p = ((sx - ofx) z / H,
 * (sy - ofy) z / H, z), the space the snap stage rebuilds a fragment in. */
#include "renderer/moby_snap.h"
#include "renderer/terrain.h"
#include "renderer/orchestrator.h"   /* canonical epoch: the carry's tables */
#include "openpete_profile.h"        /* mcan.resolve / mrex.resolve */
#include "openpete_native_render.h"
#include "openpete_fiber.h"          /* openpete_feed_key_now: census line label */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "openpete_thread_roles.h"

/** @brief The smaller of two finite floats, inline (fminf is a library call on this target).
 * @param a First value.
 * @param b Second value.
 * @return The smaller. */
static inline float ms_minf(float a, float b) { return b < a ? b : a; }

/** @brief The larger of two finite floats, inline (fmaxf is a library call on this target).
 * @param a First value.
 * @param b Second value.
 * @return The larger. */
static inline float ms_maxf(float a, float b) { return b > a ? b : a; }

/** @brief The smaller of two finite doubles, inline.
 * @param a First value.
 * @param b Second value.
 * @return The smaller. */
static inline double ms_min(double a, double b) { return b < a ? b : a; }

/** @brief The larger of two finite doubles, inline.
 * @param a First value.
 * @param b Second value.
 * @return The larger. */
static inline double ms_max(double a, double b) { return b > a ? b : a; }

/* One queued moby: its identity, ordinal, pull, run of s_q_tri and the
 * bounds of that run (screen x0, y0, x1, y1, view-Z z0, z1); its index in
 * the queue as collected (qi) and whether its planes go to the scene's
 * table (add; 0 for a moby the carry already placed). */
typedef struct { uint32_t mid; int mord, dofs; float off; int t0, nt; float b[6]; int qi; int add; } ms_moby;

static ms_moby*   s_q_mob;  static int s_q_nm, s_q_capm;
static float    (*s_q_tri)[3][3]; static int s_q_nt, s_q_capt;
static float    (*s_q_bnd)[6]; static int s_q_capb;   /* per s_q_tri: its bounds */
static unsigned   s_q_epoch = ~0u;

/* The planes one moby may own; more are counted as overflow. */
#define MS_MAX_PLANES 64
/* Contact planes merge when their normals agree to within this cosine
 * deficit and their offsets to within one world unit. */
#define MS_MERGE_DN 1e-4
#define MS_MERGE_DD 1.0

/* Per-resolve cost counters (the census line). */
typedef struct {
    long pulled, faces, tris, sectors, tests, contacts, planes, ovf;
    long contacts_t4;     /* contacts with a face drawn as 16 or more triangles */
    long overlaps;        /* pairs of snapped mobys whose screen boxes overlap */
    float zmin;           /* the nearest key depth of a snapped moby */
    uint64_t cap_ns;
    int timed;            /* the phase clocks below run */
    uint64_t near_ns;     /* candidate triangles and the near moby triangles */
    uint64_t test_ns;     /* the exact triangle tests and the plane merge */
    uint64_t plane_ns;    /* the plane pass: corners, side and slab */
    long cut_mobys;       /* snapped mobys a coplanar drawn triangle still cuts (ms_falsify) */
    long cut_samples;     /* their cut sample points */
    long cut_cls[3];      /* the cut samples by class (ms_falsify): 0 outside the plane's slab
                           * or disc, 1 inside both under one of the plane's contact
                           * triangles, 2 inside both under a coplanar non-contact triangle */
    long cut_b_mobys;     /* mobys with a class-1 cut */
    long carried;         /* mobys whose planes the carry placed (ms_carry_begin) */
    long fresh;           /* mobys of a carried present resolved by the contact pass */
} ms_census;

/** @brief The monotonic clock in nanoseconds, for the census's phase split.
 * @return The time. */
static uint64_t ms_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/** @brief Grow a queue array to hold need elements.
 * @param p The array pointer.
 * @param cap Its capacity, updated.
 * @param need Elements wanted.
 * @param esz Element size.
 * @return 1 when it holds them, 0 on an allocation failure. */
static int ms_grow(void** p, int* cap, int need, size_t esz) {
    if (need <= *cap) return 1;
    int nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    void* q = realloc(*p, (size_t)nc * esz);
    if (!q) return 0;
    *p = q; *cap = nc;
    return 1;
}

/* The scene's ranked mobys, one (identity, ordinal) per walk pass entry,
 * and its snapped mobys with an ordinal: the ordinal-collision census. */
typedef struct { uint32_t mid; int ord; } ms_ord;
static ms_ord s_rk[1024]; static int s_rk_n;
static ms_ord s_sn[256];  static int s_sn_n;

/** @brief Start the scene's queue and ordinal census when the scene changed. */
static void ms_scene_sync(void) {
    const unsigned ep = openpete_native_scene_epoch();
    if (ep == s_q_epoch) return;
    s_q_epoch = ep; s_q_nm = s_q_nt = 0; s_rk_n = s_sn_n = 0;
}

void moby_snap_note_ranked(const uint32_t* mids, int n, int base) {
    ms_scene_sync();
    for (int k = 0; k < n && s_rk_n < 1024; k++) { s_rk[s_rk_n].mid = mids[k]; s_rk[s_rk_n++].ord = base + k + 1; }
}

/** @brief Count the pairs (snapped moby with ordinal k, another ranked moby of the scene with ordinal k): faces whose ordinal lookup would find the wrong entry.
 * @return The pair count. */
static long ms_ord_collisions(void) {
    long n = 0;
    for (int a = 0; a < s_sn_n; a++)
        for (int b = 0; b < s_rk_n; b++)
            n += s_rk[b].ord == s_sn[a].ord && s_rk[b].mid != s_sn[a].mid;
    return n;
}

/** @brief Write each triangle's bounds (screen x0, y0, x1, y1, view-Z z0, z1) and their union.
 * @param t The triangles' corners (sx, sy, view-Z).
 * @param b Receives one bounds record per triangle.
 * @param n Triangle count.
 * @param u Receives the union. */
static void ms_bounds_tris(const float (*t)[3][3], float (*b)[6], int n, float u[6]) {
    u[0] = u[1] = u[4] = 1e30f; u[2] = u[3] = u[5] = -1e30f;
    for (int i = 0; i < n; i++) {
        const float(*c)[3] = t[i];
        b[i][0] = ms_minf(ms_minf(c[0][0], c[1][0]), c[2][0]); b[i][2] = ms_maxf(ms_maxf(c[0][0], c[1][0]), c[2][0]);
        b[i][1] = ms_minf(ms_minf(c[0][1], c[1][1]), c[2][1]); b[i][3] = ms_maxf(ms_maxf(c[0][1], c[1][1]), c[2][1]);
        b[i][4] = ms_minf(ms_minf(c[0][2], c[1][2]), c[2][2]); b[i][5] = ms_maxf(ms_maxf(c[0][2], c[1][2]), c[2][2]);
        u[0] = ms_minf(u[0], b[i][0]); u[1] = ms_minf(u[1], b[i][1]); u[2] = ms_maxf(u[2], b[i][2]);
        u[3] = ms_maxf(u[3], b[i][3]); u[4] = ms_minf(u[4], b[i][4]); u[5] = ms_maxf(u[5], b[i][5]);
    }
}

void moby_snap_queue(uint32_t mid, int mordinal, int dofs, float off_m,
                     const float (*v)[3][3], int n) {
    ms_scene_sync();
    if (n <= 0 || !mid || dofs <= 0) return;
    if (!ms_grow((void**)&s_q_mob, &s_q_capm, s_q_nm + 1, sizeof *s_q_mob) ||
        !ms_grow((void**)&s_q_tri, &s_q_capt, s_q_nt + n, sizeof *s_q_tri) ||
        !ms_grow((void**)&s_q_bnd, &s_q_capb, s_q_nt + n, sizeof *s_q_bnd)) return;
    ms_moby* m = &s_q_mob[s_q_nm++];
    m->mid = mid; m->mord = mordinal; m->dofs = dofs; m->off = off_m;
    m->t0 = s_q_nt; m->nt = n;
    memcpy(&s_q_tri[s_q_nt], v, (size_t)n * sizeof *s_q_tri);
    ms_bounds_tris(&s_q_tri[s_q_nt], &s_q_bnd[s_q_nt], n, m->b);
    s_q_nt += n;
}

/* ---- geometry ------------------------------------------------------------ */

typedef struct { double x, y, z; } ms_v3;
/** @brief Return a minus b. */
static ms_v3 v3sub(ms_v3 a, ms_v3 b) { ms_v3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
/** @brief Return the dot product of a and b. */
static double v3dot(ms_v3 a, ms_v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
/** @brief Return the cross product of a and b. */
static ms_v3 v3cross(ms_v3 a, ms_v3 b) {
    ms_v3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}
/** @brief Return the length of a. */
static double v3len(ms_v3 a) { return sqrt(v3dot(a, a)); }

/* A camera-space triangle with its plane and bounds. */
typedef struct { ms_v3 p[3]; ms_v3 n; double d; ms_v3 lo, hi; int ok; } ms_tri;

/** @brief Build a camera-space triangle from screen corners, with its unit normal, offset and bounds.
 * @param o Receives the triangle; ok is 0 for a degenerate one.
 * @param sx Corner screen X.
 * @param sy Corner screen Y.
 * @param z Corner view-Z.
 * @param H Projection distance.
 * @param ofx Optical axis X.
 * @param ofy Optical axis Y. */
static void ms_tri_make(ms_tri* o, const float sx[3], const float sy[3], const float z[3],
                        double H, double ofx, double ofy) {
    for (int k = 0; k < 3; k++) {
        o->p[k].x = ((double)sx[k] - ofx) * z[k] / H;
        o->p[k].y = ((double)sy[k] - ofy) * z[k] / H;
        o->p[k].z = z[k];
    }
    ms_v3 n = v3cross(v3sub(o->p[1], o->p[0]), v3sub(o->p[2], o->p[0]));
    const double l = v3len(n);
    o->ok = l > 0.0;
    if (o->ok) { n.x /= l; n.y /= l; n.z /= l; }
    o->n = n; o->d = v3dot(n, o->p[0]);
    o->lo = o->hi = o->p[0];
    for (int k = 1; k < 3; k++) {
        o->lo.x = ms_min(o->lo.x, o->p[k].x); o->hi.x = ms_max(o->hi.x, o->p[k].x);
        o->lo.y = ms_min(o->lo.y, o->p[k].y); o->hi.y = ms_max(o->hi.y, o->p[k].y);
        o->lo.z = ms_min(o->lo.z, o->p[k].z); o->hi.z = ms_max(o->hi.z, o->p[k].z);
    }
}

/** @brief Whether the segment a-b crosses triangle T (inside its edges, the plane crossing included).
 * @param a Segment start.
 * @param b Segment end.
 * @param T The triangle.
 * @return 1 when it does, else 0. */
static int ms_seg_tri(ms_v3 a, ms_v3 b, const ms_tri* T) {
    const double da = v3dot(T->n, a) - T->d, db = v3dot(T->n, b) - T->d;
    if (da * db > 0.0 || da == db) return 0;
    const double t = da / (da - db);
    const ms_v3 x = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    for (int i = 0; i < 3; i++) {
        const ms_v3 e = v3sub(T->p[(i + 1) % 3], T->p[i]), w = v3sub(x, T->p[i]);
        const double c = v3dot(T->n, v3cross(e, w));
        /* outside the edge by more than 1e-6 of |e||w| (plus 1e-9) */
        if (c < -1e-9 && (c + 1e-9) * (c + 1e-9) > 1e-12 * v3dot(e, e) * v3dot(w, w)) return 0;
    }
    return 1;
}

/** @brief Exact triangle-triangle intersection: an edge of either crosses the other.
 * @param A First triangle.
 * @param B Second triangle.
 * @return 1 when they intersect, else 0. */
static int ms_tri_tri(const ms_tri* A, const ms_tri* B) {
    int pa = 0, na = 0, pb = 0, nb = 0;
    for (int k = 0; k < 3; k++) {
        const double sb = v3dot(A->n, B->p[k]) - A->d, sa = v3dot(B->n, A->p[k]) - B->d;
        pb += sb > 0.0; nb += sb < 0.0; pa += sa > 0.0; na += sa < 0.0;
    }
    if (pb == 3 || nb == 3 || pa == 3 || na == 3) return 0;
    for (int i = 0; i < 3; i++) {
        if (ms_seg_tri(A->p[i], A->p[(i + 1) % 3], B)) return 1;
        if (ms_seg_tri(B->p[i], B->p[(i + 1) % 3], A)) return 1;
    }
    return 0;
}

/** @brief Whether two bounds overlap.
 * @param a First triangle.
 * @param b Second triangle.
 * @return 1 when they do. */
static int ms_box_overlap(const ms_tri* a, const ms_tri* b) {
    return !(a->hi.x < b->lo.x || a->lo.x > b->hi.x || a->hi.y < b->lo.y ||
             a->lo.y > b->hi.y || a->hi.z < b->lo.z || a->lo.z > b->hi.z);
}

/* ---- planes -------------------------------------------------------------- */

/* A contact plane under construction: its normal and offset (the first
 * contact face's) and whether any of its faces is double-sided. */
typedef struct { ms_v3 n; double d; int dside; } ms_plane;

/** @brief Finish one contact plane into the snap table's form: side (openpete_snap_side) and slab, 128 dofs, the game's own pull.
 * @param T The moby's triangles in camera space (corners only are read).
 * @param nt Triangle count.
 * @param dofs The moby's authored depth offset.
 * @param pl The plane.
 * @param up World up in camera space (unit), or NULL when unknown.
 * @param o Receives the plane. */
static void ms_plane_finish(const ms_tri* T, int nt, int dofs, const ms_plane* pl,
                            const double* up, openpete_snap_plane* o) {
    double mass = 0.0;
    if (pl->dside) {
        for (int i = 0; i < nt; i++)
            for (int k = 0; k < 3; k++) mass += v3dot(pl->n, T[i].p[k]) - pl->d;
        mass /= 3.0 * (nt > 0 ? nt : 1);
    }
    const double n[3] = { pl->n.x, pl->n.y, pl->n.z };
    o->n[0] = (float)pl->n.x; o->n[1] = (float)pl->n.y; o->n[2] = (float)pl->n.z;
    o->d = (float)pl->d;
    o->side = (float)openpete_snap_side(n, pl->d, pl->dside, up, mass);
    o->slab = 128.0f * (float)dofs;
    o->dside = pl->dside;
}

/** @brief Add a contact face to the plane it lies in, or open a plane for it.
 * @param P The moby's planes.
 * @param np Their count, updated.
 * @param F The contact face.
 * @param dside Whether it is double-sided.
 * @return 1 when placed, 0 when the moby's plane cap is full. */
static int ms_plane_add(ms_plane* P, int* np, const ms_tri* F, int dside) {
    ms_plane* pl = NULL;
    for (int j = 0; j < *np && !pl; j++) {
        const double c = v3dot(P[j].n, F->n);
        if (fabs(c) > 1.0 - MS_MERGE_DN && fabs(P[j].d - (c < 0.0 ? -F->d : F->d)) < MS_MERGE_DD)
            pl = &P[j];
    }
    if (!pl) {
        if (*np >= MS_MAX_PLANES) return 0;
        pl = &P[(*np)++];
        pl->n = F->n; pl->d = F->d; pl->dside = 0;
    }
    pl->dside |= dside;
    return 1;
}

/** @brief Print a moby's contact planes at the feed keys the OPENPETE_MOBY_SNAP_DUMP knob names, an instrument.
 *
 * One line per plane: the moby's identity, ordinal, pull and offset, the
 * view, and the plane's normal, offset, signed slab (side * slab) and
 * double-sided flag.
 * @param mb The moby.
 * @param pl Its planes.
 * @param np Their count.
 * @param view H, ofx, ofy. */
static void ms_dump(const ms_moby* mb, const openpete_snap_plane* pl, int np, const float view[3]) {
    static int s_init; static unsigned s_keys[16]; static int s_nk;
    if (!s_init) {
        s_init = 1;
        const char* e = getenv("OPENPETE_MOBY_SNAP_DUMP");
        while (e && *e && s_nk < 16) {
            s_keys[s_nk++] = (unsigned)strtoul(e, (char**)&e, 10);
            if (*e == '+') e++; else break;
        }
    }
    const unsigned key = openpete_feed_key_now();
    int on = 0;
    for (int k = 0; k < s_nk; k++) on |= s_keys[k] == key;
    for (int k = 0; on && k < np; k++)
        fprintf(stderr, "[moby-snap-plane] key=%u mid=%u mord=%d dofs=%d off=%.3f H=%.4f ofx=%.4f ofy=%.4f "
                        "n=%.7f,%.7f,%.7f d=%.4f sslab=%.4f dside=%d\n",
                key, mb->mid, mb->mord, mb->dofs, mb->off, view[0], view[1], view[2],
                pl[k].n[0], pl[k].n[1], pl[k].n[2], pl[k].d, pl[k].side * pl[k].slab, pl[k].dside);
}

/* World up in the snap's camera space for the resolve under way, from the
 * camera the terrain walk published (openpete_native_occl_live_camera):
 * the game's camera matrix carries world +Z as its input (0, -1, 0), so up
 * is minus its second column. s_up_p points at it, or is NULL when no
 * camera is published. */
static double s_up[3];
static const double* s_up_p;

/** @brief Set s_up and s_up_p from the camera the terrain walk now feeding the scene published. */
static void ms_up_sync(void) {
    openpete_occl_camera c;
    openpete_native_occl_live_camera(&c);
    const double u[3] = { -(double)c.M[1], -(double)c.M[4], -(double)c.M[7] };
    const double l = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    s_up_p = NULL;
    if (!c.valid || !(l > 1e-6)) return;
    for (int k = 0; k < 3; k++) s_up[k] = u[k] / l;
    s_up_p = s_up;
}

/* Scratch shared by the per-moby contact pass, grown on demand. */
static ms_tri*  s_mt;  static int s_mt_cap;
static ms_tri*  s_tt;  static int s_tt_cap;      /* per captured triangle: camera-space form */
static float  (*s_tb)[6]; static int s_tb_cap;   /* per captured triangle: bounds */
static int*     s_cj;  static int s_cj_cap;      /* one moby's usable candidates */
static unsigned* s_tcon; static int s_tcon_cap;  /* per captured triangle: the last moby (s_mserial) it touched */
static unsigned  s_mserial;                      /* the contact pass's moby counter */
static int*     s_ix;  static int s_ix_cap;
static uint8_t* s_made; static int s_made_cap;   /* s_mt[i] is built (ms_make_near) */
static int*     s_ml;  static int s_ml_cap;      /* the moby triangles near the candidates */
static int*     s_mstamp; static int s_mstamp_cap; /* per moby triangle: the last query that met it */
static unsigned s_mquery;                        /* the query counter s_mstamp holds */

/** @brief Grow every scratch array of the contact pass for a moby of nt triangles against nc candidates.
 * @param nt Moby triangles.
 * @param nc Candidate terrain triangles.
 * @return 1 when all hold, 0 on an allocation failure. */
static int ms_scratch(int nt, int nc) {
    return ms_grow((void**)&s_mt, &s_mt_cap, nt, sizeof *s_mt) &&
           ms_grow((void**)&s_cj, &s_cj_cap, nc > 0 ? nc : 1, sizeof *s_cj) &&
           ms_grow((void**)&s_ix, &s_ix_cap, nt * 2, sizeof *s_ix) &&
           ms_grow((void**)&s_made, &s_made_cap, nt, 1) &&
           ms_grow((void**)&s_ml, &s_ml_cap, nt, sizeof *s_ml) &&
           ms_grow((void**)&s_mstamp, &s_mstamp_cap, nt, sizeof *s_mstamp);
}

/** @brief Widen a bounds record (x0, y0, x1, y1, z0, z1) by one screen point.
 * @param b The bounds, updated.
 * @param x Screen X.
 * @param y Screen Y.
 * @param z View-Z. */
static void ms_bounds_add(float b[6], float x, float y, float z) {
    b[0] = ms_minf(b[0], x); b[2] = ms_maxf(b[2], x);
    b[1] = ms_minf(b[1], y); b[3] = ms_maxf(b[3], y);
    b[4] = ms_minf(b[4], z); b[5] = ms_maxf(b[5], z);
}

/** @brief Build the camera-space form (s_mt) of the moby triangles whose bounds meet u (the candidates' bounds) and list them in s_ml.
 *
 * The rest are marked not built (s_made 0).
 * @param m The triangles' screen corners.
 * @param bnd Their bounds records (ms_bounds_tris).
 * @param nt Their count.
 * @param u The candidates' bounds.
 * @param view H, ofx, ofy.
 * @return The count listed. */
static int ms_make_near(const float (*m)[3][3], const float (*bnd)[6], int nt, const float u[6],
                        const float view[3]) {
    int nl = 0;
    for (int i = 0; i < nt; i++) {
        const float* b = bnd[i];
        s_made[i] = !(b[2] < u[0] || b[0] > u[2] || b[3] < u[1] || b[1] > u[3] || b[5] < u[4] || b[4] > u[5]);
        if (!s_made[i]) continue;
        const float sx[3] = { m[i][0][0], m[i][1][0], m[i][2][0] };
        const float sy[3] = { m[i][0][1], m[i][1][1], m[i][2][1] };
        const float z[3]  = { m[i][0][2], m[i][1][2], m[i][2][2] };
        ms_tri_make(&s_mt[i], sx, sy, z, view[0], view[1], view[2]);
        if (s_mt[i].ok) s_ml[nl++] = i;
    }
    return nl;
}

/** @brief Unproject the corners (s_mt[i].p) of every moby triangle ms_make_near did not build; the plane pass reads only the corners.
 * @param m The triangles' screen corners.
 * @param nt Their count.
 * @param view H, ofx, ofy. */
static void ms_make_rest(const float (*m)[3][3], int nt, const float view[3]) {
    const double H = view[0], ofx = view[1], ofy = view[2];
    for (int i = 0; i < nt; i++) {
        if (s_made[i]) continue;
        for (int k = 0; k < 3; k++) {
            const double z = m[i][k][2];
            s_mt[i].p[k].x = ((double)m[i][k][0] - ofx) * z / H;
            s_mt[i].p[k].y = ((double)m[i][k][1] - ofy) * z / H;
            s_mt[i].p[k].z = z;
        }
    }
}

/** @brief Build the camera-space form (s_tt) and the bounds (s_tb) of every captured terrain triangle, once per capture.
 * @param tt The captured triangles.
 * @param n Their count.
 * @param view H, ofx, ofy.
 * @return 0 on an allocation failure, else 1. */
static int ms_terrain_make(const terrain_snap_tri* tt, int n, const float view[3]) {
    if (!ms_grow((void**)&s_tt, &s_tt_cap, n > 0 ? n : 1, sizeof *s_tt) ||
        !ms_grow((void**)&s_tb, &s_tb_cap, n > 0 ? n : 1, sizeof *s_tb) ||
        !ms_grow((void**)&s_tcon, &s_tcon_cap, n > 0 ? n : 1, sizeof *s_tcon)) return 0;
    memset(s_tcon, 0, (size_t)(n > 0 ? n : 1) * sizeof *s_tcon);
    for (int j = 0; j < n; j++) {
        const terrain_snap_tri* T = &tt[j];
        float* b = s_tb[j];
        b[0] = b[1] = b[4] = 1e30f; b[2] = b[3] = b[5] = -1e30f;
        for (int k = 0; k < 3; k++) ms_bounds_add(b, T->sx[k], T->sy[k], T->vz[k]);
        ms_tri_make(&s_tt[j], T->sx, T->sy, T->vz, view[0], view[1], view[2]);
    }
    return 1;
}

/* The near moby triangles (s_ml) binned on an MS_NG x MS_NG screen grid
 * over the candidates' rectangle, cell by cell (s_ng_first, s_ng_ent): a
 * terrain triangle reads the cells its rectangle covers instead of the
 * whole near list. Built for a moby whose near list times its candidates
 * exceeds MS_NG_PAIRS. */
#define MS_NG 8
#define MS_NG_PAIRS 256
static int  s_ng_first[MS_NG * MS_NG + 1];
static int* s_ng_ent; static int s_ng_cap;
static float s_ng_x0, s_ng_y0, s_ng_ix, s_ng_iy;

/** @brief Return the near grid's cell range [lo, hi] an interval covers, clamped to the grid.
 * @param a Interval start.
 * @param b Interval end.
 * @param o The grid origin on this axis.
 * @param ic The inverse cell size on this axis.
 * @param c Receives the first and the last cell. */
static void ms_ng_cells(float a, float b, float o, float ic, int c[2]) {
    const float fl = (a - o) * ic, fh = (b - o) * ic;
    const int l = fl < 0.0f ? 0 : (int)fl, h = fh < 0.0f ? 0 : (int)fh;
    c[0] = l > MS_NG - 1 ? MS_NG - 1 : l;
    c[1] = h > MS_NG - 1 ? MS_NG - 1 : h;
}

/** @brief Bin the near list (s_ml, nl long) on the near grid over rectangle u.
 * @param mb The moby triangles' bounds.
 * @param nl The near list's length.
 * @param u The candidates' bounds.
 * @return 0 on an allocation failure, else 1. */
static int ms_ng_build(const float (*mb)[6], int nl, const float u[6]) {
    int cnt[MS_NG * MS_NG] = { 0 };
    s_ng_x0 = u[0]; s_ng_y0 = u[1];
    s_ng_ix = (float)MS_NG / ms_maxf(u[2] - u[0], 1e-3f);
    s_ng_iy = (float)MS_NG / ms_maxf(u[3] - u[1], 1e-3f);
    int n = 0;
    for (int j = 0; j < nl; j++) {
        int cx[2], cy[2];
        ms_ng_cells(mb[s_ml[j]][0], mb[s_ml[j]][2], s_ng_x0, s_ng_ix, cx);
        ms_ng_cells(mb[s_ml[j]][1], mb[s_ml[j]][3], s_ng_y0, s_ng_iy, cy);
        for (int y = cy[0]; y <= cy[1]; y++)
            for (int x = cx[0]; x <= cx[1]; x++) { cnt[y * MS_NG + x]++; n++; }
    }
    if (!ms_grow((void**)&s_ng_ent, &s_ng_cap, n > 0 ? n : 1, sizeof *s_ng_ent)) return 0;
    s_ng_first[0] = 0;
    for (int c = 0; c < MS_NG * MS_NG; c++) { s_ng_first[c + 1] = s_ng_first[c] + cnt[c]; cnt[c] = s_ng_first[c]; }
    for (int j = 0; j < nl; j++) {
        int cx[2], cy[2];
        ms_ng_cells(mb[s_ml[j]][0], mb[s_ml[j]][2], s_ng_x0, s_ng_ix, cx);
        ms_ng_cells(mb[s_ml[j]][1], mb[s_ml[j]][3], s_ng_y0, s_ng_iy, cy);
        for (int y = cy[0]; y <= cy[1]; y++)
            for (int x = cx[0]; x <= cx[1]; x++) s_ng_ent[cnt[y * MS_NG + x]++] = s_ml[j];
    }
    return 1;
}

/** @brief Test one moby triangle against a terrain triangle and list it at s_ix[nh] when they intersect.
 *
 * Screen bounds reject first (a shared point projects into both
 * rectangles), then camera-space bounds, then the exact test.
 * @param F The terrain triangle.
 * @param fb Its bounds.
 * @param mb The moby triangles' bounds.
 * @param i The moby triangle.
 * @param nh Where to list it.
 * @param cs The census; counts the exact tests run.
 * @return 1 when listed, else 0. */
static int ms_hit_one(const ms_tri* F, const float fb[6], const float (*mb)[6], int i, int nh, ms_census* cs) {
    const float* b = mb[i];
    if (b[2] < fb[0] || b[0] > fb[2] || b[3] < fb[1] || b[1] > fb[3] || b[5] < fb[4] || b[4] > fb[5])
        return 0;
    if (!ms_box_overlap(&s_mt[i], F)) return 0;
    cs->tests++;
    if (!ms_tri_tri(&s_mt[i], F)) return 0;
    s_ix[nh] = i;
    return 1;
}

/** @brief List the near moby triangles (s_ml) a terrain triangle intersects into s_ix.
 * @param F The terrain triangle.
 * @param fb Its bounds.
 * @param mb The moby triangles' bounds.
 * @param nl The near list's length.
 * @param cs The census; counts the exact tests run.
 * @return The count listed. */
static int ms_hits(const ms_tri* F, const float fb[6], const float (*mb)[6], int nl, ms_census* cs) {
    int nh = 0;
    for (int j = 0; j < nl; j++) nh += ms_hit_one(F, fb, mb, s_ml[j], nh, cs);
    return nh;
}

/** @brief List the near moby triangles of the near grid's cells a terrain triangle's rectangle covers that it intersects into s_ix, each tested once.
 * @param F The terrain triangle.
 * @param fb Its bounds.
 * @param mb The moby triangles' bounds.
 * @param cs The census; counts the exact tests run.
 * @return The count listed. */
static int ms_hits_grid(const ms_tri* F, const float fb[6], const float (*mb)[6], ms_census* cs) {
    int nh = 0, cx[2], cy[2];
    ms_ng_cells(fb[0], fb[2], s_ng_x0, s_ng_ix, cx);
    ms_ng_cells(fb[1], fb[3], s_ng_y0, s_ng_iy, cy);
    const int q = (int)++s_mquery;
    for (int y = cy[0]; y <= cy[1]; y++)
        for (int e = s_ng_first[y * MS_NG + cx[0]]; e < s_ng_first[y * MS_NG + cx[1] + 1]; e++) {
            const int i = s_ng_ent[e];
            if (s_mstamp[i] == q) continue;
            s_mstamp[i] = q;
            nh += ms_hit_one(F, fb, mb, i, nh, cs);
        }
    return nh;
}


/** @brief The cut falsifier's mode (ms_falsify), a census instrument: OPENPETE_MOBY_SNAP_FALSIFY=1 runs it, =2 runs it as if no plane moved a point (its calibration: the cuts the snap removes).
 * @return 0 (off), 1 or 2. */
static int ms_falsify_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_FALSIFY"); s = e && (*e == '1' || *e == '2') ? *e - '0' : 0; }
    return s;
}

/** @brief The depth the snap stage writes for a moby point at its key depth, or -1 when a plane hides it behind its owner (openpete_snap_rule_vz).
 *
 * The terrain key owner at the point's pixel lies at view-Z zt on the
 * point's ray and the face's bucket is taken as strictly nearer than the
 * owner's (the CPU holds no key image).
 * @param p The point (camera space).
 * @param out The moby's planes.
 * @param np Their count.
 * @param S The largest slab of the present.
 * @param zt The owner's view-Z on the point's ray.
 * @return The written view-Z, or -1. */
static double ms_snap_vz(ms_v3 p, const openpete_snap_plane* out, int np, double S, double zt) {
    const double q[3] = { p.x, p.y, p.z };
    const double vz = openpete_snap_rule_vz(q, p.z, zt, 1, out, np, S);
    return vz > p.z ? -1.0 : vz;
}

/** @brief Whether a screen point lies inside a captured terrain triangle.
 * @param T The triangle.
 * @param x Screen X.
 * @param y Screen Y.
 * @return 1 when inside or on an edge, else 0. */
static int ms_in_tri(const terrain_snap_tri* T, double x, double y) {
    double sg = 0.0;
    for (int k = 0; k < 3; k++) {
        const int l = (k + 1) % 3;
        const double c = ((double)T->sx[l] - T->sx[k]) * (y - T->sy[k]) - ((double)T->sy[l] - T->sy[k]) * (x - T->sx[k]);
        if (c * sg < 0.0) return 0;
        if (c != 0.0) sg = c;
    }
    return 1;
}

/** @brief List the candidates lying in one of a moby's planes (within the merge tolerance).
 * @param out The moby's planes.
 * @param np Their count.
 * @param cj The moby's usable candidates.
 * @param nc Their count.
 * @param own Receives the coplanar candidates.
 * @param ownk Receives, per listed candidate, the plane it lies in.
 * @param cap The capacity of @p own.
 * @return The count listed. */
static int ms_falsify_own(const openpete_snap_plane* out, int np, const int* cj, int nc, int* own, int* ownk, int cap) {
    int no = 0;
    for (int q = 0; q < nc && no < cap; q++) {
        const ms_tri* T = &s_tt[cj[q]];
        for (int k = 0; k < np; k++) {
            const ms_v3 n = { out[k].n[0], out[k].n[1], out[k].n[2] };
            const double c = v3dot(n, T->n);
            if (fabs(c) > 1.0 - MS_MERGE_DN && fabs((c < 0.0 ? -T->d : T->d) - out[k].d) < MS_MERGE_DD) {
                ownk[no] = k;
                own[no++] = cj[q];
                break;
            }
        }
    }
    return no;
}

/** @brief Sample point w of a moby triangle (s_mt): w 0-2 its corners, 3-5 its edge midpoints, 6 its centroid.
 * @param T The triangle.
 * @param w The sample.
 * @return The point. */
static ms_v3 ms_falsify_sample(const ms_tri* T, int w) {
    if (w < 3) return T->p[w];
    if (w == 6) {
        const ms_v3 c = { (T->p[0].x + T->p[1].x + T->p[2].x) / 3.0, (T->p[0].y + T->p[1].y + T->p[2].y) / 3.0,
                          (T->p[0].z + T->p[1].z + T->p[2].z) / 3.0 };
        return c;
    }
    const ms_v3 a = T->p[w - 3], b = T->p[(w - 2) % 3];
    const ms_v3 m = { (a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5 };
    return m;
}

/** @brief Find the nearest coplanar captured triangle that covers a moby point's screen position, the stand-in for the terrain key owner at its pixel.
 * @param p The point.
 * @param tt The captured triangles.
 * @param own The coplanar candidates.
 * @param no Their count.
 * @param view H, ofx, ofy.
 * @param zt Receives its view-Z along the point's ray.
 * @return Its index in @p own, or -1. */
static int ms_falsify_owner(ms_v3 p, const terrain_snap_tri* tt, const int* own, int no, const float view[3],
                            double* zt) {
    const ms_v3 ray = { p.x / p.z, p.y / p.z, 1.0 };
    const double x = ray.x * view[0] + view[1], y = ray.y * view[0] + view[2];
    int best = -1;
    for (int q = 0; q < no; q++) {
        const ms_tri* T = &s_tt[own[q]];
        const double nr = v3dot(T->n, ray);
        if (fabs(nr) < 1e-9 || !ms_in_tri(&tt[own[q]], x, y)) continue;
        const double zT = T->d / nr;
        if (zT > 1.0 && (best < 0 || zT < *zt)) { *zt = zT; best = q; }
    }
    return best;
}

/** @brief Classify one cut sample (ms_census.cut_cls) and print it under the census when it lies past the covering triangle's plane from the moby's side, within the slab.
 * @param p The sample.
 * @param vz The depth the snap writes for it.
 * @param zt The covering triangle's view-Z on its ray.
 * @param P The covering triangle's plane.
 * @param k That plane's index.
 * @param contact Whether the covering triangle is one of the moby's contact triangles.
 * @param tri The moby triangle and sample (tri * 7 + sample).
 * @param view H, ofx, ofy.
 * @return The class: 0 on the moby's side or past the slab, 1 within it under a contact triangle, 2 within it under a non-contact one. */
static int ms_falsify_class(ms_v3 p, double vz, double zt, const openpete_snap_plane* P, int k, int contact,
                            int tri, const float view[3]) {
    const ms_v3 n = { P->n[0], P->n[1], P->n[2] };
    const double s = P->side * (v3dot(n, p) - P->d), slab = P->slab;
    if (s >= 0.0 || s < -slab) return 0;
    const int cls = contact ? 1 : 2;
    fprintf(stderr, "[moby-snap-cut] key=%u class=%d tri=%d sample=%d sx=%.2f sy=%.2f p=%.2f,%.2f,%.2f vz=%.2f zt=%.2f "
                    "plane=%d s=%.3f slab=%.3f\n",
            openpete_feed_key_now(), cls, tri / 7, tri % 7, p.x / p.z * view[0] + view[1], p.y / p.z * view[0] + view[2],
            p.x, p.y, p.z, vz, zt, k, s, slab);
    return cls;
}

/** @brief Count the sample points of a snapped moby that a drawn terrain triangle lying in one of its own planes still covers in front after the snap (a falsifier census).
 *
 * Samples: each triangle's corners, edge midpoints and centroid at key
 * depth. The nearest captured triangle coplanar with one of the moby's
 * planes (the merge tolerance) covering a sample's screen point stands in
 * for the terrain key owner there; the sample is cut when that triangle
 * lies more than one world unit nearer than the depth the snap writes for
 * it.
 * @param nt The moby's triangle count (s_mt).
 * @param out Its planes.
 * @param np Their count.
 * @param tt The captured terrain triangles.
 * @param cj The moby's usable candidates.
 * @param nc Their count.
 * @param view H, ofx, ofy.
 * @param cls The cut samples by class (ms_falsify_class), updated.
 * @return The cut sample count. */
static long ms_falsify(int nt, const openpete_snap_plane* out, int np,
                       const terrain_snap_tri* tt, const int* cj, int nc, const float view[3], long cls[3]) {
    double S = 0.0;
    for (int k = 0; k < np; k++) S = ms_max(S, (double)out[k].slab);
    int own[512], ownk[512];
    const int no = ms_falsify_own(out, np, cj, nc, own, ownk, 512);
    long cut = 0;
    for (int i = 0; i < nt && no; i++)
        for (int w = 0; w < 7; w++) {
            const ms_v3 p = ms_falsify_sample(&s_mt[i], w);
            if (!(p.z > 1.0)) continue;
            double zt = 0.0;
            const int q = ms_falsify_owner(p, tt, own, no, view, &zt);
            if (q < 0) continue;
            const double vz = ms_falsify_on() == 2 ? p.z : ms_snap_vz(p, out, np, S, zt);
            if (vz < 0.0 || !(zt < vz - 1.0)) continue;
            cut++;
            cls[ms_falsify_class(p, vz, zt, &out[ownk[q]], ownk[q], s_tcon[own[q]] == s_mserial, i * 7 + w, view)]++;
        }
    return cut;
}

static void ms_result(const ms_moby* mb, const openpete_snap_plane* out, int np);

/** @brief Find one queued moby's contact planes against the captured terrain.
 * @param mb The moby.
 * @param tt The captured terrain triangles.
 * @param cand The indices into @p tt of those its search box reaches.
 * @param ncand Their count.
 * @param view H, ofx, ofy.
 * @param cs The census, updated.
 * @param out Receives the planes, MS_MAX_PLANES at most.
 * @return The plane count, or -1 on a scratch allocation failure. */
static int ms_moby_planes(const ms_moby* mb, const terrain_snap_tri* tt, const int* cand,
                          int ncand, const float view[3], ms_census* cs, openpete_snap_plane* out) {
    const float (*m)[3][3] = (const float (*)[3][3])&s_q_tri[mb->t0];
    const int nt = mb->nt;
    if (!ncand) return 0;
    if (!ms_scratch(nt, ncand)) return -1;
    s_mserial++;
    uint64_t t = cs->timed ? ms_mono_ns() : 0, t1;
    float u[6] = { 1e30f, 1e30f, -1e30f, -1e30f, 1e30f, -1e30f };
    int nc = 0;
    for (int q = 0; q < ncand; q++) {
        const float* b = s_tb[cand[q]];
        u[0] = ms_minf(u[0], b[0]); u[1] = ms_minf(u[1], b[1]); u[2] = ms_maxf(u[2], b[2]);
        u[3] = ms_maxf(u[3], b[3]); u[4] = ms_minf(u[4], b[4]); u[5] = ms_maxf(u[5], b[5]);
        if (s_tt[cand[q]].ok) s_cj[nc++] = cand[q];
    }
    const float (*mbnd)[6] = (const float (*)[6])&s_q_bnd[mb->t0];
    const int nl = ms_make_near(m, mbnd, nt, u, view);
    if (t) { t1 = ms_mono_ns(); cs->near_ns += t1 - t; t = t1; }
    ms_plane P[MS_MAX_PLANES];
    int np = 0;
    const int grid = nl * nc > MS_NG_PAIRS && ms_ng_build(mbnd, nl, u);
    for (int q = 0; q < nc && nl; q++) {
        const int j = s_cj[q];
        const int nh = grid ? ms_hits_grid(&s_tt[j], s_tb[j], mbnd, cs) : ms_hits(&s_tt[j], s_tb[j], mbnd, nl, cs);
        if (!nh) continue;
        s_tcon[j] = s_mserial;
        cs->contacts++;
        cs->contacts_t4 += tt[j].ftris >= 16;
        if (!ms_plane_add(P, &np, &s_tt[j], tt[j].dside)) cs->ovf++;
    }
    if (t) { t1 = ms_mono_ns(); cs->test_ns += t1 - t; t = t1; }
    if (!np) return 0;
    ms_make_rest(m, nt, view);
    for (int k = 0; k < np; k++)
        ms_plane_finish(s_mt, nt, mb->dofs, &P[k], s_up_p, &out[k]);
    cs->planes += np;
    if (t) cs->plane_ns += ms_mono_ns() - t;
    if (ms_falsify_on()) {
        const long b0 = cs->cut_cls[1];
        const long cut = ms_falsify(nt, out, np, tt, s_cj, nc, view, cs->cut_cls);
        cs->cut_samples += cut;
        cs->cut_mobys += cut > 0;
        cs->cut_b_mobys += cs->cut_cls[1] > b0;
    }
    ms_dump(mb, out, np, view);
    return np;
}

/** @brief Add a moby's planes to the scene's snap table and note its ordinal for the collision census.
 * @param mb The moby.
 * @param out Its planes.
 * @param np Their count, > 0.
 * @param cs The census; counts refused planes as overflow.
 * @return np when added, 0 when the table refused them. */
static int ms_add(const ms_moby* mb, const openpete_snap_plane* out, int np, ms_census* cs) {
    if (!openpete_native_snap_add(mb->mid, mb->mord, mb->off, out, np)) { cs->ovf += np; return 0; }
    if (mb->mord > 0 && s_sn_n < 256) { s_sn[s_sn_n].mid = mb->mid; s_sn[s_sn_n++].ord = mb->mord; }
    return np;
}

/** @brief Find one queued moby's contact planes against the captured terrain, hand them to the carry and add them to the scene's snap table.
 *
 * A moby the carry already placed (add 0) is resolved for the verify
 * instrument only and adds nothing.
 * @param mb The moby.
 * @param tt The captured terrain triangles.
 * @param cand The indices into @p tt of those its search box reaches.
 * @param ncand Their count.
 * @param view H, ofx, ofy.
 * @param cs The census, updated.
 * @return The planes added. */
static int ms_moby_contact(const ms_moby* mb, const terrain_snap_tri* tt, const int* cand,
                           int ncand, const float view[3], ms_census* cs) {
    openpete_snap_plane out[MS_MAX_PLANES];
    const int np = ms_moby_planes(mb, tt, cand, ncand, view, cs, out);
    if (np < 0) return 0;
    ms_result(mb, out, np);
    if (!np || !mb->add) return 0;
    return ms_add(mb, out, np, cs);
}

/** @brief The search box of one queued moby: its screen rectangle widened by one pixel and its key depth range.
 * @param mb The moby.
 * @param b Receives the box. */
static void ms_box(const ms_moby* mb, terrain_snap_box* b) {
    b->x0 = mb->b[0] - 1.0f; b->y0 = mb->b[1] - 1.0f;
    b->x1 = mb->b[2] + 1.0f; b->y1 = mb->b[3] + 1.0f;
    b->z0 = mb->b[4]; b->z1 = mb->b[5];
}

/** @brief Whether OPENPETE_MOBY_RANK_TIME=1, the moby paint-key cost instrument, also prints the snap's contact cost.
 * @return 1 when it does, else 0. */
static int ms_census_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_TIME"); s = e && *e == '1'; }
    return s;
}

/** @brief The calling thread's CPU time in nanoseconds.
 * @return The time. */
static uint64_t ms_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/** @brief Print one resolve's census line with the running mean contact-stage CPU time.
 * @param cs The resolve's counters.
 * @param ns Its CPU time. */
static void ms_census_print(const ms_census* cs, uint64_t ns) {
    static double s_sum_us; static long s_n;
    s_sum_us += (double)ns / 1000.0; s_n++;
    int ne = 0, np = 0; unsigned ovf = 0;
    openpete_native_snap_census(&ne, &np, &ovf);
    fprintf(stderr, "[moby-snap] key=%u pulled=%ld cand_faces=%ld cand_tris=%ld owned_sectors=%ld "
                    "tritri=%ld contact_tris=%ld contact_t4=%ld overlaps=%ld snap_zmin=%.0f ord_collide=%ld planes=%ld table_ents=%d table_planes=%d "
                    "overflow=%ld+%u cut_mobys=%ld cut_samples=%ld cut_a=%ld cut_b=%ld cut_c=%ld cut_b_mobys=%ld cpu_us=%.1f capture_us=%.1f near_us=%.1f test_us=%.1f plane_us=%.1f mean_cpu_us=%.1f n=%ld "
                    "carried=%ld carry_fresh=%ld\n",
            openpete_feed_key_now(), cs->pulled, cs->faces, cs->tris, cs->sectors, cs->tests,
            cs->contacts, cs->contacts_t4, cs->overlaps, (double)cs->zmin, ms_ord_collisions(), cs->planes, ne, np, cs->ovf, ovf, cs->cut_mobys, cs->cut_samples, cs->cut_cls[0], cs->cut_cls[1], cs->cut_cls[2], cs->cut_b_mobys, (double)ns / 1000.0,
            (double)cs->cap_ns / 1000.0, (double)cs->near_ns / 1000.0, (double)cs->test_ns / 1000.0,
            (double)cs->plane_ns / 1000.0, s_sum_us / (double)s_n, s_n, cs->carried, cs->fresh);
}

/** @brief Bucket the captured triangles by the search boxes they reach (a counting sort over the hit sets).
 * @param tt The captured triangles.
 * @param ntt Their count.
 * @param nb Box count.
 * @param first Receives, for each box k, the start of its run in the
 *        result, and first[nb] the total.
 * @return The indices into @p tt, box by box; valid until the next call. */
static const int* ms_cands(const terrain_snap_tri* tt, int ntt, int nb, int* first) {
    static int* s_c; static int s_cap;
    int cnt[129] = { 0 };
    for (int j = 0; j < ntt; j++)
        for (int w = 0; w < 2; w++)
            for (uint64_t r = tt[j].hit[w]; r; r &= r - 1) cnt[w * 64 + __builtin_ctzll(r)]++;
    first[0] = 0;
    for (int k = 0; k < nb; k++) first[k + 1] = first[k] + cnt[k];
    if (!ms_grow((void**)&s_c, &s_cap, first[nb] > 0 ? first[nb] : 1, sizeof *s_c)) {
        for (int k = 0; k <= nb; k++) first[k] = 0;
        return s_c;
    }
    int at[128];
    memcpy(at, first, (size_t)nb * sizeof at[0]);
    for (int j = 0; j < ntt; j++)
        for (int w = 0; w < 2; w++)
            for (uint64_t r = tt[j].hit[w]; r; r &= r - 1) s_c[at[w * 64 + __builtin_ctzll(r)]++] = j;
    return s_c;
}

/** @brief Count the pairs of snapped mobys whose screen boxes overlap: places where two sunk parts may share a ray.
 * @param box The mobys' search boxes.
 * @param snapped 1 for a moby that received planes.
 * @param nb Box count.
 * @return The pair count. */
static long ms_overlaps(const terrain_snap_box* box, const uint8_t* snapped, int nb) {
    long n = 0;
    for (int a = 0; a < nb; a++)
        for (int b = a + 1; b < nb; b++)
            n += snapped[a] && snapped[b] && box[a].x0 < box[b].x1 && box[b].x0 < box[a].x1 &&
                 box[a].y0 < box[b].y1 && box[b].y0 < box[a].y1;
    return n;
}

/* ---- the snap carry ------------------------------------------------------
 * A canonical resolve (one outside a sub-tick re-extract, moby_snap_present)
 * stores each queued moby's contact planes in world space, keyed by moby
 * identity, in the table of its canonical epoch and native scene build; the
 * tables of the two newest epochs are kept (T-1 and T). While the carry is
 * on (OPENPETE_MOBY_SNAP_CARRY=1), a sub-tick re-extract maps the planes of
 * the endpoint the moby rank carry selects into its own camera instead of
 * capturing and testing the terrain again, and resolves only the mobys the
 * endpoint did not resolve. The terrain walk's camera space is
 * c = L (w - P) (terrain_snap_camera), so a camera plane n.c = d is the
 * world plane (L^T n).w = d + (L^T n).P, and a world plane m.w = e is
 * (L'^-T m).c' = e - m.P' in a camera c' = L' (w - P'), scaled to a unit
 * normal. Both maps keep the plane function's value at every world point
 * up to a positive factor, so the side the moby stands on, the slab and the
 * double-sided flag carry unchanged, except that a single-sided plane takes
 * the eye's side in the present camera (openpete_snap_side), which a camera
 * crossing the plane within the tick changes. A present whose camera
 * equals the endpoint's bit for bit takes the planes as they were
 * resolved. */

/* One stored plane in world space, and the camera-space plane it came from. */
typedef struct { double n[3], d; } mc_wplane;
/* One stored moby: identity, first plane, plane count (0: resolved, no
 * contact), and, while the verify runs, the world box of its corners at
 * true depth (wb, valid when wb_ok). */
typedef struct { uint32_t mid; int p0, np; float wb[6]; int wb_ok; } mc_ent;
#define MC_HBITS 10
#define MC_HSIZE (1 << MC_HBITS)
typedef struct {
    uint32_t  epoch;           /* canonical epoch (spyro_native_canon_epoch_stamp) */
    unsigned  scene;           /* native scene build (openpete_native_scene_frame) */
    int       valid;
    double    L[3][3], P[3];   /* the camera of the first store */
    int       cam_mixed;       /* a later store of the table ran under another camera */
    mc_ent*   e;   int ne, cap_e;
    mc_wplane* w;  int npl, cap_p;
    openpete_snap_plane* c; int cap_c;   /* parallel to w: the plane as resolved */
    unsigned  hgen;            /* generation the slots below hold */
    unsigned  hg[MC_HSIZE];
    int       hi[MC_HSIZE];
    long      dup, full;       /* repeated identities; mobys refused at 3/4 of MC_HSIZE */
} mc_tab;
static mc_tab   s_mc[2];
static int      s_mc_cur;      /* the latest epoch's table */
static unsigned s_mc_gen;
static long     s_mc_rebuilds; /* tables emptied by a second scene build within one epoch */

/* The leg of the resolve under way (moby_snap_present) and, for the present
 * that resolves, the per-present counts the profiler emits. */
static int      s_rex;
static double   s_rex_alpha = 1.0;
static long     s_pc_carried, s_pc_fresh;

/* The resolve under way: the table a canonical resolve stores into, and the
 * walk camera it stores under. */
static mc_tab*  s_store;
static double   s_rs_L[3][3], s_rs_P[3];
static const mc_tab* s_rs_ep;   /* a sub-tick resolve's endpoint, for its verify */
static double   s_rs_Li[3][3];  /* the inverse of s_rs_L, while the verify runs */
static float    s_rs_view[3];   /* H, ofx, ofy of the resolve under way */
static int      s_rs_wb;        /* s_rs_Li holds: moby world boxes are computed */

/** @brief The world box of a queued moby's triangle corners at true depth (key depth less off), under the resolve's camera.
 * @param mb The moby.
 * @param wb Receives x0, y0, z0, x1, y1, z1. */
static void ms_moby_wbox(const ms_moby* mb, float wb[6]) {
    const float H = s_rs_view[0], ofx = s_rs_view[1], ofy = s_rs_view[2];
    for (int i = 0; i < 3; i++) { wb[i] = 1e30f; wb[i + 3] = -1e30f; }
    for (int t = 0; t < mb->nt; t++)
        for (int k = 0; k < 3; k++) {
            const float* v = s_q_tri[mb->t0 + t][k];
            const double z = (double)v[2] - mb->off;
            const double c[3] = { ((double)v[0] - ofx) * z / H, ((double)v[1] - ofy) * z / H, z };
            for (int i = 0; i < 3; i++) {
                const float w = (float)(s_rs_Li[i][0] * c[0] + s_rs_Li[i][1] * c[1] + s_rs_Li[i][2] * c[2] + s_rs_P[i]);
                wb[i] = ms_minf(wb[i], w); wb[i + 3] = ms_maxf(wb[i + 3], w);
            }
        }
}

/** @brief Whether the snap carry is on (OPENPETE_MOBY_SNAP_CARRY=1), read once.
 * @return 1 when on, else 0. */
static int mc_carry_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_CARRY"); s = e && *e == '1'; }
    return s;
}

/** @brief The carry verify instrument's mode (OPENPETE_MOBY_SNAP_CARRY_VERIFY), read once.
 *
 * 0 off; 1 compares the carry against a fresh resolve on every sub-tick
 * resolve; 2 is 1 with the carry mapped under the endpoint's own camera (the
 * identity transform, the negative control); 3 is 1 with the map applied
 * even where the cameras are equal (calibrates the map on a static camera).
 * @return The mode. */
static int mc_verify_mode(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_CARRY_VERIFY");
                 s = e && *e >= '1' && *e <= '3' ? *e - '0' : 0; }
    return s;
}

void moby_snap_present(int rex, double alpha) {
    s_rex = rex;
    s_rex_alpha = rex ? alpha : 1.0;
    if (rex) s_pc_carried = s_pc_fresh = 0;
}

/** @brief Hash a moby identity to a table slot.
 * @param mid The identity.
 * @return The slot. */
static unsigned mc_hash(uint32_t mid) { return (mid * 2654435761u) >> (32 - MC_HBITS); }

/** @brief Find a moby in a table.
 * @param t The table.
 * @param mid The identity.
 * @return Its entry index, or -1. */
static int mc_find(const mc_tab* t, uint32_t mid) {
    for (unsigned h = mc_hash(mid); t->hg[h] == t->hgen; h = (h + 1) & (MC_HSIZE - 1))
        if (t->e[t->hi[h]].mid == mid) return t->hi[h];
    return -1;
}

/** @brief Empty a table for a store under canonical epoch ep from scene build scene.
 *
 * Hands out a fresh slot generation; a wrap clears both tables' slots first.
 * @param t The table.
 * @param ep The canonical epoch.
 * @param scene The native scene build. */
static void mc_empty(mc_tab* t, uint32_t ep, unsigned scene) {
    if (!++s_mc_gen) {
        for (int k = 0; k < 2; k++) memset(s_mc[k].hg, 0, sizeof s_mc[k].hg);
        s_mc_gen = 1;
    }
    t->hgen = s_mc_gen; t->ne = t->npl = 0; t->epoch = ep; t->scene = scene; t->valid = 1;
    t->cam_mixed = -1;
    t->dup = t->full = 0;
}

/** @brief The table a canonical resolve under epoch ep from scene build scene stores into.
 *
 * The rank carry's rule (rank_carry_table_for): the latest table while it
 * holds ep and was stored by the same build, so the passes of one build
 * accumulate; a later build under the same epoch empties it in place; the
 * first store of a new epoch turns the latest table into T-1 and empties the
 * other for the new tick.
 * @param ep The canonical epoch.
 * @param scene The native scene build.
 * @return The table. */
static mc_tab* mc_table_for(uint32_t ep, unsigned scene) {
    mc_tab* t = &s_mc[s_mc_cur];
    if (t->valid && ep == t->epoch) {
        if (scene != t->scene) { mc_empty(t, ep, scene); s_mc_rebuilds++; }
        return t;
    }
    s_mc_cur ^= 1;
    t = &s_mc[s_mc_cur];
    mc_empty(t, ep, scene);
    return t;
}

/** @brief Note the camera a store into a table runs under: the first store's is kept, a different later one marks the table mixed.
 * @param t The table.
 * @param L The walk camera's linear part.
 * @param P Its position. */
static void mc_cam_note(mc_tab* t, const double L[3][3], const double P[3]) {
    if (t->cam_mixed < 0) {
        memcpy(t->L, L, sizeof t->L); memcpy(t->P, P, sizeof t->P);
        t->cam_mixed = 0;
    } else if (memcmp(t->L, L, sizeof t->L) || memcmp(t->P, P, sizeof t->P)) {
        t->cam_mixed = 1;
    }
}

/** @brief Store one resolved moby's planes, mapped to world space under the resolve's camera, in the table.
 *
 * A moby stored twice in one table keeps the later planes (counted as dup).
 * While the verify runs the entry also keeps the moby's world box.
 * @param t The table.
 * @param mb The moby.
 * @param out Its camera-space planes.
 * @param np Their count, 0 for a moby resolved with no contact. */
static void mc_store(mc_tab* t, const ms_moby* mb, const openpete_snap_plane* out, int np) {
    const uint32_t mid = mb->mid;
    if (t->ne >= MC_HSIZE - (MC_HSIZE >> 2)) { t->full++; return; }
    if (!ms_grow((void**)&t->e, &t->cap_e, t->ne + 1, sizeof *t->e) ||
        !ms_grow((void**)&t->w, &t->cap_p, t->npl + np + 1, sizeof *t->w) ||
        !ms_grow((void**)&t->c, &t->cap_c, t->npl + np + 1, sizeof *t->c)) return;
    unsigned h = mc_hash(mid);
    while (t->hg[h] == t->hgen && t->e[t->hi[h]].mid != mid) h = (h + 1) & (MC_HSIZE - 1);
    t->dup += t->hg[h] == t->hgen;
    t->hg[h] = t->hgen; t->hi[h] = t->ne;
    mc_ent* e = &t->e[t->ne++];
    e->mid = mid; e->p0 = t->npl; e->np = np;
    e->wb_ok = s_rs_wb;
    if (s_rs_wb) ms_moby_wbox(mb, e->wb);
    for (int k = 0; k < np; k++) {
        mc_wplane* w = &t->w[t->npl];
        double dw = out[k].d;
        for (int i = 0; i < 3; i++) {
            w->n[i] = s_rs_L[0][i] * out[k].n[0] + s_rs_L[1][i] * out[k].n[1] + s_rs_L[2][i] * out[k].n[2];
            dw += w->n[i] * s_rs_P[i];
        }
        w->d = dw;
        t->c[t->npl++] = out[k];
    }
}

/** @brief The table whose planes this present's re-extract carries, or NULL when that endpoint has none.
 *
 * The rank carry's endpoint (rank_carry_endpoint): alpha at or above 0.5
 * takes the latest table, below it the T-1 table; the latest counts only
 * while its epoch is the last canonical present's, T-1 only while its epoch
 * directly precedes the latest's.
 * @return The table, or NULL. */
static const mc_tab* mc_endpoint(void) {
    const mc_tab* latest = &s_mc[s_mc_cur];
    const mc_tab* prev = &s_mc[s_mc_cur ^ 1];
    if (!latest->valid || !spyro_native_canon_epoch_fresh(latest->epoch)) return NULL;
    if (s_rex_alpha >= 0.5) return latest;
    return (prev->valid && prev->epoch + 1u == latest->epoch) ? prev : NULL;
}

/** @brief Write the inverse transpose of a 3x3 matrix.
 * @param L The matrix.
 * @param Q Receives L^-T.
 * @return 0 when L is singular, else 1. */
static int mc_inv_t(const double L[3][3], double Q[3][3]) {
    double C[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            C[r][c] = L[r1][c1] * L[r2][c2] - L[r1][c2] * L[r2][c1];
        }
    const double det = L[0][0] * C[0][0] + L[0][1] * C[0][1] + L[0][2] * C[0][2];
    if (!(fabs(det) > 1e-30)) return 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) Q[r][c] = C[r][c] / det;
    return 1;
}

/** @brief Map one stored world plane into a camera, with a unit normal.
 * @param w The world plane.
 * @param src The plane as resolved (side, slab, double-sided flag).
 * @param Q The camera's L^-T.
 * @param P The camera's position.
 * @param o Receives the camera-space plane.
 * @return 0 when the mapped normal is degenerate, else 1. */
static int mc_map_plane(const mc_wplane* w, const openpete_snap_plane* src, const double Q[3][3],
                        const double P[3], openpete_snap_plane* o) {
    double n[3], d = w->d;
    for (int i = 0; i < 3; i++) {
        n[i] = Q[i][0] * w->n[0] + Q[i][1] * w->n[1] + Q[i][2] * w->n[2];
        d -= w->n[i] * P[i];
    }
    const double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(l > 0.0)) return 0;
    *o = *src;
    for (int i = 0; i < 3; i++) o->n[i] = (float)(n[i] / l);
    o->d = (float)(d / l);
    if (!src->dside) {   /* openpete_snap_side: a single-sided plane's side is the eye's */
        const double nn[3] = { o->n[0], o->n[1], o->n[2] };
        o->side = (float)openpete_snap_side(nn, o->d, 0, NULL, 0.0);
    }
    return 1;
}

/* The carry's planes for the present's queue, per queued moby (qi): the
 * count (-1 when the endpoint did not resolve the moby) and first plane in
 * s_vc_pl. */
static int* s_vc_np; static int* s_vc_p0; static int s_vc_cap, s_vc_cap0;
static openpete_snap_plane* s_vc_pl; static int s_vc_plcap, s_vc_n;

/** @brief Map the endpoint's planes of every queued moby into the present camera (s_vc_np, s_vc_p0, s_vc_pl).
 *
 * The planes are taken as resolved when the camera equals the endpoint's
 * and the verify mode is not 3; verify mode 2 maps under the endpoint's own
 * camera (the identity).
 * @param ep The endpoint, or NULL (every moby absent).
 * @param L The present walk camera's linear part.
 * @param P Its position.
 * @return 0 on an allocation failure, else 1. */
static int mc_map(const mc_tab* ep, const double L[3][3], const double P[3]) {
    if (!ms_grow((void**)&s_vc_np, &s_vc_cap, s_q_nm, sizeof *s_vc_np) ||
        !ms_grow((void**)&s_vc_p0, &s_vc_cap0, s_q_nm, sizeof *s_vc_p0)) return 0;
    s_vc_n = 0;
    const int vm = mc_verify_mode();
    const double (*Lm)[3] = vm == 2 && ep ? ep->L : L;
    const double* Pm = vm == 2 && ep ? ep->P : P;
    const int same = ep && !ep->cam_mixed && vm != 3 &&
                     !memcmp(ep->L, Lm, sizeof ep->L) && !memcmp(ep->P, Pm, 3 * sizeof *Pm);
    double Q[3][3];
    const int inv = ep && mc_inv_t(Lm, Q);
    for (int k = 0; k < s_q_nm; k++) {
        const int x = inv || same ? mc_find(ep, s_q_mob[k].mid) : -1;
        s_vc_np[k] = -1;
        if (x < 0) continue;
        const mc_ent* e = &ep->e[x];
        if (!ms_grow((void**)&s_vc_pl, &s_vc_plcap, s_vc_n + e->np + 1, sizeof *s_vc_pl)) return 0;
        s_vc_p0[k] = s_vc_n;
        int ok = 1;
        for (int j = 0; j < e->np && ok; j++) {
            if (same) s_vc_pl[s_vc_n + j] = ep->c[e->p0 + j];
            else ok = mc_map_plane(&ep->w[e->p0 + j], &ep->c[e->p0 + j], Q, Pm, &s_vc_pl[s_vc_n + j]);
        }
        if (!ok) continue;
        s_vc_np[k] = e->np;
        s_vc_n += e->np;
    }
    return 1;
}

/* ---- carry verify (OPENPETE_MOBY_SNAP_CARRY_VERIFY) ----------------------
 * Every sub-tick resolve also runs the fresh contact pass over the whole
 * queue and compares, per queued moby, the carried planes against the fresh
 * ones. Planes match when their normals agree to within twice the merge
 * cosine deficit and their offsets to within twice the merge distance (each
 * plane is its group's first contact face, within the merge tolerance of
 * every member), up to the normal's orientation. Classes: a moby the
 * endpoint did not resolve (absent, resolved fresh under the carry); one
 * the fresh pass did not resolve (unresolved); carry-only and fresh-only
 * mobys (planes on one side only) and planes (unmatched); matched planes
 * whose side, slab or double-sided flag differ; table refusals (overflow).
 * A mismatching moby whose world box (corners at true depth) moved by more
 * than MC_MOVED world units from the endpoint's is counted as moved (a
 * moby moving or animating across faces within the tick); one whose every
 * unmatched plane its corners straddle by less than MC_GRAZE world units (a
 * grazing contact, which the exact triangle test decides differently under
 * another camera's float projection) as grazing; the rest as static.
 * Engagement: resolves and presents compared, presents at alpha != 0,
 * mobys and planes compared, and the present-to-endpoint camera delta. */
#define MC_MOVED 0.5
#define MC_GRAZE 2.0
static int* s_vf_np; static int* s_vf_p0; static int s_vf_cap, s_vf_cap0;
static openpete_snap_plane* s_vf_pl; static int s_vf_plcap, s_vf_n;
static uint32_t* s_vq_mid; static int s_vq_cap, s_vq_n;
static float (*s_vq_wb)[6]; static int s_vq_wbcap;   /* per queue index: the present's world box */
static int s_vf_on;
typedef struct {
    long resolves, presents, presents_anz, mobys, planes, absent, unresolved;
    long carry_only_m, fresh_only_m, carry_only_p, fresh_only_p, side, slab, dside;
    long mism_mobys, mism_cls[3], ovf, cam_moved;   /* mism_cls: moved, grazing, static */
    double dp_sum, dp_max, dl_max;
    unsigned last_epoch;
} mc_vstat;
static mc_vstat s_vs = { .last_epoch = ~0u };

/** @brief Record one moby's fresh planes for the verify comparison.
 * @param qi The moby's queue index.
 * @param out Its planes.
 * @param np Their count. */
static void mc_vf_put(int qi, const openpete_snap_plane* out, int np) {
    if (qi < 0 || qi >= s_vq_n ||
        !ms_grow((void**)&s_vf_pl, &s_vf_plcap, s_vf_n + np + 1, sizeof *s_vf_pl)) return;
    s_vf_np[qi] = np; s_vf_p0[qi] = s_vf_n;
    memcpy(&s_vf_pl[s_vf_n], out, (size_t)np * sizeof *out);
    s_vf_n += np;
}

/** @brief Hand a resolved moby's planes to the carry: a canonical resolve stores them, a verified sub-tick resolve records them for the comparison.
 * @param mb The moby.
 * @param out Its planes.
 * @param np Their count, 0 for no contact. */
static void ms_result(const ms_moby* mb, const openpete_snap_plane* out, int np) {
    if (s_store) mc_store(s_store, mb, out, np);
    if (s_vf_on) mc_vf_put(mb->qi, out, np);
}

/** @brief Start the verify comparison of this resolve: every moby unresolved, the queue's identities kept by queue index.
 * @return 0 on an allocation failure, else 1. */
static int mc_vf_begin(void) {
    if (!ms_grow((void**)&s_vf_np, &s_vf_cap, s_q_nm, sizeof *s_vf_np) ||
        !ms_grow((void**)&s_vf_p0, &s_vf_cap0, s_q_nm, sizeof *s_vf_p0) ||
        !ms_grow((void**)&s_vq_mid, &s_vq_cap, s_q_nm, sizeof *s_vq_mid) ||
        !ms_grow((void**)&s_vq_wb, &s_vq_wbcap, s_q_nm, sizeof *s_vq_wb)) return 0;
    s_vq_n = s_q_nm; s_vf_n = 0;
    for (int k = 0; k < s_q_nm; k++) {
        s_vf_np[k] = -1; s_vq_mid[k] = s_q_mob[k].mid;
        if (s_rs_wb) ms_moby_wbox(&s_q_mob[k], s_vq_wb[k]);
    }
    return 1;
}

/** @brief Print the verify census, cumulative over the run. */
static void mc_vf_print(void) {
    OP_ROLE_ASSERT(sim);
    const mc_vstat* v = &s_vs;
    fprintf(stderr, "[snap-carry-verify] mode=%d carry=%d resolves=%ld presents=%ld presents_anz=%ld mobys=%ld "
                    "planes=%ld mismatch_mobys=%ld mismatch_moved=%ld mismatch_grazing=%ld mismatch_static=%ld carry_only_mobys=%ld fresh_only_mobys=%ld carry_only_planes=%ld "
                    "fresh_only_planes=%ld side_flips=%ld slab_diff=%ld dside_diff=%ld absent=%ld unresolved=%ld "
                    "overflow=%ld cam_moved=%ld cam_dp_mean=%.3f cam_dp_max=%.3f cam_dl_max=%.6f dup=%ld full=%ld "
                    "rebuilds=%ld\n",
            mc_verify_mode(), mc_carry_on(), v->resolves, v->presents, v->presents_anz, v->mobys, v->planes,
            v->mism_mobys, v->mism_cls[0], v->mism_cls[1], v->mism_cls[2], v->carry_only_m, v->fresh_only_m, v->carry_only_p, v->fresh_only_p, v->side,
            v->slab, v->dside, v->absent, v->unresolved, v->ovf, v->cam_moved,
            v->resolves ? v->dp_sum / (double)v->resolves : 0.0, v->dp_max, v->dl_max,
            s_mc[0].dup + s_mc[1].dup, s_mc[0].full + s_mc[1].full, s_mc_rebuilds);
}

/** @brief Find the fresh plane that matches a carried one under the verify tolerance.
 * @param c The carried plane.
 * @param f The fresh planes.
 * @param nf Their count.
 * @param used Per fresh plane, 1 once matched; updated.
 * @param sg Receives +1 or -1, the fresh normal's orientation against the carried one.
 * @return The fresh plane's index, or -1. */
static int mc_vf_match(const openpete_snap_plane* c, const openpete_snap_plane* f, int nf, uint8_t* used, double* sg) {
    for (int j = 0; j < nf; j++) {
        if (used[j]) continue;
        const double cs = (double)c->n[0] * f[j].n[0] + (double)c->n[1] * f[j].n[1] + (double)c->n[2] * f[j].n[2];
        const double s = cs < 0.0 ? -1.0 : 1.0;
        if (fabs(cs) > 1.0 - 2.0 * MS_MERGE_DN && fabs((double)c->d - s * f[j].d) < 2.0 * MS_MERGE_DD) {
            used[j] = 1; *sg = s;
            return j;
        }
    }
    return -1;
}

/** @brief How far a queued moby's world box moved from the endpoint's, the largest of the six bound deltas.
 * @param qi The moby's queue index.
 * @return The delta in world units, or -1 when either box is missing. */
static double mc_vf_moved(int qi) {
    const int x = s_rs_wb && s_rs_ep ? mc_find(s_rs_ep, s_vq_mid[qi]) : -1;
    if (x < 0 || !s_rs_ep->e[x].wb_ok) return -1.0;
    double d = 0.0;
    for (int i = 0; i < 6; i++) d = ms_max(d, fabs((double)s_rs_ep->e[x].wb[i] - s_vq_wb[qi][i]));
    return d;
}

/** @brief Print one mismatching moby's carried and fresh planes, the first 64 of the run.
 * @param qi The moby's queue index.
 * @param why The class.
 * @param moved Its world box delta (mc_vf_moved). */
static void mc_vf_detail(int qi, const char* why, double moved) {
    static int s_n;
    if (s_n >= 64) return;
    s_n++;
    fprintf(stderr, "[snap-carry-mismatch] key=%u alpha=%.3f mid=%u class=%s carry_np=%d fresh_np=%d moved=%.2f\n",
            openpete_feed_key_now(), s_rex_alpha, s_vq_mid[qi], why, s_vc_np[qi], s_vf_np[qi], moved);
    for (int j = 0; j < s_vc_np[qi]; j++) {
        const openpete_snap_plane* p = &s_vc_pl[s_vc_p0[qi] + j];
        fprintf(stderr, "  carry n=%.6f,%.6f,%.6f d=%.3f side=%.0f slab=%.0f dside=%d\n",
                p->n[0], p->n[1], p->n[2], p->d, p->side, p->slab, p->dside);
    }
    for (int j = 0; j < s_vf_np[qi]; j++) {
        const openpete_snap_plane* p = &s_vf_pl[s_vf_p0[qi] + j];
        fprintf(stderr, "  fresh n=%.6f,%.6f,%.6f d=%.3f side=%.0f slab=%.0f dside=%d\n",
                p->n[0], p->n[1], p->n[2], p->d, p->side, p->slab, p->dside);
    }
}

/** @brief How deep a queued moby's triangle corners (key depth, present camera) straddle a plane: the smaller of the deepest corner on each side.
 *
 * Negative when every corner lies on one side: minus the nearest corner's
 * distance.
 * @param mb The moby.
 * @param pl The plane.
 * @return The straddle depth, world units. */
static double mc_straddle(const ms_moby* mb, const openpete_snap_plane* pl) {
    const double H = s_rs_view[0], ofx = s_rs_view[1], ofy = s_rs_view[2];
    double hi = -1e30, lo = 1e30;
    for (int t = 0; t < mb->nt; t++)
        for (int k = 0; k < 3; k++) {
            const float* v = s_q_tri[mb->t0 + t][k];
            const double z = v[2];
            const double sd = pl->n[0] * ((v[0] - ofx) * z / H) + pl->n[1] * ((v[1] - ofy) * z / H) +
                              pl->n[2] * z - pl->d;
            hi = ms_max(hi, sd); lo = ms_min(lo, sd);
        }
    return hi > 0.0 && lo < 0.0 ? ms_min(hi, -lo) : -ms_min(fabs(hi), fabs(lo));
}

/** @brief Compare one moby's carried planes against its fresh ones, counting the plane classes.
 * @param qi The moby's queue index.
 * @param graze Cleared when an unmatched plane is not a grazing contact (mc_straddle within MC_GRAZE).
 * @return The count of mismatching planes and plane attributes. */
static long mc_vf_planes(int qi, int* graze) {
    const int nc = s_vc_np[qi], nf = s_vf_np[qi];
    const ms_moby* mb = &s_q_mob[qi];
    uint8_t used[MS_MAX_PLANES] = { 0 };
    const openpete_snap_plane* f = &s_vf_pl[s_vf_p0[qi]];
    long bad = 0;
    for (int j = 0; j < nc; j++) {
        const openpete_snap_plane* c = &s_vc_pl[s_vc_p0[qi] + j];
        double sg = 1.0;
        const int m = mc_vf_match(c, f, nf, used, &sg);
        s_vs.planes++;
        if (m < 0) {
            s_vs.carry_only_p++; bad++;
            *graze &= fabs(mc_straddle(mb, c)) < MC_GRAZE;
            continue;
        }
        const int side = (c->side < 0.0f) != (sg * f[m].side < 0.0);
        s_vs.side += side; s_vs.slab += c->slab != f[m].slab; s_vs.dside += c->dside != f[m].dside;
        bad += side + (c->slab != f[m].slab) + (c->dside != f[m].dside);
        *graze &= !(side || c->slab != f[m].slab || c->dside != f[m].dside);
    }
    for (int j = 0; j < nf; j++) {
        if (used[j]) continue;
        s_vs.fresh_only_p++; bad++;
        *graze &= fabs(mc_straddle(mb, &f[j])) < MC_GRAZE;
    }
    return bad;
}

/** @brief Whether every one of a moby's planes is a grazing contact (mc_straddle within MC_GRAZE).
 * @param qi The moby's queue index.
 * @param pl The planes.
 * @param n Their count.
 * @return 1 when every plane grazes, else 0. */
static int mc_vf_graze_all(int qi, const openpete_snap_plane* pl, int n) {
    for (int j = 0; j < n; j++)
        if (!(fabs(mc_straddle(&s_q_mob[qi], &pl[j])) < MC_GRAZE)) return 0;
    return 1;
}

/** @brief Compare one moby's carried planes against its fresh ones and count the classes.
 *
 * A mismatching moby is counted as moved (mc_vf_moved over MC_MOVED), else
 * as grazing when every unmatched plane is a grazing contact, else as
 * static.
 * @param qi The moby's queue index.
 * @return 1 when any class other than absent or unresolved holds, else 0. */
static int mc_vf_moby(int qi) {
    const int nc = s_vc_np[qi], nf = s_vf_np[qi];
    if (nc < 0) { s_vs.absent++; return 0; }
    if (nf < 0) { s_vs.unresolved++; return 0; }
    s_vs.mobys++;
    if (!nc && !nf) return 0;
    int graze = 1;
    const char* why = "planes";
    if (nc && !nf) { s_vs.carry_only_m++; why = "carry_only_moby"; }
    if (nf && !nc) { s_vs.fresh_only_m++; why = "fresh_only_moby"; }
    const long bad = nc && nf ? mc_vf_planes(qi, &graze) : 0;
    if (nc && nf && !bad) return 0;
    if (!(nc && nf))
        graze = nc ? mc_vf_graze_all(qi, &s_vc_pl[s_vc_p0[qi]], nc) : mc_vf_graze_all(qi, &s_vf_pl[s_vf_p0[qi]], nf);
    const double mv = mc_vf_moved(qi);
    const int cls = mv > MC_MOVED ? 0 : graze ? 1 : 2;
    s_vs.mism_cls[cls]++;
    if (cls == 2 || mc_verify_mode() != 1) mc_vf_detail(qi, why, mv);
    return 1;
}

/** @brief Compare the carry against the fresh pass for every queued moby of this resolve and count the engagement.
 * @param ep The endpoint, or NULL.
 * @param L The present walk camera's linear part.
 * @param P Its position. */
static void mc_vf_compare(const mc_tab* ep, const double L[3][3], const double P[3]) {
    mc_vstat* v = &s_vs;
    v->resolves++;
    const unsigned ep_scene = openpete_native_scene_epoch();
    if (ep_scene != v->last_epoch) { v->last_epoch = ep_scene; v->presents++; v->presents_anz += s_rex_alpha != 0.0; }
    if (ep) {
        double dp = 0.0, dl = 0.0;
        for (int i = 0; i < 3; i++) {
            dp += (P[i] - ep->P[i]) * (P[i] - ep->P[i]);
            for (int j = 0; j < 3; j++) dl = ms_max(dl, fabs(L[i][j] - ep->L[i][j]));
        }
        dp = sqrt(dp);
        v->cam_moved += dp > 0.0 || dl > 0.0;
        v->dp_sum += dp; v->dp_max = ms_max(v->dp_max, dp); v->dl_max = ms_max(v->dl_max, dl);
    }
    for (int k = 0; k < s_vq_n; k++) v->mism_mobys += mc_vf_moby(k);
    static int s_atexit;
    if (!s_atexit) { s_atexit = 1; atexit(mc_vf_print); }
    if (v->resolves % 512 == 0) mc_vf_print();
}

/** @brief Move the queued mobys still to be added (add 1) to the front of the queue, keeping their order.
 * @return Their count. */
static int ms_compact(void) {
    int n = 0;
    for (int k = 0; k < s_q_nm; k++) {
        if (!s_q_mob[k].add) continue;
        if (n != k) { const ms_moby t = s_q_mob[n]; s_q_mob[n] = s_q_mob[k]; s_q_mob[k] = t; }
        n++;
    }
    return n;
}

/** @brief Add the carried planes of every queued moby the endpoint resolved, in queue order, and mark those mobys added.
 * @param cs The census: carried and fresh mobys, overflow. */
static void ms_carry_add(ms_census* cs) {
    for (int k = 0; k < s_q_nm; k++) {
        ms_moby* mb = &s_q_mob[k];
        if (s_vc_np[k] < 0) { cs->fresh++; continue; }
        mb->add = 0;
        cs->carried++;
        if (s_vc_np[k] > 0) ms_add(mb, &s_vc_pl[s_vc_p0[k]], s_vc_np[k], cs);
    }
}


/** @brief Set up the carry for the resolve under way and return how many queued mobys the contact pass resolves.
 *
 * A canonical resolve (carry or verify on) stores into its epoch's table
 * and resolves the whole queue. A sub-tick resolve maps the endpoint's
 * planes (mc_map); with the carry on it adds them and resolves only the
 * mobys the endpoint did not, moved to the front of the queue, unless the
 * verify runs, which resolves the whole queue and adds only those.
 * @param cs The census.
 * @return The count of queued mobys, from the front, the contact pass resolves. */
static int ms_carry_begin(ms_census* cs) {
    s_store = NULL; s_vf_on = 0; s_rs_ep = NULL; s_rs_wb = 0;
    for (int k = 0; k < s_q_nm; k++) { s_q_mob[k].qi = k; s_q_mob[k].add = 1; }
    const int carry = mc_carry_on(), vm = mc_verify_mode();
    if (!carry && !vm) return s_q_nm;
    if (!terrain_snap_camera(s_rs_L, s_rs_P)) return s_q_nm;
    double Q[3][3];
    if (vm && mc_inv_t(s_rs_L, Q)) {
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) s_rs_Li[r][c] = Q[c][r];
        s_rs_wb = 1;
    }
    if (!s_rex) {
        s_store = mc_table_for(spyro_native_canon_epoch_stamp(), openpete_native_scene_frame());
        mc_cam_note(s_store, s_rs_L, s_rs_P);
        return s_q_nm;
    }
    s_rs_ep = mc_endpoint();
    if (!mc_map(s_rs_ep, s_rs_L, s_rs_P)) return s_q_nm;
    s_vf_on = vm && mc_vf_begin();
    if (!carry) return s_q_nm;
    ms_carry_add(cs);
    s_pc_carried += cs->carried; s_pc_fresh += cs->fresh;
    return s_vf_on ? s_q_nm : ms_compact();
}

/** @brief Finish the carry of the resolve under way: the verify comparison, and the per-present counts for the profiler.
 * @param cs The resolve's census; its overflow counts into the verify's. */
static void ms_carry_end(const ms_census* cs) {
    if (s_vf_on) { s_vs.ovf += cs->ovf; mc_vf_compare(s_rs_ep, s_rs_L, s_rs_P); }
    s_store = NULL; s_vf_on = 0;
    if (s_rex && mc_carry_on() && op_prof_on()) {
        static int id_c = -1, id_f = -1;
        if (id_c < 0) { id_c = op_prof_intern("mrexn.snap_carried"); id_f = op_prof_intern("mrexn.snap_fresh"); }
        op_prof_count(id_c, (uint64_t)s_pc_carried);
        op_prof_count(id_f, (uint64_t)s_pc_fresh);
    }
}

/** @brief Resolve one chunk of at most 128 queued mobys: capture the terrain their boxes reach, find each one's contact planes, add them to the table.
 * @param g The chunk's first queued moby.
 * @param nb Its moby count.
 * @param view H, ofx, ofy.
 * @param cs The census, updated (its clocks run when cs->timed).
 * @return 0 when the capture failed (the rest of the queue is dropped), else 1. */
static int ms_resolve_chunk(int g, int nb, const float view[3], ms_census* cs) {
    terrain_snap_box box[128];
    for (int k = 0; k < nb; k++) ms_box(&s_q_mob[g + k], &box[k]);
    const terrain_snap_tri* tt = NULL;
    terrain_snap_census tc;
    const uint64_t c0 = cs->timed ? ms_cpu_ns() : 0;
    const int ntt = terrain_snap_capture(box, nb, &tt, &tc);
    if (cs->timed) cs->cap_ns += ms_cpu_ns() - c0;
    if (ntt < 0) return 0;
    const uint64_t m0 = cs->timed ? ms_mono_ns() : 0;
    if (!ms_terrain_make(tt, ntt, view)) return 0;
    if (cs->timed) cs->near_ns += ms_mono_ns() - m0;
    cs->faces += tc.faces; cs->tris += tc.tris; cs->sectors += tc.sectors;
    uint8_t snapped[128];
    int first[129];
    const int* cand = ms_cands(tt, ntt, nb, first);
    for (int k = 0; k < nb; k++)
        snapped[k] = ms_moby_contact(&s_q_mob[g + k], tt, cand + first[k],
                                     first[k + 1] - first[k], view, cs) > 0;
    cs->overlaps += ms_overlaps(box, snapped, nb);
    for (int k = 0; k < nb; k++) if (snapped[k]) cs->zmin = ms_minf(cs->zmin, box[k].z0);
    return 1;
}

/** @brief Resolve the queue when it and the last terrain build belong to the scene now accumulating.
 *
 * Mobys go to the capture in groups of up to 128 (one bit each in the hit
 * sets); the queue empties afterwards. The carry (ms_carry_begin) may place
 * some mobys' planes first and leave the capture the rest. */
static void ms_resolve(void) {
    if (!s_q_nm || s_q_epoch != openpete_native_scene_epoch() || !terrain_snap_ready()) return;
    const uint64_t t0 = ms_census_on() ? ms_cpu_ns() : 0;
    ms_census cs; memset(&cs, 0, sizeof cs);
    cs.zmin = 1e30f;
    cs.timed = t0 != 0;
    float view[3];
    terrain_snap_view(&view[0], &view[1], &view[2]);
    if (view[0] > 0.0f) {
        openpete_native_snap_view(view[0], view[1], view[2]);
        ms_up_sync();
        memcpy(s_rs_view, view, sizeof s_rs_view);
        const int nrun = ms_carry_begin(&cs);
        for (int g = 0; g < nrun; g += 128)
            if (!ms_resolve_chunk(g, nrun - g < 128 ? nrun - g : 128, view, &cs)) break;
        ms_carry_end(&cs);
    }
    cs.pulled = s_q_nm;
    s_q_nm = s_q_nt = 0;
    if (t0) ms_census_print(&cs, ms_cpu_ns() - t0);
}

/** @brief Run ms_resolve inside the profiler scope of its leg: mrex.resolve in a sub-tick re-extract, else mcan.resolve. */
static void ms_resolve_scoped(void) {
    if (s_rex) { OP_PROF_ACCUM_SCOPE("mrex.resolve"); ms_resolve(); }
    else       { OP_PROF_ACCUM_SCOPE("mcan.resolve"); ms_resolve(); }
}

void moby_snap_moby_fed(void) { ms_resolve_scoped(); }

void moby_snap_terrain_fed(struct CPUState* cpu) {
    (void)cpu;
    ms_resolve_scoped();
}
