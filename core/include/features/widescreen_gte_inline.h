// core/include/features/widescreen_gte_inline.h — GTE-side widescreen
// hooks, inlined.
//
// Phase 3 of notes/plans/openpete-feature-modules.md. The GTE
// per-vertex hot path (gte_rtps_internal in
// third_party/psxrecomp/runner/src/gte.cpp) calls into widescreen logic
// for FOV-H override, OFX widening (with the UI-moby exception), and
// canonical-SX FIFO capture. Putting these as `static inline` here
// (instead of routing through a function-pointer vtable) keeps the
// hot-path codegen byte-identical to the open-coded math today —
// verified by objdump-diff'ing gte.o before/after this change is
// applied.
//
// Includers must already have <stdint.h> in scope (or this header will
// pull it in via the typedefs below).
//
// All symbols below have C linkage and resolve at link time to the
// definitions in core/features/widescreen.c +
// core/runtime/openpete_ui_moby.c.

#ifndef OPENPETE_FEATURES_WIDESCREEN_GTE_INLINE_H
#define OPENPETE_FEATURES_WIDESCREEN_GTE_INLINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern float   openpete_disp_w_factor;
extern int     openpete_render_pass_active;
extern int     openpete_fov_h_override;
extern int64_t openpete_canonical_sx0_q16;
extern int64_t openpete_canonical_sx1_q16;
extern int64_t openpete_canonical_sx2_q16;

int openpete_ui_moby_is_current_ui(void);

/* GTE H override. 0 sentinel = no override → return the native value
 * (typically 341 for Spyro's 4:3 horizontal FOV ~50.4°). --fov sets
 * the override at boot; --aspect leaves H native and widens via OFX +
 * GL ortho instead. */
static inline int32_t widescreen_eff_h(int32_t native_h) {
    return (openpete_fov_h_override != 0)
           ? (int32_t)openpete_fov_h_override
           : native_h;
}

/* OFX widening. See gte.cpp's original commentary on standard vs
 * UI-moby paths. Returns the widened OFX in q16 to feed projection. */
static inline int64_t widescreen_eff_ofx(int64_t native_ofx_q16) {
    if (openpete_disp_w_factor > 0.0f && openpete_render_pass_active) {
        /* §13.16: NON-CANONICAL OFX is the PSYQ 2D-via-GTE glyph-emit
         * idiom (gte.cpp Phase 1.0 finding) — Spyro's HUD writes each
         * icon/glyph's screen-X into OFX via inline CTC2, while every
         * gameplay 3D projection uses the canonical 256. The in-level
         * HUD renders inside func_80023AC4 (the Spyro renderer) under
         * the constant g_Spyro identity, so no per-moby UI classifier
         * can see those glyphs — the OFX value itself is the
         * authoritative UI signal. Canonical-OFX projections are
         * byte-unchanged (both formulas coincide at OFX=256 only for
         * factor terms; the explicit test keeps world widening exact). */
        if (native_ofx_q16 != ((int64_t)256 << 16) ||
            openpete_ui_moby_is_current_ui()) {
            const int64_t centring_shift_q16 = (int64_t)(
                ((double)openpete_disp_w_factor - 1.0) * 256.0 * 65536.0);
            return native_ofx_q16 + centring_shift_q16;
        } else {
            return (int64_t)((double)native_ofx_q16
                             * (double)openpete_disp_w_factor);
        }
    }
    return native_ofx_q16;
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
