// games/spyro-1/module_main.c — the Spyro 1 recomp-module descriptor.
//
// Compiled INTO the module (alongside the generated *_full.c/*_dispatch.c
// and dispatch_chain.c): this TU is the single symbol the host resolves
// from a dynamically loaded module in the distribution build (plan
// openpete-distribution-local-recomp, D1). In the static build, game main
// registers it directly.
#include "openpete_recomp_module.h"

/* dispatch_chain.c — fans out to psx_dispatch_main_exe + active overlay. */
extern int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);

/* The boot-compile pipeline overrides both stamps with -D defines
 * (codegen = hash of PSXRecomp + flags; rom = the disc's sha256); the
 * dev static build is pinned by the decomp submodule instead. */
#ifndef OPENPETE_CODEGEN_STAMP
#define OPENPETE_CODEGEN_STAMP "dev-static"
#endif
#ifndef OPENPETE_ROM_SHA256
#define OPENPETE_ROM_SHA256 ""
#endif

const openpete_recomp_module spyro1_recomp_module = {
    OPENPETE_RECOMP_ABI_VERSION,
    OPENPETE_CODEGEN_STAMP,
    OPENPETE_ROM_SHA256,
    psx_dispatch_compiled,
};
