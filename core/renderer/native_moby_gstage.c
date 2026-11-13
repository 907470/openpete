/* native_moby_gstage.c - the moby vertex stage's readback against the CPU
 * projection (OPENPETE_MOBY_GSTAGE_AB); see openpete_moby_gstage.h. */
#include "openpete_moby_gstage.h"

#include "rhi.h"
#include "openpete_mutex_once.h"
#include "openpete_native_render.h"   /* openpete_scene_tri: the presented scene's identities */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Snapshots kept: the one being built and the scenes before it. */
#define GSAB_RING 6
/* Readback target width; moby_rb.vert RB_W. */
#define GSAB_RB_W 4096u
/* Pixels per element; moby_rb.vert writes three. */
#define GSAB_PX 3u
/* Vec4s per instance and per vertex record; moby_stage.glsl MOBY_INST_VEC4, MOBY_VERT_VEC4. */
#define GSAB_INST_VEC4 8u
#define GSAB_VERT_VEC4 2u
/* Output px per screen px at which the control moves a vertex 1/64 output px (moby_rb.vert). */
#define GSAB_CTL_SCALE 36u

/* One committed record: its instance terms, identity and vertex range. */
typedef struct {
    openpete_gsab_inst inst;
    uint32_t mid;
    int first;
} gsab_rec;

/* The committed records of one scene epoch. */
typedef struct {
    int valid;
    unsigned epoch;
    gsab_rec *rec;
    int nrec, crec;
    openpete_gsab_vert *vert;
    int nvert, cvert;
    int skipped;   /* commits whose slots another decode had reused */
} gsab_snap;

/* A presented scene vertex's identity and screen position bits. */
typedef struct { uint32_t mid, x, y; } gsab_key;

/* Push constants of moby_rb.vert. */
typedef struct { uint32_t vbase, h, ebase; float ctl; } gsab_pc;

static struct {
    int on;                       /* -1 unread, else the latched switch */
    int negctl;                   /* the stage arm draws the control */
    openpete_gsab_inst *inst;     /* OPENPETE_GSAB_PROJ_CAP slots */
    openpete_gsab_vert *vert;
    uint32_t serial;
    void *mx;                     /* SDL_Mutex over ring, cur and the dump copy */
    gsab_snap ring[GSAB_RING];
    int cur;
    gsab_snap dump;               /* the snapshot the dump drew */
    unsigned match, total;        /* the dump's scene vertices found in it, and examined */
    unsigned known;               /* the examined ones whose moby identity it holds */
    int armed;
    rhi_device *dev;
    rhi_pipeline *pipe;
    rhi_buffer *vb;
    rhi_storage *st;
    size_t st_bytes;
    rhi_texture *rt;
    uint32_t h;
} s_gs = { .on = -1 };

int openpete_gsab_on(void)
{
    if (s_gs.on < 0) {
        const char *e = getenv("OPENPETE_MOBY_GSTAGE_AB");
        const char *c = getenv("OPENPETE_MOBY_GSTAGE_AB_NEGCTL");
        s_gs.negctl = c && *c && *c != '0';
        s_gs.on = e && *e && *e != '0';
    }
    return s_gs.on;
}

/** @brief Allocate the slot arrays on first use.
 * @return 1 when they exist, else 0. */
static int gsab_slots(void)
{
    if (s_gs.inst && s_gs.vert) return 1;
    s_gs.inst = calloc(OPENPETE_GSAB_PROJ_CAP, sizeof *s_gs.inst);
    s_gs.vert = calloc(OPENPETE_GSAB_PROJ_CAP, sizeof *s_gs.vert);
    return s_gs.inst && s_gs.vert;
}

openpete_gsab_inst *openpete_gsab_rec_at(int idx)
{
    if (!openpete_gsab_on() || idx < 0 || idx >= OPENPETE_GSAB_PROJ_CAP || !gsab_slots()) return NULL;
    return &s_gs.inst[idx];
}

openpete_gsab_inst *openpete_gsab_rec_open(int idx)
{
    openpete_gsab_inst *I = openpete_gsab_rec_at(idx);
    if (!I) return NULL;
    memset(I, 0, sizeof *I);
    I->serial = ++s_gs.serial;
    return I;
}

openpete_gsab_vert *openpete_gsab_vert_at(int idx)
{
    if (!openpete_gsab_on() || idx < 0 || idx >= OPENPETE_GSAB_PROJ_CAP || !gsab_slots()) return NULL;
    return &s_gs.vert[idx];
}

/** @brief Grow an array to hold at least need elements, by half again its size.
 * @param p The array pointer, replaced on growth.
 * @param cap Its capacity in elements, updated on growth.
 * @param need The elements it must hold.
 * @param elem Bytes per element.
 * @return 1 when it holds them, else 0. */
static int gsab_grow(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return 1;
    int nc = *cap < 1024 ? 1024 : *cap;
    while (nc < need) nc += nc / 2;
    void *q = realloc(*p, (size_t)nc * elem);
    if (!q) return 0;
    *p = q; *cap = nc;
    return 1;
}

/** @brief Return the snapshot being built for an epoch, closing the previous one into the ring when the epoch changed.
 *
 * Caller holds the lock.
 * @param epoch The scene epoch.
 * @return The snapshot. */
static gsab_snap *gsab_snap_for(unsigned epoch)
{
    gsab_snap *S = &s_gs.ring[s_gs.cur];
    if (S->valid && S->epoch == epoch) return S;
    s_gs.cur = (s_gs.cur + 1) % GSAB_RING;
    S = &s_gs.ring[s_gs.cur];
    S->valid = 1; S->epoch = epoch; S->nrec = 0; S->nvert = 0; S->skipped = 0;
    return S;
}

void openpete_gsab_commit(unsigned epoch, int idx, int n, uint32_t mid, uint32_t flags)
{
    if (!openpete_gsab_on() || n <= 0 || idx < 0 || idx + n > OPENPETE_GSAB_PROJ_CAP || !gsab_slots())
        return;
    SDL_Mutex *mx = openpete_mutex_once(&s_gs.mx);
    SDL_LockMutex(mx);
    gsab_snap *S = gsab_snap_for(epoch);
    const openpete_gsab_inst *I = &s_gs.inst[idx];
    if (I->serial == 0u || I->n != n) {
        S->skipped++;
    } else if (gsab_grow((void **)&S->rec, &S->crec, S->nrec + 1, sizeof *S->rec) &&
               gsab_grow((void **)&S->vert, &S->cvert, S->nvert + n, sizeof *S->vert)) {
        gsab_rec *R = &S->rec[S->nrec++];
        R->inst = *I;
        R->inst.flags |= flags;
        R->mid = mid;
        R->first = S->nvert;
        memcpy(&S->vert[S->nvert], &s_gs.vert[idx], (size_t)n * sizeof *S->vert);
        S->nvert += n;
    }
    SDL_UnlockMutex(mx);
}

/** @brief Order two scene vertex keys by identity alone.
 * @param a The first key.
 * @param b The second key.
 * @return Negative, zero or positive. */
static int gsab_mid_cmp(const void *a, const void *b)
{
    const gsab_key *p = a, *q = b;
    return p->mid == q->mid ? 0 : (p->mid < q->mid ? -1 : 1);
}

/** @brief Order two scene vertex keys by identity, then x bits, then y bits.
 * @param a The first key.
 * @param b The second key.
 * @return Negative, zero or positive. */
static int gsab_key_cmp(const void *a, const void *b)
{
    const gsab_key *p = a, *q = b;
    if (p->mid != q->mid) return p->mid < q->mid ? -1 : 1;
    if (p->x != q->x) return p->x < q->x ? -1 : 1;
    return p->y == q->y ? 0 : (p->y < q->y ? -1 : 1);
}

/** @brief Return the bits of a float.
 * @param f The value.
 * @return Its bits. */
static uint32_t gsab_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/** @brief Count the presented scene's moby vertices whose (identity, screen x, screen y) a snapshot holds.
 *
 * A snapshot vertex's screen position is the float the walk copies into the
 * scene, so a scene vertex the projection emitted unclipped matches bit for
 * bit. A vertex in the GTE divide-overflow regime is also keyed at the
 * position the near clip emits for an uncut corner, its homogeneous position
 * over W in float (hx / tvz, hy / tvz, each rounded to float first), so a
 * record whose every face takes the near clip still matches; near-cut
 * corners match nothing.
 * @param S The snapshot.
 * @param tris The scene's triangles.
 * @param n Their count.
 * @param stride Bytes between triangles.
 * @param total Receives the scene vertices with a moby identity.
 * @param known Receives those whose identity the snapshot holds.
 * @return The matching scene vertices. */
static unsigned gsab_match(const gsab_snap *S, const void *tris, int n, size_t stride, unsigned *total,
                           unsigned *known)
{
    *total = 0; *known = 0;
    gsab_key *k = S->nvert > 0 ? malloc(2u * (size_t)S->nvert * sizeof *k) : NULL;
    if (!k) return 0;
    size_t nk = 0;
    for (int r = 0; r < S->nrec; r++)
        for (int j = 0; j < S->rec[r].inst.n; j++) {
            const openpete_gsab_vert *V = &S->vert[S->rec[r].first + j];
            k[nk++] = (gsab_key){ S->rec[r].mid, gsab_bits((float)V->sx), gsab_bits((float)V->sy) };
            if (V->unr)
                k[nk++] = (gsab_key){ S->rec[r].mid, gsab_bits((float)V->hx / (float)V->tvz),
                                      gsab_bits((float)V->hy / (float)V->tvz) };
        }
    qsort(k, nk, sizeof *k, gsab_key_cmp);
    unsigned hit = 0;
    for (int t = 0; t < n; t++) {
        const openpete_scene_tri *T = (const openpete_scene_tri *)((const char *)tris + (size_t)t * stride);
        if (T->mid == 0u) continue;
        for (int v = 0; v < 3; v++) {
            const gsab_key q = { T->mid, gsab_bits(T->v[v].sx), gsab_bits(T->v[v].sy) };
            (*total)++;
            hit += bsearch(&q, k, nk, sizeof *k, gsab_key_cmp) != NULL;
            *known += bsearch(&q, k, nk, sizeof *k, gsab_mid_cmp) != NULL;
        }
    }
    free(k);
    return hit;
}

/** @brief Copy a snapshot into the dump's own storage.
 * @param S The snapshot.
 * @return 1 when copied, else 0. */
static int gsab_copy_dump(const gsab_snap *S)
{
    gsab_snap *D = &s_gs.dump;
    if (!gsab_grow((void **)&D->rec, &D->crec, S->nrec, sizeof *D->rec) ||
        !gsab_grow((void **)&D->vert, &D->cvert, S->nvert, sizeof *D->vert))
        return 0;
    if (S->nrec) memcpy(D->rec, S->rec, (size_t)S->nrec * sizeof *D->rec);
    if (S->nvert) memcpy(D->vert, S->vert, (size_t)S->nvert * sizeof *D->vert);
    D->nrec = S->nrec; D->nvert = S->nvert; D->epoch = S->epoch; D->skipped = S->skipped;
    D->valid = 1;
    return 1;
}

/** @brief Pick the snapshot the presented scene shows and copy it into the dump.
 * @param tris The scene's triangles.
 * @param n Their count.
 * @param stride Bytes between triangles.
 * @return 1 when a snapshot was copied, else 0. */
static int gsab_pick(const void *tris, int n, size_t stride)
{
    SDL_Mutex *mx = openpete_mutex_once(&s_gs.mx);
    SDL_LockMutex(mx);
    int best = -1;
    unsigned bm = 0, bt = 0, bk = 0;
    for (int i = 0; i < GSAB_RING; i++) {
        if (!s_gs.ring[i].valid || s_gs.ring[i].nvert == 0) continue;
        unsigned tot = 0, kn = 0;
        const unsigned m = gsab_match(&s_gs.ring[i], tris, n, stride, &tot, &kn);
        if (best < 0 || m > bm) { best = i; bm = m; bt = tot; bk = kn; }
    }
    for (int i = 0; i < GSAB_RING; i++)
        if (s_gs.ring[i].valid)
            fprintf(stderr, "[moby-gstage] ring epoch=%u records=%d vertices=%d%s\n", s_gs.ring[i].epoch,
                    s_gs.ring[i].nrec, s_gs.ring[i].nvert, i == best ? " picked" : "");
    const int ok = best >= 0 && gsab_copy_dump(&s_gs.ring[best]);
    SDL_UnlockMutex(mx);
    s_gs.match = bm; s_gs.total = bt; s_gs.known = bk;
    if (!ok) s_gs.dump.nrec = s_gs.dump.nvert = 0;
    return ok;
}

/** @brief Write one record's instance terms as MOBY_INST_VEC4 vec4s (moby_stage.glsl moby_inst_unpack).
 * @param I The record.
 * @param q Receives 4 * GSAB_INST_VEC4 floats. */
static void gsab_pack_inst(const openpete_gsab_inst *I, float *q)
{
    memset(q, 0, 4u * GSAB_INST_VEC4 * sizeof *q);
    for (int r = 0; r < 3; r++) {
        q[4 * r + 0] = I->M[r][0]; q[4 * r + 1] = I->M[r][1]; q[4 * r + 2] = I->M[r][2];
        q[4 * r + 3] = I->TR[r];
    }
    q[12] = I->ofx; q[13] = I->ofy; q[14] = I->H; q[15] = I->H_canon;
    q[16] = I->div_cap; q[17] = I->unit; q[18] = I->depth_k;
    q[20] = I->tie_pull; q[21] = I->lift; q[22] = I->rho; q[23] = I->lift2;
    q[24] = I->alpha;
    memcpy(&q[25], &I->flags, sizeof I->flags);
    q[26] = I->TR_lo[0]; q[27] = I->TR_lo[1]; q[28] = I->TR_lo[2];
}

/** @brief Fill the stage's storage with the dump's records, growing it when it is short.
 * @param dev Device.
 * @return The vertex records' first vec4, or -1 on failure. */
static long gsab_upload(rhi_device *dev)
{
    const gsab_snap *D = &s_gs.dump;
    const size_t vbase = (size_t)D->nrec * GSAB_INST_VEC4;
    const size_t bytes = (vbase + (size_t)D->nvert * GSAB_VERT_VEC4) * 16u;
    if (!s_gs.st || bytes > s_gs.st_bytes) {
        if (s_gs.st) { rhi_wait_idle(dev); rhi_storage_destroy(dev, s_gs.st); }
        s_gs.st_bytes = bytes + bytes / 2u;
        s_gs.st = rhi_storage_create(dev, s_gs.st_bytes);
        if (!s_gs.st) { s_gs.st_bytes = 0; return -1; }
    }
    float *q = rhi_storage_map(s_gs.st);
    if (!q) return -1;
    for (int r = 0; r < D->nrec; r++) {
        gsab_pack_inst(&D->rec[r].inst, q + (size_t)r * 4u * GSAB_INST_VEC4);
        for (int j = 0; j < D->rec[r].inst.n; j++) {
            const openpete_gsab_vert *V = &D->vert[D->rec[r].first + j];
            float *w = q + (vbase + (size_t)(D->rec[r].first + j) * GSAB_VERT_VEC4) * 4u;
            const uint32_t ri = (uint32_t)r;
            w[0] = V->c[0]; w[1] = V->c[1]; w[2] = V->c[2]; memcpy(&w[3], &ri, sizeof ri);
            w[4] = V->p[0]; w[5] = V->p[1]; w[6] = V->p[2]; w[7] = 0.0f;
        }
    }
    rhi_storage_commit(s_gs.st);
    return (long)vbase;
}

/** @brief Make the readback target hold both arms of nvert elements and the pipeline and dummy vertex buffer exist.
 * @param dev Device.
 * @param nvert Elements per arm.
 * @return 1 when ready, else 0. */
static int gsab_resources(rhi_device *dev, int nvert)
{
    const uint64_t rows = (2ull * GSAB_PX * (uint64_t)nvert + GSAB_RB_W - 1u) / GSAB_RB_W;
    if (rows == 0 || rows > rhi_max_image_dimension(dev)) return 0;
    if (!s_gs.rt || s_gs.h < rows) {
        if (s_gs.rt) rhi_destroy_texture_deferred(dev, s_gs.rt);
        s_gs.h = (uint32_t)((rows + 63u) & ~63ull);
        s_gs.rt = rhi_create_render_target_format(dev, GSAB_RB_W, s_gs.h, RHI_COLOR_FORMAT_RGBA32_UINT);
    }
    if (!s_gs.pipe) {
        const rhi_pipeline_desc d = {
            .vert_spv = "moby_rb.vert.spv", .frag_spv = "terrain_readback.frag.spv",
            .blend = RHI_BLEND_NONE, .ssbo_set2 = true, .push_vertex = true,
            .push_const_size = sizeof(gsab_pc),
            .topology = RHI_TOPOLOGY_POINT_LIST, .color_format = RHI_COLOR_FORMAT_RGBA32_UINT,
        };
        s_gs.pipe = rhi_create_pipeline(dev, &d);
    }
    if (!s_gs.vb) {
        const rhi_vertex zero[GSAB_PX] = { { 0 } };
        s_gs.vb = rhi_create_vertex_buffer(dev, sizeof zero);
        if (s_gs.vb) rhi_update_buffer(s_gs.vb, zero, sizeof zero);
    }
    return s_gs.rt && s_gs.pipe && s_gs.vb;
}

int openpete_gsab_dump_record(rhi_device *dev, const void *tris, int n, size_t stride)
{
    if (!openpete_gsab_on() || !dev) return 0;
    s_gs.dev = dev;
    s_gs.armed = 0;
    if (!gsab_pick(tris, n, stride) || s_gs.dump.nvert == 0) {
        fprintf(stderr, "[moby-gstage] no snapshot to draw (scene moby vertices=%u)\n", s_gs.total);
        s_gs.armed = 1;   /* the write records the empty dump: the tool calls it VACUOUS */
        return 0;
    }
    const long vbase = gsab_upload(dev);
    if (vbase < 0 || !gsab_resources(dev, s_gs.dump.nvert)) {
        fprintf(stderr, "[moby-gstage] storage, target or pipeline setup failed\n");
        return 0;
    }
    rhi_begin_render_target(dev, s_gs.rt, 0.0f, 0.0f, 0.0f, 0.0f);
    rhi_bind_pipeline(dev, s_gs.pipe);
    rhi_bind_vertex_buffer(dev, s_gs.vb);
    rhi_storage_bind(dev, s_gs.st, 2);
    for (int arm = 0; arm < 2; arm++) {
        const gsab_pc pc = { (uint32_t)vbase, s_gs.h, (uint32_t)(arm * s_gs.dump.nvert),
                             (arm == 1 || s_gs.negctl) ? 1.0f : 0.0f };
        rhi_push_constants(dev, &pc, sizeof pc);
        rhi_draw_instanced(dev, GSAB_PX, (uint32_t)s_gs.dump.nvert, 0, 0);
    }
    rhi_end_render_target(dev, s_gs.rt);
    s_gs.armed = 2;
    fprintf(stderr, "[moby-gstage] recorded records=%d vertices=%d scene_match=%u/%u known=%u epoch=%u negctl=%d\n",
            s_gs.dump.nrec, s_gs.dump.nvert, s_gs.match, s_gs.total, s_gs.known, s_gs.dump.epoch,
            s_gs.negctl);
    return 1;
}

/** @brief Write the dump's records and vertices, field by field, little-endian.
 * @param f The file.
 * @return 1 when every write succeeded, else 0. */
static int gsab_write_arm(FILE *f)
{
    const gsab_snap *D = &s_gs.dump;
    int ok = 1;
    for (int r = 0; r < D->nrec && ok; r++) {
        const openpete_gsab_inst *I = &D->rec[r].inst;
        float q[4 * GSAB_INST_VEC4];
        gsab_pack_inst(I, q);
        const uint32_t tail[3] = { D->rec[r].mid, (uint32_t)D->rec[r].first, (uint32_t)I->n };
        ok = fwrite(q, sizeof q, 1, f) == 1 && fwrite(tail, sizeof tail, 1, f) == 1;
    }
    for (int v = 0; v < D->nvert && ok; v++) {
        const openpete_gsab_vert *V = &D->vert[v];
        const float in[6] = { V->c[0], V->c[1], V->c[2], V->p[0], V->p[1], V->p[2] };
        const double ref[9] = { V->sx, V->sy, V->mac3, V->tvz, V->hx, V->hy, V->ex, V->ey, V->ez };
        const float dz[2] = { V->vz, V->kvz };
        const uint8_t fl[4] = { V->unr, V->sat, 0u, 0u };
        ok = fwrite(in, sizeof in, 1, f) == 1 && fwrite(ref, sizeof ref, 1, f) == 1 &&
             fwrite(dz, sizeof dz, 1, f) == 1 && fwrite(fl, sizeof fl, 1, f) == 1;
    }
    return ok;
}

int openpete_gsab_dump_write(const char *path)
{
    if (!s_gs.armed || !path) return 1;
    const int drawn = s_gs.armed == 2;
    s_gs.armed = 0;
    const gsab_snap *D = &s_gs.dump;
    const size_t px = drawn ? 2u * GSAB_PX * (size_t)D->nvert : 0u;
    uint32_t *tex = NULL;
    if (drawn) {
        const size_t bytes = (size_t)GSAB_RB_W * s_gs.h * 16u;
        tex = malloc(bytes);
        if (!tex || !rhi_read_render_target_texels(s_gs.dev, s_gs.rt, tex, bytes, NULL, NULL)) {
            free(tex);
            fprintf(stderr, "[moby-gstage] read of the target failed\n");
            return -1;
        }
    }
    FILE *f = fopen(path, "wb");
    const uint32_t hdr[12] = { 0x4253474Du /* "MGSB" */, 1u, (uint32_t)D->nrec,
                               drawn ? (uint32_t)D->nvert : 0u, s_gs.match, s_gs.total,
                               (uint32_t)s_gs.negctl, D->epoch, (uint32_t)D->skipped,
                               GSAB_CTL_SCALE, s_gs.known, 0u };
    int ok = f && fwrite(hdr, sizeof hdr, 1, f) == 1;
    if (ok && drawn) ok = gsab_write_arm(f) && fwrite(tex, 16u, px, f) == px;
    if (f && fclose(f) != 0) ok = 0;
    free(tex);
    fprintf(stderr, "[moby-gstage] %s %s records=%d vertices=%d\n", ok ? "wrote" : "FAILED writing",
            path, D->nrec, drawn ? D->nvert : 0);
    return ok ? 0 : -1;
}

void openpete_gsab_shutdown(rhi_device *dev)
{
    if (!dev) return;
    if (s_gs.st) rhi_storage_destroy(dev, s_gs.st);
    if (s_gs.rt) rhi_destroy_texture(dev, s_gs.rt);
    if (s_gs.pipe) rhi_destroy_pipeline(dev, s_gs.pipe);
    if (s_gs.vb) rhi_destroy_buffer(dev, s_gs.vb);
    s_gs.st = NULL; s_gs.rt = NULL; s_gs.pipe = NULL; s_gs.vb = NULL;
    s_gs.st_bytes = 0; s_gs.h = 0; s_gs.armed = 0;
}
