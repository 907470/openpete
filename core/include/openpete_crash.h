/* openpete_crash.h - per-thread setup for the crash handler, the immediate
 * process exit, and the flush a non-signal fatal exit runs before it
 * (core/diag/crash.c; the installer is declared in openpete_psx.h). */
#ifndef OPENPETE_CRASH_H
#define OPENPETE_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Give the calling thread the stack the crash handler runs on when that thread's stack is exhausted.
 *
 * POSIX: a 64 KiB alternate signal stack (sigaltstack), once per thread;
 * the game fiber runs on the thread that switches to it, so that thread's
 * alternate stack also serves a fiber overflow. Windows: a 64 KiB
 * SetThreadStackGuarantee. Every engine thread calls it at its start;
 * openpete_psx_install_crash_handler calls it for the installing thread. */
void openpete_crash_thread_attach(void);

/** @brief End the process at once with the given exit code, running no atexit handler and no DLL or static destructor.
 *
 * Windows: TerminateProcess, since the CRT's _Exit leaves through
 * ExitProcess, which ends the other threads where they stand and then runs
 * every DLL's static destructors (a C++ condition variable whose waiter
 * was ended mid-wait asserts there). POSIX: _Exit.
 * @param code Process exit code. */
void openpete_crash_terminate_now(int code);

/* Longest breadcrumb kept, terminator included; longer text is cut. */
#define OPENPETE_CRASH_CRUMB_MAX 768

/** @brief Record the calling thread's breadcrumb: one line naming the call it is about to make, which the crash report prints if the process faults before the matching clear.
 *
 * The text is copied into one of a fixed set of slots (claimed by the
 * thread on its first set, released by openpete_crash_crumb_clear), so the
 * crash handler reads plain bytes with no allocation or lock. With every
 * slot taken by other threads the call records nothing. A later set
 * replaces the text.
 * @param text The line, without a newline. */
void openpete_crash_crumb_set(const char* text);

/** @brief Clear the calling thread's breadcrumb and release its slot; no-op when it has none. */
void openpete_crash_crumb_clear(void);

/* The exit flush slots, run in this order: the movie being recorded
 * first, so a bundle that fails or runs out of time cannot cost it. */
typedef enum {
    OP_EXIT_FLUSH_RECORD = 0,     /* --bk2-record's movie */
    OP_EXIT_FLUSH_BUGREPORT = 1,  /* the bug-report bundle of a fatal exit */
    OP_EXIT_FLUSH_SLOTS
} op_exit_flush_slot_t;

/** @brief Register the function a non-signal fatal exit runs in a slot before it terminates the process; NULL clears the slot.
 *
 * A later registration in a slot replaces an earlier one. The functions
 * run from openpete_crash_run_exit_flush in slot order, on the thread that
 * is exiting: the game fiber's thread for a hang-watchdog trip or the
 * CdSync spin guard, or the stall monitor thread for its fallback, while
 * other threads keep running. They may use stdio and the heap; the crash
 * handler never calls them, since a signal or SEH filter context allows
 * neither.
 * @param slot The slot.
 * @param fn The flush, or NULL. */
void openpete_crash_set_exit_flush(op_exit_flush_slot_t slot, void (*fn)(void));

/** @brief Run the registered exit flushes once per process, in slot order; a second call does nothing.
 *
 * Called on a fatal exit path whose context allows stdio and the heap,
 * before openpete_crash_terminate_now or _Exit.
 * @param reason The exit's name ("hang", "cdsync-spin"), which the flushes
 *     read through openpete_crash_exit_reason; a static string. */
void openpete_crash_run_exit_flush(const char* reason);

/** @brief Return the reason the running fatal exit passed to openpete_crash_run_exit_flush.
 * @return The reason, or NULL outside a fatal exit. */
const char* openpete_crash_exit_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CRASH_H */
