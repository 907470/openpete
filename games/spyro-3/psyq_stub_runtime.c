// games/spyro-3/psyq_stub_runtime.c — backing implementation for the generated
// psyq_stubs.c. It lives outside the generated file because that file avoids
// <stdio.h>/<stdlib.h>: PSYQ exports names like printf, exit and sprintf,
// which would collide with libc.

#include "psx_runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* OPENPETE_PSYQ_STUB_CONTINUE=1 logs each distinct unimplemented PSYQ symbol
 * once and returns instead of aborting, so one run enumerates the whole
 * missing surface rather than only its first member. The guest then runs on
 * with that call having done nothing, which is not a state to draw
 * conclusions from -- it is a census of what to implement, not a test. */
#define STUB_SEEN_CAP 256
static const char* s_seen[STUB_SEEN_CAP];
static int s_seen_n;
static int s_continue = -1;

void psyq_stub_hit(const char* name, uint32_t addr, CPUState* cpu) {
    if (s_continue < 0) {
        const char* e = getenv("OPENPETE_PSYQ_STUB_CONTINUE");
        s_continue = (e && *e && *e != '0') ? 1 : 0;
    }

    if (s_continue) {
        for (int i = 0; i < s_seen_n; i++) {
            if (strcmp(s_seen[i], name) == 0) return;
        }
        if (s_seen_n < STUB_SEEN_CAP) s_seen[s_seen_n++] = name;
        fprintf(stderr, "psyq-missing: %s @ 0x%08X (ra=0x%08X)\n",
                name, addr, cpu ? cpu->ra : 0u);
        fflush(stderr);
        return;
    }

    fprintf(stderr,
            "openpete: PSYQ stub %s @ 0x%08X reached (no thunk for it yet)\n"
            "  ra=0x%08X sp=0x%08X gp=0x%08X\n",
            name, addr,
            cpu ? cpu->ra : 0u, cpu ? cpu->sp : 0u, cpu ? cpu->gp : 0u);
    fflush(stderr);
    abort();
}
