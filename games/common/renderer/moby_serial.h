/* moby_serial.h - per-slot allocation serials of the moby array.
 *
 * A moby's interp identity is its address, and the game's allocator can hand
 * a slot freed in a tick to a new moby in the same tick, so the address alone
 * pairs the dead moby with the newborn. The allocator hook gives every
 * allocation a serial naming its tick and its ordinal in that tick
 * (alloc_serial_next), so a tick re-run from a restored state stamps the
 * serials its first run did; the object capture appends
 * the slot's serial to the moby's record. Where the layout sets
 * alloc_respawn a changed serial makes the capture a despawn of the old record
 * and a spawn of the new one; elsewhere the record keeps its address key and
 * the renderer holds a pair whose serials differ as a class change
 * (moby_pair_reborn).
 *
 * An allocation made inside a host-initiated guest call (a mod's api->call)
 * of a slot the previous host allocation pass also allocated that way keeps
 * the slot's serial: a mod that allocates a moby for the draw and frees it
 * before the next update hands the same moby the same slot on every tick. */
#ifndef OPENPETE_RENDERER_MOBY_SERIAL_H
#define OPENPETE_RENDERER_MOBY_SERIAL_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "openpete_interp_objs.h"

/** @brief Whether the moby capture appends the slot serial to each record: the layout names a moby allocator.
 *
 * @return 1 when game_layout.moby.alloc is set, else 0. */
int moby_serial_enabled(void);

/** @brief The serial of the allocation that last handed out the moby slot at `moby`.
 *
 * Two slots never hold the same nonzero serial, and a slot's serial changes on
 * every allocation of it within 2^20 ticks, so a moby whose serial is
 * unchanged between two captures is one continuing record.
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

/** @brief Whether a moby's interp pair spans two allocations of its slot: both states carry a slot serial and the two differ.
 *
 * A state carries the serial when its capture declared it at
 * game_layout.moby.array_stride (curr_serial_off, prev_serial_off): the
 * moby-array walk does, a capture of a record outside the array does not.
 * @param o The moby's interp pair.
 * @return 1 when the serials differ, else 0 (no allocator hooked, or either
 *         state captured without a serial). */
int moby_pair_reborn(const openpete_interp_obj_t* o);

/** @brief Count one present's verdict on a moby pair for the allocation census (OPENPETE_MOBY_SERIAL_CENSUS=1).
 *
 * Counts each (slot, tick) once per verdict: `lerped` pairs shown at an alpha
 * in (0,1), `reuses` pairs moby_pair_reborn names, `reborn_lerped` reuses shown
 * at an alpha in (0,1), `reborn_held` reuses held; prints one line per reuse
 * event and the totals at exit. No effect without the variable.
 * @param moby The moby's guest address.
 * @param o The moby's interp pair.
 * @param alpha The phase the present shows the record at (0 for a hold).
 * @param held Nonzero when the present holds the record instead of lerping it. */
void moby_serial_census(uint32_t moby, const openpete_interp_obj_t* o, double alpha, int held);

#endif
