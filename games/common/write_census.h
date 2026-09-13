/* games/common/write_census.h — address-range write attribution.
 *
 * OPENPETE_WRITE_CENSUS=lo..hi[,lo..hi] (hex, inclusive) installs wrappers
 * over the five store hooks at boot and appends one row per store that lands
 * in a range to OPENPETE_WRITE_CENSUS_OUT (default write-census.csv):
 *
 *   vbl,seq,addr,size,value,ra,entry,phase,overlay
 *
 * `ra` is cpu->ra at the store (the guest's $ra: the caller for a leaf,
 * the function itself after its first jal), `entry` is the address the
 * dispatch preamble last reported (write_census_note_entry), `phase` is 1
 * from the entry named by OPENPETE_WRITE_CENSUS_PHASE=<hex> until the game
 * clears it (write_census_set_phase(0), at its per-VBL hook) and `seq`
 * counts the phase-set events, so a row is placed before or after that
 * entry for its tick. The saved originals let a host-side poke store
 * without producing a row. */
#ifndef OPENPETE_WRITE_CENSUS_H
#define OPENPETE_WRITE_CENSUS_H

#include <stdint.h>
#include "psx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the wrappers when the knob is set; a no-op otherwise. */
void write_census_install(CPUState* cpu);
int  write_census_active(void);
/* Every dispatch entry reports here; the last one is the `entry` column,
 * and the phase entry sets phase 1. Call only when write_census_active(). */
void write_census_note_entry(uint32_t addr);
/* Phase 0 = before the phase entry for this tick. */
void write_census_set_phase(int phase);
/* Store hooks as they were before installation (NULL when inactive). */
void write_census_orig_word(uint32_t addr, uint32_t value);
void write_census_orig_half(uint32_t addr, uint16_t value);

#ifdef __cplusplus
}
#endif
#endif
