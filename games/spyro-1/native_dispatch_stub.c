/* Stub used when -DOPENPETE_SKIP_DECOMP=ON. Replaces the generated
 * native_dispatch.c so every PSX vaddr falls through to recomp instead
 * of dispatching into a rewritten decomp TU. */

#include "openpete_psx.h"

#include <stdint.h>

/** @brief Declines every address, so each guest function runs its recompiled code.
 *
 * Stands in for the generated native-decomp dispatch when the build skips
 * the decomp.
 * @param cpu Unused.
 * @param addr Unused.
 * @return 0. */
int spyro_native_dispatch(CPUState* cpu, uint32_t addr) {
    (void)cpu;
    (void)addr;
    return 0;
}
