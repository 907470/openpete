/* tests/unit/tile_census_test.c - the tile census's class-bit, 2D-primitive
 * and near-cut counters on a synthetic scene.
 *
 * A tally of triangles carrying known class bits writes one counter per
 * bit, named by tile_census.def's TC_BIT rows, under the own and the
 * `backdrop.` prefix; every defined bit (OPENPETE_CB_COUNT) has a name; a
 * triangle carrying a bit with no registered counter refuses the sidecar
 * (nothing written), which is the negative control. A 2D flag counts runs
 * of one primitive address; ui.tris counts the
 * triangles with an address; the in-view near-cut count adds the cache's
 * cut triangles to the walked ones; tc_tri_in_view separates triangles
 * that only share the view's bounding box. */

#include "openpete_tile_census.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg) do {                                      \
    if (!(cond)) {                                                 \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                msg);                                              \
        return 1;                                                  \
    }                                                              \
} while (0)

/** @brief A tag-kind mapping that names every family's path 0.
 * @param family The tag family.
 * @param kind The kind, or -1 for the family's own path.
 * @return 0, the one registered path. */
static int path_of(int family, int kind)
{
    (void)family; (void)kind;
    return 0;
}

/** @brief Read a whole file into a NUL-terminated buffer.
 * @param path The file.
 * @return The text (caller frees), or NULL when unreadable. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = calloc(1, 1 << 16);
    const size_t n = buf ? fread(buf, 1, (1 << 16) - 1, f) : 0;
    fclose(f);
    if (buf && n == 0) { free(buf); return NULL; }
    return buf;
}

int main(int argc, char **argv)
{
    static const char *const names[] = { "moby" };
    char path[1024];
    snprintf(path, sizeof path, "%s/synthetic-vk.census", argc > 1 ? argv[1] : ".");
    openpete_native_register_tag_paths(path_of, names, 1);

    tc_tally_t own, bd;
    tc_tally_clear(&own);
    tc_tally_clear(&bd);
    /* Bit b on b + 1 triangles; one more triangle with GPF and UNR together. */
    for (unsigned b = 0; b < OPENPETE_CB_COUNT; b++)
        for (unsigned k = 0; k <= b; k++) {
            const tc_tri_t t = { .tag = 0x10000000u | 7u, .cbits = 1u << b };
            tc_tally_tri(&own, &t);
        }
    const tc_tri_t both = { .tag = 0x10000000u | 7u,
                            .cbits = OPENPETE_CB_MOBY_GPF | OPENPETE_CB_MOBY_UNR };
    tc_tally_tri(&own, &both);
    const tc_tri_t ter = { .tag = 0x20000000u, .cbits = OPENPETE_CB_TERRAIN_NEARCLIP };
    tc_tally_tri(&bd, &ter);

    /* Two flagged quads (two triangles each) and one addressless flagged
     * triangle: 3 carry primitives, 4 ui triangles. */
    static const uint32_t prims[5] = { 0x80100000u, 0x80100000u, 0x80100040u, 0x80100040u, 0u };
    for (int k = 0; k < 5; k++) {
        const tc_tri_t q = { .prim = prims[k], .prims = 1u << TC_PRIM_CARRY, .category = 2 };
        tc_tally_tri(&own, &q);
    }
    /* Near-cut: one walked cut triangle in view, one out of view; the cache
     * adds 4 cut triangles, 1 in view. The bit counter totals 9 + 1 + 4. */
    const tc_tri_t cut_in = { .tag = 0x20000000u, .cbits = OPENPETE_CB_TERRAIN_NEARCLIP, .inview = 1 };
    const tc_tri_t cut_out = { .tag = 0x20000000u, .cbits = OPENPETE_CB_TERRAIN_NEARCLIP, .inview = 0 };
    tc_tally_tri(&own, &cut_in);
    tc_tally_tri(&own, &cut_out);
    own.ncut_cache = 4;
    own.ncut_cache_view = 1;

    tc_sidecar_begin();
    tc_sidecar_add_tally(&own, "");
    tc_sidecar_add_tally(&bd, "backdrop.");
    remove(path);
    CHECK(tc_sidecar_write(path) == 0, "sidecar refused");
    char *text = slurp(path);
    CHECK(text, "sidecar not written");
    static const char *const want[] = {
        "moby.gpf.tris\t2\n", "moby.env.tris\t2\n", "moby.simple.tris\t3\n",
        "moby.screen.tris\t4\n", "moby.unr.tris\t6\n", "moby.sat.tris\t6\n",
        "moby.nearcut.tris\t7\n", "moby.tiepull.tris\t8\n", "terrain.nearclip.tris\t15\n",
        "backdrop.terrain.nearclip.tris\t1\n", "flag.carry.prims\t3\n", "ui.tris\t4\n",
        "terrain.nearclip.inview.tris\t2\n" };
    _Static_assert(sizeof want / sizeof want[0] == OPENPETE_CB_COUNT + 4,
                   "one expected line per defined bit, the backdrop line and three tally lines");
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        if (!strstr(text, want[i])) {
            fprintf(stderr, "FAIL: sidecar lacks \"%.*s\"\n%s", (int)strlen(want[i]) - 1, want[i], text);
            return 1;
        }
    }
    free(text);

    /* tc_tri_in_view: inside, across an edge, over a corner, beyond a corner
     * with a bounding box that overlaps the view, outside, zero area. */
    const float in_x[3] = { -0.5f, 0.5f, 0.0f }, in_y[3] = { -0.5f, -0.5f, 0.5f };
    const float cr_x[3] = { 0.5f, 3.0f, 3.0f }, cr_y[3] = { 0.0f, -1.0f, 1.0f };
    const float sl_x[3] = { 1.2f, 3.0f, 3.0f }, sl_y[3] = { 3.0f, 1.2f, 3.0f };
    const float dg_x[3] = { -0.5f, 0.0f, 0.5f }, dg_y[3] = { -0.5f, 0.0f, 0.5f };
    CHECK(tc_tri_in_view(in_x, in_y) == 1, "a triangle inside the view tested out");
    CHECK(tc_tri_in_view(cr_x, cr_y) == 1, "a triangle crossing the view's edge tested out");
    const float co_x[3] = { 0.5f, 3.0f, 0.5f }, co_y[3] = { 3.0f, 0.5f, 0.5f };
    CHECK(tc_tri_in_view(co_x, co_y) == 1, "a triangle over the view's corner tested out");
    const float sv_x[3] = { 0.9f, 3.0f, 3.0f }, sv_y[3] = { 3.0f, 0.9f, 3.0f };
    CHECK(tc_tri_in_view(sv_x, sv_y) == 0, "a triangle beyond the view's corner tested in");
    CHECK(tc_tri_in_view(sl_x, sl_y) == 0, "a triangle outside the view tested in");
    CHECK(tc_tri_in_view(dg_x, dg_y) == 0, "a zero-area triangle tested in");

    /* Negative control: a bit no TC_BIT row names refuses the sidecar. */
    const tc_tri_t stray = { .tag = 0x10000000u, .cbits = 1u << 20 };
    tc_tally_tri(&own, &stray);
    tc_sidecar_begin();
    tc_sidecar_add_tally(&own, "");
    remove(path);
    CHECK(tc_sidecar_write(path) != 0, "a sidecar with an unnamed bit was written");
    FILE *f = fopen(path, "rb");
    CHECK(f == NULL, "a refused sidecar left a file");
    printf("tile_census_test: %u bits named and counted, unnamed bit refused\n", OPENPETE_CB_COUNT);
    return 0;
}
