/* texture_pack.c — see texture_pack.h.
 *
 * Registry model: one entry per live upload rect. A new load that overlaps a
 * live entry kills it (exact-rect re-upload replaces in place, keeping the
 * slot). Moves duplicate identity to the destination (translated); clears
 * kill covered entries. CLUTs are ordinary entries (their rects are CLUT
 * rows); the texture<->CLUT pairing happens at resolve time, where the used
 * index range and the trimmed palette hash are computed lazily from the live
 * native VRAM model (authoritative by construction — the draw samples that
 * same model) and cached against the registry generation.
 *
 * Hashing: XXH3-64 (vendored third_party/xxhash). The texture hash covers
 * the FULL upload source; sub-rect draws share it and carry their box in the
 * resolved id (openpete-texture-replacement plan: hash exactly what was
 * uploaded, never a guessed VRAM region). */
#include "texture_pack.h"
#include "openpete_native_vram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#define TP_MAX_ENTRIES   4096
#define TP_MAX_ANNOT     128
#define TP_RESOLVE_SLOTS 4096   /* open-addressed resolve cache (power of 2) */

typedef struct {
    int      alive;
    int      x, y, w, h;        /* VRAM 16-bit-pixel units */
    uint64_t thash;             /* XXH3-64 of the full upload source */
    const char *asset;          /* symbolic name or NULL */
} tp_entry;

typedef struct {
    int x, y, w, h;
    const char *name;
} tp_annot;

typedef struct {                /* resolve cache slot */
    uint64_t key;               /* packed material+box; 0 = empty */
    uint32_t gen;
    texture_pack_id id;
    int      hit;               /* 1 = resolves, 0 = negative entry */
} tp_slot;

static tp_entry s_ent[TP_MAX_ENTRIES];
static int      s_ent_n = 0;          /* high-water mark */
static tp_annot s_annot[TP_MAX_ANNOT];
static int      s_annot_n = 0;
static uint32_t s_gen = 1;            /* bumped on any registry mutation */
static tp_slot  s_cache[TP_RESOLVE_SLOTS];
static int      s_inited = 0;

void texture_pack_init(void)  { s_inited = 1; }
void texture_pack_shutdown(void)
{
    if (getenv("OPENPETE_TEXPACK_DUMP_REGISTRY")) texture_pack_debug_dump();
    s_ent_n = 0; s_annot_n = 0; s_gen++; s_inited = 0;
}

static int tp_overlap(const tp_entry *e, int x, int y, int w, int h)
{
    return e->alive && x < e->x + e->w && e->x < x + w &&
                       y < e->y + e->h && e->y < y + h;
}

static tp_entry *tp_alloc(void)
{
    for (int i = 0; i < s_ent_n; i++) if (!s_ent[i].alive) return &s_ent[i];
    if (s_ent_n < TP_MAX_ENTRIES) return &s_ent[s_ent_n++];
    /* Pool exhausted: drop the oldest slot (index 0 heuristic). Should not
     * happen in practice (level loads clear-invalidate); log once. */
    static int warned = 0;
    if (!warned) { warned = 1; fprintf(stderr, "[texpack] entry pool full\n"); }
    return &s_ent[0];
}

void texture_pack_on_load(int x, int y, int w, int h,
                          const uint16_t *src, int src_stride)
{
    if (w <= 0 || h <= 0 || !src) return;
    s_gen++;
    tp_entry *reuse = NULL;
    for (int i = 0; i < s_ent_n; i++) {
        tp_entry *e = &s_ent[i];
        if (!tp_overlap(e, x, y, w, h)) continue;
        if (e->x == x && e->y == y && e->w == w && e->h == h) reuse = e;
        else e->alive = 0;                 /* partial overwrite: identity gone */
    }
    tp_entry *e = reuse ? reuse : tp_alloc();
    e->alive = 1; e->x = x; e->y = y; e->w = w; e->h = h;
    if (src_stride == w) {
        e->thash = XXH3_64bits(src, (size_t)w * h * 2);
    } else {                               /* hash rows through the stride */
        XXH3_state_t st;
        XXH3_64bits_reset(&st);
        for (int i = 0; i < h; i++)
            XXH3_64bits_update(&st, src + (size_t)i * src_stride, (size_t)w * 2);
        e->thash = XXH3_64bits_digest(&st);
    }
    e->asset = NULL;
    for (int i = 0; i < s_annot_n; i++)
        if (s_annot[i].x == x && s_annot[i].y == y &&
            s_annot[i].w == w && s_annot[i].h == h) e->asset = s_annot[i].name;
}

void texture_pack_on_move(int src_x, int src_y, int w, int h,
                          int dst_x, int dst_y)
{
    if (w <= 0 || h <= 0) return;
    s_gen++;
    /* Kill identities the destination overwrites, then duplicate any entry
     * fully contained in the source rect, translated. (Partial moves shear a
     * texture — no stable identity; those entries just die at the dest.) */
    for (int i = 0; i < s_ent_n; i++)
        if (tp_overlap(&s_ent[i], dst_x, dst_y, w, h)) s_ent[i].alive = 0;
    int n = s_ent_n;                       /* don't walk entries we add */
    for (int i = 0; i < n; i++) {
        tp_entry *e = &s_ent[i];
        if (!e->alive) continue;
        if (e->x >= src_x && e->y >= src_y &&
            e->x + e->w <= src_x + w && e->y + e->h <= src_y + h) {
            tp_entry *d = tp_alloc();
            *d = *e;
            d->x = e->x - src_x + dst_x;
            d->y = e->y - src_y + dst_y;
        }
    }
}

void texture_pack_on_clear(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    s_gen++;
    for (int i = 0; i < s_ent_n; i++)
        if (tp_overlap(&s_ent[i], x, y, w, h)) s_ent[i].alive = 0;
}

void texture_pack_annotate(int x, int y, int w, int h, const char *name)
{
    if (s_annot_n >= TP_MAX_ANNOT) return;
    s_annot[s_annot_n++] = (tp_annot){ x, y, w, h, name };
    /* Retroactively name an already-live exact-rect entry. */
    for (int i = 0; i < s_ent_n; i++) {
        tp_entry *e = &s_ent[i];
        if (e->alive && e->x == x && e->y == y && e->w == w && e->h == h)
            e->asset = name;
    }
}

/* ---- resolve -------------------------------------------------------------- */

static const tp_entry *tp_cover(int x, int y, int w, int h)
{
    for (int i = 0; i < s_ent_n; i++) {
        const tp_entry *e = &s_ent[i];
        if (e->alive && x >= e->x && y >= e->y &&
            x + w <= e->x + e->w && y + h <= e->y + e->h) return e;
    }
    return NULL;
}

/* Trimmed palette hash: scan the draw's texel indices (from the live VRAM
 * model) for the used index range, then XXH3 the CLUT entries over exactly
 * that range. 16bpp has no palette (phash 0). */
static uint64_t tp_clut_hash(int bpp, int tpage_x, int tpage_y,
                             int clut_x, int clut_y,
                             int u0, int v0, int u1, int v1)
{
    if (bpp == 16) return 0;
    const uint16_t *vr = openpete_vram_data();
    unsigned lo = 255, hi = 0;
    for (int v = v0; v <= v1; v++) {
        const uint16_t *row = vr + (size_t)(tpage_y + v) * OPENPETE_VRAM_W;
        for (int u = u0; u <= u1; u++) {
            unsigned idx;
            if (bpp == 8) {
                idx = (row[tpage_x + (u >> 1)] >> (8 * (u & 1))) & 0xFFu;
            } else {
                idx = (row[tpage_x + (u >> 2)] >> (4 * (u & 3))) & 0xFu;
            }
            if (idx < lo) lo = idx;
            if (idx > hi) hi = idx;
        }
    }
    if (lo > hi) return 0;
    const uint16_t *clut = vr + (size_t)clut_y * OPENPETE_VRAM_W + clut_x;
    return XXH3_64bits(clut + lo, (size_t)(hi - lo + 1) * 2);
}

int texture_pack_resolve(int tpage_x, int tpage_y, int clut_x, int clut_y,
                         int bpp, int u0, int v0, int u1, int v1,
                         texture_pack_id *out)
{
    if (!s_inited || u1 < u0 || v1 < v0) return 0;
    /* Cache key: material + box packed. Coordinates all fit their fields
     * (VRAM is 1024x512, boxes are <=256 texels). */
    uint64_t key = ((uint64_t)(uint16_t)tpage_x << 48) ^ ((uint64_t)(uint16_t)tpage_y << 32) ^
                   ((uint64_t)(uint16_t)clut_x  << 16) ^  (uint64_t)(uint16_t)clut_y ^
                   ((uint64_t)(uint8_t)bpp << 56) ^
                   ((uint64_t)(uint8_t)u0 << 8) ^ ((uint64_t)(uint8_t)v0 << 16) ^
                   ((uint64_t)(uint8_t)(u1 - u0) << 24) ^ ((uint64_t)(uint8_t)(v1 - v0) << 40);
    if (!key) key = 1;
    tp_slot *sl = &s_cache[(key * 0x9E3779B97F4A7C15ull >> 32) & (TP_RESOLVE_SLOTS - 1)];
    if (sl->key == key && sl->gen == s_gen) {
        if (!sl->hit) return 0;
        *out = sl->id; return 1;
    }
    sl->key = key; sl->gen = s_gen; sl->hit = 0;

    /* The VRAM columns the texel box occupies depend on bpp packing. */
    int px0 = tpage_x, px1 = tpage_x;
    if (bpp == 16)     { px0 += u0;      px1 += u1; }
    else if (bpp == 8) { px0 += u0 >> 1; px1 += u1 >> 1; }
    else               { px0 += u0 >> 2; px1 += u1 >> 2; }
    const tp_entry *e = tp_cover(px0, tpage_y + v0, px1 - px0 + 1, v1 - v0 + 1);
    if (!e) return 0;

    texture_pack_id id;
    id.thash = e->thash;
    id.phash = tp_clut_hash(bpp, tpage_x, tpage_y, clut_x, clut_y, u0, v0, u1, v1);
    id.bpp   = bpp;
    /* Box relative to the upload origin, in TEXEL units of this bpp. */
    int shift = (bpp == 16) ? 0 : (bpp == 8) ? 1 : 2;
    id.box_x = ((tpage_x - e->x) << shift) + u0;   /* page origin offset + u */
    id.box_y = (tpage_y - e->y) + v0;
    id.box_w = u1 - u0 + 1;
    id.box_h = v1 - v0 + 1;
    id.asset = e->asset;
    sl->id = id; sl->hit = 1;
    *out = id;
    return 1;
}

void texture_pack_debug_dump(void)
{
    int live = 0;
    for (int i = 0; i < s_ent_n; i++) {
        const tp_entry *e = &s_ent[i];
        if (!e->alive) continue;
        live++;
        fprintf(stderr, "[texpack] entry %d,%d %dx%d thash=%016llx%s%s\n",
                e->x, e->y, e->w, e->h, (unsigned long long)e->thash,
                e->asset ? " asset=" : "", e->asset ? e->asset : "");
    }
    fprintf(stderr, "[texpack] registry: %d live / %d slots, gen %u\n",
            live, s_ent_n, s_gen);
}
