// core/psx/fiber.c — cooperative scheduler globals.
//
// See core/include/openpete_fiber.h. The actual fiber spawn/loop lives in
// run.c; this TU only owns the shared state so PSYQ thunks (in games/<g>/)
// can call `openpete_yield_to_host()` without dragging in libco directly.

#include "openpete_fiber.h"
#include "openpete_func_trace.h"

#include <libco.h>
#include <stddef.h>

void*        openpete_host_fiber       = NULL;
void*        openpete_game_fiber       = NULL;
SDL_AtomicInt openpete_vbl             = {0};
int openpete_speculative               = 0;
int openpete_replaying                 = 0;
int openpete_ra_input_edge             = 0;
unsigned     openpete_frame            = 0u;
unsigned     openpete_canon_tick       = 0u;
unsigned     openpete_primitives_drawn = 0u;
int          openpete_present_pending  = 0;
int          openpete_fast_boot_active = 1;
int          openpete_cd_load_delay    = 0;
int          openpete_skip_present_until_level = 0;
int          openpete_skip_ffwd_windowed       = 0;
/* openpete_no_lod lives in core/renderer/no_lod.c. */

void openpete_yield_to_host(void) {
    if (!openpete_host_fiber) return;  /* not in fiber mode (smoke test path) */
    /* `openpete_vbl` belongs to psyq_VSync, which advances it once per
     * logical frame waited. Yields only bump the host-side render counter
     * so frame-limit / screenshot / bk2 alignment stay yield-driven. */
    openpete_frame++;
    op_functrace_pause();     /* exclude host render+sleep from func-trace */
    co_switch((cothread_t)openpete_host_fiber);
    op_functrace_resume();
}

void openpete_yield_to_host_no_tick(void) {
    if (!openpete_host_fiber) return;
    op_functrace_pause();
    co_switch((cothread_t)openpete_host_fiber);
    op_functrace_resume();
}
