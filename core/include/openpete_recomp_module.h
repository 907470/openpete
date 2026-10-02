// core/include/openpete_recomp_module.h — the recomp-module descriptor.
//
// The ROM-derived recompiled code (generated *_full.c / *_dispatch.c +
// dispatch_chain.c) reaches the runtime through psx_runtime.h; the runtime
// reaches the recompiled code ONLY through this descriptor. Static builds
// register the built-in module from game main; the distribution build
// resolves the same descriptor from a dlopen'd module compiled on the
// user's machine and verifies the stamps before running.
#ifndef OPENPETE_RECOMP_MODULE_H
#define OPENPETE_RECOMP_MODULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUState CPUState;

/* Bump on ANY change to the psx_runtime.h ABI or to this struct. A
 * dynamically loaded module with a different abi_version is refused. */
#define OPENPETE_RECOMP_ABI_VERSION 1u

typedef struct openpete_recomp_module {
    uint32_t abi_version;      /* OPENPETE_RECOMP_ABI_VERSION at module build */
    const char* codegen_stamp; /* hash(PSXRecomp + emission metadata +
                                  toolchain id/flags + opt level); the static
                                  dev build carries "dev-static"; a
                                  boot-compiled module carries the hash */
    const char* rom_sha256;    /* identity of the ROM this module was built
                                  from; "" for dev-static builds (the regen
                                  inputs are pinned by the decomp submodule) */
    int (*dispatch)(CPUState* cpu, uint32_t addr); /* dispatch_chain entry */
} openpete_recomp_module;

/** @brief Register the active recomp module.
 *
 * Called before the first dispatch: by game main in static builds, by the
 * module loader in shared builds.
 * @param m Module descriptor; must outlive every dispatch.
 * @return 0 on success; -1 when @p m is NULL or its abi_version differs
 *         from OPENPETE_RECOMP_ABI_VERSION, and the active module is
 *         unchanged. */
int openpete_recomp_register(const openpete_recomp_module* m);

/** @brief Return the active recomp module.
 *
 * Before registration it is a sentinel whose dispatch logs the address and
 * aborts, so a missed registration fails at the first dispatch.
 * @return The module; never NULL. */
const openpete_recomp_module* openpete_recomp_module_get(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RECOMP_MODULE_H */
