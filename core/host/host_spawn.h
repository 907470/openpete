// core/host/host_spawn.h - silent child-process spawn for host-side
// build steps (the first-boot module build, mod compile-on-boot).
//
// The child is started from an argument vector, never through a shell, so
// a path holding a space, a quote, `%NAME%` or `$(...)` is one argument
// and nothing in it is interpreted. The Windows exe is a GUI-subsystem app
// (-mwindows) with no console of its own; the child runs with
// CREATE_NO_WINDOW so no console window appears, and grandchildren
// inherit the hidden console.
//
// The stoppable form starts the child as the leader of its own process
// group on POSIX (POSIX_SPAWN_SETPGROUP) and inside a job object on Windows,
// polls the registered stop predicate while it waits, and on a stop ends
// the group (SIGTERM, then SIGKILL after 500 ms) or terminates the job, so
// the compiler driver's own children (cc1, the linker) end with it.
#ifndef OPENPETE_HOST_SPAWN_H
#define OPENPETE_HOST_SPAWN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Run the program an argument vector names, with its arguments, and wait for it, its stdout and stderr going to a file or to a buffer.
 *
 * POSIX: posix_spawnp, so argv[0] is searched on PATH when it has no
 * slash. Windows: CreateProcessW with no shell; the command line is built
 * from argv with the quoting CommandLineToArgvW and the C runtime undo
 * (each argument in double quotes, backslashes before a quote doubled,
 * the quote escaped), converted through the active code page.
 * @param argv NULL-terminated argument vector; argv[0] is the program.
 * @param log_path When non-NULL, stdout and stderr are written to this
 *        file (created or truncated).
 * @param capture When non-NULL (and log_path is NULL), stdout and stderr
 *        are collected here, NUL-terminated; output past cap - 1 bytes is
 *        read and discarded, so the child never blocks on a full pipe.
 * @param cap Size of capture in bytes (at least 1 when capture is used).
 * @return The child's exit code (0 on success), or -1 when it could not
 *         be started or did not exit normally. */
int openpete_host_spawn_argv(const char* const* argv, const char* log_path,
                             char* capture, size_t cap);

/* openpete_host_spawn_argv_stoppable's result for a child killed by a stop. */
#define OPENPETE_HOST_SPAWN_STOPPED (-2)

/** @brief Register the predicate openpete_host_spawn_argv_stoppable polls; NULL clears it.
 *
 * @param stop Returns non-zero when the calling thread's work was asked to
 *        stop; called on the thread that spawns, every few milliseconds
 *        while a child runs. */
void openpete_host_spawn_set_stop(int (*stop)(void));

/** @brief Run a program as openpete_host_spawn_argv does, killing it and every process it started when the registered stop predicate reports a stop.
 *
 * The predicate is read before the start and every 2 ms while the child
 * runs. On a stop the child's process group (POSIX: SIGTERM, then SIGKILL
 * when the child has not exited 500 ms later) or job object (Windows) is
 * ended and the child reaped before the return.
 * @param argv NULL-terminated argument vector; argv[0] is the program.
 * @param log_path As openpete_host_spawn_argv.
 * @param capture As openpete_host_spawn_argv.
 * @param cap As openpete_host_spawn_argv.
 * @return The child's exit code, -1 when it could not be started or did not
 *         exit normally, or OPENPETE_HOST_SPAWN_STOPPED when a stop ended it
 *         or came before its start. */
int openpete_host_spawn_argv_stoppable(const char* const* argv, const char* log_path,
                                       char* capture, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HOST_SPAWN_H */
