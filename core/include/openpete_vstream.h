// core/include/openpete_vstream.h — moby vertex-stream address resolver.
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
// as a HANDLE — an index into an engine table of full 32-bit vaddrs —
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

/* Resolve a masked 21-bit field value to the stream's vaddr. Stock values
 * (>= the bound) pass through bit-identical: the byte address, doubled on
 * the halved path. A handle resolves to its table entry; an unallocated
 * handle resolves to 0 (logged once; fatal under OPENPETE_VSTREAM_FATAL). */
uint32_t openpete_vstream_addr(uint32_t masked, int halved);

/* Same, plus a bounded host pointer to the stream's bytes so a native walk
 * can read without the guest access path: *host is NULL if the vaddr maps
 * nothing, else *remaining is the byte count readable from it before the
 * mapping ends. */
uint32_t openpete_vstream_resolve(uint32_t masked, int halved,
                                  const uint8_t** host, uint32_t* remaining);

/* Table writer: bind handle `index` (1..MAX-1) to `vaddr`. An entry is
 * written once; rebinding a live entry to a different vaddr is refused
 * (returns 0). Returns 1 on success. Owned by the mod arena's handle
 * ledger, which decides numbering. */
int openpete_vstream_table_set(uint32_t index, uint32_t vaddr);

/* Lookup: the handle already bound to `vaddr`, or 0. */
uint32_t openpete_vstream_table_find(uint32_t vaddr);

/* The vaddr bound to `index`, or 0. */
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
void openpete_vstream_census(openpete_vstream_census_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_VSTREAM_H */
