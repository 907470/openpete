/* openpete_fatal.h — tell the player why the program is about to exit. */
#ifndef OPENPETE_FATAL_H
#define OPENPETE_FATAL_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Allow or forbid the message box openpete_fatal_message shows.
 *
 * The game's main sets it from the options (on unless --headless), so an
 * unattended run never blocks on a modal box. Off until called.
 * @param on Nonzero allows the box. */
void openpete_fatal_set_interactive(int on);

/** @brief Report a fatal condition: one line on stderr (and so in the log file), and on Windows, when interactive, a message box.
 *
 * The Windows distribution build has no console, so without the box the
 * window closes or never opens with no explanation. The box text is the
 * message followed by the log file's path when the file sink is active.
 * Other platforms print only; their launchers keep a terminal or journal.
 * @param title Box title and stderr line prefix.
 * @param msg The message, one or more sentences. */
void openpete_fatal_message(const char* title, const char* msg);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FATAL_H */
