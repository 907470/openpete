// core/include/openpete_vstream.h - moby vertex-stream address resolver.
//
// Each animation frame word packs the vertex-stream address into bits
// 0-20 (a byte address for moby frames; halved for Spyro's own, whose
// streams reach 4 MB), the collision-model index into bits 21-23 and the
// frame sound into bits 24-31. Every reader in the game turns the field
// into a pointer with the same shift pair (`sll 11; srl 11|10`), and
// those readers are substituted to call the hooks below; the native
// walks call the resolver directly.
//
// Stock words never address below 0x10000: the executable loads there
// and every level's model data sits above it. So a field value below
// VSTREAM_HANDLE_BOUND cannot be a stream, and the resolver treats it
// as a HANDLE - an index into an engine table of full 32-bit vaddrs -
// which is how a stream parked anywhere (the mod arena past 2 MB, say)
// is reachable through a 21-bit field. Handles are the raw field value
// in both encodings, so one table serves both; the halved encoding's
// stock values start at 0x8000 (byte address 0x10000 halved), which is
// what fixes the bound. Handle 0 is never allocated: a zero field stays
// on the loud path.
//
// Table entries must be a vaddr both reader forms reach: the renderers
// use the resolved value raw (KUSEG), the collision code ORs the KSEG0
// segment bit in first. KSEG0 addresses satisfy both.

#ifndef OPENPETE_VSTREAM_H
#define OPENPETE_VSTREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VSTREAM_HANDLE_BOUND 0x8000u
#define VSTREAM_HANDLE_MAX   VSTREAM_HANDLE_BOUND /* table size; index 0 reserved */

/** @brief Resolve a frame word's 21-bit stream field to the stream's vaddr.
 *
 * The value is masked to 21 bits. Stock values (at or above
 * VSTREAM_HANDLE_BOUND) pass through bit-identical: the byte address,
 * doubled on the halved path. A handle resolves to its table entry; an
 * unbound handle (including 0) resolves to 0, is logged on the first
 * occurrence, and aborts under OPENPETE_VSTREAM_FATAL. Each call counts
 * into the census.
 * @param masked Field value.
 * @param halved Nonzero for the halved encoding (Spyro's own streams).
 * @return The vaddr, or 0 for an unbound handle. */
uint32_t openpete_vstream_addr(uint32_t masked, int halved);

/** @brief Resolve a stream field as openpete_vstream_addr does, plus a bounded host pointer to the stream's bytes.
 *
 * Lets a native walk read the stream without the guest access path. The
 * vaddr maps through main RAM, then through the registered host regions.
 * @param masked Field value.
 * @param halved Nonzero for the halved encoding.
 * @param host Receives the host pointer, NULL when the vaddr maps nothing;
 *        may be NULL.
 * @param remaining Receives the bytes readable from @p host before the
 *        mapping ends, 0 when nothing maps; may be NULL.
 * @return The vaddr, or 0 for an unbound handle. */
uint32_t openpete_vstream_resolve(uint32_t masked, int halved,
                                  const uint8_t** host, uint32_t* remaining);

/** @brief Bind a handle to a vaddr in the stream table.
 *
 * An entry is written once: rebinding a bound entry to a different vaddr
 * is refused and logged; rebinding it to the same vaddr succeeds. The mod
 * arena's handle ledger owns the table and decides numbering.
 * @param index Handle, 1..VSTREAM_HANDLE_MAX-1.
 * @param vaddr Stream vaddr (KSEG0, see above); nonzero.
 * @return 1 on success; 0 when @p index is out of range, @p vaddr is 0, or
 *         the entry holds a different vaddr. */
int openpete_vstream_table_set(uint32_t index, uint32_t vaddr);

/** @brief Return the lowest handle bound to a vaddr.
 *
 * @param vaddr Stream vaddr.
 * @return The handle, or 0 when none is bound or @p vaddr is 0. */
uint32_t openpete_vstream_table_find(uint32_t vaddr);

/** @brief Return the vaddr bound to a handle.
 *
 * @param index Handle.
 * @return The vaddr, or 0 when unbound or out of range. */
uint32_t openpete_vstream_table_get(uint32_t index);

/* Census counters: stock resolutions, handle resolutions, and resolutions
 * of a field value below the bound that no table entry backs. The last
 * must stay 0 in a run with no handle-writing mod: it is the evidence
 * that no stock frame word lives below the bound. */
typedef struct {
    uint64_t stock;
    uint64_t handle;
    uint64_t unresolved;
    uint32_t first_unresolved; /* field value of the first miss */
} openpete_vstream_census_t;
/** @brief Copy the resolution counters into a caller-provided struct.
 *
 * @param out Receives the counters; nothing happens when NULL. */
void openpete_vstream_census(openpete_vstream_census_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_VSTREAM_H */
