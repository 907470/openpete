// games/common/spyro_overlay_table.h - registry of a game's recompiled overlays.
//
// scripts/regen-recomp.sh emits generated/spyro_overlay_table.c with one entry
// per overlay it translated, mapping the overlay-id stored at the head of the
// overlay's RAM region to that overlay's sed-renamed dispatch function
// (psx_dispatch_<name>).
//
// Every overlay of a game loads at the same vaddr and only one is resident at
// a time, so dispatch must be routed to whichever one's bytes are actually in
// RAM. The active-overlay tracker consults this table: when a disc read
// settles bytes at the overlay vaddr, the first word there is the overlay id.

#ifndef OPENPETE_SPYRO_OVERLAY_TABLE_H
#define OPENPETE_SPYRO_OVERLAY_TABLE_H

#include "psx_runtime_internal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*spyro_overlay_dispatch_fn)(CPUState* cpu, uint32_t addr);

typedef struct {
    uint32_t                  magic;  /* overlay-id LONG at the overlay vaddr */
    const char*               name;
    spyro_overlay_dispatch_fn fn;
} spyro_overlay_entry_t;

extern const spyro_overlay_entry_t spyro_overlay_table[];
extern const size_t                spyro_overlay_table_count;
/* The shared overlay vaddr window [lo, hi): every overlay of the game loads
 * at lo, and hi bounds the largest one. */
extern const uint32_t              spyro_overlay_window_lo;
extern const uint32_t              spyro_overlay_window_hi;

/** @brief Look up a registry entry by overlay id.
 *
 * @param magic Overlay id, the first word of the overlay region.
 * @return The entry, or NULL when no registered overlay has that id. */
const spyro_overlay_entry_t* spyro_overlay_find(uint32_t magic);

/** @brief Make the overlay with the given id the one whose dispatcher decides overlay-window calls.
 *
 * An unknown id leaves the current overlay active, and the already-active
 * id changes nothing. A change is logged at info level under `dispatch`.
 * @param magic Overlay id. */
void spyro_set_active_overlay_by_magic(uint32_t magic);

/** @brief Return the id of the overlay the dispatcher treats as resident.
 *
 * @return The overlay id, or 0 before any registered overlay has been set. */
uint32_t spyro_get_active_overlay_magic(void);

/** @brief Register the callback that re-derives the resident overlay from guest RAM.
 *
 * psx_dispatch_compiled invokes it before the active overlay's table decides
 * a dispatch into the overlay window, so a game whose overlays can be staged
 * elsewhere and copied into the overlay region, arriving without the disc
 * read that would announce them, never dispatches against the outgoing
 * overlay's table. psyq_overlay_resync_from_ram
 * (games/common/psyq/psyq_game_hooks.h) is the implementation every such
 * game passes; NULL disables the check.
 * @param fn The resync callback, or NULL. */
void spyro_overlay_set_resync(void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif
