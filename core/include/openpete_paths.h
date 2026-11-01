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
 * Deliberately SDL-free - config.c resolves paths before SDL exists, and
 * links into a console unit test.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Path of the running executable.
 *
 * Read from GetModuleFileNameW (converted to the active code page) on
 * Windows and /proc/self/exe elsewhere.
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @return 0 on success, -1 when the path cannot be read or does not fit. */
int openpete_exe_path(char* out, size_t cap);

/** @brief Take an exclusive, non-blocking lock on a file, creating it when absent: flock on POSIX, LockFileEx on Windows.
 *
 * The lock lasts until openpete_paths_unlock or the process ends.
 * @param path The file.
 * @param handle Receives the lock's handle.
 * The reason for a -2 is logged.
 * @return 0 when taken, -1 when another process holds it, -2 when the file cannot be opened or locked. */
int openpete_paths_lock(const char* path, intptr_t* handle);

/** @brief Release a lock openpete_paths_lock took.
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
 * The openpete_paths_set_portable preference, else the platform default,
 * degraded to 0 when the exe directory cannot be located or is not writable.
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
 * @param leaf "library", "modules", "mods", "assets", "bk2" or "screenshots".
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @param create Non-zero to create the directory.
 * @return 0 on success, -1 when no base resolves or the path does not fit. */
int openpete_paths_data_dir(const char* leaf, char* out, size_t cap, int create);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PATHS_H */
