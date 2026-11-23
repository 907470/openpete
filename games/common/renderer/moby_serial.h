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
 * The n-th allocation made inside host-initiated guest calls (a mod's
 * api->call) in one tick continues the previous such tick's n-th allocation
 * when that one's slot no longer holds it: it keeps its serial, and when the
 * allocator handed it another slot the next capture moves the record's
 * history to that slot (moby_serial_take_hops), so a moby a mod allocates for
 * the draw and frees before the next update keeps one identity whichever
 * slot it takes. */
#ifndef OPENPETE_RENDERER_MOBY_SERIAL_H
#define OPENPETE_RENDERER_MOBY_SERIAL_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "openpete_interp_objs.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/** @brief Whether the moby capture appends the slot serial to each record: the layout names a moby allocator.
 *
 * @return 1 when game_layout.moby.alloc is set, else 0. */
int moby_serial_enabled(void);

/** @brief The serial of the allocation that last handed out the moby slot at `moby`.
 *
 * A serial names one moby identity: a slot's serial changes on every
 * allocation of it within 2^20 ticks, except a host continuation, which
 * carries its predecessor's serial into the slot it takes; a slot the
 * predecessor left keeps that serial until its next allocation.
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

/** @brief Take the moves of continued host allocations since the last call: each record's identity moved from slot `from[k]` to slot `to[k]`, carrying `serial[k]`.
 *
 * A chain of moves within the interval is one move from its first slot to its
 * last. The object capture applies them before the moby walk. Clears the list.
 * @param from Receives the slots the records were in.
 * @param to Receives the slots they are in now.
 * @param serial Receives the serial each record carries.
 * @param cap The arrays' length.
 * @return The moves written. */
uint32_t moby_serial_take_hops(uint32_t* from, uint32_t* to, uint32_t* serial, uint32_t cap) OP_REQUIRES(sim);

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

/** @brief Name the host-hop check moby of the newest tick: its slot, its class, and whether the previous tick also had one.
 *
 * Set by the check that allocates a moby from a host call at every draw
 * (OPENPETE_S1_HOST_HOP_TEST); a slot of 0 clears it.
 * @param moby The check moby's slot.
 * @param cls The class the check moby carries.
 * @param expected Nonzero when the previous tick drew the check moby too, so every present of this tick owes it a draw. */
void moby_serial_hop_probe_set(uint32_t moby, uint16_t cls, int expected);

/** @brief The host-hop check moby a sub-tick pass looks for.
 *
 * @param moby Receives the check moby's slot.
 * @param cls Receives its class.
 * @return 1 when a check moby is set and owed a draw, else 0. */
int moby_serial_hop_probe_target(uint32_t* moby, uint16_t* cls);

/** @brief Count one sub-tick moby pass for the host-hop check: a present whose passes drew no record of the check moby is an absent present.
 *
 * Passes with one present stamp are one present; the totals print at exit
 * as `[host-hop-test] hops=H captured=C presents=P absent=A`, and each
 * absent present prints one line with the VBL and the slot.
 * @param present_ns The present's stamp.
 * @param drawn Nonzero when the pass drew the check moby. */
void moby_serial_hop_probe_pass(int64_t present_ns, int drawn);

/** @brief Count one hop of the host-hop check: its allocation took a slot other than the previous tick's.
 *
 * A hop into a slot the object store tracks a record under (a moby the
 * previous capture saw there) counts as captured, the case the check exists
 * for; the totals print `captured=C` beside `hops=H`.
 * @param from The previous tick's slot.
 * @param to This tick's slot. */
void moby_serial_hop_probe_hop(uint32_t from, uint32_t to);

#endif
