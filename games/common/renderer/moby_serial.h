/* moby_serial.h - per-slot allocation serials of the moby array.
 *
 * A moby's interp identity is its address, and the game's allocator can hand
 * a slot freed in a tick to a new moby in the same tick, so the address alone
 * pairs the dead moby with the newborn. The allocator hook gives every
 * allocation the next serial of one global count; the object capture appends
 * the slot's serial to the moby's record, and a changed serial makes the
 * capture a despawn of the old record and a spawn of the new one. */
#ifndef OPENPETE_RENDERER_MOBY_SERIAL_H
#define OPENPETE_RENDERER_MOBY_SERIAL_H

#include <stdint.h>
#include "psx_runtime_internal.h"

/** @brief Whether the moby capture appends the slot serial to each record: the layout names a moby allocator.
 *
 * @return 1 when game_layout.moby.alloc is set, else 0. */
int moby_serial_enabled(void);

/** @brief The serial of the allocation that last handed out the moby slot at `moby`.
 *
 * Two slots never hold the same nonzero serial, and a slot's serial changes on
 * every allocation of it, so a moby whose serial is unchanged between two
 * captures is one continuing record.
 * @param moby The moby's guest address.
 * @return The serial, or 0 when no allocation of the slot has been seen. */
uint32_t moby_slot_serial(uint32_t moby);

/** @brief The moby allocator hook: runs the guest allocator once, then stamps the slot it returned with a new allocation serial.
 *
 * Registered at game_layout.moby.alloc by the moby feeder. Reads guest RAM,
 * writes host state only.
 * @param cpu The guest CPU state.
 * @param addr The allocator's entry address.
 * @return 1 when the allocator ran here, 0 on the re-entry its own dispatch makes. */
int moby_serial_on_alloc(CPUState* cpu, uint32_t addr);

#endif
