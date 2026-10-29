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

/** @brief Register the function a non-signal fatal exit runs before it terminates the process; NULL clears it.
 *
 * One slot; a later registration replaces an earlier one. The function
 * runs from openpete_crash_run_exit_flush, on the thread that is exiting:
 * the game fiber's thread for a hang-watchdog trip, or the stall monitor
 * thread for its fallback, while other threads keep running. It may use
 * stdio and the heap; the crash handler never calls it, since a signal or
 * SEH filter context allows neither.
 * @param fn The flush, or NULL. */
void openpete_crash_set_exit_flush(void (*fn)(void));

/** @brief Run the registered exit flush once per process; a second call, or a call with none registered, does nothing.
 *
 * Called on a fatal exit path whose context allows stdio and the heap,
 * before openpete_crash_terminate_now or _Exit. */
void openpete_crash_run_exit_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CRASH_H */
