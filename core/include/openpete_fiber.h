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

#include <SDL3/SDL_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void*    openpete_host_fiber;     /* set in openpete_run before fiber spawn */
extern void*    openpete_game_fiber;

/* PSX-facing vblank counter. Monotonic from boot, including fast-boot
 * iterations where the host fiber is never resumed. This is the value
 * `psyq_VSync` returns to the game (libetc's "vbl counter" contract).
 * The game's busy-waits like `int s = VSync(-1); while (VSync(-1) - s < N) VSync(0);`
 * key off this. Atomic because the 60 Hz ticker thread (see
 * openpete_ticker.h) advances it free-running, racing PSX-fiber readers. */
extern SDL_AtomicInt openpete_vbl;

/* Host-facing render-frame counter. Stays at 0 until the first DrawOTag
 * (i.e. until the game actually starts rendering), then increments on each
 * VSync(0) yield. Every host-side consumer — frame-limit, screenshot/RAM-dump
 * gates, hang detector, dump windows in dump tools, traces — uses this so
 * "frame N" means the same thing across runs regardless of disc-seek timing
 * during the splash sequence. */
extern unsigned openpete_frame;

/* Canonical-present counter: increments exactly once per CANONICAL game
 * draw (the host loop's `openpete_present_pending` block), i.e. once per
 * game draw-tick. Unlike `openpete_frame` — a VBL/yield counter whose
 * mapping to sim ticks drifts run-to-run with host timing (CD-load VSync
 * spins, audio backend) — the Nth canonical present is the SAME sim
 * moment in every run of a tick-deterministic sim, so tick-keyed triggers
 * (`--screenshot-on-tick`) sample identical game states across sessions.
 * (The W1.2 gate flake: frame-keyed captures forked at session-dependent
 * points while the sim itself stayed deterministic.) */
extern unsigned openpete_canon_tick;

extern unsigned openpete_primitives_drawn;  /* per-frame; cleared on each yield */
extern int      openpete_present_pending;   /* set by DrawOTag thunk; consumed by host loop */

/* 1 while the host is running SPECULATIVE guest ticks that will be rolled
 * back (runahead lookahead — savestate plan P5). Externally-visible host
 * side effects fired from guest thunks must be suppressed while set: the
 * guest-RAM half of the effect rolls back, the file/audio half wouldn't.
 * Checked by the memcard flush; audio's audible half is bracketed with
 * SPU mute by the driver. Plain int: only the host loop writes it, only
 * host-thread thunk code reads it. */
extern int openpete_speculative;

/* 1 for the duration of a runahead tick whose speculation input DIFFERS
 * from the previous tick's — the led pair crosses prediction branches, so
 * presentation-side consumers may REBASE their interpolation at the last
 * SHOWN state instead of the stale mispredicted prev (converts the
 * correction snap into a one-tick glide). Written by the runahead driver;
 * read by the sub-tick feeders. Always 0 with runahead off. */
extern int openpete_ra_input_edge;

/* Fast-boot gate. While set, psyq_VSync(0) bumps `openpete_vbl` without
 * yielding to the host fiber, so the game's Initialize() busy-waits run as
 * fast as the CPU allows (nothing is rendered during them anyway — the
 * Sony+Universal splashes are intentional VSync(-1) spins). Cleared on the
 * first call to psyq_DrawOTag, which is the natural signal that the game
 * has progressed from boot init to issuing real draw commands. */
extern int      openpete_fast_boot_active;

/* --skip-to-level "fast-boot to the level": while set, the host loop skips
 * presents AND the frame pacer (like fast-boot, but AFTER the first DrawOTag),
 * so the logos / title / loading / level-load all fast-forward with a black
 * window instead of playing out at 60 Hz for ~25 s. Unlike fast-boot, the game
 * still yields each frame, so `openpete_frame` advances normally — bk2 input
 * timing and frame-keyed captures are unaffected. Armed at boot iff
 * --skip-to-level is set and NOT headless (headless is already unlocked and has
 * no window); the game side (game_on_frame_pre) clears it the moment the level
 * becomes playable (g_Gamestate == GS_Playing — past the black "THE ADVENTURE
 * BEGINS" flyby), or on a generous frame-cap fail-safe so a mis-induced skip can
 * never leave a permanently black window. */
extern int      openpete_skip_present_until_level;

/* Game fiber → host fiber. Bumps `openpete_frame` on the way through;
 * `openpete_vbl` is *not* touched here — the ticker (openpete_ticker.h)
 * is the sole writer once running. Used by the VSync(0) thunk outside
 * fast-boot. */
void openpete_yield_to_host(void);

/* Game fiber → host fiber WITHOUT advancing the frame counter. Used by stub
 * misses inside untranslated overlays (Phase 1d band-aid) so the host can
 * still drive frame_limit/max-runtime exits. Phase 2 retires this. */
void openpete_yield_to_host_no_tick(void);

/* Horizontal-FOV override expressed as a target GTE H register value.
 * 0 = no override. Set once at boot from --fov (PsyCross renderer
 * mode); consumed by widescreen_eff_h in gte_rtps_internal. See
 * notes/plans/openpete-widescreen-fov.md "FOV slider". */
extern int openpete_fov_h_override;

/* openpete_no_lod has moved to core/features/no_lod.h (declared there
 * and in third_party/psxrecomp/runner/include/psx_runtime.h for the
 * recompiler-generated C). */

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FIBER_H */
