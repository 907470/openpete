// games/common/module_main.c — the recomp-module descriptor.
//
// Compiled into the module beside the generated *_full.c/*_dispatch.c and
// overlay_dispatch.c: the single symbol the host resolves to reach recompiled
// code. The static build registers it from game main; the shared-linkage
// build resolves it by name from the dlopened module, and the boot-compile
// pipeline compiles this TU with both stamps overridden by -D (codegen = hash
// of PSXRecomp + flags; rom = the disc's sha256).

#include "openpete_recomp_module.h"

/* overlay_dispatch.c — fans out to psx_dispatch_main_exe + active overlay. */
extern int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);

#ifndef OPENPETE_CODEGEN_STAMP
#define OPENPETE_CODEGEN_STAMP "dev-static"
#endif
#ifndef OPENPETE_ROM_SHA256
#define OPENPETE_ROM_SHA256 ""
#endif

const openpete_recomp_module openpete_game_recomp_module = {
    OPENPETE_RECOMP_ABI_VERSION,
    OPENPETE_CODEGEN_STAMP,
    OPENPETE_ROM_SHA256,
    psx_dispatch_compiled,
};
