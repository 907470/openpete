// games/spyro-1/dispatch_chain.c — Phase 1d.5.
//
// Fans `psx_dispatch_compiled` out to each generated dispatcher. Each
// dispatcher is renamed at compile time via `-Dpsx_dispatch_compiled=...`
// (see CMakeLists.txt) so their switches don't collide. Add overlays by
// extending the chain below.

#include "psx_runtime.h"

extern int psx_dispatch_main_exe  (CPUState* cpu, uint32_t addr);
extern int psx_dispatch_titlescreen(CPUState* cpu, uint32_t addr);

int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    if (psx_dispatch_main_exe(cpu, addr)) return 1;
    if (psx_dispatch_titlescreen(cpu, addr)) return 1;
    return 0;
}
