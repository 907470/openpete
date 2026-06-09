// games/spyro-1/features/polybuf.c — Spyro 1 polygon-buffer host-redirect
// hooks. Under wide aspects (>=21:9) / --no-lod the polybuf is host-backed and
// grown; these named hooks (recompiler-called, channels widescreen_polybuf_
// lower / polybuf_hudmoby_canonical) keep the gameplay-readable derived
// pointers (g_HudMobys) canonical while the cursor itself is redirected.
//
// The relocation machinery itself (widescreen_polybuf_setup, the host region,
// psx_runtime_polybuf_to_canon, the polybuf_size/lower knobs) stays in
// core/features/widescreen.c because the widescreen apply path calls it — this
// CU only holds the recompiler-facing hooks. Moved out of the
// core/features/widescreen.c grab-bag so the feature is siloed.

#include "psx_runtime.h"

#include <stdint.h>

/* m_LowerPolyBuffer cursor-reset read: return the host-backed polybuf base
 * when relocated (knob != 0), else the canonical load. Identity at default. */
uint32_t psx_runtime_polybuf_lower_load(CPUState* cpu, uint32_t addr) {
    uint32_t v = cpu->read_word(addr);
    return psx_runtime_polybuf_lower != 0u ? psx_runtime_polybuf_lower : v;
}
/* g_HudMobys store (Gamestate-11): map the stored host cursor back to its
 * canonical PSX vaddr so the gameplay-readable HUD-moby pointer matches
 * hardware. Identity at 4:3 (polybuf_to_canon is the identity map). */
void psx_runtime_polybuf_hudmoby_store(CPUState* cpu, uint32_t addr,
                                       uint32_t val) {
    cpu->write_word(addr, psx_runtime_polybuf_to_canon(val));
}
/* g_HudMobys store (GamestateDraw frame reset): the compiler reuses the grown
 * end-cursor (cursor + grown size); recover the cursor base, map to canonical,
 * then re-add the canonical 0x1C000 slot stride so the HUD pool stays one
 * canonical slot above the cursor as on hardware. */
void psx_runtime_polybuf_hudmoby_reset_store(CPUState* cpu, uint32_t addr,
                                             uint32_t val) {
    cpu->write_word(addr,
        psx_runtime_polybuf_to_canon(val - psx_runtime_polybuf_size) + 0x1C000u);
}
