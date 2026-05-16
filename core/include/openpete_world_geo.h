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

/* PHASE-B: return the current strip's emission ordinal and post-
 * increment it. set_current resets the counter to 0, so the Nth
 * vertex GTE-projected within a strip gets ordinal N. G1-stable
 * emission order ⇒ (strip, ordinal) is an exact cross-frame
 * per-vertex world key. Called once per world vertex from
 * gte_rtps_internal's cache emit. */
uint16_t openpete_world_geo_next_vidx(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_WORLD_GEO_H */
