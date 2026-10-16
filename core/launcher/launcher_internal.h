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

/* The launcher's start-up events, each logged once by launcher_stamp. */
enum launcher_event {
    LAUNCHER_EV_WINDOW,          /* the launcher's window and device exist */
    LAUNCHER_EV_FIRST_PRESENT,   /* the first launcher frame was presented */
    LAUNCHER_EV_SCAN_START,      /* the library scan began */
    LAUNCHER_EV_SCAN_END,        /* the library scan returned */
    LAUNCHER_EV_LIST_SHOWN,      /* the first selector frame with its entries was presented */
    LAUNCHER_EV_COUNT
};

/** @brief Log a start-up event once, in ms after process start.
 *
 * Process start is the launcher translation unit's static initialization,
 * before main. A later call for the same event is ignored. Safe on any
 * thread.
 * @param ev The event. */
void launcher_stamp(launcher_event ev);

/** @brief Report whether the launcher names no game: an executable linking several games, before one is installed.
 *
 * @return true while the descriptor passed to openpete_launcher_begin
 *         carries no game id. */
bool launcher_neutral(void);

/** @brief Take the harness drop OPENPETE_LAUNCHER_DROP once, as if that path had been dropped.
 *
 * The first call that finds the variable set stores its path for
 * launcher_dropped_path and returns it; every later call returns NULL, so
 * the scripted drop is ingested once per process.
 * @return The path, owned by the launcher, or NULL. */
const char* launcher_scripted_drop(void);

/** @brief The game the launcher was started for (openpete_launcher_begin).
 *
 * @return The game, or NULL before openpete_launcher_begin. */
const openpete_launcher_game* launcher_game(void);

/** @brief Run the version selector over the library's entries.
 *
 * Opens the launcher's window when it is not open. Up and down (keys,
 * d-pad, left stick) move the selection; Enter, the gamepad's south button
 * or a click picks; Escape, the east button or closing the window quits; a
 * file dropped on the window, or chosen in the file dialog Space or the
 * gamepad's west button opens, returns LAUNCHER_SEL_DROPPED with its path in
 * launcher_dropped_path. The harness variable OPENPETE_LAUNCHER_DROP acts
 * as a drop after the first frame, and without a screen at once
 * (launcher_scripted_drop); OPENPETE_LAUNCHER_SELECT set to an index below
 * @p count picks that entry after the first frame.
 * @param labels The entry labels, most recently played first.
 * @param count Their number, at least one; at most six show.
 * @return The picked index, or a LAUNCHER_SEL_* value. */
int launcher_select(const char* const* labels, int count);

/** @brief Run the library scan on a worker while the launcher shows its pending screen.
 *
 * With screens (a windowed run before the hand-off) the scan starts on a
 * worker thread, then the launcher's window and device are created and the
 * pending screen (the title and a reading line) presents until the scan
 * returns; a quit or a dropped disc during the wait is returned for the
 * caller to act on once the scan is done. A device that cannot be created
 * is reported by the next launcher_open that knows whether the run can
 * continue. Without screens, or when the worker cannot start, @p fn runs on
 * the calling thread. @p fn must not touch SDL video or the launcher's
 * screens.
 * @param fn The scan.
 * @param userdata Its argument.
 * @return 0, LAUNCHER_SEL_QUIT or LAUNCHER_SEL_DROPPED. */
int launcher_await_scan(openpete_launcher_task_fn fn, void* userdata);

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
 * library; otherwise the entries recorded for this game (every game's
 * while the launcher names none), and the unidentified ones, are scanned.
 * The selector shows for several entries, and for any number while the
 * launcher names no game; one entry of a named game boots without it, and
 * without a screen the most recently played boots. A disc dropped on the
 * selector, or the scripted drop, is ingested and boots instead. Booting points opts->disc_path at the entry's
 * disc.cue, writes the module path, and stamps the entry played unless it
 * is another game's.
 * @param opts The options.
 * @param module_path Receives the module path, "" for none; NULL for none.
 * @param module_cap Its capacity.
 * @return 0 on a boot, a bypass or an empty library, else a
 *         LAUNCHER_LIB_* value. */
int library_resolve(openpete_options_t* opts, char* module_path, size_t module_cap);

#endif /* OPENPETE_LAUNCHER_INTERNAL_H */
