/* The present thread: composes and presents the scenes the guest-side
 * producer publishes into the scene ring, on their own target instants.
 * See core/runtime/present_thread.c. */
#ifndef OPENPETE_PRESENT_THREAD_H
#define OPENPETE_PRESENT_THREAD_H

#include "openpete_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Report whether eager production and the present thread are enabled.
 *
 * On by default; OPENPETE_EAGER set to a value starting with '0' disables
 * it, which returns presents to the sim thread and removes the scene ring,
 * the lag and the present thread together. Read once per process.
 * @return 1 when enabled, else 0. */
int  openpete_present_thread_enabled(void);
/** @brief Return 1 when OPENPETE_EAGER_LATTICE=1 makes the present thread target and compose on the display's fitted vblank lattice.
 *
 * The lattice implies present timing. Off by default: the thread then
 * composes each scene at its own target and observes flips only under
 * OPENPETE_PACE_FLIP_LOG. The variable is read once per run.
 * @return 1 when the lattice is enabled, else 0. */
int  openpete_present_thread_lattice_enabled(void);
/** @brief Create the present thread, inactive, unless eager production is disabled.
 *
 * Idempotent while the thread exists. The thread issues nothing until
 * openpete_present_thread_set_active(1).
 * @return 1 when the thread exists, 0 when eager production is disabled or
 *         the mutex, condition variables or thread could not be created. */
int  openpete_present_thread_start(void);
/** @brief Take back the display, stop the present thread and destroy its synchronisation objects.
 *
 * Calls openpete_present_thread_set_active(0), then signals the thread to
 * quit and joins it. A no-op before the thread has started. The quit request
 * is never cleared, so a thread created by a later start exits at once. */
void openpete_present_thread_stop(void);

/** @brief Hand the display to the present thread (1) or take it back (0).
 *
 * Handing over (a 0 -> 1 transition) starts a new activation and wakes the
 * thread, which resets its lattice and flip state and claims the display
 * before its next device call, also when a take-back and handover both
 * completed while it waited and it never observed the inactive state.
 * Taking it back flushes the scene ring, waits out a compose or
 * flip wait in flight, discards the staged scene, claims the display for
 * the calling thread, and releases the stage's acquired swapchain image by
 * presenting it once more (openpete_native_present_stage_release); on
 * return the present thread issues nothing further. Idempotent; a no-op
 * before the thread has started.
 * @param on 1 to hand the display to the thread, 0 to take it back. */
void openpete_present_thread_set_active(int on);
/** @brief Take the display back from the present thread for a blocking device read on the calling thread, when the thread holds it.
 *
 * Pairs with openpete_present_thread_return; the two calls are the
 * handoff openpete_present_thread_set_active makes, and nothing else.
 * @return 1 when the present thread held the display and was drained, else 0. */
int  openpete_present_thread_borrow(void);
/** @brief Hand the display back to the present thread after a borrow that took it.
 * @param was The value openpete_present_thread_borrow returned; 0 does nothing. */
void openpete_present_thread_return(int was);
/** @brief Report whether the present thread currently owns the display.
 *
 * Reads the flag without the thread's mutex.
 * @return 1 between set_active(1) and the start of set_active(0), else 0. */
int  openpete_present_thread_active(void);

/** @brief Wake the present thread after a scene was published so it re-reads the earliest target.
 *
 * Signals the thread's condition variable under its mutex; a no-op before
 * the thread has started. */
void openpete_present_thread_kick(void);

/* Per-present callback on the present thread (frame-time log, profiler). */
typedef void (*openpete_present_thread_cb)(void *ctx, int kind, double alpha, int paired);
/** @brief Register the callback the present thread calls after each scene it presents.
 *
 * The callback runs on the present thread once per presented scene with
 * that scene's pace kind, alpha and pairing flag. The pointers are stored
 * without synchronisation, so the call belongs before the thread is
 * activated.
 * @param cb The callback, or NULL for none.
 * @param ctx Passed to cb unchanged. */
void openpete_present_thread_set_callback(openpete_present_thread_cb cb, void *ctx);

/** @brief Read the display's vblank lattice v(n) = base + n * period fitted from observed flips.
 *
 * The fit locks once the window holds 48 flips and the last 24 fell within
 * an eighth of a slot of it; that needs a vsynced swapchain, present timing
 * and OPENPETE_EAGER_LATTICE. The eager planner puts its targets on the
 * lattice so each target is a vblank. The outputs are written only when
 * locked.
 * @param base_ns Receives the time of lattice instant 0 on the op_now()
 *        clock; may be NULL.
 * @param period_ns Receives the fitted period; may be NULL.
 * @return 1 when the lattice is locked, 0 when not or before the thread
 *         has started. */
int      openpete_present_thread_lattice(int64_t *base_ns, double *period_ns);

/** @brief Log the present thread's census and the scene ring's report.
 *
 * Reports scenes presented, refused and late, dropped and discarded staged
 * frames, render and present costs, drains, and the lattice state when flips
 * were observed. Silent about the thread when nothing was presented or
 * refused. Reads the counters without the mutex, so it belongs after
 * openpete_present_thread_stop. */
void     openpete_present_thread_report(void);
/** @brief Return the number of scenes the present thread has presented.
 *
 * @return The count, a relaxed atomic load without the thread's mutex, safe on any thread. */
unsigned openpete_present_thread_presents(void);
/** @brief Return what one present costs the present thread, render plus present step, as a moving average.
 *
 * With vsync off this is the display's throughput ceiling: the planner
 * spaces targets no denser than it, since closer ones would be built only to
 * be superseded. Read without the thread's mutex; each average is a relaxed atomic.
 * @return The average in nanoseconds, 0 before the first present. */
int64_t  openpete_present_thread_cost_ns(void);
/** @brief Return the present thread's busy time per present, as a moving average.
 *
 * Sums the scene renders, the swapchain acquires and the present steps made
 * since the previous present, scenes rendered but dropped or refused
 * included, and folds the sum at each present into an 8-sample exponential
 * average (the re-warm window applies). The target-instant and flip waits
 * are left out; the acquire's wait for a free image is in. Under immediate
 * present a GPU-bound presenter waits in that acquire, so the average is the
 * rate at which it can consume scenes; under FIFO it carries the display's
 * back-pressure and sits near one refresh. Read without the thread's mutex;
 * the average is a relaxed atomic.
 * @return The average in nanoseconds, 0 before the first present. */
int64_t  openpete_present_thread_service_ns(void);
/** @brief Read the present thread's running scene-pick counts: every pick, and the picks that found no scene due.
 *
 * A pick is the choice of the scene the next present shows
 * (openpete_native_scene_pick). It is starved when no pending scene's target
 * lies at or before the pick instant: the presenter was ready before the
 * producer's next scene was due, so it was not behind. A presenter with a
 * backlog finds one or more scenes due at every pick. Read without the
 * thread's mutex; each count is a relaxed atomic, modulo 2^32.
 * @param picks Receives the number of picks.
 * @param starved Receives the number of starved picks. */
void     openpete_present_thread_picks(unsigned *picks, unsigned *starved);
/** @brief Read the two moving averages openpete_present_thread_cost_ns sums: the scene render and the present step.
 *
 * The render average is the render into a stage image, per scene; the
 * present average is the acquire, blit and queue present, per frame. Each is
 * an 8-sample exponential average, 0 before its first sample. Read without
 * the thread's mutex; the caller only reads them.
 * @param render_ns Receives the render average in nanoseconds.
 * @param present_ns Receives the present-step average in nanoseconds. */
void     openpete_present_thread_emas(int64_t *render_ns, int64_t *present_ns);
/** @brief Open a re-warm window: for the next renders, a render or present cost below its average replaces the average.
 *
 * Called when the world goes live, so the averages a slow first scene of a
 * level seeded fall to the steady cost at the first fast render instead of
 * losing an eighth per render. A cost above the average enters as usual
 * (op_eager_cost_ema). 0 closes the window.
 * @param renders The window's length in renders. */
void     openpete_present_thread_rewarm(int renders);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_PRESENT_THREAD_H */
