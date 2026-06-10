/* openpete_world_geo.h — side-channel world-strip identity.
 *
 * transform-interp §7.5-R3. The render cache needs a STABLE cross-
 * frame per-batch identity for world (terrain) geometry so the
 * matcher can pair the same world span across game frames K-1/K and
 * Tier-1-lerp its projected vertices. §7.5-R proved world IS
 * reprojectable at the cache layer (cached rt = live view matrix,
 * forward model exact) but the cache carries no stable world key
 * (v_local is camera-relative; the no-hook reconstruction was
 * disproven in §7.5-R2 — the GTE input origin is sector-relative).
 *
 * A recompiler instruction substitution at the game's dominant
 * terrain strip-entry stashes the freshly-aligned strip-base pointer
 * here (a stable per-strip address for static level geometry) just
 * before the strip's vertices are GTE-projected. gte_rtps_internal
 * then stamps obj_tag = world_geo present ? (strip | TAG_BIT) :
 * per-moby vaddr. TAG_BIT is free in 2 MB-RAM KSEG pointers, so the
 * marked world tag never collides with a moby vaddr or 0.
 *
 * Scope discipline: the per-moby loop substitution also calls
 * openpete_world_geo_clear(), so once moby rendering begins the
 * world tag falls back to the moby-vaddr path. Games that render
 * world before mobys each frame get correct dominant-path tagging.
 *
 * Game-agnostic in core/: the game-specific instruction addresses
 * live in the recompiler injection + manifest, never here. */

#ifndef OPENPETE_WORLD_GEO_H
#define OPENPETE_WORLD_GEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Marker bit OR-ed into the strip pointer to form the world obj_tag.
 * Free in 2 MB-RAM KSEG vaddrs ⇒ disjoint from moby vaddrs and 0. */
#define OPENPETE_WORLD_GEO_TAG_BIT 0x40000000u

/* PHASE-D key-family discriminator: world_vkey values with this bit
 * set are vertex SOURCE ADDRESSES (KSEG0 pointers); clear means the
 * PHASE-B per-strip emission ordinal (small int). Not an artificial
 * marker — KSEG0 addresses simply have bit31 set. */
#define OPENPETE_WORLD_VKEY_SRC_BIT 0x80000000u

/* Stash the strip-base pointer the GTE will project next. Called
 * from the recompiler-emitted prologue of the dominant terrain
 * strip loop in func_800258F0. */
void openpete_world_geo_set_current(uint32_t strip_addr);

/* Clear the stashed strip pointer (0 = "not in world geometry").
 * Called from the per-moby loop injection so moby projections fall
 * back to the moby-vaddr tag. */
void openpete_world_geo_clear(void);

/* Read back the stashed strip pointer (0 if cleared / unset). The
 * GTE emit path forms the final tag as
 * `strip ? (strip | OPENPETE_WORLD_GEO_TAG_BIT) : 0`. */
uint32_t openpete_world_geo_get_current(void);

/* PHASE-D: source-stable per-vertex world keys.
 *
 * The stream-variant terrain loops (dominant + the 3-loop family)
 * load one packed vertex word per RTPS through a software-pipelined
 * `lw; addi +4` stream — at RTPS time the NEXT vertex's load already
 * fired, so a "last load address" stash would be off by one. Instead
 * each vertex-load site pushes its effective load address into a
 * small FIFO and the GTE cache emit pops the OLDEST: pipelining
 * keeps the queue depth constant (prologue primes 2, each iteration
 * push 1 / pop 1) so pop order equals vertex order exactly.
 *
 * The near-clip refinement path (one of the family loops re-reads V0
 * from the GTE, scales it x16 and RTPS's the SAME vertex again) must
 * NOT pop — a recompiler marker on a refine-path-only instruction
 * arms `mark_refine`, and the next take reuses the last key.
 *
 * The subdivided/LOD variants (indexed corner loads + synthesized
 * scratchpad midpoints) have no per-vertex source pointer; they push
 * nothing and fall back to the PHASE-B per-strip emission counter.
 * The two key families are distinguishable by bit31: source keys are
 * KSEG0 addresses (bit31 set), counter keys are small ints (clear) —
 * the matcher uses this to retire the count-eq gate exactly where
 * source keys make it redundant. */
void openpete_world_geo_push_vsrc(uint32_t src_addr);
void openpete_world_geo_mark_refine(void);

/* Probe-tag variant: same stash, tag OR'd with bit29 so visibility-
 * probe RTPS batches (1 vert, never drawn) occupy a rank group
 * disjoint from the piece's render batches. See the .c rationale. */
void openpete_world_geo_set_current_probe(uint32_t strip_addr);

/* Per-world-vertex key, called once per world vertex from
 * gte_rtps_internal's cache emit: refine-reuse > FIFO pop (source
 * key) > per-strip counter (PHASE-B fallback). set_current resets
 * FIFO + counter + refine state. */
uint32_t openpete_world_geo_take_vkey(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_WORLD_GEO_H */
