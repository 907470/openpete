#ifndef OPENPETE_PATHS_H
#define OPENPETE_PATHS_H

/*
 * openpete_paths.h — where openpete keeps the files it writes: the
 * openpete.toml holding the [saved] zone, memory-card images, savestates,
 * the disc library, drop-in mods, user assets and recordings.
 *
 * Two layouts:
 *
 *   portable  everything in the game folder, next to the exe —
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
 * Deliberately SDL-free — config.c resolves paths before SDL exists, and
 * links into a console unit test.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Directory containing the running executable, no trailing separator.
 * Returns 0 on success, -1 if it cannot be determined. */
int openpete_exe_dir(char* out, size_t cap);

/* <dir><sep><leaf>, using the platform separator and tolerating a `dir` that
 * already ends in one. Truncates rather than overruns. */
void openpete_paths_join(char* out, size_t cap, const char* dir, const char* leaf);

/* Select the layout: 1 portable, 0 user, -1 platform default. Called by
 * the config reader for a `portable` directive; invalidates the cached
 * resolution, so it may run after a probe. */
void openpete_paths_set_portable(int tri);

/* The resolved layout — the preference above, degraded to 0 when the exe
 * directory is not writable. Warns once about the degrade. */
int openpete_paths_portable(void);

/* The per-user config directory (%APPDATA%\openpete,
 * $XDG_CONFIG_HOME/openpete). `create` makes it; pass 0 to merely name it —
 * the layered config reads that file whether or not it is the write target,
 * and a portable install must not leave an empty directory in AppData just
 * for having looked. Returns 0 on success.
 *
 * There is no matching "give me the write target" call: config.c already
 * holds both candidate paths as config layers, and picks between them with
 * openpete_paths_portable(). */
int openpete_paths_user_config_dir(char* out, size_t cap, int create);

/* Directory holding a class of state — `leaf` is "cards" or "states".
 * `create` makes it; pass 0 to merely name it, so a query (--config-report)
 * does not leave directories behind for a session that never ran.
 * Returns 0 on success. */
int openpete_paths_state_dir(const char* leaf, char* out, size_t cap, int create);

/* Directory holding a class of bulk user data — `leaf` is "library",
 * "mods", "assets" or "bk2". Same portable-vs-user rule as the state
 * dirs, but the per-user spelling is the XDG *data* directory: these hold
 * ingested discs, compiled modules and user-authored content, not
 * machine-local session state.
 *
 * One extra rule the state dirs do not need: when the user layout is in
 * force and <exe>/<leaf> already exists in a writable install directory,
 * that directory is adopted. These four were unconditionally exe-relative
 * before, so every install predating the split already has content there,
 * and moving it out from under a launcher would read as data loss. A
 * read-only install (an AppImage mount, Program Files) has no such
 * directory to adopt and lands in the per-user location.
 *
 * `create` makes it; pass 0 to merely name it. Returns 0 on success. */
int openpete_paths_data_dir(const char* leaf, char* out, size_t cap, int create);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PATHS_H */
