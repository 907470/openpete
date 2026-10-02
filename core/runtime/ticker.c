// core/runtime/ticker.c - host-loop pacing clock lifecycle.
//
// The host loop is self-paced: it advances phase-locked absolute
// integer-ns deadlines and sleeps to them via op_sleep_until, whose
// hybrid waiter (high-resolution OS timer to deadline - slack, then a
// QPC spin) lands within microseconds of the target. See the pacer at
// the bottom of the host loop in run.c.
//
// No pacing thread exists: the host loop sleeps inline, and the pacer's
// slip clamp in run.c is the signal for pacing slips. What is left here is
// the clock's init/teardown pair and the fast-forward flag.
//
// Determinism contract: pacing changes the *timing* of host-loop
// iterations, never the count or order of game-fiber co_switches.
// openpete_vbl is advanced synchronously by psyq_VSync(0) - exactly one
// increment per yield - because BK2 input replay relies on
// yields-per-logical-frame being deterministic regardless of pacing.

#include "openpete_ticker.h"
#include "openpete_time.h"

int openpete_unlock_fps = 0;

void openpete_ticker_start(void) {
    /* Idempotent; pays the Win32 timer-resolution boost and the QPC
     * frequency probe up front rather than inside the first deadline. */
    op_time_init();
}

void openpete_ticker_stop(void) {
    op_time_shutdown();
}
