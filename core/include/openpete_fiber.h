// core/include/openpete_fiber.h — cooperative scheduler globals for Phase 1d.
//
// The recompiled game runs on `openpete_game_fiber`. PSYQ thunks that need to
// "wait for vblank" (VSync(0), VSync busy waits) call `openpete_yield_to_host`
// to switch back to the host fiber, which advances the frame counter, presents
// the current PsyCross scene, and resumes the game on the next iteration.
//
// `cothread_t` from libco.h is `void*`; we use that here so this header
// stays libco-free for downstream TUs that don't need the libco symbols.

#ifndef OPENPETE_FIBER_H
#define OPENPETE_FIBER_H

#ifdef __cplusplus
extern "C" {
#endif

extern void*    openpete_host_fiber;     /* set in openpete_run before fiber spawn */
extern void*    openpete_game_fiber;
extern unsigned openpete_frame;          /* incremented on each VSync(0) yield */
extern unsigned openpete_primitives_drawn;  /* per-frame; cleared on each yield */

/* Game fiber → host fiber. Bumps `openpete_frame` on the way through; this is
 * the "VBlank yield" used by the VSync(0) thunk. */
void openpete_yield_to_host(void);

/* Game fiber → host fiber WITHOUT advancing the frame counter. Used by stub
 * misses inside untranslated overlays (Phase 1d band-aid) so the host can
 * still drive frame_limit/max-runtime exits. Phase 2 retires this. */
void openpete_yield_to_host_no_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FIBER_H */
