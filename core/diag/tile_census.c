/* tile_census.c - tile tallies, their counter names and the capture sidecar.
 *
 * The renderer fills a tc_tally_t from the scene a capture presents and asks
 * this module to name its counts (the names and their meaning are the rows
 * of tile_census.def) and write them beside the capture's PNG. The counter
 * table is sized at build time; a name that does not fit marks the sidecar
 * failed, and a failed sidecar is not written, so the capture harness fails
 * the leg on the missing file instead of reading a partial one.
 *
 * Host-side only; nothing here reads or writes PSX RAM.
 */
#include "openpete_tile_census.h"
#include "openpete_atomic_file.h"
#include "openpete_log.h"
#include "openpete_thread_roles.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The registry's generation and the class bits' counter names are what this
 * module needs from it; every other row is read by scripts/golden-census.py. */
#define TC_GEN(g) enum { TC_REGISTRY_GEN = (g) };
#define TC_PATH(name)
#define TC_FAMILY(name)
#define TC_ROW(id, name, unit, source, family, since, state, meaning)
#define TC_BIT(bit, name)
#define TC_LEGVAR(pattern)
#include "tile_census.def"
#undef TC_GEN
#undef TC_BIT
#define TC_GEN(g)
#define TC_BIT(bit, name) [bit] = name,
/* Counter name of each class bit, indexed by bit; NULL for an unnamed bit. */
static const char *const k_cbit_names[TC_CBITS] = {
#include "tile_census.def"
};
#undef TC_GEN
#undef TC_PATH
#undef TC_FAMILY
#undef TC_ROW
#undef TC_BIT
#undef TC_LEGVAR

#define TC_NAMES_MAX 2048   /* counters one sidecar holds */
#define TC_NAME_LEN  64

void tc_tally_clear(tc_tally_t *t)
{
    memset(t, 0, sizeof *t);
}

/** @brief Count one triangle of a moby class in the tally's open-addressed class table.
 *
 * The slot holds class + 1 so 0 marks it empty; a full table sets overflow.
 * @param t The tally.
 * @param cls The moby class (tag bits 15:0). */
static void tally_moby_class(tc_tally_t *t, uint32_t cls)
{
    const uint16_t key = (uint16_t)(cls + 1u);
    unsigned h = (cls * 2654435761u) & (TC_MOBY_TYPES - 1);
    for (int probe = 0; probe < TC_MOBY_TYPES; probe++) {
        if (t->mcls[h] == key) { t->mcnt[h]++; return; }
        if (t->mcls[h] == 0) { t->mcls[h] = key; t->mcnt[h] = 1; return; }
        h = (h + 1) & (TC_MOBY_TYPES - 1);
    }
    t->overflow = 1;
}

void tc_tally_tri(tc_tally_t *t, const tc_tri_t *tri)
{
    const uint32_t f = tri->tag >> 28;
    t->fk[f][openpete_tag_kind(tri->tag)]++;
    t->cat[(tri->category >= 0 && tri->category < TC_CATS - 1) ? tri->category : TC_CATS - 1]++;
    t->blend[(tri->blend >= 0 && tri->blend < TC_BLENDS - 1) ? tri->blend : TC_BLENDS - 1]++;
    for (int k = 0; k < TC_FLAGS; k++)
        t->flag[k] += (tri->flags >> k) & 1u;
    for (uint32_t b = tri->cbits; b; b &= b - 1u)
        t->cbit[__builtin_ctz(b)]++;
    if ((tri->cbits & OPENPETE_CB_TERRAIN_NEARCLIP) && tri->inview) t->ncut_view++;
    for (int k = 0; k < TC_PRIMS; k++) {
        const int on = (int)((tri->prims >> k) & 1u);
        if (on && !(t->prim_open[k] && tri->prim && t->prim_last[k] == tri->prim))
            t->prim[k]++;
        t->prim_open[k] = (uint8_t)on;
        t->prim_last[k] = tri->prim;
    }
    if (tri->prim) t->ui++;
    if (f == 1u) tally_moby_class(t, tri->tag & 0xFFFFu);
}

int tc_tri_in_view(const float x[3], const float y[3])
{
    float x0 = x[0], x1 = x[0], y0 = y[0], y1 = y[0];
    for (int k = 1; k < 3; k++) {
        x0 = x[k] < x0 ? x[k] : x0; x1 = x[k] > x1 ? x[k] : x1;
        y0 = y[k] < y0 ? y[k] : y0; y1 = y[k] > y1 ? y[k] : y1;
    }
    if (!(x1 >= -1.0f && x0 <= 1.0f && y1 >= -1.0f && y0 <= 1.0f)) return 0;
    const double area = ((double)x[1] - x[0]) * ((double)y[2] - y[0])
                      - ((double)x[2] - x[0]) * ((double)y[1] - y[0]);
    if (area == 0.0) return 0;
    static const float cx[4] = { -1.0f, 1.0f, 1.0f, -1.0f }, cy[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    for (int e = 0; e < 3; e++) {
        const int a = e, b = (e + 1) % 3;
        const double ex = (double)x[b] - x[a], ey = (double)y[b] - y[a];
        int outside = 0;
        for (int c = 0; c < 4; c++) {
            const double side = ex * ((double)cy[c] - y[a]) - ey * ((double)cx[c] - x[a]);
            outside += side * area < 0.0;   /* on the side away from the third corner */
        }
        if (outside == 4) return 0;
    }
    return 1;
}

/* The render-path mapping of tag families, registered by the game layer. */
static int (*s_path_of)(int family, int kind);
static const char *const *s_path_names;
static int s_path_count;

void openpete_native_register_tag_paths(int (*path_of)(int family, int kind),
                                        const char *const *names, int count)
{
    s_path_of = path_of;
    s_path_names = names;
    s_path_count = count;
}

/** @brief Name of a path index from the registered mapping.
 * @param p The path index.
 * @return The name, or NULL when the index names no registered path. */
static const char *path_name(int p)
{
    return (s_path_names && p >= 0 && p < s_path_count) ? s_path_names[p] : NULL;
}

int tile_census_on(void)
{
    static OP_ONCE_INT on = -1;
    int v = op_once_int_get(&on);
    if (v < 0) {
        const char *e = getenv("OPENPETE_TILE_CENSUS");
        v = op_once_int_fill(&on, (e && *e && strcmp(e, "0") != 0) ? 1 : 0);
    }
    return v;
}

/** @brief The tree the sidecar header names: OPENPETE_TILE_CENSUS when it is a 40-digit hex tree hash, else "unrecorded".
 * @return The tree text. */
static const char *census_tree(void)
{
    const char *e = getenv("OPENPETE_TILE_CENSUS");
    if (!e || strlen(e) != 40) return "unrecorded";
    for (const char *p = e; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return "unrecorded";
    return e;
}

/* The sidecar being assembled: name/value pairs in insertion order. */
static struct {
    char     name[TC_NAMES_MAX][TC_NAME_LEN];
    uint32_t val[TC_NAMES_MAX];
    int      n;
    char     why[160];
} s_sc;

void tc_sidecar_begin(void)
{
    s_sc.n = 0;
    s_sc.why[0] = '\0';
}

void tc_sidecar_fail(const char *why)
{
    if (!s_sc.why[0]) snprintf(s_sc.why, sizeof s_sc.why, "%s", why);
}

void tc_sidecar_add(const char *name, uint32_t v)
{
    if (!v) return;
    for (int i = 0; i < s_sc.n; i++)
        if (strcmp(s_sc.name[i], name) == 0) { s_sc.val[i] += v; return; }
    if (s_sc.n >= TC_NAMES_MAX || strlen(name) >= TC_NAME_LEN) {
        tc_sidecar_fail("counter table full");
        return;
    }
    snprintf(s_sc.name[s_sc.n], TC_NAME_LEN, "%s", name);
    s_sc.val[s_sc.n] = v;
    s_sc.n++;
}

/** @brief Add one formatted counter name and value to the sidecar.
 * @param v Value to add.
 * @param fmt printf format of the name. */
__attribute__((format(printf, 2, 3))) static void sc_addf(uint32_t v, const char *fmt, ...)
{
    if (!v) return;
    char name[TC_NAME_LEN + 16];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    tc_sidecar_add(name, v);
}

/** @brief Add a tally's tag counts: per render path, per family and kind, and per moby class.
 * @param t The tally.
 * @param pre The name prefix. */
static void sidecar_add_tags(const tc_tally_t *t, const char *pre)
{
    for (int f = 1; f < TC_FAMILIES; f++)
        for (int k = 0; k < (int)OPENPETE_TAG_KINDS; k++) {
            const uint32_t v = t->fk[f][k];
            const char *path = s_path_of ? path_name(s_path_of(f, k)) : NULL;
            const char *fam = s_path_of ? path_name(s_path_of(f, -1)) : NULL;
            if (!v) continue;
            if (path) sc_addf(v, "%s%s.tris", pre, path);
            if (fam) sc_addf(v, "%s%s.tris.k%d", pre, fam, k);
            else sc_addf(v, "%stag.unmapped.tris", pre);
        }
    for (int i = 0; i < TC_MOBY_TYPES; i++)
        if (t->mcls[i]) sc_addf(t->mcnt[i], "%smoby.type.%u", pre, (unsigned)t->mcls[i] - 1u);
}

void tc_sidecar_add_tally(const tc_tally_t *t, const char *prefix)
{
    static const char *const flag_names[TC_FLAGS] = {
        "shell", "key_order", "kfold", "portal_rim", "host_added", "fog_delta" };
    static const char *const prim_names[TC_PRIMS] = { "over_fade", "under_player", "carry" };
    static const char *const sec_names[4] = { "", "_hp", "_lp", "_near" };
    if (!s_path_of) tc_sidecar_fail("no render-path mapping registered");
    if (t->overflow) tc_sidecar_fail("more moby classes than the tally holds");
    sidecar_add_tags(t, prefix);
    for (int c = 0; c < TC_CATS; c++) sc_addf(t->cat[c], "%scat.%d.tris", prefix, c);
    for (int b = 0; b < TC_BLENDS; b++) sc_addf(t->blend[b], "%sblend.%d.tris", prefix, b);
    for (int k = 0; k < TC_FLAGS; k++) sc_addf(t->flag[k], "%sflag.%s.tris", prefix, flag_names[k]);
    for (int k = 0; k < TC_PRIMS; k++) sc_addf(t->prim[k], "%sflag.%s.prims", prefix, prim_names[k]);
    for (int k = 0; k < 4; k++)
        sc_addf(t->sec[k] > 0 ? (uint32_t)t->sec[k] : 0u, "%sterrain.sec.retained%s", prefix, sec_names[k]);
    for (int b = 0; b < TC_CBITS; b++) {
        if (t->cbit[b] && !k_cbit_names[b]) tc_sidecar_fail("a class bit with no registered counter");
        if (k_cbit_names[b]) sc_addf(t->cbit[b], "%s%s", prefix, k_cbit_names[b]);
    }
    /* The retained cache's cut fan triangles count with the walked ones the
     * class bit counts, so the near-clip counters mean the same on both paths. */
    sc_addf(t->ncut_cache, "%sterrain.nearclip.tris", prefix);
    sc_addf(t->ncut_view + t->ncut_cache_view, "%sterrain.nearclip.inview.tris", prefix);
    sc_addf(t->ui, "%sui.tris", prefix);
}

/** @brief qsort order of sidecar entries: by name, byte order.
 * @param a An index into the sidecar.
 * @param b Another index.
 * @return strcmp of the two names. */
static int sc_cmp(const void *a, const void *b)
{
    return strcmp(s_sc.name[*(const int *)a], s_sc.name[*(const int *)b]);
}

int tc_sidecar_write(const char *path)
{
    if (s_sc.why[0]) {
        OPL_ERROR_F("tile-census", "FAIL %s: %s; no sidecar written", path, s_sc.why);
        return -1;
    }
    static int order[TC_NAMES_MAX];
    for (int i = 0; i < s_sc.n; i++) order[i] = i;
    qsort(order, (size_t)s_sc.n, sizeof order[0], sc_cmp);
    const size_t cap = 96 + (size_t)s_sc.n * (TC_NAME_LEN + 16);
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t len = (size_t)snprintf(buf, cap, "# gen %d tree %s\n", TC_REGISTRY_GEN, census_tree());
    for (int i = 0; i < s_sc.n && len < cap; i++)
        len += (size_t)snprintf(buf + len, cap - len, "%s\t%u\n", s_sc.name[order[i]], s_sc.val[order[i]]);
    const int rc = op_atomic_write_file_ex(path, buf, len, OP_ATOMIC_NOSYNC);
    free(buf);
    if (rc != 0) OPL_ERROR_F("tile-census", "FAIL %s: write failed", path);
    return rc == 0 ? 0 : -1;
}
