// games/spyro-1/dispatch_chain.c — Phase 1d.5 / Phase 2.
//
// Fans `psx_dispatch_compiled` out to each generated dispatcher. Each
// dispatcher is renamed at compile time via `-Dpsx_dispatch_compiled=...`
// (see CMakeLists.txt) so their switches don't collide. Add overlays by
// extending the chain below.
//
// Overlay-resident dispatchers handle the same kseg0 vaddr range
// (0x8007AA38..) but for different bytes — only one overlay is mapped
// at runtime. The chain order is "first hit wins"; correctness relies
// on each overlay's discovered function addresses being disjoint. If
// two overlays ever expose the same address (none do today across
// titlescreen + level_10), this needs a state-driven swap on
// SetOverlayPointers / g_LevelId rather than blind chaining.

#include "psx_runtime.h"

extern int psx_dispatch_main_exe   (CPUState* cpu, uint32_t addr);
extern int psx_dispatch_titlescreen(CPUState* cpu, uint32_t addr);
extern int psx_dispatch_level_10   (CPUState* cpu, uint32_t addr);

int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    if (psx_dispatch_main_exe   (cpu, addr)) return 1;
    if (psx_dispatch_titlescreen(cpu, addr)) return 1;
    if (psx_dispatch_level_10   (cpu, addr)) return 1;
    return 0;
}
