// core/runtime/fiber.c — cooperative scheduler globals (Phase 1d).
//
// See core/include/openpete_fiber.h. The actual fiber spawn/loop lives in
// run.c; this TU only owns the shared state so PSYQ thunks (in games/<g>/)
// can call `openpete_yield_to_host()` without dragging in libco directly.

#include "openpete_fiber.h"

#include <libco.h>
#include <stddef.h>

void*    openpete_host_fiber       = NULL;
void*    openpete_game_fiber       = NULL;
unsigned openpete_frame             = 0u;
unsigned openpete_primitives_drawn  = 0u;
int      openpete_present_pending   = 0;

void openpete_yield_to_host(void) {
    if (!openpete_host_fiber) return;  /* not in fiber mode (smoke test path) */
    /* Each yield is one frame from the host's perspective: scene swap +
     * VBlank counter tick. Bump before switching so callers querying the
     * frame counter via VSync(-1) on resume see the new value. */
    openpete_frame++;
    co_switch((cothread_t)openpete_host_fiber);
}

void openpete_yield_to_host_no_tick(void) {
    if (!openpete_host_fiber) return;
    co_switch((cothread_t)openpete_host_fiber);
}
