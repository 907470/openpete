/* moby_snap.c - contact planes of pulled mobys against the drawn terrain;
 * see moby_snap.h. Camera space throughout is the PSX screen unprojected at
 * view-Z with the terrain build's projection: p = ((sx - ofx) z / H,
 * (sy - ofy) z / H, z), the space the snap stage rebuilds a fragment in. */
#include "renderer/moby_snap.h"
#include "renderer/terrain.h"
#include "openpete_native_render.h"
#include "openpete_fiber.h"          /* openpete_feed_key_now: census line label */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
 * bounds of that run (screen x0, y0, x1, y1, view-Z z0, z1). */
typedef struct { uint32_t mid; int mord, dofs; float off; int t0, nt; float b[6]; } ms_moby;

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
    uint64_t plane_ns;    /* the plane pass: corners, side, component, slab, disc */
    long cut_mobys;       /* snapped mobys a coplanar drawn triangle still cuts (ms_falsify) */
    long cut_samples;     /* their cut sample points */
    long cut_cls[3];      /* the cut samples by class (ms_falsify): 0 outside the plane's slab
                           * or disc, 1 inside both under one of the plane's contact
                           * triangles, 2 inside both under a coplanar non-contact triangle */
    long cut_b_mobys;     /* mobys with a class-1 cut */
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
 * contact face's), the moby triangles that intersect its faces, and whether
 * any of them is double-sided. */
typedef struct { ms_v3 n; double d; int dside; int nh; int* hit; } ms_plane;

/* A far-side vertex key of one moby triangle, for the connectivity sort. */
typedef struct { float k[3]; int tri; } ms_vkey;

/** @brief Order vertex keys by their exact coordinates (qsort).
 * @param a First key.
 * @param b Second key.
 * @return Negative, zero or positive. */
static int ms_vkey_cmp(const void* a, const void* b) {
    const ms_vkey *x = a, *y = b;
    for (int i = 0; i < 3; i++) {
        if (x->k[i] < y->k[i]) return -1;
        if (x->k[i] > y->k[i]) return 1;
    }
    return 0;
}

/** @brief Find a union-find root, halving the path.
 * @param uf The parent array.
 * @param i The element.
 * @return Its root. */
static int ms_root(int* uf, int i) {
    while (uf[i] != i) { uf[i] = uf[uf[i]]; i = uf[i]; }
    return i;
}

/** @brief Mark the sunk component of a plane: the moby triangles beyond it connected through shared far-side vertices to one that intersects its faces.
 *
 * Triangles share a vertex when the walk emitted the same projected vertex
 * for both (bitwise equal screen point and key depth).
 * @param m Screen corners of the moby's triangles.
 * @param sd The corners' signed distances to the plane, 3 per triangle.
 * @param nt Triangle count.
 * @param pl The plane.
 * @param side The side holding the moby's centre (+1 or -1).
 * @param uf Scratch, nt ints.
 * @param keys Scratch, 3 nt keys.
 * @param seen Receives 1 for each triangle of the component. */
static void ms_component(const float (*m)[3][3], const double* sd, int nt, const ms_plane* pl,
                         double side, int* uf, ms_vkey* keys, uint8_t* seen) {
    int nk = 0;
    for (int i = 0; i < nt; i++) {
        uf[i] = i; seen[i] = 0;
        for (int k = 0; k < 3; k++)
            if (sd[i * 3 + k] * side < 0.0) {
                memcpy(keys[nk].k, m[i][k], sizeof keys[nk].k);
                keys[nk++].tri = i;
            }
    }
    qsort(keys, (size_t)nk, sizeof *keys, ms_vkey_cmp);
    for (int j = 1; j < nk; j++)
        if (!ms_vkey_cmp(&keys[j - 1], &keys[j]))
            uf[ms_root(uf, keys[j].tri)] = ms_root(uf, keys[j - 1].tri);
    for (int i = 0; i < nt; i++) uf[i] = ms_root(uf, i);
    for (int h = 0; h < pl->nh; h++) seen[uf[pl->hit[h]]] = 2;   /* bit 1: a hit triangle's root */
    for (int i = 0; i < nt; i++) seen[i] |= (uint8_t)(seen[uf[i]] >> 1);
    for (int i = 0; i < nt; i++) seen[i] &= 1;
}

/** @brief Finish one contact plane into the snap table's form: side, slab and disc.
 *
 * side: the side of the plane holding the midpoint of the moby's vertex
 * extent along its normal. The sunk component's deepest vertex beyond the
 * plane gives the penetration; slab = min(128 dofs, penetration + 1), the
 * game's own pull bound. The disc is the component projected onto the
 * plane: the centre of its bounds and the largest distance from it, plus 1.
 * @param m Screen corners of the moby's triangles.
 * @param T Their camera-space form.
 * @param nt Triangle count.
 * @param dofs The moby's authored depth offset.
 * @param pl The plane.
 * @param uf Scratch, nt ints.
 * @param keys Scratch, 3 nt keys.
 * @param seen Scratch, nt bytes.
 * @param sdv Scratch, 3 nt doubles: the corners' signed distances.
 * @param o Receives the plane. */
static void ms_plane_finish(const float (*m)[3][3], const ms_tri* T, int nt, int dofs,
                            const ms_plane* pl, int* uf, ms_vkey* keys, uint8_t* seen,
                            double* sdv, openpete_snap_plane* o) {
    double smin = 1e300, smax = -1e300;
    for (int i = 0; i < nt; i++)
        for (int k = 0; k < 3; k++) {
            const double sd = v3dot(pl->n, T[i].p[k]) - pl->d;
            sdv[i * 3 + k] = sd;
            smin = ms_min(smin, sd); smax = ms_max(smax, sd);
        }
    const double side = (smin + smax) > 0.0 ? 1.0 : -1.0;
    ms_component(m, sdv, nt, pl, side, uf, keys, seen);
    double pen = 0.0;
    ms_v3 lo = { 1e300, 1e300, 1e300 }, hi = { -1e300, -1e300, -1e300 };
    for (int i = 0; i < nt; i++) {
        if (!seen[i]) continue;
        for (int k = 0; k < 3; k++) {
            const double sd = sdv[i * 3 + k];
            pen = ms_max(pen, -sd * side);
            const ms_v3 q = { T[i].p[k].x - sd * pl->n.x, T[i].p[k].y - sd * pl->n.y,
                              T[i].p[k].z - sd * pl->n.z };
            lo.x = ms_min(lo.x, q.x); lo.y = ms_min(lo.y, q.y); lo.z = ms_min(lo.z, q.z);
            hi.x = ms_max(hi.x, q.x); hi.y = ms_max(hi.y, q.y); hi.z = ms_max(hi.z, q.z);
        }
    }
    const ms_v3 c = { (lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5 };
    double r = 0.0;
    for (int i = 0; i < nt; i++) {
        if (!seen[i]) continue;
        for (int k = 0; k < 3; k++) {
            const double sd = sdv[i * 3 + k];
            const ms_v3 q = { T[i].p[k].x - sd * pl->n.x, T[i].p[k].y - sd * pl->n.y,
                              T[i].p[k].z - sd * pl->n.z };
            r = ms_max(r, v3dot(v3sub(q, c), v3sub(q, c)));
        }
    }
    r = sqrt(r);
    const double slab = ms_min(128.0 * (double)dofs, pen + 1.0);
    o->n[0] = (float)pl->n.x; o->n[1] = (float)pl->n.y; o->n[2] = (float)pl->n.z;
    o->d = (float)pl->d;
    o->sslab = (float)(side * slab);
    o->c[0] = (float)c.x; o->c[1] = (float)c.y; o->c[2] = (float)c.z;
    o->r = (float)(r + 1.0);
    o->dside = pl->dside;
}

/** @brief Add a contact face to the plane it lies in, or open a plane for it.
 * @param P The moby's planes.
 * @param np Their count, updated.
 * @param F The contact face.
 * @param dside Whether it is double-sided.
 * @param hit The moby triangles it intersects.
 * @param nh Their count.
 * @param pool Storage for the planes' hit lists, nt ints per plane.
 * @param nt The moby's triangle count.
 * @return 1 when placed, 0 when the moby's plane cap is full. */
static int ms_plane_add(ms_plane* P, int* np, const ms_tri* F, int dside, const int* hit, int nh,
                        int* pool, int nt) {
    ms_plane* pl = NULL;
    for (int j = 0; j < *np && !pl; j++) {
        const double c = v3dot(P[j].n, F->n);
        if (fabs(c) > 1.0 - MS_MERGE_DN && fabs(P[j].d - (c < 0.0 ? -F->d : F->d)) < MS_MERGE_DD)
            pl = &P[j];
    }
    if (!pl) {
        if (*np >= MS_MAX_PLANES) return 0;
        pl = &P[(*np)++];
        pl->n = F->n; pl->d = F->d; pl->dside = 0; pl->nh = 0;
        pl->hit = pool + (size_t)(*np - 1) * (size_t)nt;
    }
    pl->dside |= dside;
    for (int h = 0; h < nh; h++) {
        int dup = 0;
        for (int q = 0; q < pl->nh && !dup; q++) dup = pl->hit[q] == hit[h];
        if (!dup) pl->hit[pl->nh++] = hit[h];
    }
    return 1;
}

/** @brief Print a moby's contact planes at the feed keys the OPENPETE_MOBY_SNAP_DUMP knob names, an instrument.
 *
 * One line per plane: the moby's identity, ordinal, pull and offset, the
 * view, and the plane's normal, offset, signed slab, disc and double-sided
 * flag.
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
                        "n=%.7f,%.7f,%.7f d=%.4f sslab=%.4f c=%.3f,%.3f,%.3f r=%.3f dside=%d\n",
                key, mb->mid, mb->mord, mb->dofs, mb->off, view[0], view[1], view[2],
                pl[k].n[0], pl[k].n[1], pl[k].n[2], pl[k].d, pl[k].sslab,
                pl[k].c[0], pl[k].c[1], pl[k].c[2], pl[k].r, pl[k].dside);
}

/* Scratch shared by the per-moby contact pass, grown on demand. */
static ms_tri*  s_mt;  static int s_mt_cap;
static ms_tri*  s_tt;  static int s_tt_cap;      /* per captured triangle: camera-space form */
static float  (*s_tb)[6]; static int s_tb_cap;   /* per captured triangle: bounds */
static int*     s_cj;  static int s_cj_cap;      /* one moby's usable candidates */
static unsigned* s_tcon; static int s_tcon_cap;  /* per captured triangle: the last moby (s_mserial) it touched */
static unsigned  s_mserial;                      /* the contact pass's moby counter */
static int*     s_ix;  static int s_ix_cap;
static int*     s_pool; static int s_pool_cap;
static ms_vkey* s_keys; static int s_keys_cap;
static uint8_t* s_seen; static int s_seen_cap;
static uint8_t* s_made; static int s_made_cap;   /* s_mt[i] is built (ms_make_near) */
static int*     s_ml;  static int s_ml_cap;      /* the moby triangles near the candidates */
static int*     s_mstamp; static int s_mstamp_cap; /* per moby triangle: the last query that met it */
static unsigned s_mquery;                        /* the query counter s_mstamp holds */
static double*  s_sdv; static int s_sdv_cap;     /* per corner: signed distance to a plane */

/** @brief Grow every scratch array of the contact pass for a moby of nt triangles against nc candidates.
 * @param nt Moby triangles.
 * @param nc Candidate terrain triangles.
 * @return 1 when all hold, 0 on an allocation failure. */
static int ms_scratch(int nt, int nc) {
    return ms_grow((void**)&s_mt, &s_mt_cap, nt, sizeof *s_mt) &&
           ms_grow((void**)&s_cj, &s_cj_cap, nc > 0 ? nc : 1, sizeof *s_cj) &&
           ms_grow((void**)&s_ix, &s_ix_cap, nt * 2, sizeof *s_ix) &&
           ms_grow((void**)&s_pool, &s_pool_cap, nt * MS_MAX_PLANES, sizeof *s_pool) &&
           ms_grow((void**)&s_keys, &s_keys_cap, nt * 3, sizeof *s_keys) &&
           ms_grow((void**)&s_seen, &s_seen_cap, nt, 1) &&
           ms_grow((void**)&s_made, &s_made_cap, nt, 1) &&
           ms_grow((void**)&s_ml, &s_ml_cap, nt, sizeof *s_ml) &&
           ms_grow((void**)&s_mstamp, &s_mstamp_cap, nt, sizeof *s_mstamp) &&
           ms_grow((void**)&s_sdv, &s_sdv_cap, nt * 3, sizeof *s_sdv);
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

/** @brief The depth the snap stage writes for a moby point at its key depth, or -1 when a hide plane moves it behind (op_snap_depth on the CPU).
 * @param p The point (camera space).
 * @param out The moby's planes.
 * @param np Their count.
 * @param S The largest slab of the moby's planes.
 * @return The written view-Z, or -1. */
static double ms_snap_vz(ms_v3 p, const openpete_snap_plane* out, int np, double S) {
    const ms_v3 ray = { p.x / p.z, p.y / p.z, 1.0 };
    double rev = 3.0e38, hid = -1.0;
    for (int k = 0; k < np; k++) {
        const openpete_snap_plane* P = &out[k];
        const ms_v3 n = { P->n[0], P->n[1], P->n[2] };
        const double sd = v3dot(n, p) - P->d, s = P->sslab < 0.0f ? -sd : sd;
        if (s >= 0.0 || s < -fabs((double)P->sslab)) continue;
        const ms_v3 q = { p.x - sd * n.x - P->c[0], p.y - sd * n.y - P->c[1], p.z - sd * n.z - P->c[2] };
        if (v3len(q) > P->r) continue;
        const double nr = v3dot(n, ray);
        if (fabs(nr) < 1e-9) continue;
        const double zc = P->d / nr;
        if (zc <= 1.0) continue;
        const double e = ms_max(0.5 + 2e-5 * zc, 4.0 * 6e-8 * zc) * (2.0 + s / ms_max(S, 1.0));
        if ((-P->d > 0.0f) == (P->sslab > 0.0f)) rev = ms_min(rev, zc - e);
        else if (P->dside) hid = ms_max(hid, zc + e);
    }
    if (hid > p.z) return -1.0;
    return ms_max(ms_min(p.z, rev), 1.0);
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

/** @brief Find a coplanar captured triangle that covers a moby point's screen position more than one world unit nearer than the depth written for it.
 * @param p The point.
 * @param vz The view-Z the snap stage writes for it.
 * @param tt The captured triangles.
 * @param own The coplanar candidates.
 * @param no Their count.
 * @param view H, ofx, ofy.
 * @param zt Receives its view-Z along the point's ray.
 * @return Its index in @p own, or -1. */
static int ms_falsify_cut(ms_v3 p, double vz, const terrain_snap_tri* tt, const int* own, int no, const float view[3],
                          double* zt) {
    const ms_v3 ray = { p.x / p.z, p.y / p.z, 1.0 };
    const double x = ray.x * view[0] + view[1], y = ray.y * view[0] + view[2];
    for (int q = 0; q < no; q++) {
        const ms_tri* T = &s_tt[own[q]];
        const double nr = v3dot(T->n, ray);
        if (fabs(nr) < 1e-9 || !ms_in_tri(&tt[own[q]], x, y)) continue;
        const double zT = T->d / nr;
        if (zT > 1.0 && zT < vz - 1.0) { *zt = zT; return q; }
    }
    return -1;
}

/** @brief Classify one cut sample (ms_census.cut_cls) and print it under the census when it lies inside the slab and disc of the covering triangle's plane.
 * @param p The sample.
 * @param vz The depth the snap writes for it.
 * @param zt The covering triangle's view-Z on its ray.
 * @param P The covering triangle's plane.
 * @param k That plane's index.
 * @param contact Whether the covering triangle is one of the moby's contact triangles.
 * @param tri The moby triangle and sample (tri * 7 + sample).
 * @param view H, ofx, ofy.
 * @return The class: 0 outside the slab or disc, 1 inside under a contact triangle, 2 inside under a non-contact one. */
static int ms_falsify_class(ms_v3 p, double vz, double zt, const openpete_snap_plane* P, int k, int contact,
                            int tri, const float view[3]) {
    const ms_v3 n = { P->n[0], P->n[1], P->n[2] };
    const double sd = v3dot(n, p) - P->d, s = P->sslab < 0.0f ? -sd : sd;
    const ms_v3 q = { p.x - sd * n.x - P->c[0], p.y - sd * n.y - P->c[1], p.z - sd * n.z - P->c[2] };
    const double disc = v3len(q), slab = fabs((double)P->sslab);
    if (s >= 0.0 || s < -slab || disc > P->r) return 0;
    const int cls = contact ? 1 : 2;
    fprintf(stderr, "[moby-snap-cut] key=%u class=%d tri=%d sample=%d sx=%.2f sy=%.2f p=%.2f,%.2f,%.2f vz=%.2f zt=%.2f "
                    "plane=%d s=%.3f slab=%.3f disc=%.2f r=%.2f\n",
            openpete_feed_key_now(), cls, tri / 7, tri % 7, p.x / p.z * view[0] + view[1], p.y / p.z * view[0] + view[2],
            p.x, p.y, p.z, vz, zt, k, s, slab, disc, (double)P->r);
    return cls;
}

/** @brief Count the sample points of a snapped moby that a drawn terrain triangle lying in one of its own planes still covers in front after the snap (a falsifier census).
 *
 * Samples: each triangle's corners, edge midpoints and centroid at key
 * depth. A sample is cut when a captured triangle coplanar with one of the
 * moby's planes (the merge tolerance) covers its screen point at a view-Z
 * more than one world unit nearer than the depth the snap writes for it.
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
    for (int k = 0; k < np; k++) S = ms_max(S, fabs((double)out[k].sslab));
    int own[512], ownk[512];
    const int no = ms_falsify_own(out, np, cj, nc, own, ownk, 512);
    long cut = 0;
    for (int i = 0; i < nt && no; i++)
        for (int w = 0; w < 7; w++) {
            const ms_v3 p = ms_falsify_sample(&s_mt[i], w);
            if (!(p.z > 1.0)) continue;
            const double vz = ms_falsify_on() == 2 ? p.z : ms_snap_vz(p, out, np, S);
            double zt = 0.0;
            const int q = vz >= 0.0 ? ms_falsify_cut(p, vz, tt, own, no, view, &zt) : -1;
            if (q < 0) continue;
            cut++;
            cls[ms_falsify_class(p, vz, zt, &out[ownk[q]], ownk[q], s_tcon[own[q]] == s_mserial, i * 7 + w, view)]++;
        }
    return cut;
}

/** @brief Find one queued moby's contact planes against the captured terrain and add them to the scene's snap table.
 * @param mb The moby.
 * @param tt The captured terrain triangles.
 * @param cand The indices into @p tt of those its search box reaches.
 * @param ncand Their count.
 * @param view H, ofx, ofy.
 * @param cs The census, updated.
 * @return The planes added. */
static int ms_moby_contact(const ms_moby* mb, const terrain_snap_tri* tt, const int* cand,
                           int ncand, const float view[3], ms_census* cs) {
    const float (*m)[3][3] = (const float (*)[3][3])&s_q_tri[mb->t0];
    const int nt = mb->nt;
    if (!ncand || !ms_scratch(nt, ncand)) return 0;
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
        if (!ms_plane_add(P, &np, &s_tt[j], tt[j].dside, s_ix, nh, s_pool, nt)) cs->ovf++;
    }
    if (t) { t1 = ms_mono_ns(); cs->test_ns += t1 - t; t = t1; }
    if (!np) return 0;
    ms_make_rest(m, nt, view);
    openpete_snap_plane out[MS_MAX_PLANES];
    for (int k = 0; k < np; k++)
        ms_plane_finish(m, s_mt, nt, mb->dofs, &P[k], s_ix, s_keys, s_seen, s_sdv, &out[k]);
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
    if (!openpete_native_snap_add(mb->mid, mb->mord, mb->off, out, np)) { cs->ovf += np; return 0; }
    if (mb->mord > 0 && s_sn_n < 256) { s_sn[s_sn_n].mid = mb->mid; s_sn[s_sn_n++].ord = mb->mord; }
    return np;
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
                    "overflow=%ld+%u cut_mobys=%ld cut_samples=%ld cut_a=%ld cut_b=%ld cut_c=%ld cut_b_mobys=%ld cpu_us=%.1f capture_us=%.1f near_us=%.1f test_us=%.1f plane_us=%.1f mean_cpu_us=%.1f n=%ld\n",
            openpete_feed_key_now(), cs->pulled, cs->faces, cs->tris, cs->sectors, cs->tests,
            cs->contacts, cs->contacts_t4, cs->overlaps, (double)cs->zmin, ms_ord_collisions(), cs->planes, ne, np, cs->ovf, ovf, cs->cut_mobys, cs->cut_samples, cs->cut_cls[0], cs->cut_cls[1], cs->cut_cls[2], cs->cut_b_mobys, (double)ns / 1000.0,
            (double)cs->cap_ns / 1000.0, (double)cs->near_ns / 1000.0, (double)cs->test_ns / 1000.0,
            (double)cs->plane_ns / 1000.0, s_sum_us / (double)s_n, s_n);
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
 * sets); the queue empties afterwards. */
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
        for (int g = 0; g < s_q_nm; g += 128)
            if (!ms_resolve_chunk(g, s_q_nm - g < 128 ? s_q_nm - g : 128, view, &cs)) break;
    }
    cs.pulled = s_q_nm;
    s_q_nm = s_q_nt = 0;
    if (t0) ms_census_print(&cs, ms_cpu_ns() - t0);
}

void moby_snap_moby_fed(void) { ms_resolve(); }

void moby_snap_terrain_fed(struct CPUState* cpu) {
    (void)cpu;
    ms_resolve();
}
