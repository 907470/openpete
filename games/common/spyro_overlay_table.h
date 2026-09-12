// games/common/spyro_overlay_table.h — registry of a game's recompiled overlays.
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

/* Look up an overlay by its id-magic; NULL if unknown. */
const spyro_overlay_entry_t* spyro_overlay_find(uint32_t magic);

/* Set the active overlay by id-magic. No-op if magic is unknown. */
void spyro_set_active_overlay_by_magic(uint32_t magic);

/* Id-magic of the overlay currently resident, or 0 before any has loaded. */
uint32_t spyro_get_active_overlay_magic(void);

#ifdef __cplusplus
}
#endif

#endif
