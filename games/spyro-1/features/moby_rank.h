/* Same-moby painter RANK source: read the game's OWN OT chain.
 *
 * The PSX game inserts every moby face into the ordering table with its own
 * integer GTE key arithmetic; DrawOTag then walks the fully-resolved chain in
 * paint order. That walk IS the ground truth for the same-moby painter select
 * (native_render's rank image + fragment select): re-deriving keys in float
 * misphases at slot boundaries, so instead the DrawOTag thunk records the
 * chain's geometry prims (walk position + vertex XY/UV) and the moby feeder
 * matches them to its own faces once per table.
 *
 * Pipelining: the chain a DrawOTag call submits is the PREVIOUS tick's pose
 * (the game double-buffers its OT), and the feeder keeps a snapshot of that
 * pose's faces — so the match compares identical poses. The resulting
 * (moby, fkey) -> rank map is applied to the CURRENT pose by face-topology
 * identity: one tick of rank latency, the same latency og itself displays,
 * and a whole-face paint verdict that rarely changes between adjacent poses.
 * Faces with no unique match keep rank 0 = today's true-depth resolve.
 *
 * Display-only by construction: reads game RAM via the walk, writes nothing
 * gameplay-readable. */
#ifndef OPENPETE_MOBY_RANK_H
#define OPENPETE_MOBY_RANK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Master gate (OPENPETE_MOBY_RANK, default on; 0 disables the whole source —
 * no ingest, no matching, every face rank 0). Boot-latched. */
int  openpete_mobyrank_on(void);

/* OT ingest — called from the DrawOTag thunk while it walks the chain.
 * begin() opens a fresh table unless one is already open for this frame
 * (several DrawOTag calls in one frame keep appending: walk position is
 * global paint order). prim() appends one geometry prim (POLY_* families
 * 0x20/0x30; payload = the prim words after the P_TAG). end() publishes. */
void openpete_mobyrank_ot_begin(unsigned frame);
void openpete_mobyrank_ot_prim(int code, const uint8_t* payload, unsigned payload_bytes);
void openpete_mobyrank_ot_end(void);

/* Feeder-side pose snapshot: the eligible faces of the most recent canonical
 * extraction (screen XY floats + integer texel UV + material identity). */
void openpete_mobyrank_snap_begin(unsigned frame);
void openpete_mobyrank_snap_face(uint32_t moby, uint32_t fkey,
                                 const float sx3[3], const float sy3[3],
                                 const uint8_t u3[3], const uint8_t v3[3],
                                 int textured, int clut_x, int clut_y);
void openpete_mobyrank_snap_end(void);

/* Consume a newly-published table (idempotent per table version): match it
 * against the last completed snapshot, rebuild the rank map. Call BEFORE
 * snap_begin at each extraction so the snapshot it consumes is the one whose
 * pose the table carries. */
void openpete_mobyrank_match(void);

/* Rank for a face of the CURRENT pose: >=1, or 0 = unranked (no table yet,
 * no unique match, or source off). */
int  openpete_mobyrank_lookup(uint32_t moby, uint32_t fkey);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOBY_RANK_H */
