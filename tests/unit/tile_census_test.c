/* tests/unit/tile_census_test.c - the tile census's class-bit counters on a
 * synthetic scene.
 *
 * A tally of triangles carrying known class bits writes one counter per
 * bit, named by tile_census.def's TC_BIT rows, under the own and the
 * `backdrop.` prefix; every defined bit (OPENPETE_CB_COUNT) has a name; a
 * triangle carrying a bit with no registered counter refuses the sidecar
 * (nothing written), which is the negative control. */

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
        "moby.nearcut.tris\t7\n", "moby.tiepull.tris\t8\n", "terrain.nearclip.tris\t9\n",
        "backdrop.terrain.nearclip.tris\t1\n" };
    _Static_assert(sizeof want / sizeof want[0] == OPENPETE_CB_COUNT + 1,
                   "one expected line per defined bit and the backdrop line");
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        if (!strstr(text, want[i])) {
            fprintf(stderr, "FAIL: sidecar lacks \"%.*s\"\n%s", (int)strlen(want[i]) - 1, want[i], text);
            return 1;
        }
    }
    free(text);

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
