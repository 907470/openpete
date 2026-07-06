// core/include/features/widescreen_gte_inline.h — GTE-side hooks,
// inlined: the --fov H override + the canonical-SX FIFO capture.
//
// Phase 3 of notes/plans/openpete-feature-modules.md. The GTE
// per-vertex hot path (gte_rtps_internal in
// third_party/psxrecomp/runner/src/gte.cpp) calls these as
// `static inline` (instead of a function-pointer vtable) to keep the
// hot-path codegen byte-identical to open-coded math. (The Hor+ OFX
// widening + UI-moby exception that lived here was retired 2026-07-06
// with the PsyCross-mode --aspect substitution stack.)
//
// Includers must already have <stdint.h> in scope (or this header will
// pull it in via the typedefs below).
//
// All symbols below have C linkage and resolve at link time to the
// definitions in core/features/widescreen.c + core/runtime/fiber.c.

#ifndef OPENPETE_FEATURES_WIDESCREEN_GTE_INLINE_H
#define OPENPETE_FEATURES_WIDESCREEN_GTE_INLINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int     openpete_fov_h_override;
extern int64_t openpete_canonical_sx0_q16;
extern int64_t openpete_canonical_sx1_q16;
extern int64_t openpete_canonical_sx2_q16;

/* GTE H override. 0 sentinel = no override → return the native value
 * (typically 341 for Spyro's 4:3 horizontal FOV ~50.4°). --fov sets
 * the override at boot; --aspect leaves H native and widens via OFX +
 * GL ortho instead. */
static inline int32_t widescreen_eff_h(int32_t native_h) {
    return (openpete_fov_h_override != 0)
           ? (int32_t)openpete_fov_h_override
           : native_h;
}

/* Stash this vertex's canonical (un-widened) SX in the 3-deep q16 FIFO
 * matching push_sxy semantics. Particle-cull substitutions in
 * r_particles.s read sx2_q16 (top of FIFO) for an exact 4:3 verdict. */
static inline void widescreen_push_canonical_sx_q16(int64_t sx_q16) {
    openpete_canonical_sx0_q16 = openpete_canonical_sx1_q16;
    openpete_canonical_sx1_q16 = openpete_canonical_sx2_q16;
    openpete_canonical_sx2_q16 = sx_q16;
}

#ifdef __cplusplus
}
#endif

#endif
