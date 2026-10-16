/* openpete_crash.h - per-thread setup for the crash handler
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

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CRASH_H */
