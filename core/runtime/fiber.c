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
unsigned openpete_vbl               = 0u;
unsigned openpete_frame             = 0u;
unsigned openpete_primitives_drawn  = 0u;
int      openpete_present_pending   = 0;
int      openpete_fast_boot_active  = 1;

void openpete_yield_to_host(void) {
    if (!openpete_host_fiber) return;  /* not in fiber mode (smoke test path) */
    /* Each yield is one vblank AND one rendered host frame. Bump both before
     * switching so callers that read the counters on resume see the new
     * values. (Fast-boot bypasses this path entirely — psyq_VSync bumps only
     * `openpete_vbl` and skips the co_switch until DrawOTag clears the gate.) */
    openpete_vbl++;
    openpete_frame++;
    co_switch((cothread_t)openpete_host_fiber);
}

void openpete_yield_to_host_no_tick(void) {
    if (!openpete_host_fiber) return;
    co_switch((cothread_t)openpete_host_fiber);
}
