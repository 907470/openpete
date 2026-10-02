// core/host/host_spawn.h - silent child-process spawn for host-side
// build steps (mod compile-on-boot).
//
// The child is started from an argument vector, never through a shell, so
// a path holding a space, a quote, `%NAME%` or `$(...)` is one argument
// and nothing in it is interpreted. The Windows exe is a GUI-subsystem app
// (-mwindows) with no console of its own; the child runs with
// CREATE_NO_WINDOW so no console window appears, and grandchildren
// inherit the hidden console.
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

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HOST_SPAWN_H */
