// games/common/renderer/portal_sky_s23.c : the destination sky behind a
// Spyro 2/3 homeworld portal's picture; the contract is portal_sky_s23.h.
//
// Reading the sky. A level's data is WAD entry level_wad_base + 2 * slot,
// the slot the byte the loader reads from level_slots at the level number.
// Its sub-file 1 is the block chain the loader walks: level_wad_hops blocks
// that each open with their own size, one block whose size word sits at
// +0x0C, then the sky block {size, payload} whose payload is the header the
// sky relocation reads (S2 func_8001C5EC, S3 func_8002B6C8): three colour
// bytes and a pad, the sector count, and one offset per sector relative to
// the payload. The portal block follows the sky block.
//
// The camera. The picture's vertices are the sky's vertices projected with
// the pass's screen centre (256, 120) and the scales 1024/3 across and 640/3
// down, truncated to integers, under one rotation per portal. Measured over
// the 13 homeworld portals of Summer Forest and Sunrise Spring: a free
// projective fit gives scales 341.1-341.4 and 213.0-213.6 and a centre
// within 0.6 of (255.5, 119.5); with those fixed, the rotation fit leaves a
// median 0.39-0.46 and a largest 0.74-1.33 pixels per vertex. The rotation is
// found by hypothesis: a picture face whose colours are an ordered colour
// triple of a sky face names three ray pairs; Horn's quaternion method gives
// the rotation they imply, and the rotation that brings the most picture
// vertices within PSK_SNAP pixels of a projected sky vertex wins, refined by
// Horn over every such pair. Faces are tried fewest candidates first, and a
// rotation that places every picture vertex ends the search.

#include "renderer/portal_sky_s23.h"
#include "renderer/portal_spyro23.h"   /* portal_game_s23 */
#include "renderer/cyclorama.h"        /* cyclorama_decode_sector */
#include "renderer/wad_subfile.h"
#include "openpete_disc_trace.h"       /* OPENPETE_DISC_TAG_TERRAIN */
#include "openpete_log.h"
#include "openpete_fnv1a64.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PSK_FX    (1024.0 / 3.0)   /* the picture's horizontal scale */
#define PSK_FY    (640.0 / 3.0)    /* its vertical scale */
#define PSK_CX    255.5            /* the screen centre (256, 120) less the truncation's mean */
#define PSK_CY    119.5
#define PSK_SNAP  1.5              /* pixels: a picture vertex this close is the same sky vertex */
#define PSK_NEAR  0.1              /* the cone a direction must lie in, as its unit view-Z */
#define PSK_LEVELS 64
#define PSK_CACHE  16
#define PSK_MAX_PIC_V 1024
#define PSK_MAX_PIC_F 4096
#define PSK_MAX_SKY_F 16384
#define PSK_GRID_X0 (-1024)        /* the picture-vertex grid: 4-pixel cells over */
#define PSK_GRID_Y0 (-1024)        /* [-1024, 1024) on both axes */
#define PSK_GRID_N  512
#define PSK_CELL    4

/* ---- the destination sky, per level ------------------------------------- */

typedef struct {
    int      level;
    int      state;        /* 0 unread, 1 ready, -1 unavailable */
    uint8_t* buf;          /* the sky block's payload */
    uint32_t len;
} psk_level;

static psk_level s_lv[PSK_LEVELS];

/** @brief Reads a little-endian word of a host buffer, 0 when it does not fit.
 * @param b The buffer.
 * @param n Its length.
 * @param o The offset.
 * @return The word. */
static uint32_t psk_w(const uint8_t* b, uint32_t n, uint32_t o) {
    if (o > n || n - o < 4u) return 0u;
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

/** @brief Finds the sky block's payload in a level's data sub-file by the loader's block walk.
 *
 * Steps over level_wad_hops size-prefixed blocks, then over the block whose
 * size word sits at +0x0C, and takes the next block's payload. Every size is
 * checked against the sub-file.
 * @param b The sub-file.
 * @param n Its length.
 * @param len Receives the payload's length.
 * @return The payload's offset, or 0 when the chain leaves the sub-file. */
static uint32_t psk_walk(const uint8_t* b, uint32_t n, uint32_t* len) {
    uint32_t p = 0;
    for (int i = 0; i < portal_game_s23.level_wad_hops; i++) {
        const uint32_t sz = psk_w(b, n, p);
        if (sz < 4u || sz > n - p) return 0u;
        p += sz;
    }
    const uint32_t e = p + 0x0Cu, esz = psk_w(b, n, e);
    if (e > n || esz < 4u || esz > n - e) return 0u;
    const uint32_t sky = e + esz, ssz = psk_w(b, n, sky);
    if (ssz < 12u || ssz > n - sky) return 0u;
    *len = ssz - 4u;
    return sky + 4u;
}

/** @brief The sky block of one level, read from the disc on first use and kept for the session.
 * @param cpu Reads the slot table.
 * @param level The level number.
 * @return The level's entry; its state is 1 when the payload is held. */
static const psk_level* psk_level_sky(CPUState* cpu, int level) {
    psk_level* free_slot = NULL;
    for (int i = 0; i < PSK_LEVELS; i++) {
        if (s_lv[i].state && s_lv[i].level == level) return &s_lv[i];
        if (!s_lv[i].state && !free_slot) free_slot = &s_lv[i];
    }
    static psk_level s_none = { 0, -1, NULL, 0 };
    if (!free_slot || !portal_game_s23.level_slots || level < 0 || level > 255) return &s_none;
    psk_level* L = free_slot;
    L->level = level;
    L->state = -1;
    const int slot = (int)cpu->read_byte(portal_game_s23.level_slots + (uint32_t)level);
    const int e = portal_game_s23.level_wad_base + 2 * slot;
    uint32_t n = 0, len = 0;
    uint8_t* sub = wad_subfile_load(e, 1, OPENPETE_DISC_TAG_TERRAIN, &n, "portal");
    const uint32_t at = sub ? psk_walk(sub, n, &len) : 0u;
    if (at) {
        L->buf = (uint8_t*)malloc(len);
        if (L->buf) { memcpy(L->buf, sub + at, len); L->len = len; L->state = 1; }
    }
    free(sub);
    if (L->state > 0)
        OPL_INFO_F("portal", "destination sky of level %d: WAD entry %d, %u sectors", level, e,
                   psk_w(L->buf, L->len, 4u));
    else
        OPL_WARN_F("portal", "destination sky of level %d: WAD entry %d not readable", level, e);
    return L;
}

/* ---- the picture and the sky's faces ------------------------------------ */

typedef struct {
    int      nv, nf;
    float    x[PSK_MAX_PIC_V], y[PSK_MAX_PIC_V];
    uint16_t f[PSK_MAX_PIC_F][3];
    uint32_t c[PSK_MAX_PIC_F][3];
    uint64_t hash;
} psk_pic;

typedef struct {
    int      n;
    double   p[PSK_MAX_SKY_F][3][3];   /* world corners */
    uint32_t c[PSK_MAX_SKY_F][3];
} psk_sky;

/** @brief Decodes a record's picture mesh from guest RAM, as the preview leaf reads it.
 *
 * Vertices are (y << 16 | x) << 5 (x the signed 16 bits above bit 5, y the
 * signed 11 bits above bit 21); a face's first word holds the vertex indices
 * in 12/10/10 bits and its second either one flat colour index (bit 31 set,
 * bits 29-20) or three colour byte offsets. A face naming a vertex or colour
 * past its table is skipped. The hash covers the header, the vertex table
 * without the leaf's five scratch bits, and the colour and face tables.
 * @param cpu Reads the mesh.
 * @param rec The portal record.
 * @param P Receives the picture.
 * @return 1 when the mesh lies in RAM and its counts fit, else 0. */
static int psk_read_picture(CPUState* cpu, uint32_t rec, psk_pic* P) {
    const uint32_t M = cpu->read_word(rec + 0x3C) + 0x3Cu;
    if (M < 0x80000000u || M >= 0x80200000u) return 0;
    uint32_t h[6];
    for (int i = 0; i < 6; i++) h[i] = cpu->read_word(M + (uint32_t)i * 4u);
    const uint32_t vc = h[0], cc = h[2], fc = h[4];
    if (vc == 0 || vc > PSK_MAX_PIC_V || cc == 0 || cc > 256u || fc == 0 || fc > PSK_MAX_PIC_F) return 0;
    uint64_t hash = openpete_fnv1a64_update(openpete_fnv1a64_init(), h, sizeof h);
    uint32_t col[256];
    for (uint32_t i = 0; i < vc; i++) {
        const uint32_t w = cpu->read_word(M + h[1] + i * 4u), key = w & ~0x1Fu;
        hash = openpete_fnv1a64_update(hash, &key, sizeof key);
        P->x[i] = (float)(int16_t)((w >> 5) & 0xFFFFu);
        P->y[i] = (float)((int32_t)w >> 21);
    }
    for (uint32_t j = 0; j < cc; j++) col[j] = cpu->read_word(M + h[3] + j * 4u) & 0xFFFFFFu;
    hash = openpete_fnv1a64_update(hash, col, sizeof(uint32_t) * cc);
    P->nv = (int)vc; P->nf = 0;
    for (uint32_t f = 0; f < fc; f++) {
        const uint32_t w0 = cpu->read_word(M + h[5] + f * 8u), w1 = cpu->read_word(M + h[5] + f * 8u + 4u);
        hash = openpete_fnv1a64_update(hash, &w0, sizeof w0);
        const uint32_t v[3] = { (w0 >> 20) & 0xFFFu, (w0 >> 10) & 0x3FFu, w0 & 0x3FFu };
        const uint32_t k[3] = { (w1 & 0x80000000u) ? (w1 >> 20) & 0x3FFu : ((w1 >> 18) & 0xFFCu) >> 2,
                                (w1 & 0x80000000u) ? (w1 >> 20) & 0x3FFu : ((w1 >> 8) & 0xFFCu) >> 2,
                                (w1 & 0x80000000u) ? (w1 >> 20) & 0x3FFu : ((w1 << 2) & 0xFFCu) >> 2 };
        if (v[0] >= vc || v[1] >= vc || v[2] >= vc || k[0] >= cc || k[1] >= cc || k[2] >= cc) continue;
        for (int i = 0; i < 3; i++) { P->f[P->nf][i] = (uint16_t)v[i]; P->c[P->nf][i] = col[k[i]]; }
        P->nf++;
    }
    P->hash = hash;
    return P->nf > 0;
}

/** @brief Collects one decoded sky face in world space (the decode ran under the identity).
 * @param a The first corner.
 * @param b The second corner.
 * @param c The third corner.
 * @param gouraud Unused: a flat face carries its one colour on every corner.
 * @param ctx The psk_sky receiving the face. */
static void psk_collect(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud, void* ctx) {
    (void)gouraud;
    psk_sky* S = (psk_sky*)ctx;
    if (S->n >= PSK_MAX_SKY_F) return;
    const ngte_cvert* v[3] = { &a, &b, &c };
    for (int i = 0; i < 3; i++) {
        S->p[S->n][i][0] = v[i]->m1; S->p[S->n][i][1] = v[i]->m2; S->p[S->n][i][2] = v[i]->m3;
        S->c[S->n][i] = v[i]->rgb;
    }
    S->n++;
}

/** @brief Decodes every sector of a sky payload through the shared cyclorama decode under the rotation `R`.
 * @param L The level's sky.
 * @param R The rotation, unit scale.
 * @param fn Receives each face.
 * @param ctx Passed to `fn`. */
static void psk_decode_sky(const psk_level* L, const double R[3][3], cyclorama_tri_fn fn, void* ctx) {
    const cyclorama_src src = { NULL, L->buf, 0u, L->len };
    const cyclorama_fade none[2] = { { 0u, 0 }, { 0u, 0 } };
    const uint32_t n = psk_w(L->buf, L->len, 4u);
    for (uint32_t i = 0; i < n && i < 1024u; i++) {
        const uint32_t off = psk_w(L->buf, L->len, 8u + i * 4u);
        if (off && off < L->len) cyclorama_decode_sector(&src, off, R, 1.0, none, fn, ctx);
    }
}

/* ---- the registration ---------------------------------------------------- */

/* The picture's vertices bucketed in 4-pixel cells: head[] is each cell's
 * first vertex, next[] chains the rest, -1 ends. */
typedef struct {
    int32_t head[PSK_GRID_N * PSK_GRID_N];
    int32_t next[PSK_MAX_PIC_V];
} psk_grid;

/** @brief The grid cell index of a screen point, -1 outside the grid.
 * @param x The X.
 * @param y The Y.
 * @return The cell. */
static int psk_cell(double x, double y) {
    const double cx = floor((x - PSK_GRID_X0) / PSK_CELL), cy = floor((y - PSK_GRID_Y0) / PSK_CELL);
    if (cx < 0.0 || cy < 0.0 || cx >= PSK_GRID_N || cy >= PSK_GRID_N) return -1;
    return (int)cy * PSK_GRID_N + (int)cx;
}

/** @brief Buckets the picture's vertices.
 * @param P The picture.
 * @param G Receives the grid. */
static void psk_grid_build(const psk_pic* P, psk_grid* G) {
    memset(G->head, 0xFF, sizeof G->head);
    for (int i = 0; i < P->nv; i++) {
        const int c = psk_cell(P->x[i], P->y[i]);
        if (c < 0) { G->next[i] = -1; continue; }
        G->next[i] = G->head[c];
        G->head[c] = i;
    }
}

/** @brief The picture vertex within PSK_SNAP of a screen point, -1 when none.
 * @param P The picture.
 * @param G Its grid.
 * @param x The point's X.
 * @param y The point's Y.
 * @return The nearest such vertex. */
static int psk_near(const psk_pic* P, const psk_grid* G, double x, double y) {
    int best = -1;
    double bd = PSK_SNAP * PSK_SNAP;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            const int c = psk_cell(x + dx * PSK_CELL, y + dy * PSK_CELL);
            for (int i = c < 0 ? -1 : G->head[c]; i >= 0; i = G->next[i]) {
                const double ex = P->x[i] - x, ey = P->y[i] - y, d = ex * ex + ey * ey;
                if (d <= bd) { bd = d; best = i; }
            }
        }
    return best;
}

/** @brief Projects a world direction under `R` onto the picture's screen.
 * @param R The rotation.
 * @param p The world point.
 * @param x Receives X.
 * @param y Receives Y.
 * @return 1 when the direction lies inside the PSK_NEAR cone, else 0. */
static int psk_project(const double R[3][3], const double p[3], double* x, double* y) {
    double c[3];
    for (int i = 0; i < 3; i++) c[i] = R[i][0] * p[0] + R[i][1] * p[1] + R[i][2] * p[2];
    const double n = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (n <= 0.0 || c[2] < PSK_NEAR * n) return 0;
    *x = PSK_CX + PSK_FX * c[0] / c[2];
    *y = PSK_CY + PSK_FY * c[1] / c[2];
    return 1;
}

/** @brief The unit view ray through a picture point.
 * @param x The point's X.
 * @param y The point's Y.
 * @param r Receives the ray. */
static void psk_ray(double x, double y, double r[3]) {
    r[0] = (x - PSK_CX) / PSK_FX; r[1] = (y - PSK_CY) / PSK_FY; r[2] = 1.0;
    const double n = sqrt(r[0] * r[0] + r[1] * r[1] + 1.0);
    for (int i = 0; i < 3; i++) r[i] /= n;
}

/** @brief Applies one Jacobi rotation that zeroes A[p][r], accumulating it into V.
 * @param A The symmetric matrix.
 * @param V The accumulated rotations.
 * @param p The pivot row.
 * @param r The pivot column, above p. */
static void psk_jacobi(double A[4][4], double V[4][4], int p, int r) {
    if (fabs(A[p][r]) < 1e-15) return;
    const double th = (A[r][r] - A[p][p]) / (2.0 * A[p][r]);
    const double t = (th >= 0.0 ? 1.0 : -1.0) / (fabs(th) + sqrt(th * th + 1.0));
    const double c = 1.0 / sqrt(t * t + 1.0), s = t * c;
    for (int k = 0; k < 4; k++) {
        const double akp = A[k][p], akr = A[k][r];
        A[k][p] = c * akp - s * akr; A[k][r] = s * akp + c * akr;
    }
    for (int k = 0; k < 4; k++) {
        const double apk = A[p][k], ark = A[r][k];
        A[p][k] = c * apk - s * ark; A[r][k] = s * apk + c * ark;
        const double vkp = V[k][p], vkr = V[k][r];
        V[k][p] = c * vkp - s * vkr; V[k][r] = s * vkp + c * vkr;
    }
}

/** @brief The eigenvector of a symmetric 4x4 matrix's largest eigenvalue, by cyclic Jacobi rotations.
 * @param A The matrix; destroyed.
 * @param q Receives the unit eigenvector. */
static void psk_eig4(double A[4][4], double q[4]) {
    double V[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    for (int sweep = 0; sweep < 32; sweep++)
        for (int p = 0; p < 3; p++)
            for (int r = p + 1; r < 4; r++) psk_jacobi(A, V, p, r);
    int m = 0;
    for (int i = 1; i < 4; i++) if (A[i][i] > A[m][m]) m = i;
    for (int i = 0; i < 4; i++) q[i] = V[i][m];
}

/** @brief The rotation taking world directions onto view rays in the least-squares sense (Horn's quaternion method).
 * @param d The unit world directions.
 * @param r The unit view rays they map to.
 * @param n The pair count.
 * @param R Receives the rotation. */
static void psk_horn(const double (*d)[3], const double (*r)[3], int n, double R[3][3]) {
    double S[3][3] = { { 0 } };
    for (int k = 0; k < n; k++)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) S[i][j] += d[k][i] * r[k][j];
    double N[4][4] = {
        { S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0] },
        { S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2] },
        { S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
        { S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2] } };
    double q[4];
    psk_eig4(N, q);
    const double a = q[0], b = q[1], c = q[2], e = q[3];
    R[0][0] = a*a + b*b - c*c - e*e; R[0][1] = 2*(b*c - a*e);         R[0][2] = 2*(b*e + a*c);
    R[1][0] = 2*(b*c + a*e);         R[1][1] = a*a - b*b + c*c - e*e; R[1][2] = 2*(c*e - a*b);
    R[2][0] = 2*(b*e - a*c);         R[2][1] = 2*(c*e + a*b);         R[2][2] = a*a - b*b - c*c + e*e;
}

/** @brief The unit direction of a world point.
 * @param p The point.
 * @param d Receives the direction. */
static void psk_unit(const double p[3], double d[3]) {
    const double n = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    for (int i = 0; i < 3; i++) d[i] = n > 0.0 ? p[i] / n : 0.0;
}

/* The search's scratch: per picture vertex the sky corner a rotation places
 * nearest, for the score and the refinement. */
typedef struct {
    int    hit[PSK_MAX_PIC_V];        /* sky face * 3 + corner, -1 for none */
    double d[PSK_MAX_PIC_V][3], r[PSK_MAX_PIC_V][3];
} psk_match;

/** @brief Scores a rotation: the picture vertices that some projected sky corner lands within PSK_SNAP of.
 * @param P The picture.
 * @param G Its grid.
 * @param S The sky's faces.
 * @param R The rotation.
 * @param M Receives each matched vertex's sky corner.
 * @return The matched vertex count. */
static int psk_score(const psk_pic* P, const psk_grid* G, const psk_sky* S, const double R[3][3], psk_match* M) {
    for (int i = 0; i < P->nv; i++) M->hit[i] = -1;
    int score = 0;
    for (int f = 0; f < S->n; f++)
        for (int k = 0; k < 3; k++) {
            double x, y;
            if (!psk_project(R, S->p[f][k], &x, &y)) continue;
            const int v = psk_near(P, G, x, y);
            if (v < 0) continue;
            if (M->hit[v] < 0) score++;
            M->hit[v] = f * 3 + k;
        }
    return score;
}

/** @brief The rotation one picture face implies when its corners are a sky face's corners in the given order.
 * @param P The picture.
 * @param pf The picture face.
 * @param S The sky's faces.
 * @param sf The sky face.
 * @param ord The sky corner each picture corner takes.
 * @param R Receives the rotation. */
static void psk_hypothesis(const psk_pic* P, int pf, const psk_sky* S, int sf, const int ord[3], double R[3][3]) {
    double d[3][3], r[3][3];
    for (int i = 0; i < 3; i++) {
        psk_unit(S->p[sf][ord[i]], d[i]);
        psk_ray(P->x[P->f[pf][i]], P->y[P->f[pf][i]], r[i]);
    }
    psk_horn((const double (*)[3])d, (const double (*)[3])r, 3, R);
}

/* The six corner orders a picture face's colours may take a sky face's in. */
static const int k_psk_ord[6][3] = { { 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 0, 2, 1 }, { 2, 1, 0 }, { 1, 0, 2 } };

/** @brief The corner order in which a sky face carries a picture face's colours, -1 when none does.
 * @param P The picture.
 * @param pf The picture face.
 * @param S The sky's faces.
 * @param sf The sky face.
 * @param from The first order to try.
 * @return The order's index into k_psk_ord. */
static int psk_order(const psk_pic* P, int pf, const psk_sky* S, int sf, int from) {
    for (int o = from; o < 6; o++)
        if (S->c[sf][k_psk_ord[o][0]] == P->c[pf][0] && S->c[sf][k_psk_ord[o][1]] == P->c[pf][1] &&
            S->c[sf][k_psk_ord[o][2]] == P->c[pf][2]) return o;
    return -1;
}

/** @brief Counts the sky faces whose colours a picture face can take, 0 for a face of one colour.
 * @param P The picture.
 * @param pf The picture face.
 * @param S The sky's faces.
 * @return The candidate count. */
static int psk_candidates(const psk_pic* P, int pf, const psk_sky* S) {
    if (P->c[pf][0] == P->c[pf][1] && P->c[pf][1] == P->c[pf][2]) return 0;
    int n = 0;
    for (int sf = 0; sf < S->n; sf++) n += psk_order(P, pf, S, sf, 0) >= 0;
    return n;
}

/* A picture face and its candidate count, for the fewest-first order. */
typedef struct { int pf, n; } psk_rank;

/** @brief Orders two ranked faces by candidate count, then by index.
 * @param a The first.
 * @param b The second.
 * @return Negative, zero or positive as qsort expects. */
static int psk_rank_cmp(const void* a, const void* b) {
    const psk_rank *x = (const psk_rank*)a, *y = (const psk_rank*)b;
    return x->n != y->n ? (x->n < y->n ? -1 : 1) : (x->pf - y->pf);
}

/** @brief Tries every candidate of one picture face, keeping the best-scoring rotation.
 * @param P The picture.
 * @param G Its grid.
 * @param S The sky's faces.
 * @param pf The picture face.
 * @param M Scratch for the score.
 * @param best In/out: the best score so far.
 * @param R In/out: the best rotation so far.
 * @return 1 when a rotation places every picture vertex, else 0. */
static int psk_try_face(const psk_pic* P, const psk_grid* G, const psk_sky* S, int pf, psk_match* M,
                        int* best, double R[3][3]) {
    for (int sf = 0; sf < S->n; sf++)
        for (int o = psk_order(P, pf, S, sf, 0); o >= 0; o = psk_order(P, pf, S, sf, o + 1)) {
            double H[3][3];
            psk_hypothesis(P, pf, S, sf, k_psk_ord[o], H);
            const int sc = psk_score(P, G, S, H, M);
            if (sc > *best) { *best = sc; memcpy(R, H, sizeof H); }
            if (sc == P->nv) return 1;
        }
    return 0;
}

/** @brief Recovers the rotation that projects the sky onto the picture.
 *
 * Accepts the best hypothesis when it places at least nine tenths of the
 * picture's vertices, then refines it by Horn over every matched pair.
 * @param P The picture.
 * @param G Its grid.
 * @param S The sky's faces.
 * @param R Receives the rotation.
 * @return The matched vertex count of the refined rotation, 0 when none is accepted. */
static int psk_register(const psk_pic* P, const psk_grid* G, const psk_sky* S, double R[3][3]) {
    static psk_rank rank[PSK_MAX_PIC_F];
    static psk_match M;
    int nr = 0, best = 0;
    for (int pf = 0; pf < P->nf; pf++) {
        const int n = psk_candidates(P, pf, S);
        if (n > 0) { rank[nr].pf = pf; rank[nr].n = n; nr++; }
    }
    qsort(rank, (size_t)nr, sizeof rank[0], psk_rank_cmp);
    for (int i = 0; i < nr && !psk_try_face(P, G, S, rank[i].pf, &M, &best, R); i++) {}
    if (best * 10 < P->nv * 9) return 0;
    psk_score(P, G, S, R, &M);
    int n = 0;
    for (int v = 0; v < P->nv; v++) {
        if (M.hit[v] < 0) continue;
        psk_unit(S->p[M.hit[v] / 3][M.hit[v] % 3], M.d[n]);
        psk_ray(P->x[v], P->y[v], M.r[n]);
        n++;
    }
    psk_horn((const double (*)[3])M.d, (const double (*)[3])M.r, n, R);
    return psk_score(P, G, S, R, &M);
}

/* ---- the extension -------------------------------------------------------- */

typedef struct {
    uint32_t        rec, mesh;
    int             level, used;
    uint64_t        hash;
    portal_sky_tri* tri;
    int             n, cap;
} psk_entry;

static psk_entry s_cache[PSK_CACHE];
static unsigned  s_cache_next = 0;

/* What the extension's face receiver needs. */
typedef struct {
    const psk_pic*  P;
    const psk_grid* G;
    psk_entry*      E;
} psk_build;

/** @brief Projects one corner of the extension, taking a picture vertex's position when one lies within PSK_SNAP.
 * @param B The build.
 * @param c The camera-space corner.
 * @param x Receives X.
 * @param y Receives Y. */
static void psk_place(const psk_build* B, const ngte_cvert* c, float* x, float* y) {
    const double px = PSK_CX + PSK_FX * c->m1 / c->m3, py = PSK_CY + PSK_FY * c->m2 / c->m3;
    const int v = psk_near(B->P, B->G, px, py);
    *x = v >= 0 ? B->P->x[v] : (float)px;
    *y = v >= 0 ? B->P->y[v] : (float)py;
}

/** @brief Appends one projected triangle to the entry.
 * @param B The build.
 * @param a The first camera-space corner.
 * @param b The second.
 * @param c The third. */
static void psk_append(const psk_build* B, const ngte_cvert* a, const ngte_cvert* b, const ngte_cvert* c) {
    psk_entry* E = B->E;
    if (E->n == E->cap) {
        const int cap = E->cap ? E->cap * 2 : 1024;
        portal_sky_tri* t = (portal_sky_tri*)realloc(E->tri, sizeof(portal_sky_tri) * (size_t)cap);
        if (!t) return;
        E->tri = t; E->cap = cap;
    }
    portal_sky_tri* T = &E->tri[E->n++];
    const ngte_cvert* v[3] = { a, b, c };
    for (int i = 0; i < 3; i++) { psk_place(B, v[i], &T->x[i], &T->y[i]); T->rgb[i] = v[i]->rgb; }
}

/** @brief Clips one camera-space sky face to the PSK_NEAR cone's plane and appends what remains.
 *
 * The corners are scaled to unit length first, which leaves each projected
 * edge on the same screen line, so the plane m3 = PSK_NEAR bounds every
 * corner's screen offset to 1 / PSK_NEAR of the scales.
 * @param a The first corner.
 * @param b The second.
 * @param c The third.
 * @param gouraud Unused: a flat face carries its one colour on every corner.
 * @param ctx The psk_build. */
static void psk_extend_face(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud, void* ctx) {
    (void)gouraud;
    ngte_cvert in[3] = { a, b, c }, out[4];
    for (int i = 0; i < 3; i++) {
        const double n = sqrt(in[i].m1 * in[i].m1 + in[i].m2 * in[i].m2 + in[i].m3 * in[i].m3);
        if (n <= 0.0) return;
        in[i].m1 /= n; in[i].m2 /= n; in[i].m3 /= n;
    }
    int m = 0;
    for (int i = 0; i < 3; i++) {
        const ngte_cvert cur = in[i], nxt = in[(i + 1) % 3];
        const int ci = cur.m3 >= PSK_NEAR, ni = nxt.m3 >= PSK_NEAR;
        if (ci) out[m++] = cur;
        if (ci != ni && m < 4) out[m++] = ngte_clip_lerp(cur, nxt, PSK_NEAR);
    }
    const psk_build* B = (const psk_build*)ctx;
    if (m >= 3) psk_append(B, &out[0], &out[1], &out[2]);
    if (m == 4) psk_append(B, &out[0], &out[2], &out[3]);
}

/** @brief Registers a portal's picture against its destination's sky and builds the extension into `E`.
 * @param P The picture, already read.
 * @param L The destination's sky.
 * @param E Receives the triangles. */
static void psk_build_entry(const psk_pic* P, const psk_level* L, psk_entry* E) {
    static psk_grid G;
    static psk_sky S;
    static const double I[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    psk_grid_build(P, &G);
    S.n = 0;
    psk_decode_sky(L, I, psk_collect, &S);
    double R[3][3];
    const int matched = psk_register(P, &G, &S, R);
    if (matched) {
        const psk_build B = { P, &G, E };
        psk_decode_sky(L, R, psk_extend_face, (void*)&B);
    }
    OPL_INFO_F("portal", "picture of level %d: %d of %d vertices registered, %d sky faces, %d extension triangles",
               E->level, matched, P->nv, S.n, E->n);
}

int portal_sky_s23_extension(CPUState* cpu, uint32_t rec, const portal_sky_tri** tris) {
    static psk_pic P;
    *tris = NULL;
    if (!portal_game_s23.level_slots || !psk_read_picture(cpu, rec, &P)) return 0;
    const int level = (int)cpu->read_byte(rec + 0x42);
    const uint32_t mesh = cpu->read_word(rec + 0x3C);
    for (int i = 0; i < PSK_CACHE; i++) {
        const psk_entry* E = &s_cache[i];
        if (E->used && E->rec == rec && E->mesh == mesh && E->level == level && E->hash == P.hash) {
            *tris = E->tri;
            return E->n;
        }
    }
    const psk_level* L = psk_level_sky(cpu, level);
    psk_entry* E = &s_cache[s_cache_next++ % PSK_CACHE];
    E->rec = rec; E->mesh = mesh; E->level = level; E->hash = P.hash; E->used = 1; E->n = 0;
    if (L->state > 0) psk_build_entry(&P, L, E);
    *tris = E->tri;
    return E->n;
}
