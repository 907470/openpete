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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void*    openpete_host_fiber;     /* set in openpete_run before fiber spawn */
extern void*    openpete_game_fiber;

/** @brief Create the game fiber with a stack whose lowest page is a PROT_NONE guard.
 *
 * On POSIX the stack is mmap'd and handed to libco's co_derive, so an
 * overflow faults on the guard page (SIGSEGV, reported by the crash
 * handler as a fiber stack overflow) instead of writing into whatever the
 * allocator placed below a malloc'd stack. On Windows it is co_create:
 * Windows fiber stacks carry the system's own guard page. Emscripten's libco
 * backend has no co_derive and also takes co_create.
 * @param size Usable stack size in bytes (rounded up to whole pages).
 * @param entry The fiber's body.
 * @return The fiber handle, or NULL when the mapping fails. */
void* openpete_fiber_create_guarded(unsigned size, void (*entry)(void));

/** @brief Delete a fiber made by openpete_fiber_create_guarded and release its stack.
 *
 * @param fiber The handle; NULL is a no-op. */
void openpete_fiber_delete_guarded(void* fiber);

/** @brief Report whether addr lies in the game fiber's guard page.
 *
 * Reads two words only, so the crash handler may call it.
 * @param addr A fault address.
 * @return 1 inside the guard page, else 0 (always 0 without a guard page). */
int openpete_fiber_guard_contains(uintptr_t addr);

/* PSX-facing vblank counter. Monotonic from boot, including fast-boot
 * iterations where the host fiber is never resumed. This is the value
 * `psyq_VSync` returns to the game (libetc's "vbl counter" contract).
 * The game's busy-waits like `int s = VSync(-1); while (VSync(-1) - s < N) VSync(0);`
 * key off this. Advanced only by psyq_VSync — one increment per logical
 * frame waited — plus absolute restores from the savestate path. */
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

extern unsigned openpete_primitives_drawn;  /* per-frame; cleared by the host loop after each simulated tick */
extern int      openpete_present_pending;   /* set by DrawOTag thunk; consumed by host loop */

/* 1 while the host is running SPECULATIVE guest ticks that will be rolled
 * back (runahead lookahead — savestate plan P5). Externally-visible host
 * side effects fired from guest thunks must be suppressed while set: the
 * guest-RAM half of the effect rolls back, the file/audio half wouldn't.
 * Checked by the memcard flush; audio's audible half is bracketed with
 * SPU mute by the driver. Plain int: only the host loop writes it, only
 * host-thread thunk code reads it. */
extern int openpete_speculative;

/* 1 while the host is REPLAYING committed guest ticks — the rewind driver
 * re-executing the input log from a keyframe toward its target, and any
 * state-load fast-forward that re-runs game_on_frame_pre. Distinct from
 * openpete_speculative, deliberately: speculative ticks are ROLLED BACK,
 * replayed ticks are the committed timeline being re-walked, and the dozen
 * consumers of that flag chose their behaviour for the rolled-back case.
 *
 * Presentation-side announcements read it so a held rewind does not fire one
 * notification per replayed tick, and so per-tick animation phases do not
 * sprint through a replay. It does NOT cover a backward jump into ordinary
 * live play (a savestate load, --state-stress-rewind): those frames resume the
 * normal loop and are indistinguishable from first-time ones.
 *
 * Plain int, read cross-thread — the same relaxed idiom as
 * openpete_speculative, and for the same reason: a one-present skew either way
 * on a purely presentational suppression is not worth an atomic. */
extern int openpete_replaying;

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

/* VBLs of guest time every level load waits, resolved from --load-delay or
 * the game's default before the guest starts; the thunk layer defers each
 * load read's completion against it. Constant for the run: a recorded bk2
 * carries it and replay under another value is refused. */
extern int      openpete_cd_load_delay;

/* --skip-to-level "fast-boot to the level": while set, the host loop skips
 * presents AND the frame pacer (like fast-boot, but AFTER the first DrawOTag),
 * so the logos / title / loading / level-load all fast-forward with a black
 * window instead of playing out at 60 Hz for ~25 s. Unlike fast-boot, the game
 * still yields each frame, so `openpete_frame` advances normally — bk2 input
 * timing and frame-keyed captures are unaffected. Armed by
 * game_set_skip_to_level() whenever a skip target is set while
 * openpete_skip_ffwd_windowed says presents exist — the boot CLI flag and
 * runtime title-screen warps (dev levelwarp, custom-level menu) alike; the
 * game side (game_on_frame_pre) clears it the moment the level becomes
 * playable (g_Gamestate == GS_Playing — past the black "THE ADVENTURE
 * BEGINS" flyby), or on a frame-cap fail-safe relative to arming so a
 * mis-induced skip can never leave a permanently black window. */
extern int      openpete_skip_present_until_level;

/* Set once by run.c before the first game_set_skip_to_level() call: 1 in a
 * windowed run (presents exist, skips may fast-forward them), 0 headless
 * (already unlocked; skipping presents there would break frame-keyed
 * screenshot capture). */
extern int      openpete_skip_ffwd_windowed;

/** @brief Switch from the game fiber to the host fiber, advancing openpete_frame.
 *
 * `openpete_vbl` is not touched: psyq_VSync owns that counter, so a yield
 * and a VBL stay separately counted. Function tracing is paused across the
 * switch so host time is excluded. No effect when no host fiber exists.
 * Called by the VSync(0) thunk outside fast-boot. */
void openpete_yield_to_host(void);

/** @brief Switch from the game fiber to the host fiber without advancing openpete_frame.
 *
 * Called after a dispatch to an address the recompiled image does not
 * cover is stubbed, so a main loop that never reaches VSync(0) still lets
 * the host drive the frame-limit and max-runtime exits without inflating
 * the frame counter. Function tracing is paused across the switch. No
 * effect when no host fiber exists. */
void openpete_yield_to_host_no_tick(void);

/* openpete_no_lod is declared in core/include/no_lod.h (there
 * and in third_party/psxrecomp/runner/include/psx_runtime.h for the
 * recompiler-generated C). */

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FIBER_H */
