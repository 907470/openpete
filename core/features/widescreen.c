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
//   * The polybuf relocation block: still armed by the --no-lod
//     PsyCross-mode producer stack (openpete_no_lod_producers); deletes
//     with it.

#include "features/widescreen.h"

#include "openpete_fiber.h"   /* openpete_fov_h_override */
#include "openpete_log.h"
#include "psx_runtime.h"      /* psx_runtime_register_region, polybuf_size */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Canonical (un-widened) SX values in q16, stashed by gte.cpp's RTPS
 * after each projection in a 3-deep FIFO matching push_sxy semantics.
 * sx2_q16 is the latest projection (= what mfc2 C2_SXY2 reads, but in
 * full q16 precision before int16 truncation). Used by the particle
 * cull substitutions in r_particles.s to compute exact canonical 4:3
 * verdicts without truncation ambiguity at the [0,512) boundaries. */
int64_t openpete_canonical_sx0_q16 = 0;
int64_t openpete_canonical_sx1_q16 = 0;
int64_t openpete_canonical_sx2_q16 = 0;

/* Spyro polybuf slot size. Default 0x1C000 = byte-identical to the
 * native PSX layout (4BEF8.c:25-27 + draw.c:947/1561/2665). Grown by
 * the --no-lod producer stack once the relocated buffer is registered.
 * See psx_runtime.h for the substitution-PC list. */
uint32_t psx_runtime_polybuf_size = 0x0001C000u;

/* Spyro polybuf base override. Default 0 = sentinel ("use canonical
 * m_LowerPolyBuffer"). When non-zero, the recompiler substitutes this
 * value at the two LW PCs that read m_LowerPolyBuffer for the cursor
 * reset paths (pause-resume bracket and Gamestate-10 entry). Without
 * the substitution, those resets re-anchor D_800757B0 at the canonical
 * buffer while D_80075780 is `canonical_lower + psx_runtime_polybuf_size`
 * (per the polybuf_size sub), so a relocated-buffer scene overruns into
 * m_HigherPolyBuffer / HudOTStart and DrawOTag follows a corrupted
 * P_TAG into a hang. The third polybuf_size site (GamestateDraw
 * bracket) reads `db->m_PolyBuf` directly — that pointer is already
 * redirected by the AllocateBuffers post-call hook, so no LW
 * substitution is needed there. See psx_runtime.h for the PC list. */
uint32_t psx_runtime_polybuf_lower = 0u;

void widescreen_apply_fov(const openpete_options_t* opts) {
    /* --fov: override the GTE H register to the user's explicit
     * horizontal FOV at the PSX 320-wide visible plane. Spyro's native
     * horizontal FOV at 4:3 is 2*atan(160/341) ≈ 50.4° (PSX visible
     * half-width 160 / GTE H 341). Consumed by widescreen_eff_h in
     * gte_rtps_internal. 0 = no-override sentinel. */
    if (opts->fov_deg > 0.0f) {
        double half = (double)opts->fov_deg * 0.5 * (M_PI / 180.0);
        double h = 160.0 / tan(half);
        if (h < 1.0) h = 1.0;
        if (h > 65535.0) h = 65535.0;
        openpete_fov_h_override = (int)(h + 0.5);
        OPL_INFO_F("fov", "override active: --fov=%.2f° -> GTE H=%d "
                   "(native h_fov ~50.4° at H=341)",
                   (double)opts->fov_deg, openpete_fov_h_override);
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
 * polybuf isn't relocated (default) or the address falls outside the
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
