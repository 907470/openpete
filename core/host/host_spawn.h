// core/host/host_spawn.h — silent child-process spawn for host-side
// build steps (mod compile-on-boot; see plan openpete-mod-compile-on-boot).
//
// The Windows exe is a GUI-subsystem app (-mwindows) with no console of
// its own, so a plain system()/_popen() child pops a fresh console
// window per invocation. These helpers run the command with a hidden
// console on Windows (CREATE_NO_WINDOW) and plain system()/popen() on
// POSIX — same trick as the first-boot module build in recomp_build.cpp.
#ifndef OPENPETE_HOST_SPAWN_H
#define OPENPETE_HOST_SPAWN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Run cmd through the platform shell and wait for it, with no console window on Windows.
 *
 * On Windows the command runs as `cmd.exe /C "<cmd>"` under
 * CREATE_NO_WINDOW; the extra quote pair keeps cmd.exe from stripping the
 * outer quotes of a command with several quoted tokens, and grandchildren
 * inherit the hidden console. On POSIX it is system(). The mod
 * compile-on-boot step runs the compiler through it.
 * @param cmd Shell command line.
 * @return On Windows the child's exit code; on POSIX the system() wait
 *         status, which is 0 exactly when the child exited 0; -1 when the
 *         child could not be started. */
int openpete_host_spawn(const char* cmd);

/** @brief Run cmd like openpete_host_spawn and collect its output into out.
 *
 * On Windows the child's stdout and stderr both go to a pipe; on POSIX
 * popen() collects stdout only, so a caller that wants stderr redirects it
 * in cmd (`2>&1`). Output past cap - 1 bytes is read and discarded, so the
 * child never blocks on a full pipe.
 * @param cmd Shell command line.
 * @param out Output buffer, always NUL-terminated; set to "" before the
 *        spawn. Must be non-NULL with cap at least 1 on POSIX.
 * @param cap Size of out in bytes.
 * @return On Windows the child's exit code; on POSIX the pclose() wait
 *         status; -1 when the child could not be started. */
int openpete_host_spawn_capture(const char* cmd, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HOST_SPAWN_H */
