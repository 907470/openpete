/* openpete_crash.h — per-thread setup for the crash handler
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

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CRASH_H */
