/* Paint-rank calibration instrument — see moby_rank_cal.h.
 *
 * The game inserts every prim of its render pass into the bucket array
 * `*cal_world_ot`; only at DrawOTag time does its linker join the buckets into
 * one chain (patching next pointers, zeroing the entries) and submit it. While
 * the moby feed runs, the render hooks have all run, so the array holds the
 * current pose's paint order; the walk below follows the linker's own
 * traversal: buckets from the top index down, each from its start node (+4)
 * via next links to its end node (+0), whose next word is not yet patched.
 * A prim's position in that traversal is its paint position.
 *
 * A native prim (its one or two walk triangles: consecutive, same moby, same
 * rank) is identified with a game prim by its integer screen points (the
 * draw's RTPS mirror, moby_otk_rtps): the game prim must have the vertex
 * count of the prim the native key chose (MOBY_OTK_QUAD), its vertices must
 * be exactly the native triangles' points, and it must be the only such prim.
 * A native triangle outside the prim the key chose (MOBY_OTK_FOREIGN), or a
 * triangle of a quad whose other half the float emission culled (three of the
 * quad's four points), is not identified. Prims
 * with no such match (the draw culled the face or chose another prim of it,
 * two prims share the points) are left out of the comparison. */

#include "renderer/moby_rank_cal.h"
#include "renderer/moby_rank.h"
#include "game_layout.h"
#include "openpete_fiber.h"          /* openpete_frame */
#include "openpete_subtick_view.h"   /* openpete_subtick_host_span */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int moby_rank_cal_on(void)
{
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_CAL"); s = (e && *e) ? atoi(e) : 0; }
    return s;
}

/* ---- the ordering table --------------------------------------------------- */

#define CAL_OT_MAX    8192
#define CAL_NODES_MAX 24576

typedef struct {
    int     n;
    uint8_t nv[CAL_OT_MAX];
    int16_t wb[CAL_OT_MAX];   /* world bucket index */
    int16_t x[CAL_OT_MAX][4], y[CAL_OT_MAX][4];
} cal_ot_t;

/** @brief Appends one polygon prim's vertex XY words to the table.
 *
 * Code 0x20-0x3F, bit 3 quad, bit 2 textured (a UV word follows each XY), bit
 * 4 gouraud (a colour word precedes each XY after the first); word 0 holds the
 * first colour and the code. A full table or a payload shorter than the code's
 * vertex words is skipped.
 * @param T The table.
 * @param payload The prim's words after the tag.
 * @param bytes The payload's length in bytes.
 * @param wb The world bucket the prim sits in. */
static void cal_ot_append(cal_ot_t* T, const uint8_t* payload, unsigned bytes, int wb)
{
    int code = payload[3];
    int nv = (code & 0x08) ? 4 : 3;
    int stride = 1 + ((code >> 2) & 1) + ((code >> 4) & 1);
    if (T->n >= CAL_OT_MAX || bytes < (unsigned)(1 + (nv - 1) * stride + 1) * 4u) return;
    int i = T->n++;
    T->nv[i] = (uint8_t)nv;
    T->wb[i] = (int16_t)wb;
    for (int k = 0; k < nv; k++) {
        uint32_t w;
        memcpy(&w, payload + (size_t)(1 + k * stride) * 4u, 4);
        T->x[i][k] = (int16_t)(w & 0xFFFF);
        T->y[i][k] = (int16_t)(w >> 16);
    }
}

/** @brief Walks one bucket's sub-chain from its start node to its end node, appending its polygon prims.
 *
 * @param T The table.
 * @param start The bucket's start node.
 * @param endn The bucket's end node.
 * @param nodes The node count across the whole walk, advanced per node against CAL_NODES_MAX.
 * @param wb The bucket's index.
 * @return 1 when the end node was reached, 0 on an unmapped node or the node bound. */
static int cal_walk_bucket(cal_ot_t* T, uint32_t start, uint32_t endn, int* nodes, int wb)
{
    for (uint32_t addr = start; addr && (*nodes)++ < CAL_NODES_MAX; ) {
        const uint8_t* host = (const uint8_t*)openpete_subtick_host_span(addr, 8u);
        if (!host) return 0;
        uint32_t tag;
        memcpy(&tag, host, 4);
        uint32_t plen = (tag >> 24) & 0xFF;
        int hi = host[7] & 0xE0;
        if (plen > 0 && plen <= 16 && hi == 0x20 && openpete_subtick_host_span(addr, 4u + plen * 4u))
            cal_ot_append(T, host + 4, plen * 4u, wb);
        if (addr == endn) return 1;
        addr = (tag & 0x00FFFFFFu) | 0x80000000u;
    }
    return *nodes < CAL_NODES_MAX;
}

/** @brief Walks the world ordering table's buckets from the top index down into the table.
 *
 * Stops at the first bucket whose walk fails.
 * @param T Receives the prims in paint order.
 * @return -1 when the table is not located or not mapped, else the prim count
 *         (0 when the linker has already consumed the array). */
static int cal_walk_ot(cal_ot_t* T)
{
    T->n = 0;
    const uint32_t root = game_layout.moby.cal_world_ot;
    const int buckets = (int)game_layout.moby.cal_ot_buckets;
    const uint8_t* pot = root ? (const uint8_t*)openpete_subtick_host_span(root, 4u) : NULL;
    uint32_t base = 0u;
    if (pot) memcpy(&base, pot, 4);
    if (!base || (base & 3u) || buckets <= 0) return -1;
    const uint8_t* bh = (const uint8_t*)openpete_subtick_host_span(base, (uint32_t)buckets * 8u);
    if (!bh) return -1;
    int nodes = 0;
    for (int i = buckets - 1; i >= 0; i--) {
        uint32_t endn, start;
        memcpy(&endn,  bh + (size_t)i * 8u,      4);
        memcpy(&start, bh + (size_t)i * 8u + 4u, 4);
        if (endn && start && !cal_walk_bucket(T, start, endn, &nodes, i)) break;
    }
    return T->n;
}

/* ---- exact screen-point identity ----------------------------------------- */

#define CAL_HASH_BITS 15
#define CAL_HASH_SIZE (1 << CAL_HASH_BITS)
static int32_t s_head[CAL_HASH_SIZE];
static int32_t s_next[CAL_OT_MAX * 4];
static int32_t s_prim[CAL_OT_MAX * 4];

/** @brief Hashes an integer screen point to an index of s_head.
 *
 * @param x The screen X.
 * @param y The screen Y.
 * @return An index below CAL_HASH_SIZE. */
static uint32_t cal_hash(int x, int y)
{
    return (((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u)) & (CAL_HASH_SIZE - 1);
}

/** @brief Builds the hash chains of every vertex of every prim of the table.
 *
 * Clears s_head, then chains one edge per vertex through s_next, with s_prim
 * naming the vertex's prim. The table holds at most CAL_OT_MAX prims of four
 * vertices, which is the edge arrays' size.
 * @param T The table. */
static void cal_index(const cal_ot_t* T)
{
    memset(s_head, -1, sizeof s_head);
    int e = 0;
    for (int p = 0; p < T->n; p++)
        for (int k = 0; k < T->nv[p]; k++, e++) {
            uint32_t h = cal_hash(T->x[p][k], T->y[p][k]);
            s_prim[e] = p; s_next[e] = s_head[h]; s_head[h] = e;
        }
}

/** @brief Whether prim `p` has a vertex exactly at (x, y).
 *
 * @param T The table.
 * @param p The prim.
 * @param x The screen X.
 * @param y The screen Y.
 * @return 1 when it does, 0 otherwise. */
static int cal_prim_has(const cal_ot_t* T, int p, int x, int y)
{
    for (int k = 0; k < T->nv[p]; k++)
        if (T->x[p][k] == x && T->y[p][k] == y) return 1;
    return 0;
}

/** @brief Finds the one game prim with `nv` vertices that contains every one of `n` distinct points.
 *
 * Candidates come from the hash chain of the first point (cal_index).
 * @param T The table.
 * @param pt The points.
 * @param n The point count.
 * @param nv The vertex count the prim must have.
 * @return The prim's paint position + 1; 0 for none, -1 for more than one. */
static int cal_find(const cal_ot_t* T, const int16_t (*pt)[2], int n, int nv)
{
    int found = 0, last = -1;
    for (int e = s_head[cal_hash(pt[0][0], pt[0][1])]; e >= 0; e = s_next[e]) {
        int p = s_prim[e], all = T->nv[p] == nv;
        for (int k = 0; k < n && all; k++) all = cal_prim_has(T, p, pt[k][0], pt[k][1]);
        if (p == last || !all) continue;
        if (found && found != p + 1) return -1;
        found = p + 1; last = p;
    }
    return found;
}

/** @brief Collects the distinct integer points of the walk triangles [i0, i1), the triangles of one prim.
 *
 * @param in The feed pass.
 * @param i0 The first triangle.
 * @param i1 One past the last triangle.
 * @param pt Receives the points.
 * @return The point count, or 0 past four. */
static int cal_points(const moby_rank_cal_in_t* in, int i0, int i1, int16_t pt[4][2])
{
    int n = 0;
    for (int i = i0; i < i1; i++)
        for (int k = 0; k < 3; k++) {
            int16_t x = in->isxy[i][2 * k], y = in->isxy[i][2 * k + 1];
            int seen = 0;
            for (int q = 0; q < n && !seen; q++) seen = pt[q][0] == x && pt[q][1] == y;
            if (seen) continue;
            if (n == 4) return 0;
            pt[n][0] = x; pt[n][1] = y; n++;
        }
    return n;
}

/* ---- comparison ----------------------------------------------------------- */

typedef struct { long cmp, agree; } cal_tally;

static int s_pos[1 << 15];   /* per walk triangle: cal_find, 0 when unranked */

/** @brief Compares one triangle pair's paint positions against its native ranks.
 *
 * The pair is compared when both triangles carry a paint position, and both
 * the positions and the ranks differ.
 * @param in The feed pass.
 * @param a The first triangle.
 * @param b The second triangle.
 * @param agree Receives 1 when the two orders agree, 0 when they disagree.
 * @return 1 when the pair was compared, 0 otherwise. */
static int cal_pair(const moby_rank_cal_in_t* in, int a, int b, int* agree)
{
    int pa = s_pos[a], pb = s_pos[b], ra = in->rank[a], rb = in->rank[b];
    if (pa <= 0 || pb <= 0 || pa == pb || ra == rb) return 0;
    *agree = (pa > pb) == (ra > rb);
    return 1;
}

static const cal_ot_t* s_T;   /* the table of the current run */

/** @brief Prints one triangle of a disagreeing pair to stderr: its key fields, its integer points and its prim's.
 *
 * @param in The feed pass.
 * @param f The triangle, which carries a paint position. */
static void cal_print_face(const moby_rank_cal_in_t* in, int f)
{
    const int16_t* q = in->isxy[f];
    int p = s_pos[f] - 1;
    fprintf(stderr, " [%d: bk=%d q2=%d fk=%08X %d,%d %d,%d %d,%d | prim wb=%d nv=%d", f,
            MOBY_OTK_BUCKET(in->otk[f]), (in->otk[f] & MOBY_OTK_Q2) != 0, in->fkey[f],
            q[0], q[1], q[2], q[3], q[4], q[5], s_T->wb[p], s_T->nv[p]);
    for (int k = 0; k < s_T->nv[p]; k++) fprintf(stderr, " %d,%d", s_T->x[p][k], s_T->y[p][k]);
    fputc(']', stderr);
}

/** @brief Tallies one triangle pair and, at level 2, prints a disagreeing pair to stderr.
 *
 * Nothing is counted when cal_pair does not compare the pair. A disagreement
 * is printed only when OPENPETE_MOBY_RANK_CAL is 2 or more.
 * @param in The feed pass.
 * @param a The first triangle.
 * @param b The second triangle.
 * @param t Accumulates the compared and agreeing counts.
 * @param kind The pair class named in the printed line. */
static void cal_tally_pair(const moby_rank_cal_in_t* in, int a, int b, cal_tally* t,
                           const char* kind)
{
    int ag;
    if (!cal_pair(in, a, b, &ag)) return;
    t->cmp++; t->agree += ag;
    if (ag || moby_rank_cal_on() < 2) return;
    fprintf(stderr, "[rank-cal-dis] vbl=%u pass=%s kind=%s m=%08X a=%d b=%d rank=%d,%d ot=%d,%d",
            openpete_frame, in->pass, kind, in->moby[a], a, b, in->rank[a], in->rank[b],
            s_pos[a], s_pos[b]);
    cal_print_face(in, a);
    cal_print_face(in, b);
    fputc('\n', stderr);
}

/** @brief Tallies the same-moby pairs of the triangles [i0, i1): contested pairs and all pairs.
 *
 * @param in The feed pass.
 * @param i0 The moby's first triangle.
 * @param i1 One past its last triangle.
 * @param cont Tallies the pairs whose triangles are both contested.
 * @param all Tallies every compared pair. */
static void cal_moby_pairs(const moby_rank_cal_in_t* in, int i0, int i1, cal_tally* cont,
                           cal_tally* all)
{
    for (int a = i0; a < i1; a++) {
        if (s_pos[a] <= 0) continue;
        for (int b = a + 1; b < i1; b++) {
            int ag;
            if (!cal_pair(in, a, b, &ag)) continue;
            all->cmp++; all->agree += ag;
            if (in->contest[a] && in->contest[b]) cal_tally_pair(in, a, b, cont, "contested");
        }
    }
}

typedef struct { long ranked, exact, ambig; } cal_ident;

/** @brief The end of the native prim that starts at walk triangle i0.
 *
 * A prim's triangles are consecutive, eligible, of one moby, with one rank. A
 * prim is identified only when its first triangle is eligible, ranked and not
 * foreign to the key's prim; an unidentified one is one triangle long.
 * @param in The feed pass.
 * @param i0 The prim's first triangle.
 * @param n The triangle count.
 * @param on Receives whether the prim is identified.
 * @return One past the prim's last triangle. */
static int cal_prim_end(const moby_rank_cal_in_t* in, int i0, int n, int* on)
{
    *on = in->eligible[i0] && in->rank[i0] > 0 && !(in->otk[i0] & MOBY_OTK_FOREIGN);
    int i1 = i0 + 1;
    while (*on && i1 < n && in->moby[i1] == in->moby[i0] && in->rank[i1] == in->rank[i0]
           && in->eligible[i1]) i1++;
    return i1;
}

/** @brief The paint position (cal_find) of the native prim [i0, i1) in the table.
 *
 * The prim's vertex count is 4 when its bucket word marks the whole quad, else 3.
 * @param T The table.
 * @param in The feed pass.
 * @param i0 The prim's first triangle.
 * @param i1 One past its last triangle.
 * @return As cal_find; 0 when the prim has fewer than three or more than four distinct points. */
static int cal_prim_pos(const cal_ot_t* T, const moby_rank_cal_in_t* in, int i0, int i1)
{
    int16_t pt[4][2];
    int np = cal_points(in, i0, i1, pt);
    int nv = (in->otk[i0] >= 0 && (in->otk[i0] & MOBY_OTK_QUAD)) ? 4 : 3;
    return np >= 3 ? cal_find(T, (const int16_t (*)[2])pt, np, nv) : 0;
}

/** @brief Fills s_pos for the walk triangles [0, n) and counts the identification outcomes.
 *
 * @param T The table.
 * @param tn The table's prim count; at or below 0 no prim is identified.
 * @param in The feed pass.
 * @param n The triangle count.
 * @param c Accumulates the ranked, exactly identified and ambiguous triangle counts. */
static void cal_identify(const cal_ot_t* T, int tn, const moby_rank_cal_in_t* in, int n,
                         cal_ident* c)
{
    for (int i0 = 0; i0 < n; ) {
        int on;
        int i1 = cal_prim_end(in, i0, n, &on);
        int pos = (on && tn > 0) ? cal_prim_pos(T, in, i0, i1) : 0;
        for (int i = i0; i < i1; i++) s_pos[i] = pos;
        c->ranked += (long)on * (i1 - i0);
        c->exact  += (long)(pos > 0) * (i1 - i0);
        c->ambig  += (long)(pos < 0) * (i1 - i0);
        i0 = i1;
    }
}

void moby_rank_cal_run(const moby_rank_cal_in_t* in)
{
    static cal_ot_t T;
    s_T = &T;
    int tn = cal_walk_ot(&T);
    if (tn > 0) cal_index(&T);
    int n = in->n < (int)(sizeof s_pos / sizeof s_pos[0]) ? in->n : (int)(sizeof s_pos / sizeof s_pos[0]);
    cal_ident id = {0, 0, 0};
    cal_identify(&T, tn, in, n, &id);
    cal_tally ip = {0, 0}, cont = {0, 0}, all = {0, 0};
    for (int k = 0; k < in->ip_n; k++)
        if (in->ip_a[k] < n && in->ip_b[k] < n) cal_tally_pair(in, in->ip_a[k], in->ip_b[k], &ip, "ipair");
    for (int i0 = 0; i0 < n; ) {
        int i1 = i0;
        while (i1 < n && in->moby[i1] == in->moby[i0]) i1++;
        if (in->moby[i0]) cal_moby_pairs(in, i0, i1, &cont, &all);
        i0 = i1;
    }
    fprintf(stderr, "[rank-cal] vbl=%u pass=%s ot_prims=%d ranked=%ld exact=%ld ambiguous=%ld "
                    "| ipairs cmp=%ld agree=%ld | contested cmp=%ld agree=%ld | all cmp=%ld agree=%ld\n",
            openpete_frame, in->pass, tn, id.ranked, id.exact, id.ambig, ip.cmp, ip.agree,
            cont.cmp, cont.agree, all.cmp, all.agree);
}
