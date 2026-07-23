// core/features/widescreen.c — see widescreen.h.
//
// 2026-07-06: the PsyCross-mode --aspect substitution stack was RETIRED
// (Phase 6 of notes/plans/openpete-native-renderer.md). The projection
// widening (disp_w_factor: GTE OFX + GL ortho), the cull-widening knobs
// (clip mask/threshold, moby cone, env bounds, SXY saturation, portal
// clamp, 2D full-bleed), the render bracket and the cyclorama multi-pass
// all deleted — every renderer mode now runs the game's verdict producers
// pure canonical 4:3, and widescreen is a native-renderer display policy
// (window-derived s_nr.ws_factor; see core/runtime/native_render.c).
//
// What remains here:
//   * --fov: the GTE H override (widescreen_eff_h consumer in
//     gte_rtps_internal). PsyCross-reference-mode feature; the native
//     renderer will grow its own FOV policy with the Phase-7 projection
//     work.
//   * The canonical-SX q16 FIFO: verdict machinery for the particle /
//     sparkle render->gameplay channels (games/spyro-1/features/
//     render_decouple.c reads it; the GTE stashes every RTPS).

#include "features/widescreen.h"

#include "openpete_fiber.h"   /* openpete_fov_h_override */
#include "openpete_log.h"

#include <math.h>
#include <stdint.h>

/* Canonical (un-widened) SX values in q16, stashed by gte.cpp's RTPS
 * after each projection in a 3-deep FIFO matching push_sxy semantics.
 * sx2_q16 is the latest projection (= what mfc2 C2_SXY2 reads, but in
 * full q16 precision before int16 truncation). Used by the particle
 * cull substitutions in r_particles.s to compute exact canonical 4:3
 * verdicts without truncation ambiguity at the [0,512) boundaries. */
int64_t openpete_canonical_sx0_q16 = 0;
int64_t openpete_canonical_sx1_q16 = 0;
int64_t openpete_canonical_sx2_q16 = 0;

void widescreen_set_fov(float fov_deg) {
    /* Override the GTE H register to an explicit horizontal FOV at the PSX
     * 320-wide visible plane. Spyro's native horizontal FOV at 4:3 is
     * 2*atan(160/341) ≈ 50.4° (PSX visible half-width 160 / GTE H 341).
     * Consumed by widescreen_eff_h in gte_rtps_internal, which re-reads the
     * override on every RTPS — so this is live, no latch. 0 clears. */
    if (fov_deg > 0.0f) {
        double half = (double)fov_deg * 0.5 * (M_PI / 180.0);
        double h = 160.0 / tan(half);
        if (h < 1.0) h = 1.0;
        if (h > 65535.0) h = 65535.0;
        openpete_fov_h_override = (int)(h + 0.5);
        OPL_INFO_F("fov", "override active: fov=%.2f° -> GTE H=%d "
                   "(native h_fov ~50.4° at H=341)",
                   (double)fov_deg, openpete_fov_h_override);
    } else if (openpete_fov_h_override) {
        openpete_fov_h_override = 0;
        OPL_INFO_F("fov", "%s", "override cleared (game default)");
    }
}

void widescreen_apply_fov(const openpete_options_t* opts) {
    if (opts->fov_deg > 0.0f)
        widescreen_set_fov(opts->fov_deg);
}
