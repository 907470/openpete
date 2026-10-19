/* games/common/write_census.h - address-range write attribution.
 *
 * OPENPETE_WRITE_CENSUS=lo..hi[,lo..hi...] (hex, inclusive, any count of
 * ranges; a malformed range refuses the whole spec) installs wrappers
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

/** @brief Installs the census wrappers over the CPU's five store hooks when OPENPETE_WRITE_CENSUS parses.
 *
 * Parses every hex lo..hi range, opens OPENPETE_WRITE_CENSUS_OUT
 * (default write-census.csv) and writes its header, saves the original
 * write_word, write_half, write_byte, swl and swr hooks, installs the
 * wrappers, latches OPENPETE_WRITE_CENSUS_PHASE, and registers an atexit
 * close. An unset knob, a NULL cpu, a malformed range (reported on stderr)
 * or an unopenable output leaves the hooks untouched.
 * @param cpu The guest CPU state whose store hooks are wrapped. */
void write_census_install(CPUState* cpu);
/** @brief Reports whether the census wrappers are installed.
 *
 * @return Nonzero after write_census_install wrapped the store hooks. */
int  write_census_active(void);
/** @brief Reports how many in-range stores the census has logged.
 *
 * @return The CSV row count written since install, header excluded. */
unsigned write_census_rows(void);
/** @brief Records a dispatch entry address as the entry column of subsequent rows.
 *
 * An address equal to the OPENPETE_WRITE_CENSUS_PHASE entry sets phase 1.
 * Callers call it only when write_census_active().
 * @param addr The dispatched function's entry address. */
void write_census_note_entry(uint32_t addr);
/** @brief Sets the phase column of subsequent rows.
 *
 * Phase 0 is before the phase entry for this tick. A transition from 0 to
 * non-zero increments the seq column.
 * @param phase The new phase. */
void write_census_set_phase(int phase);
/** @brief Stores a word through the store hook saved at installation, producing no census row.
 *
 * A no-op when the census is not installed.
 * @param addr The guest address.
 * @param value The word to store. */
void write_census_orig_word(uint32_t addr, uint32_t value);
/** @brief Stores a halfword through the store hook saved at installation, producing no census row.
 *
 * A no-op when the census is not installed.
 * @param addr The guest address.
 * @param value The halfword to store. */
void write_census_orig_half(uint32_t addr, uint16_t value);

#ifdef __cplusplus
}
#endif
#endif
