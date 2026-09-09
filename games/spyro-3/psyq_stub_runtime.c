// games/spyro-3/psyq_stub_runtime.c — backing implementation for the generated
// psyq_stubs.c. It lives outside the generated file because that file avoids
// <stdio.h>/<stdlib.h>: PSYQ exports names like printf, exit and sprintf,
// which would collide with libc.

#include "psx_runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>

void psyq_stub_hit(const char* name, uint32_t addr, CPUState* cpu) {
    fprintf(stderr,
            "openpete: PSYQ stub %s @ 0x%08X reached (no thunk for it yet)\n"
            "  ra=0x%08X sp=0x%08X gp=0x%08X\n",
            name, addr,
            cpu ? cpu->ra : 0u, cpu ? cpu->sp : 0u, cpu ? cpu->gp : 0u);
    fflush(stderr);
    abort();
}
