// core/runtime/ticker.c — host-loop pacing clock lifecycle.
//
// The host loop is self-paced: it advances phase-locked absolute
// integer-ns deadlines and sleeps to them via op_sleep_until, whose
// hybrid waiter (high-resolution OS timer to deadline - slack, then a
// QPC spin) lands within microseconds of the target. See the pacer at
// the bottom of the host loop in run.c.
//
// A dedicated VBL-rate ticker thread used to sleep to those deadlines
// and broadcast a condvar for the host loop to wait on. The host loop
// never adopted it — it paces inline instead — so the thread spun
// (op_sleep_until's spin tail, ~15% of a core) and signalled a condvar
// with no waiter, while its own "slip" warnings reported starvation of
// that thread rather than anything about sim pacing. It is gone; the
// pacer's real slip-clamp in run.c is the authoritative signal.
//
// Determinism contract: pacing changes the *timing* of host-loop
// iterations, never the count or order of game-fiber co_switches.
// openpete_vbl is advanced synchronously by psyq_VSync(0) — exactly one
// increment per yield — because BK2 input replay relies on
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
