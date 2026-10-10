// games/common/psyq/psyq_stub_runtime.c - backing implementation for the
// generated PSYQ abort-on-call stubs (tools/filter_psyq_stubs.py union). It
// lives outside the generated file because that file avoids
// <stdio.h>/<stdlib.h>: PSYQ exports names like printf, exit and sprintf,
// which would collide with libc. Compiled once with the stubs; the reached
// symbol's address comes from the running game's stub table.

#include "psx_runtime_internal.h"
#include "openpete_game_runtime.h"
#include "psyq/psyq_game_hooks.h"

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

/** @brief Return the running game's guest address of a stub, or 0 when its table does not name it.
 *
 * A linear scan of the game's stub table; reached only on a stub hit.
 * @param name The stub's link name. */
static uint32_t stub_addr(const char* name) {
    const psyq_stub_table_t* t = psyq_game_hooks()->stubs;
    if (!t) return 0u;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].name, name) == 0) return t->entries[i].addr;
    }
    return 0u;
}

/** @brief Report a call to a PSYQ symbol that has no thunk, then abort.
 *
 * Every generated stub of an unimplemented PSYQ function calls it. The
 * symbol's guest address is the running game's, from its stub table (0 when
 * the table does not name it). Unless
 * OPENPETE_PSYQ_STUB_CONTINUE is set and not "0" (read once), it prints the
 * symbol, its address and ra, sp and gp, and aborts. With the variable set
 * it logs each distinct symbol once (up to 256) as `psyq-missing:` and
 * returns, leaving the guest to run on with the call having done nothing.
 * @param name The symbol's name; the pointer is retained, so it must outlive
 *        the process.
 * @param cpu Guest CPU state; may be NULL. */
void psyq_stub_hit(const char* name, CPUState* cpu) {
    const uint32_t addr = stub_addr(name);
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
