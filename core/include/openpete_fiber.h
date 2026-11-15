// core/include/openpete_fiber.h - cooperative scheduler globals.
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

#include "openpete_park_site.h"

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

/** @brief Switch the calling thread to another fiber; the only caller of libco's co_switch.
 *
 * Counts a run-time check that the switching thread holds the `sim` role
 * (openpete_thread_roles.h): the host loop and the game fiber alternate
 * through this call and are one ordering domain only while both run on the
 * thread holding `sim`.
 * @param to The fiber to resume (openpete_host_fiber or openpete_game_fiber). */
void openpete_fiber_switch(void* to);

/** @brief The guest generation: a count that moves whenever guest memory may have changed since the last read of it.
 *
 * It advances on every switch into the game fiber (guest code runs) and on
 * every savestate restore, file or in-memory, quiet rollbacks included
 * (openpete_guest_generation_bump). While it holds, guest RAM holds the
 * bytes it held at the last read, so a host cache of values derived from
 * guest memory stays exact while its stamp equals this value. Read and
 * advanced on the thread holding the `sim` role.
 * @return The generation. */
unsigned openpete_guest_generation(void);

/** @brief Advance the guest generation: a caller that replaces guest memory outside the game fiber calls it. */
void openpete_guest_generation_bump(void);

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
 * key off this. Advanced only by psyq_VSync - one increment per logical
 * frame waited - plus absolute restores from the savestate path. */
extern SDL_AtomicInt openpete_vbl;

/* Device VBL counter: every VBLANK that elapsed, whether the CPU took it or
 * a critical section held it. Advanced once per VBLANK by the device half of
 * the VBL body (psyq_thunks.cpp), so it equals `openpete_vbl` until a
 * VBLANK is held and leads it by the held VBLANKs not yet taken afterwards.
 * Device time (the CD drive head, the XA stream anchor, the oracle's
 * per-record VBL delta and the device-side log labels) reads it. */
extern SDL_AtomicInt openpete_vbl_device;

/* 1 while the game fiber runs (recompiled code, the thunks and mod overrides
 * it calls), 0 while it is parked at a yield and the host fiber runs (the
 * host loop, a host pause, the devui, the pacer's sleep). Written by the game
 * fiber on each side of its switches to the host: set at its entry, cleared
 * before each yield and set again on resume. Read by the hang watchdog's
 * stall monitor thread. */
extern SDL_AtomicInt openpete_game_running;

/* Host-facing render-frame counter. Stays at 0 until the first DrawOTag
 * (i.e. until the game actually starts rendering), then increments on each
 * VSync(0) yield. Every host-side consumer - frame-limit, screenshot/RAM-dump
 * gates, hang detector, dump windows in dump tools, traces - uses this so
 * "frame N" means the same thing across runs regardless of disc-seek timing
 * during the splash sequence. */
extern unsigned openpete_frame;

/* The counter that indexes every number a run is told from outside
 * (--frame-limit, --screenshot-on-frame and the other one-shot frame
 * triggers, the presentation corpus pins). Per game: OPENPETE_FEED_KEY_FRAME
 * keys them on `openpete_frame`, OPENPETE_FEED_KEY_VBL on `openpete_vbl`. A
 * bk2's rows read the position its header names (openpete_consume and
 * openpete_consume_write for a recorded movie). Presentation stamps, logs
 * and the in-session rewind and runahead logs stay on `openpete_frame` in
 * every game. */
typedef enum openpete_feed_key {
    OPENPETE_FEED_KEY_FRAME = 0,
    OPENPETE_FEED_KEY_VBL   = 1,
} openpete_feed_key_t;

/** @brief Select the counter the run's frame triggers key on.
 *
 * Called once when the game descriptor registers, before the host loop runs.
 * @param key OPENPETE_FEED_KEY_FRAME or OPENPETE_FEED_KEY_VBL. */
void openpete_feed_key_select(openpete_feed_key_t key);

/** @brief Return the selected feed key.
 * @return OPENPETE_FEED_KEY_FRAME until a game selects another. */
openpete_feed_key_t openpete_feed_key(void);

/** @brief Return the current value of the selected feed key's counter.
 *
 * `openpete_frame`, or `openpete_vbl` read atomically. Both counters advance
 * only on the game fiber, so between yields the host loop reads a stable
 * value.
 * @return The counter's value. */
unsigned openpete_feed_key_now(void);

/* The bk2 row position (E, j). E, openpete_consume: entries of the game's
 * pad-ring consumer (the descriptor's consume_pc) since boot, counted at
 * function entry by psx_override_dispatch. j, openpete_consume_write: the
 * host's pad writes since that entry, one per yield of a VBL body (the host
 * writes the pad buffer from its stamp during the yield and the game's VSync
 * slots read it after) and one for the stamp the fast-boot VBLs repeat
 * before the first yield. A movie keyed `OpenpeteFeedKey consume` stamps row j
 * of group E before each yield, holding the group's last row past its count,
 * so its rows follow the game's ticks and every VBL inside one. Before the
 * first consumer entry (boot) and while none runs (loads) one group grows by
 * a row per VBL. Both are saved in
 * savestates and in the in-memory snapshot. */
extern unsigned openpete_consume;
extern unsigned openpete_consume_write;

/** @brief Set the guest address whose entries openpete_consume counts.
 * @param pc The consumer's entry address; 0 counts nothing. */
void openpete_consume_pc_set(uint32_t pc);

/** @brief Return the pad-ring consumer PC openpete_consume_pc_set installed.
 *
 * @return The PC; 0 without one. */
uint32_t openpete_consume_pc(void);

/* Observer of the (E, j) position, set by the host loop: `write` runs at
 * each pad write of a yield with the (E, j) it took, before j advances;
 * `close` runs at each consumer entry with the closing E and the pad writes
 * its group took, before E advances. Either may be NULL. */
typedef struct openpete_feed_observer {
    void (*write)(unsigned e, unsigned j);
    void (*close)(unsigned e, unsigned writes);
} openpete_feed_observer_t;

/** @brief Install the (E, j) observer; NULL removes it.
 * @param obs The observer; its storage must outlive the run. */
void openpete_feed_observer_set(const openpete_feed_observer_t* obs);

/** @brief Count an entry of the pad-ring consumer: report the closing group, advance E and reset j. */
void openpete_feed_consume_enter(void);

/** @brief Count the host's pad write at a VBL body's yield: report its (E, j), then advance j. */
void openpete_feed_pad_write(void);

/* Canonical-present counter: increments exactly once per CANONICAL game
 * draw (the host loop's `openpete_present_pending` block), i.e. once per
 * game draw-tick. Unlike `openpete_frame` - a VBL/yield counter whose
 * mapping to sim ticks drifts run-to-run with host timing (CD-load VSync
 * spins, audio backend) - the Nth canonical present is the SAME sim
 * moment in every run of a tick-deterministic sim, so tick-keyed triggers
 * (`--screenshot-on-tick`) sample identical game states across sessions.
 * (Frame-keyed captures fork at session-dependent points even while the
 * sim itself stays deterministic.) */
extern unsigned openpete_canon_tick;

extern unsigned openpete_primitives_drawn;  /* per-frame; cleared by the host loop after each simulated tick */
extern int      openpete_present_pending;   /* set by DrawOTag thunk; consumed by host loop */

/* 1 while the host is running SPECULATIVE guest ticks that will be rolled
 * back (runahead lookahead). Externally-visible host
 * side effects fired from guest thunks must be suppressed while set: the
 * guest-RAM half of the effect rolls back, the file/audio half wouldn't.
 * Checked by the memcard flush; audio's audible half is bracketed with
 * SPU mute by the driver. Plain int: only the host loop writes it, only
 * host-thread thunk code reads it. */
extern int openpete_speculative;

/* 1 while the host is REPLAYING committed guest ticks - the rewind driver
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
 * Plain int, read cross-thread - the same relaxed idiom as
 * openpete_speculative, and for the same reason: a one-present skew either way
 * on a purely presentational suppression is not worth an atomic. */
extern int openpete_replaying;

/* 1 for the duration of a runahead tick whose speculation input DIFFERS
 * from the previous tick's - the led pair crosses prediction branches, so
 * presentation-side consumers may REBASE their interpolation at the last
 * SHOWN state instead of the stale mispredicted prev (converts the
 * correction snap into a one-tick glide). Written by the runahead driver;
 * read by the sub-tick feeders. Always 0 with runahead off. */
extern int openpete_ra_input_edge;

/* Fast-boot gate. While set, psyq_VSync(0) bumps `openpete_vbl` without
 * yielding to the host fiber, so the game's Initialize() busy-waits run as
 * fast as the CPU allows (nothing is rendered during them anyway - the
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
 * still yields each frame, so `openpete_frame` advances normally - bk2 input
 * timing and frame-keyed captures are unaffected. Armed by
 * game_set_skip_to_level() whenever a skip target is set while
 * openpete_skip_ffwd_windowed says presents exist - the boot CLI flag and
 * runtime title-screen warps (dev levelwarp, custom-level menu) alike; the
 * game side (game_on_frame_pre) clears it the moment the level becomes
 * playable (g_Gamestate == GS_Playing - past the black "THE ADVENTURE
 * BEGINS" flyby), or on a frame-cap fail-safe relative to arming so a
 * mis-induced skip can never leave a permanently black window. */
extern int      openpete_skip_present_until_level;

/* Set once by run.c before the first game_set_skip_to_level() call: 1 in a
 * windowed run (presents exist, skips may fast-forward them), 0 headless
 * (already unlocked; skipping presents there would break frame-keyed
 * screenshot capture). */
extern int      openpete_skip_ffwd_windowed;

/* 1 while the game fiber is parked at the final yield of a psyq_VSync wait
 * (the last VBL the wait delivers), 0 at every other yield: an intermediate
 * yield of VSync(n > 1), a polling-loop or spin-tick delivery, a schedule
 * charge, a flushed VBLANK, the no-tick stub-miss yield. The host takes and
 * applies snapshots only while it is set; the guest call stack below any
 * other yield is not the one a snapshot's state continues from. Written by
 * the game fiber around the yield, read by the host loop between yields. */
extern int openpete_yield_final;

/* The site of the final yield the fiber is parked at; written by the game
 * fiber before each final yield, meaningful while openpete_yield_final is
 * set. */
extern openpete_park_site_t openpete_yield_site;

/* The header address of the emitted polling loop whose back-edge quantum
 * delivered the VBL the fiber is yielding under, 0 at every other yield.
 * Written by the game fiber around a loop delivery, read by the host loop
 * between yields (the deferral census names the site). */
extern uint32_t openpete_yield_loop_header;

/** @brief Report whether the game fiber is parked at a final yield whose site equals @p site.
 *
 * A site with ra 0 (none recorded) matches any final yield.
 * @param site The site a snapshot was taken at.
 * @return 1 when parked at a final yield at @p site, else 0. */
int openpete_park_site_matches(const openpete_park_site_t* site);

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

/** @brief Report whether the calling code runs on the game fiber.
 *
 * Recompiled guest code, the thunks it calls and mod overrides run there;
 * the host loop's per-tick hooks run on the host fiber.
 * @return 1 on the game fiber, 0 on the host fiber or before the game fiber exists. */
int openpete_fiber_in_game(void);

/* openpete_no_lod is declared in core/include/no_lod.h. */

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FIBER_H */
