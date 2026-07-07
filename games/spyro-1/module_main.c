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

const openpete_recomp_module spyro1_recomp_module = {
    OPENPETE_RECOMP_ABI_VERSION,
    /* codegen_stamp: the boot-compile pipeline (D2) stamps real hashes;
     * the dev static build is pinned by the decomp submodule instead. */
    "dev-static",
    /* rom_sha256: identity travels with boot-compiled modules only. */
    "",
    psx_dispatch_compiled,
};
