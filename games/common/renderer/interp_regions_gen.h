/* games/common/renderer/interp_regions_gen.h — builds a game's interp region
 * table from descriptor rows and registers it with the openpete_interp ring.
 *
 * A game describes its plain-global regions as a const array of
 * game_interp_row_t (address, length, timeline flag) and hands it here; rows
 * whose span is computed at registration time (the camera's
 * cam.base..cam.xform_end) are added one at a time through the same call. The
 * ring's region INDEX is therefore assigned in exactly one place, and a
 * consumer that needs an index asks for it by name instead of restating the
 * table's order (a restated index silently selects the wrong region when a row
 * is inserted).
 *
 * A row whose address or length is 0 describes a mechanism the game has no
 * located counterpart for; it is skipped and its name resolves to -1. */
#ifndef OPENPETE_GAME_INTERP_REGIONS_GEN_H
#define OPENPETE_GAME_INTERP_REGIONS_GEN_H

#include <stdint.h>

#include "openpete_interp_objs.h"   /* openpete_interp_obj_capture_fn */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct game_interp_row {
    const char* name;       /* the lookup key and the diagnostic name */
    uint32_t    addr;       /* KSEG0 PSX vaddr of the region's first byte */
    uint32_t    length;     /* bytes captured each tick */
    int         ra_commit;  /* 1 = present from the committed timeline under
                             * runahead (openpete_interp_region_t.ra_commit) */
} game_interp_row_t;

/* Append rows in table order. Returns the index of the first row added, or -1
 * when nothing was added (every row skipped, or the ring's region cap reached). */
int game_interp_rows_add(const game_interp_row_t* rows, unsigned count);
int game_interp_rows_add_one(const char* name, uint32_t addr, uint32_t length,
                             int ra_commit);

/* Hand the accumulated table to the ring. Call once, after every add. */
void game_interp_rows_commit(void);

/* The ring region index of a registered row, or -1 when the game did not
 * register it. */
int game_interp_row_index(const char* name);

/* Register the game's per-object capture callback. Routed through this layer
 * so the region census (OPENPETE_INTERP_REGION_CENSUS) gets the per-tick hook
 * it samples on: the ring's own capture runs immediately before this callback. */
void game_interp_rows_obj_capture(openpete_interp_obj_capture_fn fn,
                                  uint32_t blob_bytes, uint32_t slots);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_INTERP_REGIONS_GEN_H */
