// games/spyro-1/features/transition_probe_native.c — TEMPORARY diagnostic probe.
//
// Logs (in red) when the two still-unfed level-transition draw functions fire,
// so the in-game moments can be found and recorded as bk2s for a native-vs-
// PsyCross comparison. Both functions are presentation-only / oracle-inert; this
// probe only LOGS and returns 0 (inert fall-through), so the real functions run
// normally and NO gameplay/render behaviour changes.
//
//   func_8004F000  level-exit / balloonist edge-fill cross-fade (reads g_SonyImage,
//                  RTPS + INTPL colour lerp toward black); called from
//                  func_8001C694 (GS_ExitLevel) and func_8001E24C (GS_Balloonist).
//   func_8001C694  GS_ExitLevel grayscale screen (4x POLY_FT4 tiles + black line,
//                  emitted via the unhooked world-OT add func_800168A0).
//
// Opt-in: OPENPETE_TRANSITION_PROBE=1. Remove once feeders for these land — see
// notes/plans/openpete-native-renderer.md "Transition-layer presentation gaps".

#include "feature_registry.h"
#include "psx_runtime.h"   /* CPUState (cpu->ra) */

#include <stdio.h>
#include <stdlib.h>

extern unsigned openpete_frame;

#define CRED   "\033[1;31m"
#define CRESET "\033[0m"

static int probe_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRANSITION_PROBE"); v = (e && *e && *e != '0'); }
    return v;
}

/* One red line per (func, frame) — a multi-call frame doesn't flood, but a
 * transition that spans many frames logs a burst that brackets it. ra names the
 * caller (func_8001C694 ~0x8001C6.. = GS_ExitLevel; func_8001E24C ~0x8001E2.. =
 * GS_Balloonist) so func_8004F000's two trigger paths are distinguishable. */
static void probe_log(const char* name, const char* what, CPUState* cpu, unsigned* last) {
    if (!probe_on()) return;
    if (*last == openpete_frame) return;
    *last = openpete_frame;
    fprintf(stderr, CRED "[TRANSITION-PROBE f=%u] %s  %s  (ra=0x%08x)" CRESET "\n",
            openpete_frame, name, what, cpu->ra);
}

static int on_8004F000(CPUState* cpu, uint32_t addr) {
    (void)addr; static unsigned last = 0xFFFFFFFFu;
    probe_log("func_8004F000", "edge-fill/cross-fade (GS_ExitLevel + GS_Balloonist)", cpu, &last);
    return 0;   /* inert: let the real function run */
}
static int on_8001C694(CPUState* cpu, uint32_t addr) {
    (void)addr; static unsigned last = 0xFFFFFFFFu;
    probe_log("func_8001C694", "GS_ExitLevel grayscale screen", cpu, &last);
    return 0;
}

static const feature_dispatch_entry_t transition_probe_dispatch[] = {
    { 0x8004F000u, on_8004F000 },
    { 0x8001C694u, on_8001C694 },
    { 0u, NULL },
};

const feature_module_t transition_probe_native_module = {
    .name           = "transition_probe_native",
    .apply_options  = NULL,
    .dispatch_table = transition_probe_dispatch,
};
