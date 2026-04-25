// games/spyro-1/psyq_stub_runtime.c — backing implementation for the
// generated psyq_stubs.c. Lives outside the generated file because the
// generated file deliberately avoids <stdio.h>/<stdlib.h> (PSYQ exports
// names like printf/exit/sprintf that would collide with libc).

#include "psx_runtime.h"

#include <stdio.h>
#include <stdlib.h>

void psyq_stub_hit(const char* name, uint32_t addr, CPUState* cpu) {
    fprintf(stderr,
            "openpete: PSYQ stub %s @ 0x%08X reached "
            "(Phase 1b limit — Phase 1c wires the real call)\n"
            "  ra=0x%08X sp=0x%08X gp=0x%08X\n",
            name, addr,
            cpu ? cpu->ra : 0u, cpu ? cpu->sp : 0u, cpu ? cpu->gp : 0u);
    /* MinGW's stdio buffers stderr when stderr is redirected (e.g. via
     * `>file 2>&1` or running .exe under WSL2 interop). abort() goes
     * straight to TerminateProcess without flushing, so the stub-hit
     * message would be lost — looking like a silent window-open/close. */
    fflush(stderr);
    abort();
}
