// games/common/renderer/player_hide.h - which native draws are the player's,
// read from game state, for drawing a view without the player
// (`--hide-player`, openpete_native_get_hide_player).
//
// The avatar, its fire-breath ribbon and its ground shadow have feeders of
// their own and skip their feed while the flag is on. The companion (Sparx)
// is a moby, its glow ring a glow-pool record and the player's particles
// records of the shared particle pool; the queries below tell those apart
// from the rest of their pool by the rules game_layout.owner states.
//
// The same module drops the moby classes `--hide-classes` lists
// (openpete_native_hide_class): every face of a listed class and its drop
// shadow.
//
// Every query and the attribution leave PSX RAM unchanged; the particle
// allocator hook runs the guest allocator exactly once through the module
// dispatch and reads its return value.

#ifndef OPENPETE_PLAYER_HIDE_H
#define OPENPETE_PLAYER_HIDE_H

#include <stddef.h>
#include <stdint.h>
#include "psx_runtime.h"
#include "feature_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The draw categories the census counts. */
enum {
    PH_CAT_PLAYER = 0,     /* avatar triangles, the mirrored copy included */
    PH_CAT_FLAME,          /* fire-breath ribbon triangles */
    PH_CAT_PLAYER_SHADOW,  /* the avatar's ground-shadow fan triangles */
    PH_CAT_SPARX,          /* companion moby triangles */
    PH_CAT_SPARX_SHADOW,   /* companion drop-shadow triangles */
    PH_CAT_SPARX_GLOW,     /* companion glow-ring triangles */
    PH_CAT_PLAYER_PART,    /* particle primitives the player's update spawned */
    PH_CAT_SPARX_PART,     /* particle primitives the companion's update spawned */
    PH_CAT_OTHER_PART,     /* every other particle primitive */
    PH_CAT_OTHER_MOBY,     /* every other moby triangle outside the HUD layers */
    PH_CAT_OTHER_GLOW,     /* every other glow-ring triangle */
    PH_CAT_CLASS,          /* moby triangles of a --hide-classes class */
    PH_CAT_CLASS_SHADOW,   /* drop-shadow triangles of a --hide-classes class */
    PH_CAT_N
};

/** @brief Whether the player's draws are skipped this present: the flag is on.
 *
 * @return 1 when the flag is on, else 0. */
int player_hide_active(void);

/** @brief Whether particle ownership is tracked: the flag is on or the census is enabled.
 *
 * @return 1 when tracked, else 0. */
int player_hide_tracking(void);

/** @brief Advance the owner of the running guest call subtree at a function entry.
 *
 * Called by the dispatch preamble on every translated-function entry while
 * player_hide_tracking holds. An entry whose stack pointer is at or above the
 * one the current owner's call was entered at has left that call, which
 * clears the owner. With no owner, an entry of game_layout.owner.player_update
 * makes the player the owner, and an entry of the resident overlay's moby
 * update for owner.sparx_class on a moby of that class makes the companion
 * the owner, each recording the entry's stack pointer.
 * @param cpu The guest CPU state at the entry.
 * @param addr The entered function's guest vaddr. */
void player_hide_note_entry(CPUState* cpu, uint32_t addr);

/** @brief Tag the pool slot the particle allocator just handed out with the owner of the running call subtree.
 *
 * Called by the particle feeder's allocator hook after every allocation. Tags
 * only while player_hide_tracking holds; records allocated while it does not
 * carry no tag.
 * @param slot The slot index (particles_slot), or -1 for a slot outside the
 *        tracked pool.
 * @param pool_moved Nonzero when the pool base changed since the previous
 *        allocation, which clears every tag first. */
void player_hide_note_alloc(int slot, int pool_moved);

/** @brief Whether the particle record at `rec` was spawned by the player's or the companion's update.
 *
 * Under the census, also counts the record's type byte (+0x00) per owner
 * for the shutdown report.
 * @param cpu Reads the type byte under the census.
 * @param rec The record's guest address.
 * @return 1 for the player's, 2 for the companion's, 0 otherwise or when the
 *         record lies outside the pool. */
int player_hide_particle_owner(CPUState* cpu, uint32_t rec);

/** @brief Whether `cls` is the companion's moby class.
 *
 * @param cls A moby class halfword.
 * @return 1 when it is, else 0 (also for a game without a companion class). */
int player_hide_is_sparx_class(uint32_t cls);

/** @brief Whether the moby feed drops one face: any face of a class --hide-classes lists, or an actor-layer face of the companion's class while the player is hidden.
 *
 * A listed class's face is dropped on every layer and counted as
 * PH_CAT_CLASS (player_hide_class_face). Otherwise every actor-layer face
 * is counted for the census, as PH_CAT_SPARX or PH_CAT_OTHER_MOBY; a
 * HUD-layer face is neither counted nor dropped.
 * @param actor Nonzero for a face of the actor layers, 0 for a HUD layer.
 * @param cls The face's moby class.
 * @param tri The face as submitted, hashed by the census.
 * @param len Its size in bytes.
 * @return 1 when the face is dropped, else 0. */
int player_hide_moby_face(int actor, uint32_t cls, const void* tri, size_t len);

/** @brief Whether the moby feed drops one face of a class --hide-classes lists.
 *
 * Counts every dropped face as PH_CAT_CLASS for the census and for the
 * shutdown engagement line.
 * @param cls The face's moby class.
 * @param tri The face as submitted, hashed by the census.
 * @param len Its size in bytes.
 * @return 1 when the class is listed and the face is dropped, else 0. */
int player_hide_class_face(uint32_t cls, const void* tri, size_t len);

/** @brief Whether the shadow feed drops the drop shadow of the moby at `moby`: --hide-classes lists its class.
 *
 * Reads the class halfword only while the list is nonempty. Counts the
 * dropped fan's 4 triangles as PH_CAT_CLASS_SHADOW for the census and the
 * shutdown engagement line.
 * @param cpu Reads the class halfword.
 * @param moby The moby's guest address.
 * @return 1 when dropped, else 0. */
int player_hide_class_shadow(CPUState* cpu, uint32_t moby);

/** @brief Whether the moby at `moby` is the companion, by its class halfword.
 *
 * @param cpu Reads the class halfword.
 * @param moby The moby's guest address.
 * @return 1 when it is, else 0. */
int player_hide_is_sparx_moby(CPUState* cpu, uint32_t moby);

/** @brief The glow-pool record the companion's update owns, read through its moby tag.
 *
 * Walks the moby array for the first live moby of the companion's class and
 * reads the word owner.sparx_glow_off bytes into the tag its word at +0x00
 * points at.
 * @param cpu Reads the moby array and the tag.
 * @return The record's guest address, or 0 when there is none. */
uint32_t player_hide_sparx_glow(CPUState* cpu);

/** @brief Count `n` draws of category `cat` fed (`skipped` 0) or skipped (`skipped` 1) at the current VBL.
 *
 * Accumulates only while OPENPETE_PLAYER_HIDE_CENSUS names the VBL; the line
 * for a VBL prints when a count arrives for a later one.
 * @param cat A PH_CAT_* category.
 * @param n The draw count.
 * @param skipped 1 when the draws were skipped, else 0. */
void player_hide_count(int cat, int n, int skipped);

/** @brief Count one draw of category `cat` as player_hide_count does and fold its bytes into the category's hash.
 *
 * The hash is FNV-1a over the bytes of every draw the category counted at
 * the VBL, fed or skipped alike, so two runs that submit the same draws of
 * a category print the same hash.
 * @param cat A PH_CAT_* category.
 * @param skipped 1 when the draw was skipped, else 0.
 * @param data The draw's submitted record.
 * @param len Its size in bytes. */
void player_hide_count_data(int cat, int skipped, const void* data, size_t len);

/** @brief Whether the census is enabled.
 *
 * @return 1 when OPENPETE_PLAYER_HIDE_CENSUS is set, else 0. */
int player_hide_census_on(void);

extern const feature_module_t player_hide_module;

#ifdef __cplusplus
}
#endif

#endif
