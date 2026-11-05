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
// the payload. The portal block follows the sky block. The relocation copies
// the colour bytes to g_Cyclorama+0x08 (S2 0x800681C0, S3 0x8006E33C), the
// background colour the frame is cleared to before the sky draws (S2
// 0x8001D500-0x8001D548 copies the same bytes into both DRAWENVs' r0, g0,
// b0); the extension returns it so the area the sky has no face over shows
// it, as the destination level itself does.
//
// The strip. A picture may hold faces below the sky's lowest ring in colours
// the sky does not carry (Spyro 2's Dragon Shores picture: 27 faces down to
// 250 rows below it), which no sky face continues into the 16:9 bands. Each
// picture row whose outermost face on a side is such a face is continued
// outward past the frame in the colour the picture has at that point; these
// triangles lead the extension's list, so the sky draws over them and the
// background shows only where a band row borders sky.
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
// The eye off the centre. Some pictures were baked from an eye away from the
// sky's centre, with a taller vertical scale (Spyro 2's Dragon Shores, eye
// about 200 units along -Y, scale about 224; Spyro 3's levels 25 and 50, about
// 100 and 58 units); no rotation places nine tenths of their vertices. When
// the rotation search stays under that floor, the camera is refitted as
// q = R (p - t) with a free vertical scale. The pairs come from colours: a
// picture face whose ordered colours (or their reverse) match exactly one sky
// face pairs its corners with that face's, and a vertex takes the sky corner
// most of its faces name. The face whose rotation brings the most pairs within
// PSK_EYE_INIT pixels seeds the fit; Levenberg-Marquardt refits the rotation,
// then the rotation and eye, over the pairs within a shrinking bound; then
// each picture vertex pairs with its nearest projected sky corner and the
// rotation, eye and vertical scale are refitted over shrinking bounds. The
// fit is accepted when nine tenths of the picture vertices that carry a sky
// colour are placed; a vertex without one (the strip Spyro 2's Dragon Shores
// picture holds below the sky's lowest ring, in colours the sky does not
// carry) has no sky corner to land on. The extension then shifts every
// rotated sky corner by -R t before it is normalised and projects it with the
// fitted vertical scale. A picture the rotation registers never reaches the
// refit, and its extension is built with t = 0 and the fixed scale.
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
#include <stdio.h>
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
#define PSK_EYE_INIT 6.0           /* pixels: a colour pair this close counts for the seeding rotation */
#define PSK_EYE_MIN_PAIRS 8        /* the fewest colour pairs the eye refit starts from */
#define PSK_BAND_X0 (-86)          /* the census coverage grid over picture space: */
#define PSK_BAND_W  684            /* x [-86, 598), y [0, 240) */
#define PSK_BAND_H  240
#define PSK_FRAME_X0 0             /* the 4:3 frame's picture-space columns */
#define PSK_FRAME_X1 512
#define PSK_STRIP_ROWS 240         /* the picture rows the strip continuation covers */
#define PSK_STRIP_FAR 2048.0       /* how far past the frame a strip row runs; the outline clips it */

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

/** @brief Orders two colours ascending.
 * @param a The first.
 * @param b The second.
 * @return Negative, zero or positive as qsort expects. */
static int psk_u32_cmp(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
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

/* The eye refit's camera: a world point p maps to q = R (p - t) and projects
 * to (PSK_CX + PSK_FX q0 / q2, PSK_CY + fy q1 / q2). */
typedef struct {
    double R[3][3], t[3], fy;
} psk_eye_cam;

/* World points paired with picture points, for one fit. */
typedef struct {
    int    n;
    double w[PSK_MAX_PIC_V][3];
    double x[PSK_MAX_PIC_V], y[PSK_MAX_PIC_V];
} psk_pairs;

/* The eye refit's scratch: the colour pairs and which of them a stage fits,
 * the pairs a stage fits, the per-vertex votes, the sky's colours, the
 * nearest-corner search and the solver's residuals and Jacobian. */
typedef struct {
    psk_pairs corr, fit;
    int       in[PSK_MAX_PIC_V];             /* corr pair inside the stage's bound */
    int       vote_v[PSK_MAX_PIC_F * 3];     /* a voting face corner's picture vertex */
    int       vote_c[PSK_MAX_PIC_F * 3];     /* and its sky corner, sky face * 3 + corner */
    int       vote_next[PSK_MAX_PIC_F * 3];  /* the next vote of the same vertex, -1 ends */
    int       vote_head[PSK_MAX_PIC_V], vote_tail[PSK_MAX_PIC_V];
    int       nvote;
    uint32_t  skyc[PSK_MAX_SKY_F * 3];       /* the sky's colours, sorted, distinct */
    int       nskyc;
    uint8_t   sky_coloured[PSK_MAX_PIC_V];
    double    bd[PSK_MAX_PIC_V];             /* the nearest projected corner's squared distance */
    int       bc[PSK_MAX_PIC_V];             /* and its index in the search index, -1 for none */
    double    r[PSK_MAX_PIC_V * 2], r2[PSK_MAX_PIC_V * 2];
    double    J[PSK_MAX_PIC_V * 2][7];
} psk_eye;

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
    psk_eye   E;
} psk_ctx;

/* The camera an extension is built under: the rotation the decode applies,
 * R t subtracted from each rotated corner, and the vertical scale. */
typedef struct {
    double R[3][3], rt[3], fy;
} psk_cam;

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

/* ---- the eye refit --------------------------------------------------------- */

/** @brief Projects a world point under the eye refit's camera.
 * @param K The camera.
 * @param p The world point.
 * @param x Receives X.
 * @param y Receives Y.
 * @return 1 when the point lies inside the PSK_NEAR cone about the eye, else 0. */
static int psk_eye_project(const psk_eye_cam* K, const double p[3], double* x, double* y) {
    const double d[3] = { p[0] - K->t[0], p[1] - K->t[1], p[2] - K->t[2] };
    double c[3];
    for (int i = 0; i < 3; i++) c[i] = K->R[i][0] * d[0] + K->R[i][1] * d[1] + K->R[i][2] * d[2];
    const double n = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (n <= 0.0 || c[2] < PSK_NEAR * n) return 0;
    *x = PSK_CX + PSK_FX * c[0] / c[2];
    *y = PSK_CY + K->fy * c[1] / c[2];
    return 1;
}

/** @brief The residuals of a pair set under a camera, and their squared sum.
 *
 * A point outside the cone takes a residual of 1000 pixels on each axis.
 * @param K The camera.
 * @param S The pairs.
 * @param r Receives 2 * S->n residuals, X then Y per pair.
 * @return The squared sum. */
static double psk_eye_resid(const psk_eye_cam* K, const psk_pairs* S, double* r) {
    double c = 0.0;
    for (int i = 0; i < S->n; i++) {
        double x, y;
        if (!psk_eye_project(K, S->w[i], &x, &y)) { x = S->x[i] + 1e3; y = S->y[i] + 1e3; }
        r[2 * i] = x - S->x[i];
        r[2 * i + 1] = y - S->y[i];
        c += r[2 * i] * r[2 * i] + r[2 * i + 1] * r[2 * i + 1];
    }
    return c;
}

/** @brief Applies a parameter step to a camera: a rotation vector composed before R, an eye offset and a vertical scale offset.
 * @param K The camera.
 * @param d The step: rotation vector, eye offset, scale offset.
 * @param out Receives the stepped camera; not K. */
static void psk_eye_step(const psk_eye_cam* K, const double d[7], psk_eye_cam* out) {
    const double th = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    double M[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    if (th > 0.0) {
        const double k[3] = { d[0] / th, d[1] / th, d[2] / th }, s = sin(th), c1 = 1.0 - cos(th);
        const double W[3][3] = { { 0, -k[2], k[1] }, { k[2], 0, -k[0] }, { -k[1], k[0], 0 } };
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                M[i][j] += s * W[i][j] + c1 * (W[i][0] * W[0][j] + W[i][1] * W[1][j] + W[i][2] * W[2][j]);
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out->R[i][j] = M[i][0] * K->R[0][j] + M[i][1] * K->R[1][j] + M[i][2] * K->R[2][j];
    for (int i = 0; i < 3; i++) out->t[i] = K->t[i] + d[3 + i];
    out->fy = K->fy + d[6];
}

/** @brief Solves an n by n linear system by Gaussian elimination with partial pivoting.
 * @param A The matrix; destroyed.
 * @param b The right-hand side; destroyed.
 * @param n The order, at most 7.
 * @param x Receives the solution.
 * @return 1 when solved, 0 when a pivot vanishes. */
static int psk_solve(double A[7][7], double b[7], int n, double x[7]) {
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++) if (fabs(A[r][c]) > fabs(A[p][c])) p = r;
        if (fabs(A[p][c]) < 1e-300) return 0;
        for (int k = 0; k < n; k++) { const double t = A[c][k]; A[c][k] = A[p][k]; A[p][k] = t; }
        const double tb = b[c]; b[c] = b[p]; b[p] = tb;
        for (int r = c + 1; r < n; r++) {
            const double f = A[r][c] / A[c][c];
            for (int k = c; k < n; k++) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int c = n - 1; c >= 0; c--) {
        double s = b[c];
        for (int k = c + 1; k < n; k++) s -= A[c][k] * x[k];
        x[c] = s / A[c][c];
    }
    return 1;
}

/** @brief The forward-difference Jacobian of the residuals over the first `nfree` parameters, and its normal equations.
 * @param E The scratch; its residuals at K are set, its Jacobian is written.
 * @param K The camera.
 * @param S The pairs.
 * @param nfree The parameter count: 3 rotation, 6 with the eye, 7 with the vertical scale.
 * @param A Receives J^T J.
 * @param g Receives J^T r. */
static void psk_eye_normal(psk_eye* E, const psk_eye_cam* K, const psk_pairs* S, int nfree,
                           double A[7][7], double g[7]) {
    for (int j = 0; j < nfree; j++) {
        double d[7] = { 0 };
        d[j] = j < 3 ? 1e-6 : 1e-4;
        psk_eye_cam K2;
        psk_eye_step(K, d, &K2);
        psk_eye_resid(&K2, S, E->r2);
        for (int i = 0; i < 2 * S->n; i++) E->J[i][j] = (E->r2[i] - E->r[i]) / d[j];
    }
    memset(A, 0, sizeof(double) * 49);
    memset(g, 0, sizeof(double) * 7);
    for (int i = 0; i < 2 * S->n; i++)
        for (int a = 0; a < nfree; a++) {
            g[a] += E->J[i][a] * E->r[i];
            for (int b = 0; b < nfree; b++) A[a][b] += E->J[i][a] * E->J[i][b];
        }
}

/** @brief Solves one damped step from the normal equations and keeps it when it lowers the squared residual sum.
 * @param E The scratch; on acceptance its residuals are the stepped camera's.
 * @param K In/out: the camera.
 * @param S The pairs.
 * @param nfree The parameter count.
 * @param A J^T J.
 * @param g J^T r.
 * @param lam The damping.
 * @param c In/out: the squared residual sum at K.
 * @return 1 when the step was kept, else 0. */
static int psk_eye_try_step(psk_eye* E, psk_eye_cam* K, const psk_pairs* S, int nfree, const double A[7][7],
                            const double g[7], double lam, double* c) {
    double M[7][7], b[7], dx[7] = { 0 };
    for (int a = 0; a < nfree; a++) {
        for (int k = 0; k < nfree; k++) M[a][k] = A[a][k] + (a == k ? lam * A[a][a] : 0.0);
        b[a] = -g[a];
    }
    if (!psk_solve(M, b, nfree, dx)) return 0;
    psk_eye_cam K2;
    psk_eye_step(K, dx, &K2);
    const double c2 = psk_eye_resid(&K2, S, E->r2);
    if (!(c2 < *c)) return 0;
    *K = K2;
    *c = c2;
    memcpy(E->r, E->r2, sizeof(double) * 2u * (size_t)S->n);
    return 1;
}

/** @brief Fits the first `nfree` camera parameters to a pair set by Levenberg-Marquardt.
 *
 * At most 30 accepted steps; a step is accepted when it lowers the squared
 * residual sum, the damping falling tenfold on acceptance and rising tenfold
 * on rejection, and the fit stops once the damping passes 1e9.
 * @param E The scratch.
 * @param K In/out: the camera.
 * @param S The pairs.
 * @param nfree The parameter count: 3 rotation, 6 with the eye, 7 with the vertical scale. */
static void psk_eye_lm(psk_eye* E, psk_eye_cam* K, const psk_pairs* S, int nfree) {
    if (S->n * 2 < nfree) return;
    double lam = 1e-3, c = psk_eye_resid(K, S, E->r);
    for (int it = 0; it < 30; it++) {
        double A[7][7], g[7];
        psk_eye_normal(E, K, S, nfree, A, g);
        while (!psk_eye_try_step(E, K, S, nfree, (const double (*)[7])A, g, lam, &c))
            if ((lam *= 10.0) > 1e9) return;
        lam = lam / 10.0 > 1e-9 ? lam / 10.0 : 1e-9;
    }
}

/** @brief The one sky face and corner order of a parity class that carries a picture face's colours.
 * @param P The picture.
 * @param pf The picture face.
 * @param S The sky's faces.
 * @param X The index.
 * @param o0 The class: 0 for the three rotations, 3 for the three reflections.
 * @param sf Receives the sky face.
 * @return The order's index into k_psk_ord, -1 when no or more than one face and order match. */
static int psk_eye_unique(const psk_pic* P, int pf, const psk_sky* S, const psk_index* X, int o0, int* sf) {
    int lo, hits = 0, ord = -1;
    const int n = psk_tri_run(X, P, pf, &lo);
    for (int t = lo; t < lo + n; t++)
        for (int o = o0; o < o0 + 3; o++) {
            const int f = X->tri[t].sf;
            if (S->c[f][k_psk_ord[o][0]] != P->c[pf][0] || S->c[f][k_psk_ord[o][1]] != P->c[pf][1] ||
                S->c[f][k_psk_ord[o][2]] != P->c[pf][2]) continue;
            hits++; ord = o; *sf = f;
        }
    return hits == 1 ? ord : -1;
}

/** @brief Records every picture-corner to sky-corner vote of the faces whose colours one sky face carries.
 *
 * A face of one colour votes nothing; a face votes when exactly one sky face
 * and rotation carry its colours, else when exactly one sky face and
 * reflection do. Each vertex's votes chain in face order.
 * @param E The scratch.
 * @param P The picture.
 * @param S The sky's faces.
 * @param X The index. */
static void psk_eye_votes(psk_eye* E, const psk_pic* P, const psk_sky* S, const psk_index* X) {
    E->nvote = 0;
    for (int v = 0; v < P->nv; v++) E->vote_head[v] = E->vote_tail[v] = -1;
    for (int pf = 0; pf < P->nf; pf++) {
        if (P->c[pf][0] == P->c[pf][1] && P->c[pf][1] == P->c[pf][2]) continue;
        int sf = -1, o = psk_eye_unique(P, pf, S, X, 0, &sf);
        if (o < 0) o = psk_eye_unique(P, pf, S, X, 3, &sf);
        if (o < 0) continue;
        for (int i = 0; i < 3; i++) {
            const int a = E->nvote++, v = P->f[pf][i];
            E->vote_v[a] = v; E->vote_c[a] = sf * 3 + k_psk_ord[o][i]; E->vote_next[a] = -1;
            if (E->vote_tail[v] >= 0) E->vote_next[E->vote_tail[v]] = a; else E->vote_head[v] = a;
            E->vote_tail[v] = a;
        }
    }
}

/** @brief Pairs each voted picture vertex with the sky position most of its votes name, the first named on a tie.
 * @param E The scratch; its votes are recorded, its colour pairs are written.
 * @param P The picture.
 * @param S The sky's faces. */
static void psk_eye_corr(psk_eye* E, const psk_pic* P, const psk_sky* S) {
    psk_pairs* Q = &E->corr;
    Q->n = 0;
    for (int v = 0; v < P->nv; v++) {
        int best = -1, bn = 0;
        for (int a = E->vote_head[v]; a >= 0; a = E->vote_next[a]) {
            const double* pa = S->p[E->vote_c[a] / 3][E->vote_c[a] % 3];
            int cnt = 0;
            for (int b = E->vote_head[v]; b >= 0; b = E->vote_next[b])
                cnt += !memcmp(pa, S->p[E->vote_c[b] / 3][E->vote_c[b] % 3], sizeof(double) * 3);
            if (cnt > bn) { bn = cnt; best = a; }
        }
        if (best < 0) continue;
        memcpy(Q->w[Q->n], S->p[E->vote_c[best] / 3][E->vote_c[best] % 3], sizeof Q->w[0]);
        Q->x[Q->n] = P->x[v]; Q->y[Q->n] = P->y[v];
        E->bc[v] = Q->n++;
    }
}

/** @brief Marks each colour pair the camera projects within `thr` pixels of its picture point.
 * @param E The scratch.
 * @param K The camera.
 * @param thr The bound, exclusive.
 * @return The count marked. */
static int psk_eye_select(psk_eye* E, const psk_eye_cam* K, double thr) {
    int n = 0;
    for (int i = 0; i < E->corr.n; i++) {
        double x, y;
        E->in[i] = psk_eye_project(K, E->corr.w[i], &x, &y) && hypot(x - E->corr.x[i], y - E->corr.y[i]) < thr;
        n += E->in[i];
    }
    return n;
}

/** @brief The camera of the face rotation that brings the most colour pairs within PSK_EYE_INIT pixels, the eye at the centre.
 *
 * Each picture face whose three distinct vertices are paired gives Horn's
 * rotation of its three pairs; the first best is kept.
 * @param E The scratch; E->bc maps a paired vertex to its pair.
 * @param P The picture.
 * @param K Receives the camera.
 * @return 1 when a face gave a rotation, else 0. */
static int psk_eye_seed(psk_eye* E, const psk_pic* P, psk_eye_cam* K) {
    int best = -1;
    for (int pf = 0; pf < P->nf; pf++) {
        const int a = P->f[pf][0], b = P->f[pf][1], c = P->f[pf][2];
        if (a == b || b == c || a == c || E->bc[a] < 0 || E->bc[b] < 0 || E->bc[c] < 0) continue;
        double d[3][3], r[3][3];
        for (int i = 0; i < 3; i++) {
            const int q = E->bc[P->f[pf][i]];
            psk_unit(E->corr.w[q], d[i]);
            psk_ray(E->corr.x[q], E->corr.y[q], r[i]);
        }
        psk_eye_cam H = { { { 0 } }, { 0, 0, 0 }, PSK_FY };
        psk_horn((const double (*)[3])d, (const double (*)[3])r, 3, H.R);
        const int n = psk_eye_select(E, &H, PSK_EYE_INIT);
        if (n > best) { best = n; *K = H; }
    }
    return best >= 0;
}

/** @brief Fits the camera over the colour pairs marked by the last selection.
 * @param E The scratch.
 * @param K In/out: the camera.
 * @param nfree The parameter count. */
static void psk_eye_fit_selected(psk_eye* E, psk_eye_cam* K, int nfree) {
    psk_pairs* F = &E->fit;
    F->n = 0;
    for (int i = 0; i < E->corr.n; i++) {
        if (!E->in[i]) continue;
        memcpy(F->w[F->n], E->corr.w[i], sizeof F->w[0]);
        F->x[F->n] = E->corr.x[i]; F->y[F->n] = E->corr.y[i];
        F->n++;
    }
    psk_eye_lm(E, K, F, nfree);
}

/** @brief Offers one projected sky corner to every picture vertex within `reach` grid cells, each keeping its nearest.
 * @param E The scratch: per vertex the nearest squared distance and corner so far.
 * @param P The picture.
 * @param G Its grid.
 * @param x The corner's projected X.
 * @param y Its projected Y.
 * @param reach The cells searched on each side.
 * @param i The corner's index in the search index. */
static void psk_eye_offer(psk_eye* E, const psk_pic* P, const psk_grid* G, double x, double y, int reach, int i) {
    for (int dy = -reach; dy <= reach; dy++)
        for (int dx = -reach; dx <= reach; dx++) {
            const int c = psk_cell(x + dx * PSK_CELL, y + dy * PSK_CELL);
            for (int v = c < 0 ? -1 : G->head[c]; v >= 0; v = G->next[v]) {
                const double ex = P->x[v] - x, ey = P->y[v] - y, d = ex * ex + ey * ey;
                if (d < E->bd[v]) { E->bd[v] = d; E->bc[v] = i; }
            }
        }
}

/** @brief Pairs each picture vertex with the nearest sky corner the camera projects within `thr` pixels of it.
 * @param E The scratch; the pairs are written to E->fit.
 * @param C The builder's scratch: picture grid, sky faces, index.
 * @param P The picture.
 * @param K The camera.
 * @param thr The bound, exclusive. */
static void psk_eye_nearest(psk_eye* E, const psk_ctx* C, const psk_pic* P, const psk_eye_cam* K, double thr) {
    const psk_index* X = &C->X;
    const int reach = (int)ceil(thr / PSK_CELL);
    for (int v = 0; v < P->nv; v++) { E->bd[v] = thr * thr; E->bc[v] = -1; }
    for (int i = 0; i < X->n; i++) {
        double x, y;
        if (psk_eye_project(K, C->S.p[X->corner[i] / 3][X->corner[i] % 3], &x, &y))
            psk_eye_offer(E, P, &C->G, x, y, reach, i);
    }
    psk_pairs* F = &E->fit;
    F->n = 0;
    for (int v = 0; v < P->nv; v++) {
        if (E->bc[v] < 0) continue;
        const int k = X->corner[E->bc[v]];
        memcpy(F->w[F->n], C->S.p[k / 3][k % 3], sizeof F->w[0]);
        F->x[F->n] = P->x[v]; F->y[F->n] = P->y[v];
        F->n++;
    }
}

/** @brief Marks the picture vertices that carry a colour some sky face carries.
 * @param E The scratch; receives the sky's colours and the marks.
 * @param P The picture.
 * @param S The sky's faces.
 * @return The count marked. */
static int psk_eye_sky_coloured(psk_eye* E, const psk_pic* P, const psk_sky* S) {
    int n = 0;
    for (int f = 0; f < S->n; f++)
        for (int k = 0; k < 3; k++) E->skyc[n++] = S->c[f][k];
    qsort(E->skyc, (size_t)n, sizeof E->skyc[0], psk_u32_cmp);
    E->nskyc = 0;
    for (int i = 0; i < n; i++)
        if (!E->nskyc || E->skyc[E->nskyc - 1] != E->skyc[i]) E->skyc[E->nskyc++] = E->skyc[i];
    memset(E->sky_coloured, 0, sizeof E->sky_coloured);
    for (int pf = 0; pf < P->nf; pf++)
        for (int i = 0; i < 3; i++)
            if (bsearch(&P->c[pf][i], E->skyc, (size_t)E->nskyc, sizeof E->skyc[0], psk_u32_cmp))
                E->sky_coloured[P->f[pf][i]] = 1;
    int m = 0;
    for (int v = 0; v < P->nv; v++) m += E->sky_coloured[v];
    return m;
}

/** @brief Scores the refitted camera as psk_score does: the picture vertices some projected sky corner lands within PSK_SNAP of.
 * @param C The builder's scratch.
 * @param P The picture.
 * @param K The camera.
 * @param sky_hits Receives the count among the vertices that carry a sky colour.
 * @return The matched vertex count. */
static int psk_eye_score(psk_ctx* C, const psk_pic* P, const psk_eye_cam* K, int* sky_hits) {
    psk_match* M = &C->M;
    for (int i = 0; i < P->nv; i++) M->hit[i] = -1;
    int score = 0, sky = 0;
    for (int f = 0; f < C->S.n; f++)
        for (int k = 0; k < 3; k++) {
            double x, y;
            if (!psk_eye_project(K, C->S.p[f][k], &x, &y)) continue;
            const int v = psk_near(P, &C->G, x, y);
            if (v < 0 || M->hit[v] >= 0) continue;
            M->hit[v] = f * 3 + k;
            score++;
            sky += C->E.sky_coloured[v];
        }
    *sky_hits = sky;
    return score;
}

/** @brief Refits the camera with the eye off the sky's centre and a free vertical scale, for a picture the rotation does not register.
 *
 * The search index C->X is the one psk_register built for this sky.
 * @param C The builder's scratch; its grid, sky faces and index are built.
 * @param P The picture.
 * @param K Receives the fitted camera.
 * @param placed Receives the matched vertex count, accepted or not.
 * @param sky_placed Receives the matched count among the vertices carrying a sky colour.
 * @param nsky Receives the count of vertices carrying a sky colour.
 * @return The matched vertex count when nine tenths of the sky-coloured vertices are matched, else 0. */
static int psk_register_eye(psk_ctx* C, const psk_pic* P, psk_eye_cam* K, int* placed, int* sky_placed, int* nsky) {
    static const double k_corr3[] = { 8.0, 3.0, 1.5 }, k_corr6[] = { 20.0, 8.0, 3.0, 1.5, 1.5 };
    static const double k_near[] = { 6.0, 4.0, 3.0, 2.0, 1.5, 1.5, 1.5 };
    psk_eye* E = &C->E;
    *placed = *sky_placed = 0;
    *nsky = psk_eye_sky_coloured(E, P, &C->S);
    for (int v = 0; v < P->nv; v++) E->bc[v] = -1;
    psk_eye_votes(E, P, &C->S, &C->X);
    psk_eye_corr(E, P, &C->S);
    if (E->corr.n < PSK_EYE_MIN_PAIRS || !psk_eye_seed(E, P, K)) return 0;
    psk_eye_select(E, K, PSK_EYE_INIT);
    for (size_t i = 0; i < sizeof k_corr3 / sizeof k_corr3[0]; i++) {
        psk_eye_fit_selected(E, K, 3);
        psk_eye_select(E, K, k_corr3[i]);
    }
    for (size_t i = 0; i < sizeof k_corr6 / sizeof k_corr6[0]; i++) {
        psk_eye_fit_selected(E, K, 6);
        psk_eye_select(E, K, k_corr6[i]);
    }
    for (size_t i = 0; i < sizeof k_near / sizeof k_near[0]; i++) {
        psk_eye_nearest(E, C, P, K, k_near[i]);
        psk_eye_lm(E, K, &E->fit, 7);
    }
    *placed = psk_eye_score(C, P, K, sky_placed);
    if (*nsky <= 0 || *sky_placed * 10 < *nsky * 9) return 0;
    return *placed;
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
    int              nstrip;           /* the leading triangles that continue the picture's strip */
    uint32_t         bg;               /* the registered sky's background colour, 0x00BBGGRR */
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
    double          rt[3];   /* R t, subtracted from each rotated corner */
    double          fy;      /* the vertical scale */
} psk_build;

/** @brief Projects one corner of the extension, taking a picture vertex's position when one lies within PSK_SNAP.
 * @param B The build.
 * @param c The camera-space corner.
 * @param x Receives X.
 * @param y Receives Y. */
static void psk_place(const psk_build* B, const ngte_cvert* c, float* x, float* y) {
    const double px = PSK_CX + PSK_FX * c->m1 / c->m3, py = PSK_CY + B->fy * c->m2 / c->m3;
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
 * Each corner is moved by -R t (zero unless the eye refit registered the
 * picture) into the picture's eye space. The corners are then scaled to unit length first, which leaves each projected
 * edge on the same screen line, so the plane m3 = PSK_NEAR bounds every
 * corner's screen offset to 1 / PSK_NEAR of the scales.
 * @param a The first corner.
 * @param b The second.
 * @param c The third.
 * @param gouraud Unused: a flat face carries its one colour on every corner.
 * @param ctx The psk_build. */
static void psk_extend_face(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud, void* ctx) {
    (void)gouraud;
    const psk_build* B = (const psk_build*)ctx;
    ngte_cvert in[3] = { a, b, c }, out[4];
    for (int i = 0; i < 3; i++) {
        in[i].m1 -= B->rt[0]; in[i].m2 -= B->rt[1]; in[i].m3 -= B->rt[2];
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
 * @param cam Receives the camera: the rotation with no eye shift and the fixed
 *        vertical scale, or the eye refit's when only it registers.
 * @return The registered vertex count, 0 when the sky is not accepted. */
static int psk_try_sky(psk_ctx* C, const psk_pic* P, int level, const psk_level* L, psk_cam* cam) {
    static const double I[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    C->S.n = 0;
    psk_decode_sky(&C->D, L, I, psk_collect, &C->S);
    int best = 0;
    int matched = psk_register(C, P, psk_census_on() ? 1 : (P->nv * 9 + 9) / 10, cam->R, &best);
    cam->rt[0] = cam->rt[1] = cam->rt[2] = 0.0;
    cam->fy = PSK_FY;
    if (psk_census_on())
        OPL_INFO_F("portal", "sky census: picture of level %d against the sky of level %d: best %d, "
                   "registered %d of %d", level, L->level, best, matched, P->nv);
    if (matched) return matched;
    psk_eye_cam K = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { 0, 0, 0 }, PSK_FY };
    int placed = 0, sky_placed = 0, nsky = 0;
    matched = psk_register_eye(C, P, &K, &placed, &sky_placed, &nsky);
    if (psk_census_on() || matched)
        OPL_INFO_F("portal", "sky census: picture of level %d against the sky of level %d: eye refit %d of %d "
                   "(%d of %d sky-coloured), %s; eye (%.1f, %.1f, %.1f), vertical scale %.2f", level, L->level,
                   placed, P->nv, sky_placed, nsky, matched ? "registered" : "not registered", K.t[0], K.t[1],
                   K.t[2], K.fy);
    if (!matched) return 0;
    memcpy(cam->R, K.R, sizeof K.R);
    for (int i = 0; i < 3; i++) cam->rt[i] = K.R[i][0] * K.t[0] + K.R[i][1] * K.t[1] + K.R[i][2] * K.t[2];
    cam->fy = K.fy;
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
 * @param cam Receives the chosen sky's camera.
 * @param matched Receives the chosen sky's registered count.
 * @return The chosen candidate's index in E->L, -1 when none is taken. */
static int psk_choose_sky(psk_ctx* C, const psk_entry* E, psk_cam* cam, int* matched) {
    const psk_pic* P = &E->pic;
    psk_cam Ck;
    int pick = -1, hits = 0;
    *matched = 0;
    for (int k = 0; k < E->nl; k++) {
        if (pick == 0 && !psk_census_on()) break;
        const int m = psk_try_sky(C, P, E->level, E->L[k], &Ck);
        if (!m || pick == 0) continue;
        if (k > 0) hits++;
        if (pick < 0) { pick = k; *matched = m; *cam = Ck; }
    }
    if (pick > 0 && hits > 1) {
        OPL_WARN_F("portal", "picture of level %d: %d fallback skies register; ambiguous, no extension",
                   E->level, hits);
        *matched = 0;
        return -1;
    }
    return pick;
}

/* ---- the strip below the sky ----------------------------------------------- */

/** @brief One picture face's Gouraud colour at a point, by barycentric weights.
 * @param P The picture.
 * @param f The face.
 * @param px The point's X.
 * @param py The point's Y.
 * @param rgb Receives R, G, B. */
static void psk_face_rgb(const psk_pic* P, int f, double px, double py, double rgb[3]) {
    const double ax = P->x[P->f[f][0]], ay = P->y[P->f[f][0]], bx = P->x[P->f[f][1]], by = P->y[P->f[f][1]];
    const double cx = P->x[P->f[f][2]], cy = P->y[P->f[f][2]];
    const double den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    double w0 = 1.0 / 3.0, w1 = 1.0 / 3.0;
    if (fabs(den) > 1e-9) {
        w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den;
        w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den;
    }
    const double w[3] = { w0, w1, 1.0 - w0 - w1 };
    for (int ch = 0; ch < 3; ch++) {
        rgb[ch] = 0.0;
        for (int k = 0; k < 3; k++) rgb[ch] += w[k] * (double)((P->c[f][k] >> (8 * ch)) & 0xFFu);
    }
}

/** @brief The outermost picture coverage on one side of a picture-space row: the leftmost (side 0) or rightmost (side 1) point where the line crosses a face.
 * @param P The picture.
 * @param y The row's Y.
 * @param side 0 for the left, 1 for the right.
 * @param x Receives the outermost X.
 * @return The face holding it, -1 when the line crosses no face. */
static int psk_row_edge(const psk_pic* P, double y, int side, double* x) {
    int best = -1;
    for (int f = 0; f < P->nf; f++)
        for (int e = 0; e < 3; e++) {
            const double ax = P->x[P->f[f][e]], ay = P->y[P->f[f][e]];
            const double bx = P->x[P->f[f][(e + 1) % 3]], by = P->y[P->f[f][(e + 1) % 3]];
            if ((ay > y) == (by > y)) continue;   /* the edge does not cross the row */
            const double cx = ax + (bx - ax) * (y - ay) / (by - ay);
            if (best < 0 || (side ? cx > *x : cx < *x)) { best = f; *x = cx; }
        }
    return best;
}

/** @brief Appends one flat-coloured triangle to the entry.
 * @param E The entry.
 * @param x The corners' X.
 * @param y The corners' Y.
 * @param rgb The colour, 0x00BBGGRR. */
static void psk_push_flat(psk_entry* E, const float x[3], const float y[3], uint32_t rgb) {
    if (E->n == E->cap) {
        const int cap = E->cap ? E->cap * 2 : 1024;
        portal_sky_tri* t = (portal_sky_tri*)realloc(E->tri, sizeof(portal_sky_tri) * (size_t)cap);
        if (!t) return;
        E->tri = t; E->cap = cap;
    }
    portal_sky_tri* T = &E->tri[E->n++];
    for (int i = 0; i < 3; i++) { T->x[i] = x[i]; T->y[i] = y[i]; T->rgb[i] = rgb; }
}

/** @brief Whether a picture face carries a colour the sky does not: a face of the strip the picture holds below the sky.
 * @param X The sky's colours, sorted and distinct (psk_eye_sky_coloured).
 * @param P The picture.
 * @param f The face.
 * @return 1 when one of its colours is not a sky colour. */
static int psk_strip_face(const psk_eye* X, const psk_pic* P, int f) {
    for (int k = 0; k < 3; k++)
        if (!bsearch(&P->c[f][k], X->skyc, (size_t)X->nskyc, sizeof X->skyc[0], psk_u32_cmp)) return 1;
    return 0;
}

/** @brief Appends the strip's continuation into one side band for one picture row.
 *
 * The row's outermost covered point at its centre names the face and
 * colour; a face of the sky's colours appends nothing, so that row of the
 * band keeps the sky and the background beneath it. Otherwise one quad in
 * that colour runs from PSK_STRIP_FAR beyond the band to the outermost
 * coverage at the row's top, centre or bottom, whichever lies furthest in
 * (its overlap with the picture is drawn over by the picture), clamped to
 * the 4:3 frame's edge so the frame keeps its own pixels.
 * @param E The entry.
 * @param X The sky's colours.
 * @param j The row.
 * @param side 0 for the left band, 1 for the right. */
static void psk_strip_row(psk_entry* E, const psk_eye* X, int j, int side) {
    const psk_pic* P = &E->pic;
    double xc = 0.0;
    const int f = psk_row_edge(P, j + 0.5, side, &xc);
    if (f < 0 || !psk_strip_face(X, P, f)) return;
    double in = xc;
    for (int t = 0; t < 2; t++) {
        double xt = 0.0;
        if (psk_row_edge(P, (double)(j + t), side, &xt) >= 0) in = side ? fmin(in, xt) : fmax(in, xt);
    }
    in = side ? fmax(in, (double)PSK_FRAME_X1) : fmin(in, (double)PSK_FRAME_X0);
    const double out = side ? PSK_STRIP_FAR : -PSK_STRIP_FAR;
    if (side ? in >= out : in <= out) return;
    double rgb[3];
    psk_face_rgb(P, f, xc, j + 0.5, rgb);
    uint32_t c = 0;
    for (int ch = 0; ch < 3; ch++) c |= (uint32_t)lround(fmin(255.0, fmax(0.0, rgb[ch]))) << (8 * ch);
    const float x0 = (float)out, x1 = (float)in, y0 = (float)j, y1 = (float)(j + 1);
    const float ax[3] = { x0, x1, x1 }, ay[3] = { y0, y0, y1 }, bx[3] = { x0, x1, x0 }, by[3] = { y0, y1, y1 };
    psk_push_flat(E, ax, ay, c);
    psk_push_flat(E, bx, by, c);
}

/** @brief Builds the strip's continuation into both side bands, ahead of the sky in the entry's triangles.
 *
 * A picture whose lowest faces are not sky (a strip below the sky's lowest
 * ring, in colours the sky does not carry) leaves the band below the sky
 * with neither sky nor picture; each picture row whose outermost face on a
 * side is such a face is continued outward in the colour the picture has
 * there (psk_strip_row). The sky's colours are decoded from the chosen sky
 * into the scratch first.
 * @param C The builder's scratch.
 * @param E The entry; its triangle count is 0.
 * @param L The sky the picture registered to. */
static void psk_build_strip(psk_ctx* C, psk_entry* E, const psk_level* L) {
    static const double I[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    C->S.n = 0;
    psk_decode_sky(&C->D, L, I, psk_collect, &C->S);
    psk_eye_sky_coloured(&C->E, &E->pic, &C->S);
    for (int j = 0; j < PSK_STRIP_ROWS; j++)
        for (int side = 0; side < 2; side++) psk_strip_row(E, &C->E, j, side);
    E->nstrip = E->n;
}

/* ---- the band census ---------------------------------------------------- */

/** @brief Whether a point lies inside or on a triangle, either winding.
 * @param x The point's X.
 * @param y The point's Y.
 * @param ax The first corner's X.
 * @param ay The first corner's Y.
 * @param bx The second corner's X.
 * @param by The second corner's Y.
 * @param cx The third corner's X.
 * @param cy The third corner's Y.
 * @return 1 when inside, else 0. */
static int psk_in_tri(double x, double y, double ax, double ay, double bx, double by, double cx, double cy) {
    const double d0 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    const double d1 = (cx - bx) * (y - by) - (cy - by) * (x - bx);
    const double d2 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}

/* The census's picture-space canvas over x [PSK_BAND_X0, PSK_BAND_X0 + PSK_BAND_W),
 * y [0, PSK_BAND_H): the layers covering each cell (PSK_L_*) and the colour of
 * the last one painted, unfaded. */
enum { PSK_L_SKY = 1, PSK_L_PIC = 2, PSK_L_STRIP = 4, PSK_L_BG = 8 };
typedef struct {
    uint8_t* layer;
    uint8_t* rgb;
} psk_canvas;

/** @brief Paints one Gouraud triangle's pixel centres into the canvas.
 * @param V The canvas.
 * @param bit The layer bit set.
 * @param x The corners' X.
 * @param y The corners' Y.
 * @param rgb The corners' colours, 0x00BBGGRR. */
static void psk_paint(psk_canvas* V, uint8_t bit, const float x[3], const float y[3], const uint32_t rgb[3]) {
    const double x0 = fmin(x[0], fmin(x[1], x[2])), x1 = fmax(x[0], fmax(x[1], x[2]));
    const double y0 = fmin(y[0], fmin(y[1], y[2])), y1 = fmax(y[0], fmax(y[1], y[2]));
    const int i0 = (int)fmax(0.0, floor(x0 - PSK_BAND_X0)), i1 = (int)fmin(PSK_BAND_W - 1.0, ceil(x1 - PSK_BAND_X0));
    const int j0 = (int)fmax(0.0, floor(y0)), j1 = (int)fmin(PSK_BAND_H - 1.0, ceil(y1));
    const double den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
            const double px = PSK_BAND_X0 + i + 0.5, py = j + 0.5;
            if (!psk_in_tri(px, py, x[0], y[0], x[1], y[1], x[2], y[2])) continue;
            double w0 = 1.0 / 3.0, w1 = 1.0 / 3.0;
            if (fabs(den) > 1e-9) {
                w0 = ((y[1] - y[2]) * (px - x[2]) + (x[2] - x[1]) * (py - y[2])) / den;
                w1 = ((y[2] - y[0]) * (px - x[2]) + (x[0] - x[2]) * (py - y[2])) / den;
            }
            const double w[3] = { w0, w1, 1.0 - w0 - w1 };
            const size_t c = (size_t)j * PSK_BAND_W + (size_t)i;
            V->layer[c] |= bit;
            for (int ch = 0; ch < 3; ch++) {
                double v = 0.0;
                for (int k = 0; k < 3; k++) v += w[k] * (double)((rgb[k] >> (8 * ch)) & 0xFFu);
                V->rgb[c * 3 + (size_t)ch] = (uint8_t)lround(fmin(255.0, fmax(0.0, v)));
            }
        }
}

/** @brief Writes the canvas as <dir>/portal-mask-<level>-<rec>.pgm (16 times the layer bits) and .ppm (the colours) under OPENPETE_PORTAL3_SKY_MASK_DIR.
 * @param E The entry, for the file names.
 * @param V The canvas. */
static void psk_mask_write(const psk_entry* E, const psk_canvas* V) {
    const char* dir = getenv("OPENPETE_PORTAL3_SKY_MASK_DIR");
    if (!dir || !*dir) return;
    const size_t cells = (size_t)PSK_BAND_W * PSK_BAND_H;
    for (int kind = 0; kind < 2; kind++) {
        char path[512];
        snprintf(path, sizeof path, "%s/portal-mask-%d-%08X.%s", dir, E->level, (unsigned)E->rec, kind ? "ppm" : "pgm");
        FILE* fp = fopen(path, "wb");
        if (!fp) continue;
        fprintf(fp, "P%d\n%d %d\n255\n", kind ? 6 : 5, PSK_BAND_W, PSK_BAND_H);
        if (kind) fwrite(V->rgb, 1, cells * 3, fp);
        else for (size_t c = 0; c < cells; c++) fputc(16 * V->layer[c], fp);
        fclose(fp);
    }
}

/** @brief Logs the 16:9 side-band pixels of picture space the picture and the sky leave uncovered, and those the strip continuation then covers.
 *
 * The band is the picture-space columns a 16:9 view adds beside the 4:3
 * frame, [-85.33, 0) and [512, 597.33), over the rows the pass clamps to,
 * [12, 228). The canvas is painted in the pass's order: the background over
 * the band, the strip continuation, the sky, the picture. A pixel counts as
 * uncovered when its centre lies in no picture face and no sky triangle (the
 * backdrop showed there before the fills); of those, the strip continuation
 * covers some and the background shows on the rest. Logs the chosen
 * camera's rotation, eye shift and vertical scale beside it, and writes the
 * canvas (psk_mask_write).
 * @param E The built entry.
 * @param cam The camera it was built under. */
static void psk_band_census(const psk_entry* E, const psk_cam* cam) {
    const size_t cells = (size_t)PSK_BAND_W * PSK_BAND_H;
    psk_canvas V = { (uint8_t*)calloc(cells, 1), (uint8_t*)calloc(cells * 3, 1) };
    if (!V.layer || !V.rgb) { free(V.layer); free(V.rgb); return; }
    const double half = 256.0 * (16.0 / 9.0) / (4.0 / 3.0);
    const uint32_t bg3[3] = { E->bg, E->bg, E->bg };
    const float bx[2][2] = { { (float)(256.0 - half), (float)PSK_FRAME_X0 }, { (float)PSK_FRAME_X1, (float)(256.0 + half) } };
    for (int s = 0; s < 2; s++) {
        const float x0[3] = { bx[s][0], bx[s][1], bx[s][1] }, y0[3] = { 0, 0, PSK_BAND_H };
        const float x1[3] = { bx[s][0], bx[s][1], bx[s][0] }, y1[3] = { 0, PSK_BAND_H, PSK_BAND_H };
        psk_paint(&V, PSK_L_BG, x0, y0, bg3);
        psk_paint(&V, PSK_L_BG, x1, y1, bg3);
    }
    for (int i = 0; i < E->n; i++)
        psk_paint(&V, i < E->nstrip ? PSK_L_STRIP : PSK_L_SKY, E->tri[i].x, E->tri[i].y, E->tri[i].rgb);
    const psk_pic* P = &E->pic;
    for (int f = 0; f < P->nf; f++) {
        const float x[3] = { P->x[P->f[f][0]], P->x[P->f[f][1]], P->x[P->f[f][2]] };
        const float y[3] = { P->y[P->f[f][0]], P->y[P->f[f][1]], P->y[P->f[f][2]] };
        psk_paint(&V, PSK_L_PIC, x, y, P->c[f]);
    }
    int left = 0, right = 0, strip = 0;
    for (int j = 12; j < 228; j++)
        for (int i = 0; i < PSK_BAND_W; i++) {
            const double px = PSK_BAND_X0 + i + 0.5;
            const int in_l = px < 0.0 && px >= 256.0 - half, in_r = px >= 512.0 && px < 256.0 + half;
            const uint8_t L = V.layer[(size_t)j * PSK_BAND_W + (size_t)i];
            if ((L & (PSK_L_SKY | PSK_L_PIC)) || (!in_l && !in_r)) continue;
            left += in_l; right += in_r; strip += (L & PSK_L_STRIP) != 0;
        }
    OPL_INFO_F("portal", "sky census: picture of level %d (record 0x%08X): 16:9 band uncovered %d px (left %d, "
               "right %d), %d of them strip, the rest background 0x%06X; %d strip triangles; "
               "R=[%.5f %.5f %.5f|%.5f %.5f %.5f|%.5f %.5f %.5f] Rt=(%.1f, %.1f, %.1f) fy=%.2f", E->level,
               (unsigned)E->rec, left + right, left, right, strip, (unsigned)E->bg, E->nstrip, cam->R[0][0],
               cam->R[0][1], cam->R[0][2], cam->R[1][0], cam->R[1][1], cam->R[1][2], cam->R[2][0], cam->R[2][1],
               cam->R[2][2], cam->rt[0], cam->rt[1], cam->rt[2], cam->fy);
    psk_mask_write(E, &V);
    free(V.layer);
    free(V.rgb);
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
    psk_cam cam;
    int matched = 0;
    const int k = psk_choose_sky(C, E, &cam, &matched);
    if (k >= 0) {
        psk_build_strip(C, E, E->L[k]);
        const psk_build B = { P, &C->G, E, { cam.rt[0], cam.rt[1], cam.rt[2] }, cam.fy };
        psk_decode_sky(&C->D, E->L[k], cam.R, psk_extend_face, (void*)&B);
        const uint8_t* h = E->L[k]->buf;
        E->bg = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16);
    }
    OPL_INFO_F("portal", "picture of level %d: %d of %d vertices registered against the sky of level %d, "
               "%d extension triangles, %d of them the strip (%.1f ms, %s)", E->level, matched, P->nv,
               k >= 0 ? E->L[k]->level : -1, E->n, E->nstrip, (double)(op_now() - t0) / 1e6, who);
    if (k >= 0 && psk_census_on()) psk_band_census(E, &cam);
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
        E->rec = rec; E->mesh = mesh; E->level = level; E->hash = P->hash; E->n = 0; E->nstrip = 0; E->bg = 0;
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

int portal_sky_s23_extension(CPUState* cpu, uint32_t rec, portal_sky_ext* out) OP_REQUIRES(sim) {
    static psk_pic P OP_GUARDED_BY(sim);   /* thread-roles: the extension runs on sim */
    memset(out, 0, sizeof *out);
    if (!portal_game_s23.level_slots || !psk_read_picture(cpu, rec, &P)) return 0;
    const int level = (int)cpu->read_byte(rec + 0x42);
    const uint32_t mesh = cpu->read_word(rec + 0x3C);
    psk_entry* E = psk_find(rec, mesh, level, &P.hash);
    if (!E) E = psk_enqueue(cpu, rec, mesh, level, &P);
    if (!E) return 0;
    psk_join(E);
    out->tri = E->tri;
    out->n = E->n;
    out->nstrip = E->nstrip;
    out->bg = E->bg;
    return E->n;
}
