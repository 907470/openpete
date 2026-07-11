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

/* Run `cmd` through the platform shell and wait. Returns the child's
 * exit code, or -1 on spawn failure. No console window on Windows. */
int openpete_host_spawn(const char* cmd);

/* Same, but collect the child's stdout+stderr into `out` (always
 * NUL-terminated, truncated at cap-1). Returns the exit code, or -1 on
 * spawn failure (out is set to ""). */
int openpete_host_spawn_capture(const char* cmd, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HOST_SPAWN_H */
