// games/spyro-1/spyro_overlay_table.h — registry of recompiled overlays.
//
// scripts/regen-recomp.sh emits generated/spyro_overlay_table.c with
// one entry per overlay it could translate. Each entry maps the
// overlay-id stored at the head of the overlay's RAM region to its
// sed-renamed dispatch function (psx_dispatch_<name>). The active
// overlay tracker (in psyq_thunks.cpp / dispatch_chain.c) consults
// this table at runtime: when CdRead settles bytes at 0x8007AA38, the
// first word is the overlay-id, and we look up which dispatcher is
// now valid.

#ifndef SPYRO_OVERLAY_TABLE_H
#define SPYRO_OVERLAY_TABLE_H

#include "psx_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*spyro_overlay_dispatch_fn)(CPUState* cpu, uint32_t addr);

typedef struct {
    uint32_t                  magic;  /* overlay-id LONG at 0x8007AA38 */
    const char*               name;
    spyro_overlay_dispatch_fn fn;
} spyro_overlay_entry_t;

extern const spyro_overlay_entry_t spyro_overlay_table[];
extern const size_t                spyro_overlay_table_count;

/* Look up an overlay by its id-magic; NULL if unknown. */
const spyro_overlay_entry_t* spyro_overlay_find(uint32_t magic);

/* Set the active overlay by id-magic. No-op if magic is unknown. */
void spyro_set_active_overlay_by_magic(uint32_t magic);

/* Id-magic of the overlay currently resident in the guest's overlay
 * region, or 0 before any level overlay has been loaded. */
uint32_t spyro_get_active_overlay_magic(void);

#ifdef __cplusplus
}
#endif

#endif
