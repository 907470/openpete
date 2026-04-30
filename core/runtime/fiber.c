// core/runtime/fiber.c — cooperative scheduler globals (Phase 1d).
//
// See core/include/openpete_fiber.h. The actual fiber spawn/loop lives in
// run.c; this TU only owns the shared state so PSYQ thunks (in games/<g>/)
// can call `openpete_yield_to_host()` without dragging in libco directly.

#include "openpete_fiber.h"

#include <libco.h>
#include <stddef.h>

void*        openpete_host_fiber       = NULL;
void*        openpete_game_fiber       = NULL;
SDL_atomic_t openpete_vbl              = {0};
unsigned     openpete_frame            = 0u;
unsigned     openpete_primitives_drawn = 0u;
int          openpete_present_pending  = 0;
int          openpete_fast_boot_active = 1;
int          openpete_fov_h_override    = 0;

void openpete_yield_to_host(void) {
    if (!openpete_host_fiber) return;  /* not in fiber mode (smoke test path) */
    /* `openpete_vbl` is the ticker's job now (free-running 60 Hz, see
     * openpete_ticker.h). Yields only bump the host-side render counter so
     * frame-limit / screenshot / bk2 alignment stay yield-driven. */
    openpete_frame++;
    co_switch((cothread_t)openpete_host_fiber);
}

void openpete_yield_to_host_no_tick(void) {
    if (!openpete_host_fiber) return;
    co_switch((cothread_t)openpete_host_fiber);
}
