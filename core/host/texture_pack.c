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
#include "openpete_mods.h" /* asset-overlay roots */
#include "openpete_native_vram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <SDL3/SDL.h>  /* SDL_GetBasePath — same discovery music_override uses */
#include <dirent.h>

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

/* ---- replacement pack state (Phase 3) ------------------------------------ */
/* The pack INDEX is built at init by parsing every PNG filename in the dir
 * (identity is fully encoded in the name); images load lazily on first
 * match. Matching is containment-based — see texture_pack.h. */
#define TP_PACK_MAX 8192
typedef struct {
    uint64_t thash, phash;
    int bpp;
    int bx, by, bw, bh;            /* the file's upload-relative texel box */
    int symbolic;                  /* named asset file (<asset>.png) — matched by
                                    * the game layer's texture_pack_annotate name;
                                    * covers the FULL upload (Phase 7) */
    unsigned char *px;             /* lazy; NULL until first match */
    int w, h;
    int load_failed;
    unsigned char src;             /* index into s_srcs (0 = base pack dir) */
    char name[96];                 /* filename; for symbolic: asset name (no .png) */
} tp_pack_entry;
static tp_pack_entry s_pack[TP_PACK_MAX];
static int  s_pack_n = 0;
static char s_repl_dir[512];
static int  s_repl_active = 0;
/* Layered sources (asset-overlay v1): [0] = the base pack dir; then one
 * per enabled mod with an assets/textures/replacements/ dir, in enabled
 * order. A later source shipping the SAME identity replaces the earlier
 * entry at scan time (last-writer-wins — the overlay's single priority
 * truth), logged for the resolved-asset assert. */
#define TP_SRC_MAX 17
static struct { char dir[560]; char label[64]; } s_srcs[TP_SRC_MAX];
static int s_src_n = 0;
static char s_dir_override[512];   /* --texpack-dir; empty = unset (see set_dir_override) */

void texture_pack_set_dir_override(const char *dir)
{
    if (dir && *dir) snprintf(s_dir_override, sizeof s_dir_override, "%s", dir);
    else             s_dir_override[0] = '\0';
}

/* memo: identity key -> pack entry index (or -1 = no match) */
#define TP_MATCH_SLOTS 8192
typedef struct { uint64_t key; int entry; } tp_match_slot;
static tp_match_slot s_match[TP_MATCH_SLOTS];

/* Scan one source dir into s_pack. A duplicate identity REPLACES the
 * existing entry in place (last-writer-wins across sources). Returns
 * entries touched (added or overridden). */
static int tp_scan_dir(int src)
{
    DIR *d = opendir(s_srcs[src].dir);
    if (!d) return 0;
    int touched = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot, ".png") != 0) continue;
        if ((int)strlen(de->d_name) >= (int)sizeof(s_pack[0].name)) continue;
        tp_pack_entry tmp;
        unsigned long long th, ph;
        int bx, by, bw, bh, bpp;
        memset(&tmp, 0, sizeof tmp);
        if (sscanf(de->d_name, "%16llx-%dx%d+%dx%d-%16llx-%dbpp.png",
                   &th, &bx, &by, &bw, &bh, &ph, &bpp) == 7) {
            tmp.thash = th; tmp.phash = ph; tmp.bpp = bpp;
            tmp.bx = bx; tmp.by = by; tmp.bw = bw; tmp.bh = bh;
            snprintf(tmp.name, sizeof tmp.name, "%s", de->d_name);
        } else {
            /* Symbolic entry: <asset>.png, matched against the game layer's
             * texture_pack_annotate name; covers the full upload. */
            tmp.symbolic = 1;
            snprintf(tmp.name, sizeof tmp.name, "%.*s",
                     (int)(dot - de->d_name), de->d_name);
        }
        tmp.src = (unsigned char)src;
        /* Same identity from an earlier source? Replace (later = higher
         * priority) and say so — the resolved-asset table in log form. */
        int hit = -1;
        for (int i = 0; i < s_pack_n; i++) {
            if (s_pack[i].symbolic != tmp.symbolic) continue;
            if (tmp.symbolic ? (strcmp(s_pack[i].name, tmp.name) == 0)
                             : (s_pack[i].thash == tmp.thash &&
                                s_pack[i].phash == tmp.phash &&
                                s_pack[i].bpp == tmp.bpp &&
                                s_pack[i].bx == tmp.bx &&
                                s_pack[i].by == tmp.by &&
                                s_pack[i].bw == tmp.bw &&
                                s_pack[i].bh == tmp.bh)) { hit = i; break; }
        }
        if (hit >= 0) {
            fprintf(stderr, "[texpack] %s overrides \"%s\" (was %s)\n",
                    s_srcs[src].label, tmp.name,
                    s_srcs[s_pack[hit].src].label);
            if (s_pack[hit].px) { /* identity re-resolved: drop stale pixels */
                stbi_image_free(s_pack[hit].px);
            }
            tmp.w = tmp.h = 0;
            s_pack[hit] = tmp;
            touched++;
        } else if (s_pack_n < TP_PACK_MAX) {
            s_pack[s_pack_n++] = tmp;
            touched++;
        }
    }
    closedir(d);
    return touched;
}

static void tp_repl_scan(void)
{
    s_repl_active = 0; s_pack_n = 0;
    memset(s_match, 0, sizeof s_match);
    /* Precedence: --texpack-dir override > OPENPETE_TEXPACK_DIR env > built-in dir. */
    const char *env = getenv("OPENPETE_TEXPACK_DIR");
    if (s_dir_override[0]) {
        snprintf(s_repl_dir, sizeof s_repl_dir, "%s", s_dir_override);
    } else if (env && *env) {
        snprintf(s_repl_dir, sizeof s_repl_dir, "%s", env);
    } else {
        const char *base = SDL_GetBasePath();
        if (!base) return;
        snprintf(s_repl_dir, sizeof s_repl_dir, "%sassets/textures/replacements", base);
    }
    s_src_n = 0;
    snprintf(s_srcs[0].dir, sizeof s_srcs[0].dir, "%s", s_repl_dir);
    snprintf(s_srcs[0].label, sizeof s_srcs[0].label, "base pack");
    s_src_n = 1;
    tp_scan_dir(0);
    /* Mod asset roots, enabled order = priority (later wins). */
    int n_roots = openpete_mods_asset_root_count();
    for (int r = 0; r < n_roots && s_src_n < TP_SRC_MAX; r++) {
        const char* owner = NULL;
        const char* root = openpete_mods_asset_root(r, &owner);
        if (!root) continue;
        int si = s_src_n;
        snprintf(s_srcs[si].dir, sizeof s_srcs[si].dir,
                 "%s/textures/replacements", root);
        snprintf(s_srcs[si].label, sizeof s_srcs[si].label, "mod %s",
                 owner ? owner : "?");
        int before = s_pack_n;
        s_src_n++;
        int added = tp_scan_dir(si);
        if (added > 0 || s_pack_n != before)
            fprintf(stderr, "[texpack] + %s: %d entr%s (%s)\n",
                    s_srcs[si].label, added, added == 1 ? "y" : "ies",
                    s_srcs[si].dir);
    }
    s_repl_active = s_pack_n > 0;
    if (s_repl_active)
        fprintf(stderr, "[texpack] replacement pack active: %d entries "
                "(%d source%s)\n", s_pack_n, s_src_n,
                s_src_n == 1 ? "" : "s");
}

void texture_pack_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    tp_repl_scan();
}
void texture_pack_shutdown(void)
{
    if (getenv("OPENPETE_TEXPACK_DUMP_REGISTRY")) texture_pack_debug_dump();
    for (int i = 0; i < s_pack_n; i++)
        if (s_pack[i].px) { stbi_image_free(s_pack[i].px); s_pack[i].px = NULL; }
    s_pack_n = 0;
    s_ent_n = 0; s_annot_n = 0; s_gen++; s_inited = 0; s_repl_active = 0;
}

int texture_pack_replacements_active(void) { return s_repl_active; }

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

/* Palette hash: XXH3 over the full bpp-addressable CLUT row (16 entries for
 * 4bpp, 256 for 8bpp) read from the live VRAM model. NOT trimmed to the
 * query's used index range — that made phash a function of the QUERY BOX
 * (each clipped tri scans a different index range), so identical tiles
 * produced different identities per frame and containment matching fell
 * apart (falsified 2026-07-04; terrain never matched while rigid-box quads
 * did). The emulator-world "trim to used range" lesson guards against
 * nondeterministic garbage beyond the used indices — but this registry's
 * VRAM model is fed only by deterministic uploads, so the full row is
 * stable. 16bpp has no palette (phash 0). */
static uint64_t tp_clut_hash(int bpp, int clut_x, int clut_y)
{
    if (bpp == 16) return 0;
    const uint16_t *vr = openpete_vram_data();
    const uint16_t *clut = vr + (size_t)clut_y * OPENPETE_VRAM_W + clut_x;
    return XXH3_64bits(clut, (bpp == 8 ? 256 : 16) * 2);
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
    id.phash = tp_clut_hash(bpp, clut_x, clut_y);
    id.bpp   = bpp;
    /* Box relative to the upload origin, in TEXEL units of this bpp. */
    int shift = (bpp == 16) ? 0 : (bpp == 8) ? 1 : 2;
    id.box_x = ((tpage_x - e->x) << shift) + u0;   /* page origin offset + u */
    id.box_y = (tpage_y - e->y) + v0;
    id.box_w = u1 - u0 + 1;
    id.box_h = v1 - v0 + 1;
    id.up_w  = e->w << shift;
    id.up_h  = e->h;
    id.asset = e->asset;
    sl->id = id; sl->hit = 1;
    *out = id;
    return 1;
}

/* ---- identity key / filename (shared: dump + replacement lookup) ---------- */

static uint64_t tp_id_key(const texture_pack_id *id)
{
    uint64_t key = id->thash ^ (id->phash * 0x9E3779B97F4A7C15ull) ^
                   ((uint64_t)(uint32_t)((id->box_x << 20) ^ (id->box_y << 8) ^
                                         (id->box_w << 26) ^ id->box_h ^ id->bpp) << 1);
    return key ? key : 1;
}

uint64_t texture_pack_id_hashkey(const texture_pack_id *id) { return tp_id_key(id); }

static void tp_id_filename(const texture_pack_id *id, const char *dir,
                           char *buf, size_t n)
{
    snprintf(buf, n, "%s/%016llx-%dx%d+%dx%d-%016llx-%dbpp.png",
             dir, (unsigned long long)id->thash, id->box_x, id->box_y,
             id->box_w, id->box_h, (unsigned long long)id->phash, id->bpp);
}

/* ---- replacement lookup (Phase 3) ----------------------------------------- */

int texture_pack_match(const texture_pack_id *id, texture_pack_repl *out)
{
    if (!s_repl_active || !id) return 0;
    uint64_t key = tp_id_key(id);
    uint64_t base = key * 0xff51afd7ed558ccdull >> 32;
    tp_match_slot *m = NULL;
    for (int p = 0; p < 16; p++) {
        tp_match_slot *c = &s_match[(base + (uint64_t)p) & (TP_MATCH_SLOTS - 1)];
        if (c->key == key || c->key == 0) { m = c; break; }
    }
    int ei;
    if (m && m->key == key) {
        ei = m->entry;                          /* memoized (incl. -1 = no match) */
    } else {
        /* Symbolic first (plan: asset-ID primary): an annotated upload whose
         * name matches a <asset>.png entry. Falls back to hash containment:
         * same thash/bpp/phash, file box contains the query box; smallest
         * containing file wins. */
        ei = -1;
        long best = 0;
        if (id->asset) {
            for (int i = 0; i < s_pack_n; i++)
                if (s_pack[i].symbolic && strcmp(s_pack[i].name, id->asset) == 0) {
                    ei = i;
                    break;
                }
        }
        if (ei < 0) for (int i = 0; i < s_pack_n; i++) {
            const tp_pack_entry *e = &s_pack[i];
            if (e->symbolic) continue;
            if (e->thash != id->thash || e->bpp != id->bpp || e->phash != id->phash)
                continue;
            if (id->box_x < e->bx || id->box_y < e->by ||
                id->box_x + id->box_w > e->bx + e->bw ||
                id->box_y + id->box_h > e->by + e->bh) continue;
            long area = (long)e->bw * e->bh;
            if (ei < 0 || area < best) { ei = i; best = area; }
        }
        if (m) { m->key = key; m->entry = ei; }
    }
    if (ei < 0) return 0;
    tp_pack_entry *e = &s_pack[ei];
    if (!e->px && !e->load_failed) {            /* lazy first load */
        char path[704];
        snprintf(path, sizeof path, "%s/%s%s", s_srcs[e->src].dir, e->name,
                 e->symbolic ? ".png" : "");
        int comp = 0;
        e->px = stbi_load(path, &e->w, &e->h, &comp, 4);
        if (!e->px) e->load_failed = 1;
        else fprintf(stderr, "[texpack] replacement loaded %dx%d: %s%s\n",
                     e->w, e->h, e->name, e->symbolic ? ".png (symbolic)" : "");
    }
    if (!e->px) return 0;
    out->pixels = e->px; out->w = e->w; out->h = e->h;
    if (e->symbolic) {
        /* A symbolic file covers the FULL upload in this draw's bpp texels. */
        out->box_x = 0; out->box_y = 0;
        out->box_w = id->up_w; out->box_h = id->up_h;
    } else {
        out->box_x = e->bx; out->box_y = e->by;
        out->box_w = e->bw; out->box_h = e->bh;
    }
    return 1;
}

/* ---- dump tool (Phase 2) -------------------------------------------------- */

int stbi_write_png(const char *filename, int w, int h, int comp,
                   const void *data, int stride_in_bytes);   /* impl: screenshot.c TU */

/* Dump granularity: per-tri query boxes explode into clipped variants
 * (11k files/run measured), so the dump accumulates a BOX UNION per
 * (thash, phash, bpp) — converging to the full tile / glyph-strip / sprite
 * box — and (re)writes that union's file whenever it grows. Containment
 * matching consumes the union naturally. */
#define TP_UNION_SLOTS 16384   /* power of 2 */
typedef struct {
    uint64_t key;
    int bx, by, bw, bh;        /* union box, upload-relative texels */
    char name[96];             /* last written filename (unlinked on growth) */
} tp_union;
static tp_union s_union[TP_UNION_SLOTS];

void texture_pack_dump_draw(const texture_pack_id *id,
                            int tpage_x, int tpage_y, int clut_x, int clut_y,
                            int u0, int v0)
{
    static const char *dir = NULL;
    static int checked = 0;
    if (!checked) { checked = 1; dir = getenv("OPENPETE_TEXPACK_DUMP_DIR"); }
    if (!dir || !*dir || !id) return;

    uint64_t key = id->thash ^ (id->phash * 0x9E3779B97F4A7C15ull) ^ (uint64_t)id->bpp;
    if (!key) key = 1;
    uint64_t base = key * 0xff51afd7ed558ccdull >> 32;
    tp_union *u = NULL;
    for (int p = 0; p < 16; p++) {
        tp_union *c = &s_union[(base + (uint64_t)p) & (TP_UNION_SLOTS - 1)];
        if (c->key == key || c->key == 0) { u = c; break; }
    }
    if (!u) return;                             /* neighbourhood full */
    int grew = 0;
    if (u->key != key) {                        /* first sighting */
        u->key = key; u->name[0] = 0;
        u->bx = id->box_x; u->by = id->box_y; u->bw = id->box_w; u->bh = id->box_h;
        grew = 1;
    } else {
        int x1 = u->bx + u->bw, y1 = u->by + u->bh;
        int qx1 = id->box_x + id->box_w, qy1 = id->box_y + id->box_h;
        if (id->box_x < u->bx) { u->bx = id->box_x; grew = 1; }
        if (id->box_y < u->by) { u->by = id->box_y; grew = 1; }
        if (qx1 > x1) { x1 = qx1; grew = 1; }
        if (qy1 > y1) { y1 = qy1; grew = 1; }
        u->bw = x1 - u->bx; u->bh = y1 - u->by;
    }
    if (!grew) return;

    /* Decode the union box. This draw anchors page<->upload coords:
     * page coord of union origin = query page origin - (query - union) box
     * offset. u may walk beyond this tpage's 256-texel window — that is
     * correct: pages are x-offsets into the same VRAM rows, and the upload
     * is contiguous there. */
    int pu0 = u0 - (id->box_x - u->bx);
    int pv0 = v0 - (id->box_y - u->by);
    int W = u->bw, H = u->bh;
    const uint16_t *vr = openpete_vram_data();
    unsigned char *px = malloc((size_t)W * H * 4);
    if (!px) return;
    for (int y = 0; y < H; y++) {
        const uint16_t *row = vr + (size_t)(tpage_y + pv0 + y) * OPENPETE_VRAM_W;
        for (int x = 0; x < W; x++) {
            int uu = pu0 + x;
            uint16_t p;
            if (id->bpp == 16) {
                p = row[tpage_x + uu];
            } else if (id->bpp == 8) {
                unsigned idx = (row[tpage_x + (uu >> 1)] >> (8 * (uu & 1))) & 0xFFu;
                p = vr[(size_t)clut_y * OPENPETE_VRAM_W + clut_x + idx];
            } else {
                unsigned idx = (row[tpage_x + (uu >> 2)] >> (4 * (uu & 3))) & 0xFu;
                p = vr[(size_t)clut_y * OPENPETE_VRAM_W + clut_x + idx];
            }
            unsigned char *o = px + ((size_t)y * W + x) * 4;
            unsigned r5 = p & 31u, g5 = (p >> 5) & 31u, b5 = (p >> 10) & 31u;
            o[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
            o[1] = (unsigned char)((g5 << 3) | (g5 >> 2));
            o[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
            /* Alpha encoding (Phase 5): 0x0000 sentinel -> transparent;
             * STP-set texels -> 128 (on the HD alpha pipe, src-over at 0.5
             * reproduces the PSX (B+F)/2; the cutout pipe keeps them: 128
             * passes its 0.5 discard); plain texels -> opaque. */
            o[3] = (p == 0) ? 0 : ((p & 0x8000u) ? 128 : 255);
        }
    }
    if (u->name[0]) {                           /* superseded smaller union */
        char old[640];
        snprintf(old, sizeof old, "%s/%s", dir, u->name);
        remove(old);
    }
    snprintf(u->name, sizeof u->name, "%016llx-%dx%d+%dx%d-%016llx-%dbpp.png",
             (unsigned long long)id->thash, u->bx, u->by, u->bw, u->bh,
             (unsigned long long)id->phash, id->bpp);
    char path[740];
    snprintf(path, sizeof path, "%s/%s", dir, u->name);
    stbi_write_png(path, W, H, 4, px, W * 4);
    free(px);
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
