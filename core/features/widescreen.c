// core/features/widescreen.c — see widescreen.h.
//
// Verbatim moved from core/runtime/run.c as Phase 2.1a of
// notes/plans/openpete-feature-modules.md. No logic changes.

#include "features/widescreen.h"

#include "openpete_fiber.h"   /* openpete_disp_w_factor, openpete_fov_h_override */
#include "openpete_log.h"
#include "openpete_world_geo.h" /* openpete_world_geo_set_current (dbg/world hybrid hook) */
#include "psx_runtime.h"      /* psx_runtime_register_region, polybuf_size */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Companion to _moby_cone_extra_q12: X-bias aspect scaling. Q12 of
 * (factor-1), so 0 = identity. See psx_runtime.h. */
int32_t psx_runtime_moby_cone_bias_scale_q12 = 0;

/* Phase 7.2 near-Z clamp soften. Floor used by the cone substitutions'
 * mac3p clamp; default 0 preserves the existing native clamp. */
int32_t psx_runtime_moby_cone_mac3p_floor = 0;

/* r_environment.s func_800258F0 per-vertex camera-space X-bound cull.
 * Default 0x100 (= 256, native). Widened during the render pass to
 * 0x100 * thresh_factor; reset to 0x100 outside render so the
 * gameplay-side GTE work sees canonical bounds. See psx_runtime.h. */
uint32_t psx_runtime_env_cell_x_bound = 0x100u;

/* GTE SX/SY saturation bound. Default 0x400 = native PSX hardware
 * (±0x400 signed 11-bit clamp on push_sxy). See psx_runtime.h. */
uint32_t psx_runtime_sxy_sat_bound = 0x400u;

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

/* Companion to native_cone_pass for the DISTANCE half of the moby cull.
 * --no-lod saturates m_Distance (Phase 8) so distant mobys pass the
 * box-cull and render; but the per-bucket walker also sets m_WasDrawn on
 * them, and gameplay reads m_WasDrawn next frame to gate per-moby AI
 * (including rand() calls) — so the saturated distance leaks into
 * gameplay and cascades the RNG after the first level load. To close it
 * we replay the CANONICAL (un-saturated) box-cull verdict here and gate
 * m_WasDrawn on (native_cone_pass && native_dist_pass), exactly mirroring
 * the cone-channel closure. `mdist_thresh_delta` = (saturated −
 * canonical) m_Distance << 8, captured at the m_Distance lhu; it is 0
 * when --no-lod is off or the moby wasn't saturated, making the gate an
 * identity. Reset to 1 at each iteration's m_WasDrawn clear. */
uint8_t psx_runtime_native_dist_pass = 1;
int32_t psx_runtime_mdist_thresh_delta = 0;

/* Append-call hooks for the box-cull distance verdict (channel
 * no_lod_moby_distance_cull_verdict). The recompiler emits a CALL to one of
 * these AFTER the canonical box-cull add/sub, passing the dest-register
 * comparand `v`; we clear native_dist_pass if the canonical (un-saturated)
 * 4:3 cull would reject. This is the named-hook form of what was previously
 * inline C in the vendored recompiler — moving the verdict math out of
 * third_party and into this feature TU (no Spyro addresses, so core/ purity
 * holds). Identity when --no-lod is off (mdist_thresh_delta == 0): _lower
 * reduces to `v <= 0`, _upper to `v >= 0`, exactly the canonical box-cull
 * boundary the recompiled add/sub already computed into `v`. */
void psx_runtime_dist_gate_lower(int32_t v) {
    if ((int32_t)(v - psx_runtime_mdist_thresh_delta) <= 0)
        psx_runtime_native_dist_pass = 0;
}
void psx_runtime_dist_gate_upper(int32_t v) {
    if ((int32_t)(v + psx_runtime_mdist_thresh_delta) >= 0)
        psx_runtime_native_dist_pass = 0;
}

/* --no-lod distance-saturation ReplaceCall hooks (moved out of the
 * recompiler). The recompiler emits `rt = hook(cpu, ea)` where ea is the
 * load's effective address; the hook does the read + the --no-lod
 * conditional. Identity when openpete_no_lod == 0 (returns the raw load). */
uint32_t psx_runtime_no_lod_saturate32(CPUState* cpu, uint32_t addr) {
    uint32_t v = cpu->read_word(addr);
    return openpete_no_lod ? 0x7FFFFFFFu : v;
}
uint32_t psx_runtime_no_lod_moby_distance(CPUState* cpu, uint32_t addr) {
    uint32_t v = cpu->read_half(addr);
    if (openpete_no_lod && (v & 0x7Fu) != 0u) {
        psx_runtime_mdist_thresh_delta = (int32_t)((0x7Fu - (v & 0x7Fu)) << 8);
        return (v & 0xFF80u) | 0x7Fu;
    }
    psx_runtime_mdist_thresh_delta = 0;
    return v;
}

/* Moby cone-X cull ReplaceCall hooks (moved out of the recompiler). Each
 * records the CANONICAL (4:3) cone verdict in native_cone_pass (read by the
 * m_WasDrawn=1 gate) and RETURNS the widened cull comparand: the same
 * `4*(|MAC1|-radius) - (3 + 4*extra)*MAC3p` slope, with MAC3p clamped to
 * mac3p_floor so a behind-camera moby (MAC3p<0) never sign-flips the widening
 * and tightens the cone. Identity at default knobs (extra/bias 0 => the
 * canonical 4:3 comparand). `_permoby` (regular/specular) uses the per-moby
 * x-radius a2 and MAC3=v1, a3=a3; `_shaded` uses the fixed 102/77 constants
 * and MAC3=gte_data[27]. */
int32_t psx_runtime_cone_widen_permoby(int32_t mac1_abs, int32_t a2,
                                       int32_t mac3, int32_t a3) {
    psx_runtime_native_cone_pass =
        (uint8_t)((4 * (mac1_abs - a2) - 3 * (mac3 + a3)) < 0);
    int32_t mac3p = mac3 + a3;
    if (mac3p < psx_runtime_moby_cone_mac3p_floor)
        mac3p = psx_runtime_moby_cone_mac3p_floor;
    return mac1_abs - a2
         - (int32_t)(((int64_t)mac3p * psx_runtime_moby_cone_extra_q12) >> 12)
         - (int32_t)(((int64_t)a2 * psx_runtime_moby_cone_bias_scale_q12) >> 12);
}
int32_t psx_runtime_cone_widen_shaded(int32_t mac1_abs, int32_t mac3) {
    psx_runtime_native_cone_pass =
        (uint8_t)((4 * (mac1_abs - 102) - 3 * (mac3 + 77)) < 0);
    int32_t mac3p = mac3 + 77;
    if (mac3p < psx_runtime_moby_cone_mac3p_floor)
        mac3p = psx_runtime_moby_cone_mac3p_floor;
    return mac1_abs - 102
         - (int32_t)(((int64_t)mac3p * psx_runtime_moby_cone_extra_q12) >> 12)
         - (int32_t)((102 * (int64_t)psx_runtime_moby_cone_bias_scale_q12) >> 12);
}

/* === Misc ReplaceCall hooks (moved out of the recompiler) ============== */

/* --no-lod env HP-walk texture LOD: suppress the `addi $t5,$t5,8` HQ->LQ
 * descriptor bump so every quad reads the HQ half. Identity at default. */
int32_t psx_runtime_no_lod_tier_bump(int32_t base, int32_t bump) {
    return openpete_no_lod ? base : (base + bump);
}

/* --no-lod moby model LOD collapse: the LP-band face-list load reads
 * m_LpFaces (offset 0x1C) by default; --no-lod swings the knob to 0x14
 * (m_Faces, high-poly). The load itself stays here so the offset knob is
 * the only moving part. */
uint32_t psx_runtime_moby_lp_face_load(CPUState* cpu, uint32_t base) {
    return cpu->read_word(base + psx_runtime_moby_lp_face_offset);
}

/* Widescreen env-cell lateral-bound cull, companion -0x200 site: lands at
 * (X - bound) = base - 2*bound. Identity at default bound (256). */
int32_t psx_runtime_env_cell_x_bound_neg(int32_t base) {
    return base - 2 * (int32_t)psx_runtime_env_cell_x_bound;
}

/* Widescreen portal clip-rect X-upper test (`slti $r, 0x201`): widened to
 * `r < portal_clip_x_max_plus_1`. Identity at default (513). */
int32_t psx_runtime_portal_clip_slt(int32_t v) {
    return v < (int32_t)psx_runtime_portal_clip_x_max_plus_1;
}

/* Debug-only (OPENPETE_DBG_KILL_LP): env-renderer g_SkipLowPolyWorld read —
 * OR in 1 when the knob is set so the LP-walk pre-pass is skipped. */
uint32_t psx_runtime_dbg_skiplpworld(CPUState* cpu, uint32_t addr) {
    return cpu->read_word(addr)
         | (psx_runtime_dbg_kill_env_lp_walk ? 1u : 0u);
}

/* === Flag-gated store hooks (void ReplaceCall) ======================== */
/* The recompiler emits `hook(cpu, ea, val);` replacing the original sb. Each
 * conditionally writes the gameplay-readable "drawn" byte so it reflects the
 * CANONICAL 4:3 verdict regardless of presentation flags (an extra drawn
 * moby/particle in widescreen/no-lod would leak into AI / rand()). */

/* m_WasDrawn iter-start clear (per-bucket render walker): force-pass
 * billboards / extended-radius mobys that bypass the cone test, then store. */
void psx_runtime_mwasdrawn_clear_store(CPUState* cpu, uint32_t addr,
                                       uint32_t val) {
    psx_runtime_native_cone_pass = 1;
    psx_runtime_native_dist_pass = 1;
    cpu->write_byte(addr, (uint8_t)val);
}
/* m_WasDrawn=1 cull-pass write: gated on BOTH canonical box-cull halves
 * (cone + distance), so the saturated --no-lod / widened cone verdicts can't
 * set m_WasDrawn on a moby the canonical 4:3 cull would have dropped. */
void psx_runtime_mwasdrawn_store(CPUState* cpu, uint32_t addr, uint32_t val) {
    if (psx_runtime_native_cone_pass && psx_runtime_native_dist_pass)
        cpu->write_byte(addr, (uint8_t)val);
}
/* Particle "drawn" flag (m_03): gated on the canonical screen-cull verdict
 * so a widened-periphery particle doesn't drift psyq_rand_seed across aspects. */
void psx_runtime_part_drawn_store(CPUState* cpu, uint32_t addr, uint32_t val) {
    if (psx_runtime_native_part_pass)
        cpu->write_byte(addr, (uint8_t)val);
}

/* Debug LP-kill + world-geo strip tag hybrid (the 7th strip-list loop head,
 * co-owned with world_geo_strip_tag): force the loaded value
 * to 0 when the LP-kill knob is set (exits the loop), else read normally;
 * either way stamp the per-strip world-geo identity. Identity at default
 * knob (reads normally). */
uint32_t psx_runtime_dbg_lpwalk_strip(CPUState* cpu, uint32_t addr) {
    uint32_t v = psx_runtime_dbg_kill_env_lp_walk ? 0u : cpu->read_word(addr);
    openpete_world_geo_set_current(v);
    return v;
}

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

/* Spyro polybuf slot size. Default 0x1C000 = byte-identical to the
 * native PSX layout (4BEF8.c:25-27 + draw.c:947/1561/2665). Widened
 * by the 21:9 / 32:9 path once the relocated buffer is registered.
 * See psx_runtime.h for the substitution-PC list. */
uint32_t psx_runtime_polybuf_size = 0x0001C000u;

/* Spyro polybuf base override. Default 0 = sentinel ("use canonical
 * m_LowerPolyBuffer"). When non-zero, the recompiler substitutes this
 * value at the two LW PCs that read m_LowerPolyBuffer for the cursor
 * reset paths (pause-resume bracket and Gamestate-10 entry). Without
 * the substitution, those resets re-anchor D_800757B0 at the canonical
 * buffer while D_80075780 is `canonical_lower + psx_runtime_polybuf_size`
 * (per the polybuf_size sub), so a widened scene overruns into
 * m_HigherPolyBuffer / HudOTStart and DrawOTag follows a corrupted
 * P_TAG into a hang. The third polybuf_size site (GamestateDraw
 * bracket) reads `db->m_PolyBuf` directly — that pointer is already
 * redirected by the AllocateBuffers post-call hook, so no LW
 * substitution is needed there. See psx_runtime.h for the PC list. */
uint32_t psx_runtime_polybuf_lower = 0u;

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
         * the remaining ceiling is the PSX-side poly buffer.
         *
         * Lifting this cap was attempted via polybuf relocation through
         * the RAM virtualization primitive (Step 3 of
         * notes/plans/openpete-ram-virtualization.md) but uncovered a
         * deeper coupling: m_LowerPolyBuffer anchors transient scratch
         * BELOW it (m_LowerPolyBuffer - 0x40000 for titlescreen / update
         * image loads, m_LowerPolyBuffer - cycloramaSize for cyclorama
         * loads, m_LowerPolyBuffer - 0x2000 for memcard saves). Those
         * scratch regions are gameplay-readable, so they can't move
         * into a presentation-only registered region. See plan for the
         * follow-up work — keeping the 1.5 cap until it lands. */
        /* Polybuf cap (Option B host-side redirect).
         *
         * Beyond factor 1.5 the polybuf overflows the canonical
         * 0x1C000 slot; under Option B we register a host-backed
         * region of size ~ceil(0x1C000 * factor) per slot and
         * redirect g_DB[].m_PolyBuf into it. The layout anchor
         * (m_LowerPolyBuffer) stays canonical so the gameplay-
         * readable scratch zones (titlescreen image, cyclorama
         * dest, memcard, flight stash) are unaffected.
         *
         * If registration succeeds we lift the cap to 2.0 (the
         * cyclorama row-0 / moby cone visual ceiling). If it
         * fails (host alloc failure, region overlap), we fall
         * back to the 1.5 cap so prims continue to fit in the
         * canonical polybuf without overflow.
         *
         * See notes/plans/openpete-polybuf-host-redirect.md. */
        float thresh_factor = openpete_disp_w_factor;
        if (thresh_factor > 1.5f || openpete_no_lod) {
            uint32_t size_per_slot =
                ((uint32_t)((float)0x1C000 * thresh_factor) + 0x3FFFu)
                & ~0x3FFFu;
            /* --no-lod arms a 1 MB polybuf in game_apply_no_lod() before
             * this runs. widescreen_polybuf_setup is non-idempotent for
             * different sizes — bump our requested size up to the already-
             * armed value so the re-registration matches and returns 0
             * (idempotent). Otherwise the rc=2 reject below trips the
             * thresh_factor=1.5 fallback, clamping every right-edge cull
             * (clip_x_threshold, moby cone, env_cell_x_bound, sxy_sat)
             * while the framebuffer still draws the full wide aspect —
             * visible as missing right-edge polys at 21:9+ under --no-lod. */
            if (openpete_no_lod && size_per_slot < 0x100000u) {
                size_per_slot = 0x100000u;
            }
            if (widescreen_polybuf_setup(size_per_slot, 0x80400000u) == 0) {
                /* No upper cap once the host-backed polybuf is armed:
                 * cull surfaces (env clip_x_threshold, portal clamp, moby
                 * cone, cyclorama row-0) all scale linearly with
                 * thresh_factor and the polybuf can hold the wider scene.
                 * 32:9 (factor 2.667) and beyond render to the full
                 * host-ortho width without right-edge cutoff. */
            } else {
                OPL_WARN_F("aspect", "polybuf host-redirect failed "
                           "(size_per_slot=0x%X); falling back to 1.5 "
                           "cap on clip_x_threshold",
                           (unsigned)size_per_slot);
                thresh_factor = 1.5f;
            }
        }
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

        /* Companion X-bias scale. The slope-only widening above leaves a
         * near-Z slice with native cone width because the X-bias term
         * (102 in shaded, per-moby radius in regular/specular) doesn't
         * scale with aspect — and the `mac3p<0` clamp collapses the slope
         * widening near the camera plane. Multiplying the X-bias by the
         * factor restores aspect-proportional headroom at every Z. Q12 of
         * (factor-1) so 0 = identity. Render-pass only. */
        extern int32_t psx_runtime_moby_cone_bias_scale_q12;
        extern int32_t openpete_moby_cone_bias_scale_q12_render;
        float bias_scale = thresh_factor - 1.0f;
        if (bias_scale < 0.0f) bias_scale = 0.0f;
        if (getenv("OPENPETE_MOBY_CONE_BIAS_SCALE")) {
            bias_scale = (float)atof(getenv("OPENPETE_MOBY_CONE_BIAS_SCALE"));
            if (bias_scale < 0.0f) bias_scale = 0.0f;
        }
        openpete_moby_cone_bias_scale_q12_render =
            (int32_t)(bias_scale * 4096.0f);
        psx_runtime_moby_cone_bias_scale_q12 = 0;  /* native outside render */

        /* Phase 7.2: near-Z clamp soften. Lifts the mac3p->0 clamp to a
         * small positive floor so slope widening still applies a partial
         * cone in the strip right at the camera plane. Trades a tiny
         * near-Z over-admit (geometrically meaningless — the cone is
         * ill-defined behind-Z) for fewer aspect-asymmetric clips of
         * mobys at the screen edge near the camera. Default value 64
         * tuned to lift the floor by ~half the typical Z-bias (77 for
         * shaded), so slope widening gets meaningful traction even when
         * mac3p would have been clamped. Tunable via env var. */
        extern int32_t psx_runtime_moby_cone_mac3p_floor;
        extern int32_t openpete_moby_cone_mac3p_floor_render;
        int32_t mac3p_floor = 64;
        if (getenv("OPENPETE_MOBY_CONE_MAC3P_FLOOR")) {
            mac3p_floor = atoi(getenv("OPENPETE_MOBY_CONE_MAC3P_FLOOR"));
            if (mac3p_floor < 0) mac3p_floor = 0;
        }
        openpete_moby_cone_mac3p_floor_render = mac3p_floor;
        psx_runtime_moby_cone_mac3p_floor = 0;  /* native outside render */

        /* r_environment.s per-vertex lateral bound widening (render-pass
         * only). Native admits |MAC1| < 256; widescreen admits up to
         * 256 * thresh_factor so the env renderer projects vertices
         * across the full Hor+-widened framebuffer instead of leaving
         * the side strips uncovered (32:9 = factor 2.667, the right
         * 1/3 was bg-color before this). Y-axis bound is left native
         * since vertical FOV is preserved. */
        extern uint32_t psx_runtime_env_cell_x_bound;
        extern uint32_t openpete_env_cell_x_bound_render;
        openpete_env_cell_x_bound_render =
            (uint32_t)((float)0x100 * thresh_factor);
        psx_runtime_env_cell_x_bound = 0x100u;  /* native outside render */

        /* GTE SXY saturation bound — render-pass-only widening. Native
         * 0x400 clips widescreen vertices at PSX-X 1023. Widen to
         * 0x400 * thresh_factor during render. See psx_runtime.h. */
        extern uint32_t psx_runtime_sxy_sat_bound;
        extern uint32_t openpete_sxy_sat_bound_render;
        openpete_sxy_sat_bound_render =
            (uint32_t)((float)0x400 * thresh_factor);
        psx_runtime_sxy_sat_bound = 0x400u;  /* native outside render */

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

/* === Polybuf relocation (RAM virtualization first consumer) ============
 *
 * State for the relocated double-buffered polygon buffer. Two slots
 * sit back-to-back inside one registered region; AllocateBuffers'
 * substituted polybuf_size knob already scales the rest of the layout
 * math (D_80075780 end-cursor at draw.c:947/1561/2665, the negated
 * size in AllocateBuffers itself). The post-call hook in
 * games/spyro-1/game_extras_impl.c overwrites g_Buffers.{Lower,Higher}
 * PolyBuffer and g_DB[0/1].m_PolyBuf so reads pick up the relocated
 * pointer. See notes/plans/openpete-widescreen-fov.md item 2 and
 * notes/plans/openpete-ram-virtualization.md.
 */
static int      s_polybuf_active   = 0;
static uint32_t s_polybuf_psx_base = 0;   /* slot-0 vaddr inside region */
static uint8_t* s_polybuf_host     = NULL;
static uint32_t s_polybuf_canon_lo = 0;   /* canonical g_DB[0].m_PolyBuf */
static uint32_t s_polybuf_canon_hi = 0;   /* canonical g_DB[1].m_PolyBuf */

int      widescreen_polybuf_active(void)        { return s_polybuf_active; }
uint32_t widescreen_polybuf_lower_vaddr(void)   { return s_polybuf_psx_base; }
uint32_t widescreen_polybuf_higher_vaddr(void) {
    return s_polybuf_psx_base + psx_runtime_polybuf_size;
}

/* Record the canonical (un-relocated) per-slot poly-buffer base pointers
 * AllocateBuffers computed, before the post-call hook overwrites
 * g_DB[0/1].m_PolyBuf with the host-relocated ones. Needed so
 * psx_runtime_polybuf_to_canon() can map a host-backed poly-buffer
 * address back to the value real PSX hardware would hold. See the
 * AllocateBuffers hook in games/spyro-1/game_extras_impl.c. */
void widescreen_polybuf_set_canonical(uint32_t canon_lo, uint32_t canon_hi) {
    s_polybuf_canon_lo = canon_lo;
    s_polybuf_canon_hi = canon_hi;
}

/* Map a host-relocated poly-buffer address back to its canonical PSX
 * vaddr, preserving the within-slot byte offset. The recompiler calls
 * this at the sites where the game derives gameplay-readable pointers
 * (g_HudMobys, D_800756FC) from the per-frame write cursor: those
 * pointers must keep the value real hardware would hold even though the
 * prim backing store is relocated to host memory for capacity (see
 * notes/plans/openpete-polybuf-host-redirect.md). Identity when the
 * polybuf isn't relocated (4:3 default) or the address falls outside the
 * registered region, so substituted sites stay byte-identical. */
uint32_t psx_runtime_polybuf_to_canon(uint32_t host_addr) {
    if (!s_polybuf_active) return host_addr;
    uint32_t size = psx_runtime_polybuf_size;
    uint32_t off  = host_addr - s_polybuf_psx_base;
    if (off < size)        return s_polybuf_canon_lo + off;          /* slot 0 */
    if (off < size * 2u)   return s_polybuf_canon_hi + (off - size); /* slot 1 */
    return host_addr;  /* outside the relocated region — leave as-is */
}

int widescreen_polybuf_setup(uint32_t size_per_slot, uint32_t psx_base) {
    if (size_per_slot == 0u || psx_base < 0x80200000u) return 1;

    if (s_polybuf_active) {
        /* Idempotent if parameters match; reject otherwise to surface
         * a misconfiguration loud rather than silently re-registering. */
        if (s_polybuf_psx_base == psx_base
            && psx_runtime_polybuf_size == size_per_slot) return 0;
        OPL_ERROR_F("widescreen",
                    "polybuf already registered at 0x%08X size 0x%X; "
                    "re-registration with 0x%08X size 0x%X rejected",
                    s_polybuf_psx_base, (unsigned)psx_runtime_polybuf_size,
                    psx_base, (unsigned)size_per_slot);
        return 2;
    }

    uint32_t total = size_per_slot * 2u;
    s_polybuf_host = (uint8_t*)calloc(1, total);
    if (!s_polybuf_host) {
        OPL_ERROR_F("widescreen",
                    "polybuf host alloc failed (size 0x%X)", total);
        return 3;
    }

    int rc = psx_runtime_register_region(psx_base, total, s_polybuf_host);
    if (rc != 0) {
        OPL_ERROR_F("widescreen",
                    "psx_runtime_register_region failed rc=%d at 0x%08X "
                    "size 0x%X", rc, psx_base, total);
        free(s_polybuf_host);
        s_polybuf_host = NULL;
        return 4;
    }

    s_polybuf_psx_base        = psx_base;
    psx_runtime_polybuf_size  = size_per_slot;
    psx_runtime_polybuf_lower = psx_base;  /* arms the LW substitutions */
    s_polybuf_active          = 1;

    OPL_INFO_F("widescreen",
               "polybuf relocated to virtualized region: base=0x%08X "
               "slot=0x%X total=0x%X (slot0=0x%08X slot1=0x%08X)",
               psx_base, (unsigned)size_per_slot, total,
               psx_base, psx_base + size_per_slot);
    return 0;
}

