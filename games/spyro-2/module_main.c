// games/spyro-2/module_main.c — the Spyro 2: Ripto's Rage! recomp-module descriptor.
//
// The single symbol the host resolves to reach recompiled code. In the static
// build, game main registers it directly.

#include "openpete_recomp_module.h"

/* overlay_dispatch.c — fans out to psx_dispatch_main_exe + active overlay. */
extern int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);

#ifndef OPENPETE_CODEGEN_STAMP
#define OPENPETE_CODEGEN_STAMP "dev-static"
#endif
#ifndef OPENPETE_ROM_SHA256
#define OPENPETE_ROM_SHA256 ""
#endif

const openpete_recomp_module spyro2_recomp_module = {
    OPENPETE_RECOMP_ABI_VERSION,
    OPENPETE_CODEGEN_STAMP,
    OPENPETE_ROM_SHA256,
    psx_dispatch_compiled,
};
