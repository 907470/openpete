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
//
// The search's cost. A picture whose best rotation misses a vertex tries every
// hypothesis, a few thousand of them. Each is scored from the picture's side:
// a picture vertex's view ray, rotated back into the world, finds the sky
// corners whose directions lie near it in a hash of the sky's corner
// directions, and each is projected and snapped as the forward score does, so
// the count is the forward count. The scan stops once the vertices left cannot
// lift the count to the larger of the best so far plus one and the acceptance
// floor; such a hypothesis can neither replace the best nor be accepted, so the
// chosen rotation is the exhaustive search's.
//
// Which sky. A picture is registered against its destination's sky first. A
// picture the destination's sky does not register was baked from another
// level's sky (Spyro 3's Mushroom Speedway and Sheila's Alp pictures are
// Sunny Villa's sky), so the skies of the other destinations in the same
// record array are tried next under the same acceptance rule. The extension
// takes the one fallback sky that registers; when two or more do, the choice
// is ambiguous and the picture keeps no extension.
//
// Where the build runs. A registration takes milliseconds to a hundred, so it
// never runs on the game fiber unasked: the portal pass hands each of the
// level's records to portal_sky_s23_prefetch, which snapshots the picture,
// reads the destination's sky and queues the build for one background thread.
// A preview that needs an extension whose build has not finished waits for it,
// building it on the calling thread when the worker has not started it, so
// the triangles a present draws never depend on how far the worker got.

#include "renderer/portal_sky_s23.h"
#include "renderer/portal_spyro23.h"   /* portal_game_s23 */
#include "renderer/cyclorama.h"        /* cyclorama_decode_sector */
#include "renderer/wad_subfile.h"
#include "openpete_disc_trace.h"       /* OPENPETE_DISC_TAG_TERRAIN */
#include "openpete_log.h"
#include "openpete_fnv1a64.h"
#include "openpete_crash.h"
#include "openpete_time.h"
#include "openpete_thread_roles.h"

#include <SDL3/SDL.h>

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
#define PSK_CACHE  32
#define PSK_MAX_PIC_V 1024
#define PSK_MAX_PIC_F 4096
#define PSK_MAX_SKY_F 16384
#define PSK_GRID_X0 (-1024)        /* the picture-vertex grid: 4-pixel cells over */
#define PSK_GRID_Y0 (-1024)        /* [-1024, 1024) on both axes */
#define PSK_GRID_N  512
#define PSK_CELL    4
#define PSK_DIR_R   0.0125         /* the chord about a view ray the search takes sky corners from */
#define PSK_DIR_H   (2.0 * PSK_DIR_R) /* the sky-direction hash's cell edge, in unit-vector coordinates */
#define PSK_DIR_HASH 16384         /* its bucket count, a power of two */
#define PSK_MAX_CORNERS (PSK_MAX_SKY_F * 3)
#define PSK_MAX_SKIES 16           /* the destination plus the record array's other destinations */

/* ---- the destination sky, per level ------------------------------------- */

typedef struct {
    int      level;
    int      state;        /* 0 unread, 1 ready, -1 unavailable */
    uint8_t* buf;          /* the sky block's payload */
    uint32_t len;
} psk_level;

static psk_level s_lv[PSK_LEVELS] OP_GUARDED_BY(sim);   /* thread-roles: filled by psk_level_sky on sim; a build reads an entry through E->L[] */

/** @brief Reads a little-endian word of a host buffer, 0 when it does not fit.
 * @param b The buffer.
 * @param n Its length.
 * @param o The offset.
 * @return The word. */
static uint32_t psk_w(const uint8_t* b, uint32_t n, uint32_t o) {
    if (o > n || n - o < 4u) return 0u;
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

uint32_t portal_sky_s23_walk(const uint8_t* b, uint32_t n, uint32_t* len) {
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
static const psk_level* psk_level_sky(CPUState* cpu, int level) OP_REQUIRES_SHARED(sim) {
    psk_level* free_slot = NULL;
    for (int i = 0; i < PSK_LEVELS; i++) {
        if (s_lv[i].state && s_lv[i].level == level) return &s_lv[i];
        if (!s_lv[i].state && !free_slot) free_slot = &s_lv[i];
    }
    static const psk_level s_none = { 0, -1, NULL, 0 };
    if (!free_slot || !portal_game_s23.level_slots || level < 0 || level > 255) return &s_none;
    psk_level* L = free_slot;
    L->level = level;
    L->state = -1;
    const int slot = (int)cpu->read_byte(portal_game_s23.level_slots + (uint32_t)level);
    const int e = portal_game_s23.level_wad_base + 2 * slot;
    uint32_t n = 0, len = 0;
    uint8_t* sub = wad_subfile_load(e, 1, OPENPETE_DISC_TAG_TERRAIN, &n, "portal");
    const uint32_t at = sub ? portal_sky_s23_walk(sub, n, &len) : 0u;
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
 * @param D The building thread's decode scratch.
 * @param L The level's sky.
 * @param R The rotation, unit scale.
 * @param fn Receives each face, during this call only.
 * @param ctx Passed to `fn`. */
static void psk_decode_sky(cyclorama_sector_buf* D, const psk_level* L, const double R[3][3],
                           cyclorama_tri_fn fn OP_SYNC_FN, void* ctx) {
    const cyclorama_src src = { NULL, L->buf, 0u, L->len };
    const cyclorama_fade none[2] = { { 0u, 0 }, { 0u, 0 } };
    const uint32_t n = psk_w(L->buf, L->len, 4u);
    for (uint32_t i = 0; i < n && i < 1024u; i++) {
        const uint32_t off = psk_w(L->buf, L->len, 8u + i * 4u);
        if (off && off < L->len) cyclorama_decode_sector(D, &src, off, R, 1.0, none, fn, ctx);
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

/** @brief Whether every off-diagonal element of a symmetric 4x4 matrix is below psk_jacobi's skip bound.
 * @param A The matrix.
 * @return 1 when a further sweep would rotate nothing, else 0. */
static int psk_jacobi_done(const double A[4][4]) {
    for (int p = 0; p < 3; p++)
        for (int r = p + 1; r < 4; r++)
            if (fabs(A[p][r]) >= 1e-15) return 0;
    return 1;
}

/** @brief The eigenvector of a symmetric 4x4 matrix's largest eigenvalue, by cyclic Jacobi rotations.
 *
 * At most 32 sweeps; the sweeps stop once every off-diagonal element is below
 * the bound under which psk_jacobi rotates nothing, since the rest would leave
 * A and V unchanged.
 * @param A The matrix; destroyed.
 * @param q Receives the unit eigenvector. */
static void psk_eig4(double A[4][4], double q[4]) {
    double V[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    for (int sweep = 0; sweep < 32 && !psk_jacobi_done(A); sweep++)
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

/* The sky's faces keyed for the search. A sky face's colours sorted ascending,
 * and its index. */
typedef struct {
    uint32_t c[3];
    int      sf;
} psk_tkey;

/* The search's index of the sky. The distinct corner positions, each with its
 * unit direction and hash cell, chained per bucket of the direction hash; and
 * the faces by sorted colour triple, then index, so the faces carrying a
 * picture face's colours in some order are one ascending run. */
typedef struct {
    int      n;
    int32_t  corner[PSK_MAX_CORNERS];   /* sky face * 3 + corner */
    double   u[PSK_MAX_CORNERS][3];
    int32_t  cell[PSK_MAX_CORNERS][3];
    int32_t  next[PSK_MAX_CORNERS];
    int32_t  head[PSK_DIR_HASH];
    int      nt;
    psk_tkey tri[PSK_MAX_SKY_F];
} psk_index;

/** @brief The direction-hash cell holding a unit direction.
 * @param u The direction.
 * @param c Receives the cell's three coordinates. */
static void psk_dir_cell(const double u[3], int32_t c[3]) {
    for (int i = 0; i < 3; i++) c[i] = (int32_t)floor((u[i] + 1.0) / PSK_DIR_H);
}

/** @brief The direction-hash bucket of a cell.
 * @param c The cell.
 * @return The bucket. */
static int psk_dir_slot(const int32_t c[3]) {
    const uint32_t h = (uint32_t)c[0] * 73856093u ^ (uint32_t)c[1] * 19349663u ^ (uint32_t)c[2] * 83492791u;
    return (int)(h & (PSK_DIR_HASH - 1));
}

/** @brief Sorts three colours ascending.
 * @param in The colours.
 * @param out Receives them sorted. */
static void psk_sort3(const uint32_t in[3], uint32_t out[3]) {
    uint32_t a = in[0], b = in[1], c = in[2], t;
    if (a > b) { t = a; a = b; b = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (a > b) { t = a; a = b; b = t; }
    out[0] = a; out[1] = b; out[2] = c;
}

/** @brief Orders two keyed sky faces by sorted colours, then by index.
 * @param a The first.
 * @param b The second.
 * @return Negative, zero or positive as qsort expects. */
static int psk_tkey_cmp(const void* a, const void* b) {
    const psk_tkey *x = (const psk_tkey*)a, *y = (const psk_tkey*)b;
    for (int i = 0; i < 3; i++)
        if (x->c[i] != y->c[i]) return x->c[i] < y->c[i] ? -1 : 1;
    return x->sf - y->sf;
}

/** @brief Builds the search's index of the sky's faces.
 *
 * A corner whose world position equals an indexed corner's bit for bit
 * projects to the same point under every rotation and is not indexed again.
 * @param S The sky's faces.
 * @param X Receives the index. */
static void psk_index_build(const psk_sky* S, psk_index* X) {
    memset(X->head, 0xFF, sizeof X->head);
    X->n = 0;
    for (int f = 0; f < S->n; f++)
        for (int k = 0; k < 3; k++) {
            double u[3];
            int32_t c[3];
            psk_unit(S->p[f][k], u);
            psk_dir_cell(u, c);
            const int b = psk_dir_slot(c);
            int dup = 0;
            for (int i = X->head[b]; i >= 0 && !dup; i = X->next[i])
                dup = !memcmp(S->p[X->corner[i] / 3][X->corner[i] % 3], S->p[f][k], sizeof S->p[f][k]);
            if (dup) continue;
            const int i = X->n++;
            X->corner[i] = f * 3 + k;
            memcpy(X->u[i], u, sizeof u);
            memcpy(X->cell[i], c, sizeof c);
            X->next[i] = X->head[b];
            X->head[b] = i;
        }
    X->nt = S->n;
    for (int f = 0; f < S->n; f++) { psk_sort3(S->c[f], X->tri[f].c); X->tri[f].sf = f; }
    qsort(X->tri, (size_t)X->nt, sizeof X->tri[0], psk_tkey_cmp);
}

/** @brief The run of keyed sky faces whose colours are a picture face's colours in some order.
 * @param X The index.
 * @param P The picture.
 * @param pf The picture face.
 * @param lo Receives the run's first entry.
 * @return The run's length. */
static int psk_tri_run(const psk_index* X, const psk_pic* P, int pf, int* lo) {
    psk_tkey k;
    psk_sort3(P->c[pf], k.c);
    k.sf = -1;
    int a = 0, b = X->nt;
    while (a < b) { const int m = (a + b) / 2; if (psk_tkey_cmp(&X->tri[m], &k) < 0) a = m + 1; else b = m; }
    int e = a;
    while (e < X->nt && !memcmp(X->tri[e].c, k.c, sizeof k.c)) e++;
    *lo = a;
    return e - a;
}

/* One picture-vertex query of the search: the vertex, its ray rotated back
 * into the world, and what the test reads. */
typedef struct {
    const psk_pic*   P;
    const psk_grid*  G;
    const psk_sky*   S;
    const psk_index* X;
    const double   (*R)[3];
    double           w[3];
    int              v;
} psk_query;

/** @brief Whether a corner indexed in one direction-hash cell projects to a point whose nearest picture vertex is the query's.
 * @param Q The query.
 * @param c The cell.
 * @return 1 when one does, else 0. */
static int psk_hit_cell(const psk_query* Q, const int32_t c[3]) {
    const psk_index* X = Q->X;
    for (int i = X->head[psk_dir_slot(c)]; i >= 0; i = X->next[i]) {
        if (X->cell[i][0] != c[0] || X->cell[i][1] != c[1] || X->cell[i][2] != c[2]) continue;
        const double ex = X->u[i][0] - Q->w[0], ey = X->u[i][1] - Q->w[1], ez = X->u[i][2] - Q->w[2];
        if (ex * ex + ey * ey + ez * ez > PSK_DIR_R * PSK_DIR_R) continue;
        double x, y;
        if (psk_project(Q->R, Q->S->p[X->corner[i] / 3][X->corner[i] % 3], &x, &y) &&
            psk_near(Q->P, Q->G, x, y) == Q->v) return 1;
    }
    return 0;
}

/** @brief Whether some sky corner projects under `R` to a point whose nearest picture vertex is `v`.
 *
 * A corner within PSK_SNAP pixels of `v` lies within PSK_SNAP / PSK_FY radians
 * (0.0070) of `v`'s view ray, since the inverse projection shrinks distances
 * by at least the smaller scale. The corners searched are those whose
 * direction lies within the chord PSK_DIR_R of the ray rotated back by `R`;
 * the cell edge is twice that chord, so they lie in the one or two cells per
 * axis that the chord's interval spans.
 * @param P The picture.
 * @param G Its grid.
 * @param S The sky's faces.
 * @param X The index.
 * @param R The rotation.
 * @param v The picture vertex.
 * @return 1 when one does, else 0. */
static int psk_hit(const psk_pic* P, const psk_grid* G, const psk_sky* S, const psk_index* X,
                   const double R[3][3], int v) {
    psk_query Q = { P, G, S, X, R, { 0, 0, 0 }, v };
    double r[3];
    psk_ray(P->x[v], P->y[v], r);
    for (int j = 0; j < 3; j++) Q.w[j] = R[0][j] * r[0] + R[1][j] * r[1] + R[2][j] * r[2];
    psk_unit(Q.w, Q.w);
    double wl[3], wh[3];
    for (int j = 0; j < 3; j++) { wl[j] = Q.w[j] - PSK_DIR_R; wh[j] = Q.w[j] + PSK_DIR_R; }
    int32_t lo[3], hi[3];
    psk_dir_cell(wl, lo);
    psk_dir_cell(wh, hi);
    for (int32_t cz = lo[2]; cz <= hi[2]; cz++)
        for (int32_t cy = lo[1]; cy <= hi[1]; cy++)
            for (int32_t cx = lo[0]; cx <= hi[0]; cx++) {
                const int32_t c[3] = { cx, cy, cz };
                if (psk_hit_cell(&Q, c)) return 1;
            }
    return 0;
}

/** @brief Scores a rotation as psk_score counts, giving up once the count cannot reach `need`.
 * @param P The picture.
 * @param G Its grid.
 * @param S The sky's faces.
 * @param X The index.
 * @param R The rotation.
 * @param need The count below which the score does not matter.
 * @return The matched vertex count, or -1 when it is below `need`. */
static int psk_score_from(const psk_pic* P, const psk_grid* G, const psk_sky* S, const psk_index* X,
                          const double R[3][3], int need) {
    int score = 0;
    for (int v = 0; v < P->nv; v++) {
        if (score + (P->nv - v) < need) return -1;
        score += psk_hit(P, G, S, X, R, v);
    }
    return score >= need ? score : -1;
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
 * @param X The index.
 * @return The candidate count. */
static int psk_candidates(const psk_pic* P, int pf, const psk_index* X) {
    if (P->c[pf][0] == P->c[pf][1] && P->c[pf][1] == P->c[pf][2]) return 0;
    int lo;
    return psk_tri_run(X, P, pf, &lo);
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

/** @brief Tries every candidate of one picture face in sky-face order, keeping the best-scoring rotation.
 *
 * A hypothesis is scored only as far as it can still beat the best so far and
 * reach `floor`.
 * @param P The picture.
 * @param G Its grid.
 * @param S The sky's faces.
 * @param X The index.
 * @param pf The picture face.
 * @param floor The least count the registration accepts.
 * @param best In/out: the best score so far.
 * @param R In/out: the best rotation so far.
 * @return 1 when a rotation places every picture vertex, else 0. */
static int psk_try_face(const psk_pic* P, const psk_grid* G, const psk_sky* S, const psk_index* X, int pf,
                        int floor, int* best, double R[3][3]) {
    int lo;
    const int n = psk_tri_run(X, P, pf, &lo);
    for (int t = lo; t < lo + n; t++) {
        const int sf = X->tri[t].sf;
        for (int o = psk_order(P, pf, S, sf, 0); o >= 0; o = psk_order(P, pf, S, sf, o + 1)) {
            double H[3][3];
            psk_hypothesis(P, pf, S, sf, k_psk_ord[o], H);
            const int sc = psk_score_from(P, G, S, X, H, *best + 1 > floor ? *best + 1 : floor);
            if (sc > *best) { *best = sc; memcpy(R, H, sizeof H); }
            if (sc == P->nv) return 1;
        }
    }
    return 0;
}

/* One builder's scratch: the worker thread and the game fiber each own one,
 * so a build on either never shares memory with a build on the other or
 * with the game fiber's sky feed, whose sector decode has its own buffer. */
typedef struct {
    cyclorama_sector_buf D;
    psk_grid  G;
    psk_sky   S;
    psk_index X;
    psk_match M;
    psk_rank  rank[PSK_MAX_PIC_F];
} psk_ctx;

/** @brief Recovers the rotation that projects the sky onto the picture.
 *
 * Accepts the best hypothesis when it places at least nine tenths of the
 * picture's vertices, then refines it by Horn over every matched pair.
 * @param C The builder's scratch; its grid and sky faces are already built.
 * @param P The picture.
 * @param prune The least count a hypothesis is scored to the end for: the
 *        acceptance floor for a build, 1 for the census, whose best count is
 *        then the exhaustive one.
 * @param R Receives the rotation.
 * @param best_out Receives the best hypothesis's count before refinement.
 * @return The matched vertex count of the refined rotation, 0 when none is accepted. */
static int psk_register(psk_ctx* C, const psk_pic* P, int prune, double R[3][3], int* best_out) {
    const psk_grid* G = &C->G;
    const psk_sky* S = &C->S;
    psk_match* M = &C->M;
    psk_index_build(S, &C->X);
    const int floor = prune;
    int nr = 0, best = 0;
    for (int pf = 0; pf < P->nf; pf++) {
        const int n = psk_candidates(P, pf, &C->X);
        if (n > 0) { C->rank[nr].pf = pf; C->rank[nr].n = n; nr++; }
    }
    qsort(C->rank, (size_t)nr, sizeof C->rank[0], psk_rank_cmp);
    for (int i = 0; i < nr && !psk_try_face(P, G, S, &C->X, C->rank[i].pf, floor, &best, R); i++) {}
    *best_out = best;
    if (best * 10 < P->nv * 9) return 0;
    psk_score(P, G, S, R, M);
    int n = 0;
    for (int v = 0; v < P->nv; v++) {
        if (M->hit[v] < 0) continue;
        psk_unit(S->p[M->hit[v] / 3][M->hit[v] % 3], M->d[n]);
        psk_ray(P->x[v], P->y[v], M->r[n]);
        n++;
    }
    psk_horn((const double (*)[3])M->d, (const double (*)[3])M->r, n, R);
    return psk_score(P, G, S, R, M);
}

/* ---- the extension -------------------------------------------------------- */

/* An entry's build state. The game fiber moves an entry from FREE to QUEUED
 * (or straight to DONE when the sky is unreadable); whichever thread takes it
 * moves it from QUEUED to RUNNING and then DONE, all under s_mu. */
enum { PSK_FREE, PSK_QUEUED, PSK_RUNNING, PSK_DONE };

/* One record's extension. The key, the picture snapshot and the sky are
 * written by the game fiber before the entry is queued; the triangles by the
 * builder while it runs, and read only once the entry is DONE. */
typedef struct {
    uint32_t         rec, mesh;
    int              level, state;
    unsigned         seq;              /* queue order */
    uint64_t         hash;
    const psk_level* L[PSK_MAX_SKIES]; /* the destination's sky, then the fallbacks */
    int              nl;
    psk_pic          pic;
    portal_sky_tri*  tri;
    int              n, cap;
} psk_entry;

/* The entry table's lock as the analysis sees it: psk_lock and psk_unlock
 * take and drop s_mu and carry this capability. */
static const op_lock_cap_t s_psk_lock;
static psk_entry      s_cache[PSK_CACHE] OP_GUARDED_BY_CAP(s_psk_lock);  /* thread-roles: under s_mu */
static unsigned       s_cache_next OP_GUARDED_BY_CAP(s_psk_lock) = 0;    /* thread-roles: under s_mu */
static unsigned       s_seq OP_GUARDED_BY_CAP(s_psk_lock) = 0;           /* thread-roles: under s_mu */
static SDL_Mutex*     s_mu OP_SEALED(psk);        /* thread-roles: created before the worker starts */
static SDL_Condition* s_cv_work OP_SEALED(psk);   /* the worker waits for a queued entry; thread-roles: created before the worker starts */
static SDL_Condition* s_cv_done OP_SEALED(psk);   /* a waiter waits for an entry to finish; thread-roles: created before the worker starts */
static int            s_sched OP_GUARDED_BY(sim) = 0;   /* 1 once psk_sched_init ran; thread-roles: psk_sched_init runs on sim */
OP_SEAL_DEFINE(psk);

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

/** @brief Whether OPENPETE_PORTAL3_SKY_CENSUS registers every picture against every candidate sky and logs each count.
 *
 * Latched on the first call; set, non-empty and not starting with '0'
 * enables it. The choice of sky is the same with it on.
 * @return 1 when enabled, else 0. */
static int psk_census_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL3_SKY_CENSUS"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Registers the picture against one candidate sky, decoding the sky into the scratch first.
 * @param C The building thread's scratch; its picture grid is built.
 * @param P The picture.
 * @param level The picture's destination, for the census line.
 * @param L The candidate sky.
 * @param R Receives the rotation.
 * @return The registered vertex count, 0 when the sky is not accepted. */
static int psk_try_sky(psk_ctx* C, const psk_pic* P, int level, const psk_level* L, double R[3][3]) {
    static const double I[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    C->S.n = 0;
    psk_decode_sky(&C->D, L, I, psk_collect, &C->S);
    int best = 0;
    const int matched = psk_register(C, P, psk_census_on() ? 1 : (P->nv * 9 + 9) / 10, R, &best);
    if (psk_census_on())
        OPL_INFO_F("portal", "sky census: picture of level %d against the sky of level %d: best %d, "
                   "registered %d of %d", level, L->level, best, matched, P->nv);
    return matched;
}

/** @brief Picks the sky that registers the entry's picture: the destination's, else the one fallback that registers.
 *
 * The destination's sky is taken when it registers. Otherwise every fallback
 * is tried; exactly one registering is taken, and two or more leave the
 * picture without a sky (ambiguous). Under OPENPETE_PORTAL3_SKY_CENSUS every
 * candidate is registered and logged, and the choice is the same.
 * @param C The building thread's scratch; its picture grid is built.
 * @param E The entry.
 * @param R Receives the chosen sky's rotation.
 * @param matched Receives the chosen sky's registered count.
 * @return The chosen candidate's index in E->L, -1 when none is taken. */
static int psk_choose_sky(psk_ctx* C, const psk_entry* E, double R[3][3], int* matched) {
    const psk_pic* P = &E->pic;
    double Rk[3][3];
    int pick = -1, hits = 0;
    *matched = 0;
    for (int k = 0; k < E->nl; k++) {
        if (pick == 0 && !psk_census_on()) break;
        const int m = psk_try_sky(C, P, E->level, E->L[k], Rk);
        if (!m || pick == 0) continue;
        if (k > 0) hits++;
        if (pick < 0) { pick = k; *matched = m; memcpy(R, Rk, sizeof Rk); }
    }
    if (pick > 0 && hits > 1) {
        OPL_WARN_F("portal", "picture of level %d: %d fallback skies register; ambiguous, no extension",
                   E->level, hits);
        *matched = 0;
        return -1;
    }
    return pick;
}

/** @brief Registers an entry's picture against its sky (psk_choose_sky) and builds the extension into it.
 * @param C The building thread's scratch, or NULL when it could not be allocated (no triangles).
 * @param E The entry; its picture and candidate skies are set, its triangle count is 0.
 * @param who The building thread's name, for the log line. */
static void psk_build_entry(psk_ctx* C, psk_entry* E, const char* who) {
    if (!C || E->nl <= 0) return;
    const op_time_t t0 = op_now();
    const psk_pic* P = &E->pic;
    psk_grid_build(P, &C->G);
    double R[3][3];
    int matched = 0;
    const int k = psk_choose_sky(C, E, R, &matched);
    if (k >= 0) {
        const psk_build B = { P, &C->G, E };
        psk_decode_sky(&C->D, E->L[k], R, psk_extend_face, (void*)&B);
    }
    OPL_INFO_F("portal", "picture of level %d: %d of %d vertices registered against the sky of level %d, "
               "%d extension triangles (%.1f ms, %s)", E->level, matched, P->nv,
               k >= 0 ? E->L[k]->level : -1, E->n, (double)(op_now() - t0) / 1e6, who);
}

/** @brief Locks the entry table when a lock exists. */
static void psk_lock(void) OP_ACQUIRE_CAP(s_psk_lock) OP_NO_THREAD_SAFETY_ANALYSIS {
    if (s_mu) SDL_LockMutex(s_mu);
}

/** @brief Unlocks the entry table when a lock exists. */
static void psk_unlock(void) OP_RELEASE_CAP(s_psk_lock) OP_NO_THREAD_SAFETY_ANALYSIS {
    if (s_mu) SDL_UnlockMutex(s_mu);
}

/** @brief The background builder: takes the oldest queued entry, builds it, marks it done, forever.
 * @param arg Unused.
 * @return Never returns. */
static int SDLCALL psk_worker(void* arg) {
    OP_ROLE_BIND(portalsky);
    (void)arg;
    openpete_crash_thread_attach();
    psk_ctx* C = (psk_ctx*)calloc(1, sizeof(psk_ctx));
    psk_lock();
    for (;;) {
        psk_entry* E = NULL;
        for (int i = 0; i < PSK_CACHE; i++)
            if (s_cache[i].state == PSK_QUEUED && (!E || s_cache[i].seq - E->seq > 0x80000000u)) E = &s_cache[i];
        if (!E) { SDL_WaitCondition(s_cv_work, s_mu); continue; }
        E->state = PSK_RUNNING;
        psk_unlock();
        psk_build_entry(C, E, "worker");
        psk_lock();
        E->state = PSK_DONE;
        SDL_BroadcastCondition(s_cv_done);
    }
    return 0;
}

/** @brief Creates the lock, the two conditions and the worker thread on first use.
 *
 * Without the lock or the conditions there is no worker and every build runs
 * on the thread that needs it; without the thread, the same through psk_join. */
static void psk_sched_init(void) OP_REQUIRES(sim) {
    if (s_sched) return;
    s_sched = 1;
    op_seal_check(psk);
    s_mu = SDL_CreateMutex();
    s_cv_work = SDL_CreateCondition();
    s_cv_done = SDL_CreateCondition();
    if (!s_mu || !s_cv_work || !s_cv_done) {
        if (s_mu) SDL_DestroyMutex(s_mu);
        if (s_cv_work) SDL_DestroyCondition(s_cv_work);
        if (s_cv_done) SDL_DestroyCondition(s_cv_done);
        s_mu = NULL; s_cv_work = s_cv_done = NULL;
    }
    op_seal(psk);
    SDL_Thread* t = s_mu ? SDL_CreateThread(psk_worker, "openpete-portalsky", NULL) : NULL;
    if (t) SDL_DetachThread(t);
    else OPL_WARN_F("portal", "sky build thread not created (%s): builds run on the game fiber", SDL_GetError());
}

/** @brief The cached entry of a record's picture, or NULL.
 * @param rec The record.
 * @param mesh Its +0x3C word.
 * @param level Its destination.
 * @param hash The picture's hash, or NULL to match any picture.
 * @return The entry. */
static psk_entry* psk_find(uint32_t rec, uint32_t mesh, int level, const uint64_t* hash) {
    psk_entry* hit = NULL;
    psk_lock();
    for (int i = 0; i < PSK_CACHE && !hit; i++) {
        psk_entry* E = &s_cache[i];
        if (E->state != PSK_FREE && E->rec == rec && E->mesh == mesh && E->level == level &&
            (!hash || E->hash == *hash)) hit = E;
    }
    psk_unlock();
    return hit;
}

/** @brief The skies a record's picture is registered against: its destination's, then each other destination's in the record array.
 *
 * Each level appears once, in record order after the destination; a
 * fallback sky that cannot be read is left out, and an unreadable
 * destination sky yields none. Reads the record array and the slot table, and reads each sky from
 * the disc on first use (psk_level_sky).
 * @param cpu Reads the record array.
 * @param level The destination.
 * @param L Receives the readable skies.
 * @return The count written, at most PSK_MAX_SKIES; 0 when no sky reads. */
static int psk_candidates_of(CPUState* cpu, int level, const psk_level* L[PSK_MAX_SKIES]) OP_REQUIRES(sim) {
    int lv[PSK_MAX_SKIES], n = 0;
    lv[n++] = level;
    const uint32_t base = portal_game_s23.records ? cpu->read_word(portal_game_s23.records) : 0u;
    const uint32_t cnt = portal_game_s23.count ? cpu->read_word(portal_game_s23.count) : 0u;
    if (base >= 0x80000000u && base < 0x80200000u && cnt <= 16u)
        for (uint32_t i = 0; i < cnt && n < PSK_MAX_SKIES; i++) {
            const int d = (int)cpu->read_byte(base + i * 0x44u + 0x42u);
            int seen = 0;
            for (int k = 0; k < n; k++) seen |= lv[k] == d;
            if (!seen) lv[n++] = d;
        }
    int m = 0;
    for (int k = 0; k < n; k++) {
        const psk_level* S = psk_level_sky(cpu, lv[k]);
        if (S->state > 0) L[m++] = S;
        else if (k == 0) return 0;
    }
    return m;
}

/** @brief Takes a slot for a record's picture and queues its build.
 *
 * The slot is the next free or finished one in round-robin order; a queued or
 * running slot is never taken. The destination's sky is read here, on the
 * game fiber. An unreadable sky leaves the entry done with no triangles.
 * @param cpu Reads the slot table for the sky.
 * @param rec The record.
 * @param mesh Its +0x3C word.
 * @param level Its destination.
 * @param P The picture.
 * @return The entry, or NULL when every slot is queued or running. */
static psk_entry* psk_enqueue(CPUState* cpu, uint32_t rec, uint32_t mesh, int level, const psk_pic* P) OP_REQUIRES(sim) {
    psk_sched_init();
    const psk_level* L[PSK_MAX_SKIES];
    const int nl = psk_candidates_of(cpu, level, L);
    psk_lock();
    psk_entry* E = NULL;
    for (int k = 0; k < PSK_CACHE && !E; k++) {
        psk_entry* c = &s_cache[(s_cache_next + (unsigned)k) % PSK_CACHE];
        if (c->state == PSK_FREE || c->state == PSK_DONE) { E = c; s_cache_next += (unsigned)k + 1u; }
    }
    if (E) {
        E->rec = rec; E->mesh = mesh; E->level = level; E->hash = P->hash; E->n = 0;
        E->nl = nl;
        memcpy(E->L, L, sizeof(L[0]) * (size_t)nl);
        memcpy(&E->pic, P, sizeof *P);
        E->seq = s_seq++;
        E->state = nl > 0 ? PSK_QUEUED : PSK_DONE;
        if (s_cv_work && E->state == PSK_QUEUED) SDL_SignalCondition(s_cv_work);
    }
    psk_unlock();
    return E;
}

/** @brief Waits until an entry is built, building it on this thread when no thread has taken it.
 * @param E The entry. */
static void psk_join(psk_entry* E) OP_REQUIRES(sim) {
    static psk_ctx* s_inline OP_GUARDED_BY(sim);   /* thread-roles: psk_join runs on sim */
    psk_lock();
    if (E->state == PSK_QUEUED) {
        E->state = PSK_RUNNING;
        psk_unlock();
        if (!s_inline) s_inline = (psk_ctx*)calloc(1, sizeof(psk_ctx));
        psk_build_entry(s_inline, E, "game fiber");
        psk_lock();
        E->state = PSK_DONE;
        if (s_cv_done) SDL_BroadcastCondition(s_cv_done);
    }
    while (E->state == PSK_RUNNING && s_cv_done) SDL_WaitCondition(s_cv_done, s_mu);
    psk_unlock();
}

int portal_sky_s23_prefetch(CPUState* cpu, uint32_t rec) OP_REQUIRES(sim) {
    static psk_pic Q OP_GUARDED_BY(sim);   /* thread-roles: the prefetch runs on sim */
    if (!portal_game_s23.level_slots) return 0;
    const int level = (int)cpu->read_byte(rec + 0x42);
    const uint32_t mesh = cpu->read_word(rec + 0x3C);
    if (psk_find(rec, mesh, level, NULL) || !psk_read_picture(cpu, rec, &Q)) return 0;
    return psk_enqueue(cpu, rec, mesh, level, &Q) != NULL;
}

int portal_sky_s23_extension(CPUState* cpu, uint32_t rec, const portal_sky_tri** tris) OP_REQUIRES(sim) {
    static psk_pic P OP_GUARDED_BY(sim);   /* thread-roles: the extension runs on sim */
    *tris = NULL;
    if (!portal_game_s23.level_slots || !psk_read_picture(cpu, rec, &P)) return 0;
    const int level = (int)cpu->read_byte(rec + 0x42);
    const uint32_t mesh = cpu->read_word(rec + 0x3C);
    psk_entry* E = psk_find(rec, mesh, level, &P.hash);
    if (!E) E = psk_enqueue(cpu, rec, mesh, level, &P);
    if (!E) return 0;
    psk_join(E);
    *tris = E->tri;
    return E->n;
}
