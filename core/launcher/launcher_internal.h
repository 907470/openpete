/* launcher_internal.h - the calls between the launcher's screens
 * (launcher.cpp) and its disc library (library.cpp). */
#ifndef OPENPETE_LAUNCHER_INTERNAL_H
#define OPENPETE_LAUNCHER_INTERNAL_H

#include <stddef.h>

#include "openpete_launcher.h"

/* library_resolve results besides 0. */
#define LAUNCHER_LIB_FAILED (-1)   /* ingest failure or an unreadable entry */
#define LAUNCHER_LIB_QUIT   (-2)   /* the player quit the version selector */

/* launcher_select results besides an index. */
#define LAUNCHER_SEL_QUIT     (-1)  /* the player quit */
#define LAUNCHER_SEL_DROPPED  (-2)  /* a disc image was dropped instead */
#define LAUNCHER_SEL_NOSCREEN (-3)  /* no screen can show: take the most recent */

/** @brief The game the launcher was started for (openpete_launcher_begin).
 *
 * @return The game, or NULL before openpete_launcher_begin. */
const openpete_launcher_game* launcher_game(void);

/** @brief Run the version selector over the library's entries.
 *
 * Opens the launcher's window when it is not open. Up and down (keys,
 * d-pad, left stick) move the selection; Enter, the gamepad's south button
 * or a click picks; Escape, the east button or closing the window quits; a
 * file dropped on the window returns LAUNCHER_SEL_DROPPED with its path in
 * launcher_dropped_path. The harness variable OPENPETE_LAUNCHER_SELECT set
 * to an index below @p count picks that entry after the first frame.
 * @param labels The entry labels, most recently played first.
 * @param count Their number; at most six show.
 * @return The picked index, or a LAUNCHER_SEL_* value. */
int launcher_select(const char* const* labels, int count);

/** @brief The disc image path of the last drop or file-dialog choice.
 *
 * @return The path, "" before the first; never NULL. */
const char* launcher_dropped_path(void);

/** @brief Resolve this boot's disc and module path through the library.
 *
 * An entry already booted in this process (the pick made before a game was
 * installed) boots again, without a scan or a stamp, when the options name
 * no image. --ingest copies the image in (on a launcher task, with
 * progress) and boots its entry; --disc or --exe-path bypasses the
 * library; otherwise the
 * entries recorded for this game, and the unidentified ones, are scanned,
 * and the one entry, the selector's pick, or (without screens) the most
 * recently played boots. Booting points opts->disc_path at the entry's
 * disc.cue, writes the module path, and stamps the entry played unless it
 * is another game's.
 * @param opts The options.
 * @param module_path Receives the module path, "" for none; NULL for none.
 * @param module_cap Its capacity.
 * @return 0 on a boot, a bypass or an empty library, else a
 *         LAUNCHER_LIB_* value. */
int library_resolve(openpete_options_t* opts, char* module_path, size_t module_cap);

#endif /* OPENPETE_LAUNCHER_INTERNAL_H */
