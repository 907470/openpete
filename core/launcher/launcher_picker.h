/* launcher_picker.h - the launcher's drawn disc picker: a panel on the game
 * page listing the folders and .cue/.bin images under the home directory and
 * the mounted media (/run/media), driven by the launcher's commands (keys,
 * pad) and the mouse. It replaces the system file dialog under gamescope,
 * which shows neither the portal's dialog nor SDL's zenity fallback and
 * offers no drag and drop, and stands in for a dialog whose callback reports
 * a failure. The chosen image goes the way of a drop: its game, read at the
 * ingest, decides its box. Launcher thread only; the draw runs inside a
 * present's imgui callback.
 *
 * Harness variable: OPENPETE_LAUNCHER_PICKER_ROOTS=<dir>[:<dir>...] (';' on
 * Windows) replaces the roots, so a check lists a folder it made. */
#ifndef OPENPETE_LAUNCHER_PICKER_H
#define OPENPETE_LAUNCHER_PICKER_H

/* A command the picker takes, mapped from the page's commands. */
enum launcher_picker_cmd {
    LAUNCHER_PICKER_UP,        /* the row above */
    LAUNCHER_PICKER_DOWN,      /* the row below */
    LAUNCHER_PICKER_ACTIVATE,  /* open the folder, or choose the image, of the selected row */
    LAUNCHER_PICKER_BACK,      /* the parent folder; the root list from a root; close from the root list */
    LAUNCHER_PICKER_CLOSE      /* close without a choice */
};

/* What a command or a present's click did. */
enum launcher_picker_result {
    LAUNCHER_PICKER_STAYS,    /* the picker stays open */
    LAUNCHER_PICKER_CLOSED,   /* closed without a choice */
    LAUNCHER_PICKER_CHOSEN    /* an image was chosen (launcher_picker_chosen) and the picker closed */
};

/** @brief Report whether the system file dialog is never called: under gamescope (GAMESCOPE_WAYLAND_DISPLAY set) the drawn picker is the only way to choose a disc.
 *
 * @return true under gamescope. */
bool launcher_picker_only(void);

/** @brief Open the picker on its root list (the home directory and /run/media when present, or the harness roots), the first row selected.
 *
 * An open picker is left as it is.
 * @param why The reason, logged ("gamescope", "the file dialog failed"). */
void launcher_picker_open(const char* why);

/** @brief Report whether the picker is open.
 *
 * @return true while it is open. */
bool launcher_picker_shown(void);

/** @brief Take one command: move the selection, open a folder or choose an image, go up, or close.
 *
 * @param c The command.
 * @return What it did. */
launcher_picker_result launcher_picker_command(launcher_picker_cmd c);

/** @brief Return the image path the last LAUNCHER_PICKER_CHOSEN result chose.
 *
 * @return The path, "" before any choice. */
const char* launcher_picker_chosen(void);

/** @brief Draw the open picker into the current imgui window: the panel (Spyro 3's dialogue box at the sky's cover, so none over the flat background), the folder's path and its rows, the selected row in orange; a click on a row selects and activates it, the wheel moves the selection.
 *
 * Each line carries the chip line's text scrim and is shortened with "..."
 * to the panel's width: a path keeps its end, a name its start.
 * Elements are recorded in the element manifest as picker.panel,
 * picker.path and picker.row.<n> (n the visible row, from 0).
 * @param scale Window pixels per layout pixel.
 * @param ox The layout origin's x in window coordinates.
 * @param oy Its y.
 * @return What a click did, LAUNCHER_PICKER_STAYS without one or while closed. */
launcher_picker_result launcher_picker_draw(float scale, float ox, float oy);

#endif /* OPENPETE_LAUNCHER_PICKER_H */
