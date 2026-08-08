/* Same-moby painter RANK source — see moby_rank.h for the design.
 *
 * Threading: the OT ingest runs on the game thread inside the DrawOTag walk;
 * the snapshot/match/lookup side runs wherever the moby feeder extracts. The
 * published table crosses that boundary through a double buffer + sequence
 * counter (seqlock): the reader copies nothing — it matches directly against
 * the stable buffer and re-checks the sequence afterwards, discarding the
 * result if a writer overlapped (the previous rank map simply survives one
 * more tick). */

#include "moby_rank.h"
#include "openpete_psx.h"   /* openpete_psx_to_host_ptr — fresh-chain walk */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- gate ---------------------------------------------------------------- */

int openpete_mobyrank_on(void) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* e = getenv("OPENPETE_MOBY_RANK");
        /* Default OFF: og's whole-face relayering was authored for 320x240;
         * upscaled, the near-tie relayered edges read as worse artifacts than
         * the same-moby interpenetration they replace. Opt-in until the
         * select is scoped by authored class rather than a depth margin. */
        s_on = (e && *e) ? (*e != '0') : 0;
    }
    return s_on;
}

/* ---- OT table (written by the DrawOTag walk) ----------------------------- */

#define MR_OT_MAX 8192

typedef struct {
    unsigned frame;
    int      n;
    int16_t  x[MR_OT_MAX][4], y[MR_OT_MAX][4];
    uint8_t  u[MR_OT_MAX][4], v[MR_OT_MAX][4];
    uint16_t clut[MR_OT_MAX];          /* packed PSX clut id; 0xFFFF = none */
    uint8_t  nv[MR_OT_MAX];            /* 3 or 4 */
    uint8_t  tex[MR_OT_MAX];
} mr_ot_t;

static mr_ot_t  s_ot[2];
/* Version counter (odd = a writer is inside begin..end) + published index.
 * The reader (match) never copies: it matches against the published buffer
 * and re-checks the version afterwards — any overlapping write (including a
 * same-frame REOPEN that appends to the published buffer in place) bumps the
 * version and the reader discards its result. */
static volatile unsigned s_ot_ver = 0;
static int      s_ot_pub = -1;           /* published buffer index (-1 = none yet) */
static int      s_ot_wr = 0;             /* buffer being written */
static int      s_ot_open = 0;           /* a table is being appended */
static unsigned s_ot_frame = ~0u;

/* Prim vertex-word layout per code family (word indices into the payload
 * AFTER the P_TAG: word 0 = RGB+code). Mirrors the offline census decode. */
static int mr_fam(int code, int idx_xy[4], int idx_uv[4], int* nv, int* tex)
{
    switch (code & 0xFC) {
    case 0x20: idx_xy[0]=1; idx_xy[1]=2; idx_xy[2]=3; *nv=3; *tex=0; return 1;
    case 0x28: idx_xy[0]=1; idx_xy[1]=2; idx_xy[2]=3; idx_xy[3]=4; *nv=4; *tex=0; return 1;
    case 0x24: idx_xy[0]=1; idx_xy[1]=3; idx_xy[2]=5;
               idx_uv[0]=2; idx_uv[1]=4; idx_uv[2]=6; *nv=3; *tex=1; return 1;
    case 0x2C: idx_xy[0]=1; idx_xy[1]=3; idx_xy[2]=5; idx_xy[3]=7;
               idx_uv[0]=2; idx_uv[1]=4; idx_uv[2]=6; idx_uv[3]=8; *nv=4; *tex=1; return 1;
    case 0x30: idx_xy[0]=1; idx_xy[1]=3; idx_xy[2]=5; *nv=3; *tex=0; return 1;
    case 0x38: idx_xy[0]=1; idx_xy[1]=3; idx_xy[2]=5; idx_xy[3]=7; *nv=4; *tex=0; return 1;
    case 0x34: idx_xy[0]=1; idx_xy[1]=4; idx_xy[2]=7;
               idx_uv[0]=2; idx_uv[1]=5; idx_uv[2]=8; *nv=3; *tex=1; return 1;
    case 0x3C: idx_xy[0]=1; idx_xy[1]=4; idx_xy[2]=7; idx_xy[3]=10;
               idx_uv[0]=2; idx_uv[1]=5; idx_uv[2]=8; idx_uv[3]=11; *nv=4; *tex=1; return 1;
    default: return 0;
    }
}

void openpete_mobyrank_ot_begin(unsigned frame)
{
    if (!openpete_mobyrank_on()) return;
    if (s_ot_open && s_ot_frame == frame) return;   /* nested begin: keep appending */
    if (!s_ot_open && s_ot_frame == frame && s_ot_pub >= 0) {
        /* Second DrawOTag call in the SAME frame: reopen the just-published
         * buffer and keep appending — walk position stays global paint
         * order across the frame's calls. The version bump makes any reader
         * matching it right now discard its result. */
        __atomic_add_fetch(&s_ot_ver, 1u, __ATOMIC_RELEASE);   /* odd = writing */
        s_ot_wr = s_ot_pub;
        s_ot_open = 1;
        return;
    }
    __atomic_add_fetch(&s_ot_ver, 1u, __ATOMIC_RELEASE);       /* odd = writing */
    s_ot_wr = (s_ot_pub < 0) ? 0 : (s_ot_pub ^ 1);
    s_ot[s_ot_wr].frame = frame;
    s_ot[s_ot_wr].n = 0;
    s_ot_open = 1;
    s_ot_frame = frame;
}

static void mr_ot_append(mr_ot_t* W, int code, const uint8_t* payload, unsigned payload_bytes)
{
    if (W->n >= MR_OT_MAX) return;
    int ixy[4] = {0}, iuv[4] = {0}, nv = 0, tex = 0;
    if (!mr_fam(code, ixy, iuv, &nv, &tex)) return;
    unsigned need = (unsigned)((tex ? iuv[nv-1] : ixy[nv-1]) + 1) * 4u;
    if (payload_bytes < need) return;
    int i = W->n;
    for (int k = 0; k < nv; k++) {
        uint32_t wxy;
        memcpy(&wxy, payload + ixy[k] * 4, 4);
        W->x[i][k] = (int16_t)(wxy & 0xFFFF);
        W->y[i][k] = (int16_t)(wxy >> 16);
        if (tex) {
            uint32_t wuv;
            memcpy(&wuv, payload + iuv[k] * 4, 4);
            W->u[i][k] = (uint8_t)(wuv & 0xFF);
            W->v[i][k] = (uint8_t)((wuv >> 8) & 0xFF);
        }
    }
    if (tex) {
        uint32_t w0;
        memcpy(&w0, payload + iuv[0] * 4, 4);   /* uv0 word carries the clut */
        W->clut[i] = (uint16_t)(w0 >> 16);
    } else {
        W->clut[i] = 0xFFFF;
    }
    W->nv[i] = (uint8_t)nv;
    W->tex[i] = (uint8_t)tex;
    W->n = i + 1;
}

void openpete_mobyrank_ot_prim(int code, const uint8_t* payload, unsigned payload_bytes)
{
    if (!s_ot_open || !openpete_mobyrank_on()) return;
    mr_ot_append(&s_ot[s_ot_wr], code, payload, payload_bytes);
}

void openpete_mobyrank_ot_end(void)
{
    if (!s_ot_open) return;
    s_ot_open = 0;
    s_ot_pub = s_ot_wr;
    __atomic_add_fetch(&s_ot_ver, 1u, __ATOMIC_RELEASE);   /* even = published */
}

/* ---- fresh bucket walk (zero-latency rank source) ------------------------ */

/* The game inserts every prim into the bucket array `*g_WorldOT` during its
 * render pass; only at DrawOTag time does func_80016784 LINK the buckets
 * into one chain (patching next pointers, ZEROING the entries) and submit
 * it. At extraction time the render hooks have all run, so the bucket array
 * holds the CURRENT pose's complete paint order — walk it directly, in the
 * linker's own traversal: buckets TOP index down, each sub-chain from its
 * start node (+4) via next links to its end node (+0). If the linker beat
 * us here the buckets are zeroed and this yields nothing — the published
 * DrawOTag chain (which then carries this same pose) is the fallback. */
#define MR_G_WORLDOT 0x80075820u   /* g_WorldOT: pointer to the bucket array */
#define MR_OT_BUCKETS 0x800

static int mr_walk_buckets(mr_ot_t* W)
{
    const uint32_t* pot = (const uint32_t*)openpete_psx_to_host_ptr(MR_G_WORLDOT);
    if (!pot) return -1;
    uint32_t base = *pot;
    if (!base || (base & 3)) return -1;
    const uint8_t* bh = (const uint8_t*)openpete_psx_to_host_ptr(base);
    const uint8_t* bh_end = (const uint8_t*)openpete_psx_to_host_ptr(
        base + MR_OT_BUCKETS * 8u - 4u);
    if (!bh || !bh_end) return -1;
    W->n = 0;
    int nodes = 0;
    for (int i = MR_OT_BUCKETS - 1; i >= 0; i--) {
        uint32_t endn, start;
        memcpy(&endn,  bh + (size_t)i * 8u,      4);
        memcpy(&start, bh + (size_t)i * 8u + 4u, 4);
        if (!endn || !start) continue;
        uint32_t addr = start;
        while (addr && nodes++ < 24576) {
            const uint8_t* host = (const uint8_t*)openpete_psx_to_host_ptr(addr);
            if (!host) return -1;
            uint32_t tag;
            memcpy(&tag, host, 4);
            uint32_t plen = (tag >> 24) & 0xFF;
            if (plen > 0 && plen <= 16) {
                int code = host[7];
                int hi = code & 0xF0;
                if (hi == 0x20 || hi == 0x30)
                    mr_ot_append(W, code, host + 4, plen * 4u);
            }
            if (addr == endn) break;   /* end node's next is unpatched garbage */
            addr = (tag & 0x00FFFFFFu) | 0x80000000u;
        }
        if (nodes >= 24576) break;
    }
    return W->n;
}

/* ---- feeder pose snapshot ------------------------------------------------ */

#define MR_SNAP_MAX 32768

typedef struct {
    uint32_t moby, fkey;
    float    sx[3], sy[3];
    uint8_t  u[3], v[3];
    uint16_t clut;
    uint8_t  tex;
} mr_face_t;

static mr_face_t s_snap[MR_SNAP_MAX];
static int       s_snap_n = 0;
static unsigned  s_snap_frame = ~0u;
static int       s_snap_writing = 0;

void openpete_mobyrank_snap_begin(unsigned frame)
{
    if (!openpete_mobyrank_on()) return;
    if (frame == s_snap_frame) { s_snap_writing = 0; return; }   /* same VBL: keep */
    s_snap_frame = frame;
    s_snap_n = 0;
    s_snap_writing = 1;
}

void openpete_mobyrank_snap_face(uint32_t moby, uint32_t fkey,
                                 const float sx3[3], const float sy3[3],
                                 const uint8_t u3[3], const uint8_t v3[3],
                                 int textured, int clut_x, int clut_y)
{
    if (!s_snap_writing || s_snap_n >= MR_SNAP_MAX) return;
    mr_face_t* f = &s_snap[s_snap_n++];
    f->moby = moby; f->fkey = fkey;
    for (int k = 0; k < 3; k++) {
        f->sx[k] = sx3[k]; f->sy[k] = sy3[k];
        f->u[k] = u3[k];   f->v[k] = v3[k];
    }
    f->tex = (uint8_t)(textured != 0);
    f->clut = textured ? (uint16_t)(((clut_y & 0x1FF) << 6) | ((clut_x >> 4) & 0x3F))
                       : 0xFFFF;
}

void openpete_mobyrank_snap_end(void) { s_snap_writing = 0; }

/* ---- rank map ((moby,fkey) -> rank, rebuilt per consumed table) ---------- */

#define MR_MAP_BITS 16
#define MR_MAP_SIZE (1 << MR_MAP_BITS)

/* Double-buffered: matches build into the INACTIVE set and swap in only on
 * acceptance — a rejected match (torn read, implausible chain, collapsed
 * match rate) keeps the previous tick's ranks instead of dropping to zero. */
static uint32_t s_map_moby[2][MR_MAP_SIZE];
static uint32_t s_map_fkey[2][MR_MAP_SIZE];
static int32_t  s_map_rank[2][MR_MAP_SIZE];
static int      s_map_n[2] = {0, 0};
static int      s_map_act = 0;

static uint32_t mr_hash(uint32_t moby, uint32_t fkey)
{
    uint32_t h = (moby ^ (fkey * 2654435761u)) * 2246822519u;
    return (h >> (32 - MR_MAP_BITS)) & (MR_MAP_SIZE - 1);
}

static void mr_map_clear(int mi)
{
    memset(s_map_moby[mi], 0, sizeof s_map_moby[mi]);
    s_map_n[mi] = 0;
}

static void mr_map_put(int mi, uint32_t moby, uint32_t fkey, int rank)
{
    if (s_map_n[mi] >= MR_MAP_SIZE - (MR_MAP_SIZE >> 2)) return;   /* keep load <75% */
    uint32_t h = mr_hash(moby, fkey);
    while (s_map_moby[mi][h]) {
        if (s_map_moby[mi][h] == moby && s_map_fkey[mi][h] == fkey) return;
        h = (h + 1) & (MR_MAP_SIZE - 1);
    }
    s_map_moby[mi][h] = moby; s_map_fkey[mi][h] = fkey; s_map_rank[mi][h] = rank;
    s_map_n[mi]++;
}

int openpete_mobyrank_lookup(uint32_t moby, uint32_t fkey)
{
    int mi = s_map_act;
    if (!s_map_n[mi] || !moby) return 0;
    uint32_t h = mr_hash(moby, fkey);
    while (s_map_moby[mi][h]) {
        if (s_map_moby[mi][h] == moby && s_map_fkey[mi][h] == fkey)
            return s_map_rank[mi][h];
        h = (h + 1) & (MR_MAP_SIZE - 1);
    }
    return 0;
}

/* ---- matching ------------------------------------------------------------ */

/* Spatial hash of prim vertices (transformed-native -> prim lookup): cell
 * 4px, open addressing, one entry per prim vertex. */
#define MR_SH_BITS 16
#define MR_SH_SIZE (1 << MR_SH_BITS)
static int32_t s_sh_head[MR_SH_SIZE];
static int32_t s_sh_next[MR_OT_MAX * 4];
static int32_t s_sh_prim[MR_OT_MAX * 4];
static int8_t  s_sh_vert[MR_OT_MAX * 4];
static int     s_sh_n = 0;

static uint32_t mr_cell(int cx, int cy)
{
    uint32_t h = ((uint32_t)(cx * 73856093) ^ (uint32_t)(cy * 19349663));
    return h & (MR_SH_SIZE - 1);
}

#define MR_TOL 2.5f

static unsigned s_consumed_seq = 0;   /* last table seq the map was built from */
static unsigned s_last_stats_frame = 0;

void openpete_mobyrank_match(void)
{
    static unsigned s_matched_frame = ~0u;
    static mr_ot_t  s_fresh;
    if (!openpete_mobyrank_on() || s_snap_n == 0) return;
    if (s_snap_frame == s_matched_frame) return;   /* this pose already matched */

    /* Prefer the FRESH source — the current pose's bucket array, complete
     * once the render hooks have run and untouched until the game's own
     * linker (func_80016784) consumes it: pose-exact ranks, zero latency.
     * Fall back to the last published DrawOTag chain (one tick stale, og's
     * display latency) when the buckets are empty or already linked. */
    const mr_ot_t* T = NULL;
    unsigned seq = 0;
    if (mr_walk_buckets(&s_fresh) > 0) {
        s_fresh.frame = s_snap_frame;
        T = &s_fresh;
    }
    if (!T) {
        seq = __atomic_load_n(&s_ot_ver, __ATOMIC_ACQUIRE);
        if (seq == 0 || seq == s_consumed_seq || (seq & 1) || s_ot_pub < 0) return;
        T = &s_ot[s_ot_pub];
    }
    int tn = T->n;
    if (tn <= 0) { if (seq) s_consumed_seq = seq; return; }

    /* 1) global shift estimate (mode of per-face best shifts) over a sample:
     * the og pass bakes a draw-env offset the feeder projection doesn't. */
    int   hist_n = 0;
    float hx[256], hy[256];
    for (int s = 0; s < s_snap_n && hist_n < 256; s += (s_snap_n / 200) + 1) {
        const mr_face_t* f = &s_snap[s];
        float be = 1e30f, bdx = 0, bdy = 0;
        for (int p = 0; p < tn; p++) {
            for (int a = 0; a < T->nv[p]; a++) {
                float dx = T->x[p][a] - f->sx[0], dy = T->y[p][a] - f->sy[0];
                if (dx > 6 || dx < -6 || dy > 6 || dy < -6) continue;
                float e = 0;
                for (int k = 1; k < 3; k++) {
                    float best = 1e30f;
                    for (int b = 0; b < T->nv[p]; b++) {
                        float ex = fabsf(T->x[p][b] - (f->sx[k] + dx))
                                 + fabsf(T->y[p][b] - (f->sy[k] + dy));
                        if (ex < best) best = ex;
                    }
                    if (best > e) e = best;
                }
                if (e < be) { be = e; bdx = dx; bdy = dy; }
            }
        }
        if (be < 1.0f) { hx[hist_n] = bdx; hy[hist_n] = bdy; hist_n++; }
    }
    if (hist_n < 8) { if (seq) s_consumed_seq = seq; return; }   /* nothing recognisable: keep old map */
    /* mode via half-px binning */
    int best_cnt = 0; float gdx = 0, gdy = 0;
    for (int i = 0; i < hist_n; i++) {
        int c = 0; float sx = 0, sy = 0;
        for (int j = 0; j < hist_n; j++)
            if (fabsf(hx[j]-hx[i]) <= 1.0f && fabsf(hy[j]-hy[i]) <= 1.0f) { c++; sx += hx[j]; sy += hy[j]; }
        if (c > best_cnt) { best_cnt = c; gdx = sx / c; gdy = sy / c; }
    }

    /* 2) build the spatial hash of prim verts */
    memset(s_sh_head, -1, sizeof s_sh_head);
    s_sh_n = 0;
    for (int p = 0; p < tn; p++) {
        for (int a = 0; a < T->nv[p]; a++) {
            int cx = (int)floorf(T->x[p][a] / 4.0f);
            int cy = (int)floorf(T->y[p][a] / 4.0f);
            uint32_t h = mr_cell(cx, cy);
            s_sh_prim[s_sh_n] = p; s_sh_vert[s_sh_n] = (int8_t)a;
            s_sh_next[s_sh_n] = s_sh_head[h]; s_sh_head[h] = s_sh_n;
            s_sh_n++;
        }
    }

    /* 3) per snapshot face: unique containing prim under (gdx,gdy), UV+clut
     * exact for textured pairs. Ambiguous or absent -> stays rank 0. */
    int mi = s_map_act ^ 1;   /* build into the staging map */
    mr_map_clear(mi);
    int matched = 0, ambig = 0;
    for (int s = 0; s < s_snap_n; s++) {
        const mr_face_t* f = &s_snap[s];
        /* Two-stage adjudication per candidate prim:
         *  STRICT — every native vert finds a prim vert matching in XY AND
         *  (textured) exact UV, vertex-paired. Unique strict winner = match.
         *  LOOSE — XY coverage + UV MULTISET containment, unpaired: far-LOD
         *  prims can reverse vertex order vs the native decode (strip/cyclic
         *  winding) and tiny faces collapse to shared integer pixels, so the
         *  pairing misses them while the texel SET is still the identity.
         *  The loose tier only decides when NO strict candidate exists —
         *  loose-vs-loose ambiguity between neighbours sharing texels is
         *  common, and an ambiguous face stays rank 0 (true depth). */
        int   strict1 = -1, strict2 = -1, loose1 = -1, loose2 = -1;
        float serr1 = 1e30f, serr2 = 1e30f, lerr1 = 1e30f, lerr2 = 1e30f;
        float e0x = f->sx[0] + gdx, e0y = f->sy[0] + gdy;
        int c0x = (int)floorf((e0x - MR_TOL) / 4.0f), c1x = (int)floorf((e0x + MR_TOL) / 4.0f);
        int c0y = (int)floorf((e0y - MR_TOL) / 4.0f), c1y = (int)floorf((e0y + MR_TOL) / 4.0f);
        for (int cy = c0y; cy <= c1y; cy++) for (int cx = c0x; cx <= c1x; cx++) {
            for (int it = s_sh_head[mr_cell(cx, cy)]; it >= 0; it = s_sh_next[it]) {
                int p = s_sh_prim[it];
                if (p == strict1 || p == strict2 || p == loose1 || p == loose2) continue;
                if (f->tex != T->tex[p]) continue;
                if (f->tex && f->clut != T->clut[p]) continue;
                int strict = 1, xyok = 1;
                float err = 0.0f;
                for (int k = 0; k < 3 && xyok; k++) {
                    float ex = f->sx[k] + gdx, ey = f->sy[k] + gdy;
                    int xk = 0, sk = 0;
                    float ke = 1e30f;
                    for (int b = 0; b < T->nv[p]; b++) {
                        float dx = fabsf(T->x[p][b] - ex), dy2 = fabsf(T->y[p][b] - ey);
                        if (dx > MR_TOL || dy2 > MR_TOL) continue;
                        xk = 1;
                        float e2 = dx + dy2;
                        if (e2 < ke) ke = e2;
                        if (!f->tex || (T->u[p][b] == f->u[k] && T->v[p][b] == f->v[k]))
                            sk = 1;
                    }
                    xyok = xk;
                    err += (ke < 1e30f) ? ke : 0.0f;
                    if (!sk) strict = 0;
                }
                if (!xyok) continue;
                if (f->tex) {   /* multiset containment for the loose tier */
                    int used = 0, msok = 1;
                    for (int k = 0; k < 3 && msok; k++) {
                        msok = 0;
                        for (int b = 0; b < T->nv[p]; b++) {
                            if (used & (1 << b)) continue;
                            if (T->u[p][b] == f->u[k] && T->v[p][b] == f->v[k]) {
                                used |= 1 << b; msok = 1; break;
                            }
                        }
                    }
                    if (!msok) continue;
                }
                if (strict) {
                    if (err < serr1) { strict2 = strict1; serr2 = serr1; strict1 = p; serr1 = err; }
                    else if (err < serr2) { strict2 = p; serr2 = err; }
                } else {
                    if (err < lerr1) { loose2 = loose1; lerr2 = lerr1; loose1 = p; lerr1 = err; }
                    else if (err < lerr2) { loose2 = p; lerr2 = err; }
                }
            }
        }
        /* Tie-break by geometric error: two candidates are only AMBIGUOUS if
         * their fit quality is comparable — mirrored/stacked tiny faces pass
         * both, but the true prim fits measurably tighter. */
        int pick = -1;
        if (strict1 >= 0) {
            if (strict2 < 0 || serr2 - serr1 > 0.75f) pick = strict1; else ambig++;
        } else if (loose1 >= 0) {
            if (loose2 < 0 || lerr2 - lerr1 > 0.75f) pick = loose1; else ambig++;
        }
        if (pick >= 0) { mr_map_put(mi, f->moby, f->fkey, pick + 1); matched++; }
    }

    /* Published-table path only: a DrawOTag writer overlapped -> torn read.
     * Keep the previous map; the next extraction retries. */
    if (seq && __atomic_load_n(&s_ot_ver, __ATOMIC_ACQUIRE) != seq) return;
    /* Acceptance floor: a collapsed match rate means the source was not this
     * pose (fresh-chain race / mid-load garbage) — keep the previous map,
     * whose fkey identities still cover most faces one tick later. */
    if (matched * 4 < s_snap_n && matched < 64) return;
    s_map_act = mi;
    s_matched_frame = s_snap_frame;
    if (seq) s_consumed_seq = seq;

    { static int s_dbg = -1;
      if (s_dbg < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_DBG"); s_dbg = (e && *e && *e != '0'); }
      if (s_dbg && s_snap_frame != s_last_stats_frame) {
          s_last_stats_frame = s_snap_frame;
          fprintf(stderr, "[moby-rank] %s f=%u prims=%d snap=%d shift=(%.2f,%.2f) "
                          "matched=%d ambig=%d\n",
                  seq ? "table" : "FRESH", T->frame, tn, s_snap_n,
                  (double)gdx, (double)gdy, matched, ambig);
      } }
}
