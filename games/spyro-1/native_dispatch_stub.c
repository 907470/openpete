/* Stub used when -DOPENPETE_SKIP_DECOMP=ON. Replaces the generated
 * native_dispatch.c so every PSX vaddr falls through to recomp instead
 * of dispatching into a rewritten decomp TU. */

#include "openpete_psx.h"

#include <stdint.h>

int spyro_native_dispatch(CPUState* cpu, uint32_t addr) {
    (void)cpu;
    (void)addr;
    return 0;
}
