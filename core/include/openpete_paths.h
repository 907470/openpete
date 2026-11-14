#ifndef OPENPETE_PATHS_H
#define OPENPETE_PATHS_H

/*
 * openpete_paths.h - where openpete keeps the files it writes: the
 * openpete.toml holding the [saved] zone, memory-card images, savestates,
 * the disc library, drop-in mods, user assets and recordings.
 *
 * Two layouts:
 *
 *   portable  everything in the game folder, next to the exe -
 *             <exe>/openpete.toml, <exe>/cards/, <exe>/states/,
 *             <exe>/library/, <exe>/mods/, so an install is one directory
 *             the user can see, move, back up or run off a stick, and
 *             nothing hides in AppData.
 *   user      the platform's per-user directories: config under
 *             $XDG_CONFIG_HOME/openpete or %APPDATA%\openpete, state
 *             under $XDG_STATE_HOME/openpete or %LOCALAPPDATA%\openpete,
 *             bulk data under $XDG_DATA_HOME/openpete or
 *             %LOCALAPPDATA%\openpete.
 *
 * Windows defaults to portable, POSIX to user. A Windows install is a
 * folder the user unzipped and expects to own; a Linux one is expected to
 * honour XDG, where the per-user directories are the visible, backed-up,
 * conventional place. `portable = true|false` at the top level of
 * openpete.toml overrides on either platform.
 *
 * Whichever way portable is decided, a non-writable exe directory
 * (Program Files, a read-only mount) falls back to the user layout: a
 * game that cannot write its saves at all is worse than one that writes
 * them somewhere less obvious.
 *
 * A third layout, the sandbox (OPENPETE_SANDBOX=<root>), puts config,
 * state and data under <root>/config, <root>/state and <root>/data for
 * automated runs, which must not touch a player's files.
 *
 * Deliberately SDL-free - config.c resolves paths before SDL exists, and
 * links into a console unit test.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Exit status of a run the sandbox guard refuses (openpete_paths_sandbox_guard);
 * distinct from 64, a gate harness's SKIP and the card guard's refusal. */
#define OPENPETE_EXIT_SANDBOX_GUARD 66

/** @brief The sandbox root every per-user location resolves under, or NULL when none is set.
 *
 * Latched on the first call from OPENPETE_SANDBOX: its canonical form is
 * kept, and the directory is made by the first resolver that creates a
 * directory under it. Under a root the config, state and data
 * bases are <root>/config, <root>/state and <root>/data on every platform,
 * the portable layout and the adoption of directories beside the
 * executable are off, and writers with a working-directory fallback write
 * nothing instead. A root that does not canonicalize or fit leaves NULL and
 * makes openpete_paths_sandbox_guard refuse.
 * @return The canonical root, or NULL. */
const char* openpete_paths_sandbox(void);

/** @brief Replace the sandbox root for the rest of the process; NULL or "" clears it.
 *
 * The layout is re-resolved at the next query. Whether the boot guard is
 * enforced stays as the environment decided at the latch.
 * @param root The new root, made by the first resolver that creates under it.
 * @return 0, or -1 when the root does not canonicalize or fit (no root is
 *         then set). */
int openpete_paths_set_sandbox(const char* root);

/** @brief Refuse a run whose OPENPETE_SANDBOX is set while a write path lies in a real per-user or executable-directory location.
 *
 * Enforced only when OPENPETE_SANDBOX was set in the environment at the
 * latch. Tested: the root's config, state and data directories and every
 * state and data leaf they resolve; the output paths OPENPETE_LOG_FILE,
 * OPENPETE_HANG_TRACE, OPENPETE_HANG_RAM, OPENPETE_CARD_DIR and
 * OPENPETE_CONFIG name; and the values of the output flags on the command
 * line (--log-file, --card-dir, --card1, --card2, --config, --bk2-record,
 * --screenshot-output, --state-save-output, --ram-dump-output, --record,
 * --profile-out, --halt-cpu-dump-output, --mods-dir). A real location is
 * a per-user config, state or data openpete directory (as the environment
 * names it and, outside Windows, under the account's passwd home) or a
 * leaf the portable layout writes beside the executable. Each path is
 * compared after openpete_paths_canonical. The first match is printed.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return 0, or OPENPETE_EXIT_SANDBOX_GUARD. */
int openpete_paths_sandbox_guard(int argc, char** argv);

/** @brief Path of the running executable.
 *
 * Read from GetModuleFileNameW (converted to the active code page) on
 * Windows and /proc/self/exe elsewhere.
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @return 0 on success, -1 when the path cannot be read or does not fit. */
int openpete_exe_path(char* out, size_t cap);

/** @brief Report whether a path is itself a link, without following it: a symbolic link (lstat) on POSIX, a reparse point (a symbolic link or junction) on Windows.
 *
 * @param path The path, UTF-8 on Windows.
 * @return 1 for a link, 0 otherwise (including a missing path). */
int openpete_paths_is_link(const char* path);

/** @brief Take an exclusive, non-blocking lock on a file, creating it when absent: flock on POSIX, LockFileEx on Windows.
 *
 * The lock lasts until openpete_paths_unlock or the process ends.
 * @param path The file.
 * @param handle Receives the lock's handle.
 * The reason for a -2 is logged.
 * @return 0 when taken, -1 when another process holds it, -2 when the file cannot be opened or locked. */
int openpete_paths_lock(const char* path, intptr_t* handle);

/** @brief Take an exclusive lock on a file as openpete_paths_lock does, retrying every poll_ms milliseconds while another process holds it, for at most wait_ms.
 *
 * When the lock is found held and a retry follows, waiting is called once;
 * before each retry, cancelled is asked whether to stop. The lock is the
 * one openpete_paths_lock takes (flock, LockFileEx), so a holder that exits
 * releases it and no lock file is ever stale.
 * @param path The file, created when absent.
 * @param handle Receives the lock's handle.
 * @param wait_ms The longest wait in milliseconds; 0 tries once, a negative value waits until the lock is free or cancelled stops it.
 * @param poll_ms The retry interval in milliseconds (0 is read as 1).
 * @param cancelled Returns nonzero to stop waiting; may be NULL.
 * @param waiting Called once when the lock is found held and a retry follows; may be NULL.
 * @param user Passed to both callbacks.
 * @return 0 taken at once, 1 taken after waiting, -1 another process held it for the whole wait or cancelled stopped it, -2 when the file cannot be opened or locked (logged). */
int openpete_paths_lock_wait(const char* path, intptr_t* handle, int wait_ms, unsigned poll_ms,
                             int (*cancelled)(void* user), void (*waiting)(void* user), void* user);

/** @brief Release a lock openpete_paths_lock or openpete_paths_lock_wait took.
 *
 * @param handle The lock's handle. */
void openpete_paths_unlock(intptr_t handle);

/** @brief The running process's id.
 *
 * @return The id: getpid on POSIX, GetCurrentProcessId on Windows. */
long long openpete_process_id(void);

/** @brief Directory containing the running executable, with no trailing separator.
 *
 * Read from GetModuleFileNameW (converted to the active code page) on
 * Windows and /proc/self/exe elsewhere.
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @return 0 on success, -1 when the path cannot be read or does not fit. */
int openpete_exe_dir(char* out, size_t cap);

/** @brief Write <dir><sep><leaf> with the platform separator.
 *
 * A dir that already ends in '/' or '\\' gets no second separator. The
 * result truncates at @p cap rather than overruns.
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @param dir The directory.
 * @param leaf The name appended to it. */
void openpete_paths_join(char* out, size_t cap, const char* dir, const char* leaf);

/** @brief Select the layout: 1 portable, 0 user, -1 platform default.
 *
 * The config reader calls it for a `portable` directive, and with -1 to drop
 * the previous read's directive. It invalidates the cached resolution, so it
 * may run after openpete_paths_portable has been called.
 * @param tri 1, 0, or any negative value for the platform default. */
void openpete_paths_set_portable(int tri);

/** @brief The resolved layout: 1 portable, 0 user.
 *
 * 0 under a sandbox. Otherwise the openpete_paths_set_portable preference,
 * else the platform default, degraded to 0 when the exe directory cannot be located or is not writable.
 * The result is cached until the next openpete_paths_set_portable, so the
 * degrade warning prints once per resolution.
 * @return 1 for the portable layout, 0 for the user layout. */
int openpete_paths_portable(void);

/** @brief The per-user config directory: %APPDATA%\\openpete or $XDG_CONFIG_HOME/openpete.
 *
 * Without the environment variable the base is %USERPROFILE%\\AppData\\Roaming
 * or ~/.config. Pass @p create 0 to name the directory only: the layered
 * config reads the file there whether or not it is the write target, and a
 * portable install leaves no empty directory in AppData. There is no
 * write-target call; config.c holds both candidate paths as config layers
 * and picks between them with openpete_paths_portable.
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @param create Non-zero to create the base and openpete directories.
 * @return 0 on success, -1 when no base resolves or the path does not fit. */
int openpete_paths_user_config_dir(char* out, size_t cap, int create);

/** @brief Resolve the directory holding one class of machine-local state.
 *
 * Portable layout: <exe dir>/<leaf>. Otherwise <leaf> under the openpete
 * directory in the per-user state base (%LOCALAPPDATA% on Windows,
 * $XDG_STATE_HOME or ~/.local/state elsewhere). With @p create the directory is
 * made, and in the portable layout a one-time log line names a per-user
 * copy of the same leaf left by an earlier layout. Pass 0 to name the
 * directory only, so a query such as --config-report leaves no directories
 * behind.
 * @param leaf "cards", "states", "logs" or "cache".
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @param create Non-zero to create the directory.
 * @return 0 on success, -1 when no base resolves or the path does not fit. */
int openpete_paths_state_dir(const char* leaf, char* out, size_t cap, int create);

/** @brief Name a leaf under the per-user state directory, whatever the layout or sandbox, creating nothing.
 *
 * <leaf> under the openpete directory in the per-user state base
 * (%LOCALAPPDATA% on Windows, $XDG_STATE_HOME or ~/.local/state
 * elsewhere), the directory openpete_paths_state_dir returns outside the
 * portable layout and the sandbox.
 * @param leaf The leaf ("cards").
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @return 0 on success, -1 when no base resolves or the path does not fit. */
int openpete_paths_user_state_dir(const char* leaf, char* out, size_t cap);

/** @brief Write the absolute form of path with `.` and `..` applied and, outside Windows, symbolic links resolved.
 *
 * POSIX: realpath; a path that does not exist takes its longest existing
 * prefix's resolved form with the remaining components applied
 * lexically. Windows: _fullpath with every separator written as a
 * backslash; links are not resolved.
 * @param path The path, absolute or relative to the working directory.
 * @param out Receives the canonical path.
 * @param cap Capacity of @p out.
 * @return 0 on success, -1 on an empty path or one that does not fit. */
int openpete_paths_canonical(const char* path, char* out, size_t cap);

/** @brief Return 1 when path's canonical form equals dir's or lies below it, else 0.
 *
 * Both are canonicalized by openpete_paths_canonical; the comparison
 * stops at a component boundary and ignores case on Windows. A path
 * either argument cannot canonicalize gives 0.
 * @param path The path tested.
 * @param dir The directory.
 * @return 1 inside, 0 outside. */
int openpete_paths_within(const char* path, const char* dir);

/** @brief Resolve one game's directory inside a state leaf: <state dir>/<leaf>/<game>.
 *
 * The leaf resolves as openpete_paths_state_dir does; the game's directory
 * is joined under it, so every game keeps its files apart inside one leaf
 * ("states/spyro-1/"). A NULL or empty @p game names the leaf itself. With
 * @p create both directories are made.
 * @param leaf A leaf openpete_paths_state_dir accepts ("states").
 * @param game The game id ("spyro-1"), or NULL.
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @param create Non-zero to create the directories.
 * @return 0 on success, -1 when no base resolves or the path does not fit. */
int openpete_paths_game_state_dir(const char* leaf, const char* game, char* out,
                                  size_t cap, int create);

/** @brief Resolve the directory holding one class of bulk user data.
 *
 * Portable layout: <exe dir>/<leaf>. Otherwise, when <exe dir>/<leaf>
 * already exists and is writable, that directory is adopted, since these
 * leaves were exe-relative in earlier layouts and an older install holds
 * content there; a read-only install (an AppImage mount, Program Files)
 * has none to adopt. Failing that, <leaf> under the openpete directory in
 * the per-user data base (%LOCALAPPDATA% on Windows, $XDG_DATA_HOME or
 * ~/.local/share elsewhere). These leaves hold ingested discs, compiled
 * modules and user-authored content, where the state dirs hold
 * machine-local state. With @p create the directory is made; an adopted
 * directory already exists.
 * @param leaf "library", "modules", "mods", "assets", "bk2", "screenshots" or "bugreports".
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @param create Non-zero to create the directory.
 * @return 0 on success, -1 when no base resolves or the path does not fit. */
int openpete_paths_data_dir(const char* leaf, char* out, size_t cap, int create);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PATHS_H */
