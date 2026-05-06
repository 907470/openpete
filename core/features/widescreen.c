// core/features/widescreen.c — see widescreen.h.
//
// Verbatim moved from core/runtime/run.c as Phase 2.1a of
// notes/plans/openpete-feature-modules.md. No logic changes.

#include "features/widescreen.h"

#include "openpete_fiber.h"   /* openpete_disp_w_factor, openpete_fov_h_override */
#include "openpete_log.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* PsyCross runtime Hor+ aspect factor — see PsyX_public.h. We set it
 * post-PsyX_Initialise so the GL ortho widens by openpete's
 * disp_w_factor without touching VRAM-facing widths. */
extern float g_psyx_render_aspect_factor;

/* === Knob storage ====================================================
 * Phase 2.1c of notes/plans/openpete-feature-modules.md — these
 * globals previously lived in core/runtime/dispatch.c. The recompiler-
 * generated C reaches them via psx_runtime.h externs; the defining TU
 * is now this feature module. Defaults preserve byte-identical 4:3
 * behaviour; widescreen_apply_runtime_knobs() above writes the widened
 * values when --aspect is active.
 */

/* Per-vertex screen-space clip mask used by recompiled `andi reg, 0xFE00`
 * cull patterns (Spyro's r_environment.s SXY2 clip). Default 0xFE00 ==
 * native PSX behavior; openpete widens to 0xF800 when --aspect is set so
 * widescreen-extra primitives (SX > 512) survive the cull. */
uint32_t psx_runtime_clip_x_mask = 0xFE00u;

/* Cyclorama right-edge clip threshold; default 512<<16 (native). */
uint32_t psx_runtime_clip_x_threshold = 0x02000000u;

/* r_moby.s func_80022A2C inline cone-X cull widening. Default 0
 * preserves native 4:3 cone (slope 0.75). */
int32_t psx_runtime_moby_cone_extra_q12 = 0;

/* Per-iteration "moby would have passed the native 4:3 cone test" flag
 * computed alongside the widened cone test in each per-bucket render
 * walker. Consumed by the substituted m_WasDrawn=1 write sites so
 * gameplay's m_WasDrawn reads see the 4:3 verdict regardless of the
 * actual rendered cone width. Address tags live in the recompiler
 * substitution (third_party/psxrecomp/recompiler/src/code_generator.cpp)
 * and the doc at notes/plans/spyro-moby-update-vs-render.md; not
 * repeated here so the core/ purity check stays clean. Reset per-moby;
 * no thread or recursion concerns since render is single-threaded and
 * the read site is downstream of the cone test within the same
 * iteration. */
uint8_t psx_runtime_native_cone_pass = 0;

/* Per-iteration scratch flag for r_particles.s screen-cull. */
uint8_t psx_runtime_native_part_pass = 0;

/* Canonical 4:3 right-edge threshold expressed in widened-SX coords.
 * Default 0x02000000 (no widening). Set by widescreen_apply_runtime_knobs
 * to (0x200 + delta_OFX) << 16 when --aspect widens factor != 1.0. */
uint32_t psx_runtime_native_clip_x_in_widened_space = 0x02000000u;

/* Canonical 4:3 LEFT-edge threshold in widened-SX coords. Default 0
 * (no widening = native left edge at SX=0). Set to delta_OFX << 16
 * when --aspect widens factor != 1.0. */
uint32_t psx_runtime_native_clip_x_low_in_widened_space = 0u;

/* Canonical (un-widened) SX values in q16, stashed by gte.cpp's RTPS
 * after each projection in a 3-deep FIFO matching push_sxy semantics.
 * sx2_q16 is the latest projection (= what mfc2 C2_SXY2 reads, but in
 * full q16 precision before int16 truncation). Used by the particle
 * cull substitutions in r_particles.s to compute exact canonical 4:3
 * verdicts without delta-OFX rounding errors. */
int64_t openpete_canonical_sx0_q16 = 0;
int64_t openpete_canonical_sx1_q16 = 0;
int64_t openpete_canonical_sx2_q16 = 0;

/* r_cyclorama.s func_80050BD0 portal clip-rect X-upper clamp. Defaults
 * 512 / 513 preserve native 4:3 behavior. */
uint32_t psx_runtime_portal_clip_x_max = 0x200u;
uint32_t psx_runtime_portal_clip_x_max_plus_1 = 0x201u;

/* 2D full-bleed primitive width (cutscene letterbox + fade overlay).
 * Default 512 = byte-identical 4:3. */
uint32_t psx_runtime_fullbleed_x_max = 0x200u;

void widescreen_apply_disp_w_factor(const openpete_options_t* opts) {
    /* --aspect: widen the framebuffer + GTE offset so the GTE projects
     * vertices across a wider screen (Hor+ widescreen). The factor
     * stays as a float global; thunks multiply at every Set*Env / offset
     * call. 0.0f sentinel keeps the multiplier inactive when no
     * override is requested. */
    if (opts->aspect_w_over_h > 0.0f) {
        openpete_disp_w_factor = opts->aspect_w_over_h / (4.0f / 3.0f);
        OPL_INFO_F("aspect", "Hor+ widescreen: aspect=%.4f -> "
                   "disp_w_factor=%.4f (GTE offset 256 -> ~%d, GL ortho "
                   "widens via PsyCross g_psyx_render_aspect_factor; VRAM "
                   "DISPENV/DRAWENV stay at native 512 to preserve "
                   "texture pages)",
                   (double)opts->aspect_w_over_h, (double)openpete_disp_w_factor,
                   (int)(256.0f * openpete_disp_w_factor + 0.5f));
    }

    /* --fov / --aspect FOV resolution.
     *
     * Spyro's native horizontal FOV at 4:3 is 2*atan(160/341) ≈ 50.4°
     * (PSX visible half-width 160 / GTE H 341).
     *
     * For true Hor+ widescreen via --aspect alone we leave GTE H at the
     * game's native value: widening OFX (256 -> 256*factor) plus the GL
     * ortho widening already place the projection centre at the centre
     * of the wider framebuffer, and the wider framebuffer admits more
     * horizontal angular range at unchanged H. Vertical FOV is preserved
     * because half_screen_h / H is unchanged. (Earlier versions also
     * scaled H down by disp_w_factor; that double-applied widening on
     * top of OFX, zooming both axes out by 1/factor — the 4:3 view ended
     * up uniformly compressed inside the 16:9 frame instead of being its
     * centered crop.)
     *
     * --fov still overrides H to the user's explicit horizontal FOV at
     * the PSX 320-wide visible plane. It composes with --aspect (the
     * user-chosen cone lands centred in the widened window via OFX
     * widening) but no longer auto-derives from the aspect factor.
     *
     * 0 = no override sentinel for openpete_fov_h_override. */
    if (opts->fov_deg > 0.0f) {
        double half = (double)opts->fov_deg * 0.5 * (M_PI / 180.0);
        double h = 160.0 / tan(half);
        if (h < 1.0) h = 1.0;
        if (h > 65535.0) h = 65535.0;
        openpete_fov_h_override = (int)(h + 0.5);
        OPL_INFO_F("fov", "override active: --fov=%.2f° -> GTE H=%d "
                   "(native h_fov ~50.4° at H=341)",
                   (double)opts->fov_deg, openpete_fov_h_override);
    } else if (openpete_disp_w_factor > 0.0f) {
        /* --aspect only: keep native H. Hor+ widening is achieved by
         * OFX scaling + GL ortho widening alone. */
        double tan_h_native = 256.0 / 341.0;
        double tan_h_widened = tan_h_native * (double)openpete_disp_w_factor;
        double new_h_fov_deg = 2.0 * atan(tan_h_widened) * (180.0 / M_PI);
        OPL_INFO_F("fov", "Hor+ from --aspect (H unchanged at native): "
                   "h_fov ~%.1f° (4:3 native ~50.4°), vertical FOV "
                   "preserved at ~38.7°",
                   new_h_fov_deg);
    }
}

void widescreen_apply_runtime_knobs(const openpete_options_t* opts) {
    (void)opts;  /* derives entirely from openpete_disp_w_factor set above */

    /* Push the host's Hor+ widening factor into PsyCross's GL ortho.
     * Must happen after PsyX_Initialise (the symbol exists at link
     * time but the GL backend's per-scene state lives behind the
     * initialiser). 0.0f openpete sentinel = no override → leave the
     * PsyCross default 1.0f untouched. */
    if (openpete_disp_w_factor > 0.0f) {
        g_psyx_render_aspect_factor = openpete_disp_w_factor;

        /* Widen psxrecomp's per-vertex clip mask so the env renderer's
         * `andi SXY2, 0xFE00` cull (r_environment.s:714, 1145, 1255)
         * doesn't drop widescreen-extra primitives whose SX legitimately
         * exceeds 512. 0xF800 admits SX in [-2048, 2047], plenty for
         * 21:9 (max ~853). Off-screen primitives still get GPU-clipped
         * to drawenv [0,512]; this just keeps them out of the early-out
         * AND-fold. See third_party/psxrecomp/runner/include/psx_runtime.h. */
        extern uint32_t psx_runtime_clip_x_mask;
        extern uint32_t psx_runtime_clip_x_threshold;
        psx_runtime_clip_x_mask      = 0xF800u;
        /* Edge = 512 * factor (visible right edge in PSX coords). Capped at
         * 1.5: above that, the original Spyro poly buffer (0x1c000 bytes
         * per slot in src/4BEF8.c) overflows when env emits the extra
         * off-axis primitives, corrupting the trailing OT and crashing
         * downstream. The PsyCross host-side vertex/OT buffers were bumped
         * to remove their crash modes (DrawSplit strlen, DrawOTag scratch);
         * the remaining ceiling is the PSX-side poly buffer, which would
         * need invasive realloc to lift further. 1.5 covers full 16:9 and
         * gets ~3% improvement at 21:9/32:9. */
        float thresh_factor = openpete_disp_w_factor;
        if (thresh_factor > 1.5f) thresh_factor = 1.5f;
        uint32_t edge = (uint32_t)((float)512.0f * thresh_factor);
        psx_runtime_clip_x_threshold = edge << 16;
        /* Canonical 4:3 right-edge in widened-SX coords: native edge
         * 0x200 plus the OFX delta-shift `(factor - 1) * 256`. The
         * particle screen-cull substitution gates m_03 stores against
         * this so gameplay sees the 4:3 verdict regardless of
         * widening. See psx_runtime.h. */
        extern uint32_t psx_runtime_native_clip_x_in_widened_space;
        extern uint32_t psx_runtime_native_clip_x_low_in_widened_space;
        /* Delta-OFX in Q16 (matches gte.cpp `eff_OFX = OFX * factor` math).
         * Truncated-to-pixel rounding off-by-one'd the boundary case
         * (SX=511 canonical → SX=597 widened with actual fractional delta
         * 85.333 px, but pixel-truncated delta 85 puts the threshold at
         * 597 itself, rejecting the SX=597 widened vertex). Keeping the
         * delta in Q16 throughout closes the rounding gap. */
        int64_t ofx_canonical_q16 = (int64_t)0x01000000;  /* 256<<16, the
            canonical PSX OFX value Spyro's renderer uses; gte.cpp scales
            this when openpete_render_pass_active && factor>0. */
        int64_t ofx_widened_q16 = (int64_t)((double)ofx_canonical_q16
                                             * (double)thresh_factor);
        uint32_t delta_ofx_q16 = (uint32_t)(ofx_widened_q16 - ofx_canonical_q16);
        psx_runtime_native_clip_x_in_widened_space     = 0x02000000u + delta_ofx_q16;
        psx_runtime_native_clip_x_low_in_widened_space = delta_ofx_q16;
        OPL_INFO_F("aspect", "particle canonical-cull thresholds (Q16): "
                   "low=0x%08X high=0x%08X (delta_ofx_q16 0x%08X = %.3fpx)",
                   (unsigned)psx_runtime_native_clip_x_low_in_widened_space,
                   (unsigned)psx_runtime_native_clip_x_in_widened_space,
                   (unsigned)delta_ofx_q16,
                   (double)delta_ofx_q16 / 65536.0);
        OPL_INFO_F("aspect", "widened cull bounds: clip_x_mask "
                   "0xFE00 -> 0xF800 (env SX-overflow), clip_x_threshold "
                   "0x02000000 -> %u<<16 (env+cyclorama+moby+particles "
                   "right-edge cull, factor=%.3f)",
                   (unsigned)edge,
                   (double)(thresh_factor));

        /* r_moby.s func_80022A2C inline X-cone cull widening. The hardcoded
         * 0.75 slope (= 4:3 horizontal half-FOV, tan(36.87°)) culls
         * mobys whose world-space angle from camera-forward exceeds ~37°.
         * With H unchanged at native 341 and OFX widened to 256*factor,
         * the framebuffer right edge sx = 512*factor admits IR1/SZ up to
         * (512*factor - 256*factor)/341 = 256*factor/341 = 0.7507*factor.
         * But the env right-edge cull (clip_x_threshold above) caps the
         * effective right edge at 512*thresh_factor, so we use the same
         * thresh_factor here to keep the cone matched to the actually-
         * rendered right side at 21:9+ (where the cap kicks in).
         *
         * Substitution at addi-(-102) injects `-extra*(MAC3+77)`, so after
         * the chain the test becomes 4|x| - 408 - (3 + 4*extra)*(MAC3+77)
         * >= 0; new slope = (3 + 4*extra)/4. To match the visible cone:
         * extra = 0.7507*thresh_factor - 0.75.
         * Q12: extra_q12 = round(extra * 4096). */
        /* Stage the widened cone value into a host-side global; the
         * GamestateDraw dispatch hook swaps it into psx_runtime_moby_cone_extra_q12
         * for the render pass and zeroes it on exit, so func_80022A2C's
         * gameplay-side callers (moby ticking) keep seeing the native cone. */
        extern int32_t psx_runtime_moby_cone_extra_q12;
        extern int32_t openpete_moby_cone_extra_q12_render;
        float extra_f = (256.0f / 341.0f) * thresh_factor - 0.75f;
        if (extra_f < 0.0f) extra_f = 0.0f;
        if (getenv("OPENPETE_MOBY_CONE_EXTRA")) {
            extra_f = (float)atof(getenv("OPENPETE_MOBY_CONE_EXTRA"));
            if (extra_f < 0.0f) extra_f = 0.0f;
        }
        openpete_moby_cone_extra_q12_render = (int32_t)(extra_f * 4096.0f);
        psx_runtime_moby_cone_extra_q12 = 0;  /* native cone outside render pass */

        /* r_cyclorama.s func_80050BD0 portal clip-rect X-upper clamp.
         * Native code clamps the per-portal clip rect's right edge to
         * sx=512 (4:3 framebuffer width), so the cyclorama dome drawn
         * inside a level-entry portal gets cut off on the right at any
         * widening. Widen the clamp to 512*thresh_factor so the rect
         * matches the OFX-widened framebuffer; uses thresh_factor (cap
         * 1.5) so all right-edge cull surfaces line up. Default 512/513
         * preserves byte-identical 4:3. */
        extern uint32_t psx_runtime_portal_clip_x_max;
        extern uint32_t psx_runtime_portal_clip_x_max_plus_1;
        uint32_t portal_x_max = (uint32_t)(512.0f * thresh_factor);
        psx_runtime_portal_clip_x_max         = portal_x_max;
        psx_runtime_portal_clip_x_max_plus_1  = portal_x_max + 1u;

        /* 2D full-bleed prims (cutscene letterbox bars in func_80018F30,
         * fade-to-color overlay in func_800190D4). The PsyCross host ortho
         * widens to disp.w * openpete_disp_w_factor (left-anchored), so
         * widen these prims' PSX X-extent by the same factor — uncapped,
         * since this is a single screen-aligned quad (no poly-buffer or
         * cull-surface coupling). See psx_runtime.h. */
        extern uint32_t psx_runtime_fullbleed_x_max;
        /* ceil() not truncate: at 16:9 (factor 4/3) the exact width is
         * 682.666..., and truncating to 682 leaves a 1-PSX-px / ~1-host-px
         * sliver of the right edge uncovered. PsyCross ortho extends to
         * disp.w * factor (a float); the prim's PSX-X end must be >= that
         * after the host-px mapping, so round up. */
        float fullbleed_x_max_f = 512.0f * openpete_disp_w_factor;
        psx_runtime_fullbleed_x_max = (uint32_t)ceilf(fullbleed_x_max_f);
        OPL_INFO_F("aspect", "2D full-bleed X-max: 512 -> %u "
                   "(exact %.3f, factor=%.3f; letterbox/fade cover full "
                   "host width)",
                   (unsigned)psx_runtime_fullbleed_x_max,
                   (double)fullbleed_x_max_f,
                   (double)openpete_disp_w_factor);
        OPL_INFO_F("aspect", "portal clip-rect X-clamp: 512 -> %u "
                   "(thresh_factor=%.3f)",
                   (unsigned)portal_x_max, (double)thresh_factor);
        OPL_INFO_F("aspect", "moby cone-X widening: extra_q12=%d "
                   "(slope %.3f -> %.3f, admit half-FOV ~%.1f° -> ~%.1f°)",
                   (int)psx_runtime_moby_cone_extra_q12,
                   0.75,
                   (double)((3.0f + 4.0f * extra_f) / 4.0f),
                   36.87,
                   (double)(57.2958f * atan2f((3.0f + 4.0f * extra_f) / 4.0f,
                                              1.0f)));
    }
}
