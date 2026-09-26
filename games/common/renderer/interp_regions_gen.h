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

/* The camera's ring row. Every game registers its draw-time camera transform
 * under this one name, and every consumer resolves the ring index by it. */
#define GAME_INTERP_CAM_ROW "g_Camera"

/* The simulation-regime word's ring row. A game that has not located one
 * registers no such row and the lookup resolves to -1, which the consumers
 * read as "this game marks no regime edge". */
#define GAME_INTERP_GS_ROW "g_GameState"

/** @brief Snapshot one record per live moby of the game's moby array into the keyed prev/curr table.
 *
 * The array (game_layout.moby: array_head/array_direct, array_stride,
 * state_off, state_term) is fixed and ends at a sentinel element whose state
 * byte is state_term, so a moby's PSX address is its slot and its key across
 * ticks. A moby whose state byte is 0x80 or above is dormant and skipped. Each
 * record captured is one moby record and no bytes of the next, taken from the
 * draw-time stash when it holds the moby and live RAM otherwise. The walk
 * stops 512 slots short of the table capacity so the pools captured after it
 * always fit. OPENPETE_CAPTRACE_CLS=<class|0xFFFF> prints the tick position of
 * every captured moby of that class; OPENPETE_MOBY_STASH_CENSUS prints the
 * stash's served and differing counts per call. Read-only against guest RAM.
 * Call between openpete_interp_objs_begin_tick() and
 * openpete_interp_objs_end_tick(); game_interp_register_common() does. */
void game_interp_capture_moby_array(void);

/* Draw-time records for game_interp_capture_moby_array(). A draw hook calls
 * _begin(), then _put() per moby the draw read, while the records hold the
 * state the draw left and no later routine of the tick has written them;
 * while the draw's canonical-render epoch is in flight or last committed, the
 * capture takes the stashed record for those mobys instead of the live one. A
 * _begin() in a later epoch replaces the stash; one on a speculative pass
 * blocks it until the next committed _begin(), and _put() is then ignored.
 * Host memory only. */
void game_interp_moby_stash_begin(void);
void game_interp_moby_stash_put(uint32_t moby);

/** @brief Snapshot one record per active slot of the game's particle pool, keyed by the record's PSX address.
 *
 * The walk is the particle routine's own (game_layout.particles: array_ptr,
 * rec_stride, sel_off): from the pool base, one record per `rec_stride`, at
 * most 512, reading the selector halfword whose sign marks an inactive slot
 * and whose arithmetic shift by 8 is -1 at the end-of-pool sentinel. A slot a
 * new record reuses is the consumer's pairing verdict, not this walk's. The
 * renderer projects each record under the tick's camera and links its own
 * primitives, so the sub-tick replay of those primitives needs each record's
 * previous state to rebase onto; the pool sits outside the moby array, so the
 * moby walk never reaches it. Reached through
 * game_interp_capture_feeder_pools. Read-only against guest RAM. */
void game_interp_capture_particle_pool(void);

/** @brief Snapshot one record per live slot of the game's gem-glint pool, keyed by the record's PSX address.
 *
 * The pool (game_layout.glint: pool, slots, stride, packed_off) is a fixed
 * array outside the moby array, so the record's address is its slot; a slot
 * whose packed word has lifetime byte 0 is free and skipped. The glint
 * routine projects each record under the tick's camera and emits the star's
 * screen corners itself, so the sub-tick replay needs each record's previous
 * position and spin phase to rebase them onto. Reached through
 * game_interp_capture_feeder_pools. Read-only against guest RAM. */
void game_interp_capture_glint_pool(void);
void game_interp_capture_glow_pool(void);

/* Snapshot one whole record per live slot of Zoe's zap pool (game_layout.zap:
 * pool, slots, stride, packed_off), keyed by the record's PSX address, so the
 * sub-tick replay of its ribbon has a previous head and tail to rebase onto. */
void game_interp_capture_zap_pool(void);

/** @brief Snapshot every live tracer ribbon record and every vertex of the tracer mesh lists the most recent mesh draw read.
 *
 * A ribbon record (game_layout.tracers: ring, slots, stride) whose owner word
 * at +0x00 is 0 is free and skipped; a live one is captured whole, point ring
 * included, keyed by its PSX address. Each mesh vertex is a 12-byte world XYZ
 * int32 triple keyed by its own address, from up to eight lists
 * tracers_mesh_lists reports. Both producers project their points under the
 * tick's camera, so the sub-tick replay needs each point's previous position
 * to rebase onto. Read-only against guest RAM. */
void game_interp_capture_tracer_pool(void);

/** @brief Walk the particle, gem-glint, glow, zap and tracer pools in one call.
 *
 * The extra-pool walk a game passes to game_interp_register_common when its
 * feeders link their own primitives, so the game registers one walk rather
 * than a list kept in step with the feeders. A pool the game's layout does
 * not locate is skipped by its own walk. */
void game_interp_capture_feeder_pools(void);

/* Register a game's interp table and per-object capture in the shared shape:
 * the camera row (GAME_INTERP_CAM_ROW, `cam_length` bytes from
 * game_layout.cam.base, presented from the committed timeline under runahead)
 * first so it is region 0, then `rows`, then commit. The installed per-tick
 * capture runs the moby-array walk, then `extra_pools` (a game's pools outside
 * that array; NULL when it has none), then the env-anim scroll descriptors. */
void game_interp_register_common(const game_interp_row_t* rows, unsigned count,
                                 uint32_t cam_length, void (*extra_pools)(void));

/* Register the game's per-object capture callback. Routed through this layer
 * so the region census (OPENPETE_INTERP_REGION_CENSUS) gets the per-tick hook
 * it samples on: the ring's own capture runs immediately before this callback. */
void game_interp_rows_obj_capture(openpete_interp_obj_capture_fn fn,
                                  uint32_t blob_bytes, uint32_t slots);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_INTERP_REGIONS_GEN_H */
