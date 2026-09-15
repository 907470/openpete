// games/spyro-1/renderer/render_decouple.c — Spyro 1 render→gameplay
// decoupling: the canonical-4:3 cull verdicts that keep gameplay-readable
// state (m_WasDrawn, particle m_03, sparkle alloc) bit-identical regardless
// of presentation flags (--aspect / --no-lod).
//
// These are the named-hook implementations the recompiler calls (generated
// from games/spyro-1/manifest/render_channels.toml, channels m_WasDrawn /
// no_lod_moby_distance_cull_verdict / particle_drawn_flag / sparkle_alloc_
// state). The bespoke verdict math lives here, host-side, instead of in the
// vendored recompiler, separate from core/renderer/widescreen.c so each
// feature is siloed in its own CU. No Spyro addresses
// here — the address anchoring lives in the recompiler + the manifest.
//
// Cross-CU reads (all declared in psx_runtime.h): the canonical-SX side
// channel (openpete_canonical_sx*_q16) is stashed by the GTE. This TU OWNS
// the per-iteration verdict flags below.

#include "psx_runtime_internal.h"

#include <stdint.h>

/* === Per-iteration verdict flags (owned here) =========================
 * Each per-bucket render walker / particle pass writes these alongside the
 * widened cull; the substituted gameplay-readable store sites gate on them so
 * gameplay sees the canonical 4:3 verdict. Reset per-moby/per-particle; render
 * is single-threaded so no concurrency concerns. */

/* "moby would have passed the native 4:3 cone test" (read by the m_WasDrawn=1
 * write gate). */
uint8_t psx_runtime_native_cone_pass = 0;

/* DISTANCE half of the box-cull verdict. native_dist_pass starts 1 each
 * iteration; the dist-gate hooks clear it if the box cull rejects. (The
 * saturated-vs-canonical mdist_thresh_delta these once subtracted out was
 * retired with the --no-lod producer stack — the .s cull is
 * canonical in every mode now, so the gates replay its own verdict.) */
uint8_t psx_runtime_native_dist_pass = 1;

/* Particle screen-cull verdict (read by the m_03 "drawn"-flag store gate). */
uint8_t psx_runtime_native_part_pass = 0;

/* === Moby cone-X cull verdict (channel m_WasDrawn) ====================
 * Records the canonical (4:3) cone verdict in native_cone_pass — the
 * verdict the m_WasDrawn=1 store gate reads — and returns the canonical
 * comparand unchanged (`|MAC1| - radius`; the generated code applies the
 * `4x - 3*(MAC3+bias)` shifts after the call, byte-identical to the .s).
 * The cone-WIDENING terms these hooks used to inject (PsyCross-mode
 * --aspect: extra_q12 slope + bias_scale + mac3p floor) were retired
 * with the substitution stack; the hooks stay because the
 * verdict recording is what the channel needs.
 * `_permoby` (regular/specular) uses per-moby x-radius a2 + MAC3=v1,a3;
 * `_shaded` uses the fixed 102/77 constants + MAC3=gte_data[27]. */
int32_t psx_runtime_cone_widen_permoby(int32_t mac1_abs, int32_t a2,
                                       int32_t mac3, int32_t a3) {
    psx_runtime_native_cone_pass =
        (uint8_t)((4 * (mac1_abs - a2) - 3 * (mac3 + a3)) < 0);
    return mac1_abs - a2;
}
int32_t psx_runtime_cone_widen_shaded(int32_t mac1_abs, int32_t mac3) {
    psx_runtime_native_cone_pass =
        (uint8_t)((4 * (mac1_abs - 102) - 3 * (mac3 + 77)) < 0);
    return mac1_abs - 102;
}

/* === Box-cull distance verdict (channel no_lod_moby_distance_cull_verdict) ==
 * Appended after the canonical box-cull add/sub; clears native_dist_pass
 * when the cull comparand rejects (replays the .s verdict). */
void psx_runtime_dist_gate_lower(int32_t v) {
    if (v <= 0)
        psx_runtime_native_dist_pass = 0;
}
void psx_runtime_dist_gate_upper(int32_t v) {
    if (v >= 0)
        psx_runtime_native_dist_pass = 0;
}

/* === Flag-gated stores (channels m_WasDrawn / particle_drawn_flag) =====
 * void ReplaceCall: the hook fully replaces the original sb, conditionally
 * writing the gameplay-readable "drawn" byte per the canonical 4:3 verdict
 * (an extra drawn moby/particle in widescreen/no-lod would leak into AI /
 * rand()). */

/* m_WasDrawn iter-start clear: force-pass billboards / extended-radius mobys
 * that bypass the cone test, then store. */
void psx_runtime_mwasdrawn_clear_store(CPUState* cpu, uint32_t addr,
                                       uint32_t val) {
    psx_runtime_native_cone_pass = 1;
    psx_runtime_native_dist_pass = 1;
    cpu->write_byte(addr, (uint8_t)val);
}
/* m_WasDrawn=1 cull-pass write: gated on BOTH canonical box-cull halves. */
void psx_runtime_mwasdrawn_store(CPUState* cpu, uint32_t addr, uint32_t val) {
    if (psx_runtime_native_cone_pass && psx_runtime_native_dist_pass)
        cpu->write_byte(addr, (uint8_t)val);
}
/* Particle "drawn" flag (m_03): gated on the canonical screen-cull verdict. */
void psx_runtime_part_drawn_store(CPUState* cpu, uint32_t addr, uint32_t val) {
    if (psx_runtime_native_part_pass)
        cpu->write_byte(addr, (uint8_t)val);
}

/* === Particle / sparkle canonical-SX cull (channels particle_drawn_flag /
 * sparkle_alloc_state) =================================================
 * The widened renderer projects to a widened SX, but the gameplay-readable
 * verdict must reflect the canonical 4:3 cull. These read the canonical SX
 * side channel (openpete_canonical_sx*_q16, stashed per-RTPS by the GTE) and
 * set native_part_pass / return the canonical SX. */

/* Single-vertex particle cull: record the canonical 0<SX<512 verdict from the
 * given FIFO slot, then return the original `a - b`. sx1 = classes that mfc2
 * between two RTPSes; sx2 = single-RTPS classes. */
int32_t psx_runtime_part_cull_sx1(int32_t a, int32_t b) {
    int32_t cs = (int32_t)(openpete_canonical_sx1_q16 >> 16);
    psx_runtime_native_part_pass = (uint8_t)((cs > 0) & (cs < 512));
    return a - b;
}
int32_t psx_runtime_part_cull_sx2(int32_t a, int32_t b) {
    int32_t cs = (int32_t)(openpete_canonical_sx2_q16 >> 16);
    psx_runtime_native_part_pass = (uint8_t)((cs > 0) & (cs < 512));
    return a - b;
}
/* 4-vertex strip cull: OR the canonical verdict across the FIFO tail
 * (vertices 1/2/3 -> sx0/sx1/sx2), then return `v << 16`. */
int32_t psx_runtime_part_cull_4vert(int32_t v) {
    int32_t s0 = (int32_t)(openpete_canonical_sx0_q16 >> 16);
    int32_t s1 = (int32_t)(openpete_canonical_sx1_q16 >> 16);
    int32_t s2 = (int32_t)(openpete_canonical_sx2_q16 >> 16);
    psx_runtime_native_part_pass = (uint8_t)(((s0 > 0) & (s0 < 512))
                                           | ((s1 > 0) & (s1 < 512))
                                           | ((s2 > 0) & (s2 < 512)));
    return v << 16;
}
/* Sparkle render-cull left/right clip: substitute $a1 with the canonical SX
 * (q16, re-shifted) so the kill verdict matches canonical 4:3 — keeps the
 * g_Sparkles alloc bookkeeping bit-identical across aspects. */
int32_t psx_runtime_sparkle_canon_sx(void) {
    return (int32_t)(openpete_canonical_sx2_q16 >> 16) << 16;
}
