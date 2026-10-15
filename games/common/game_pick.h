// games/common/game_pick.h - the launcher's pick of the game, for an
// executable linking several games whose command line names none.
//
// Runs before any game is installed: the game-neutral options, the log, and
// the launcher over every game's library entries (the version selector, the
// drop screen and its ingest). The picked entry's game is then installed and
// its entry boots that entry (openpete_launcher_resolve). A run whose
// command line names a game (game_build.h) does not come here.

#ifndef OPENPETE_GAME_PICK_H
#define OPENPETE_GAME_PICK_H

#include "game_build.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Pick the game this process runs through the launcher, before any game is installed.
 *
 * Parses the game-neutral options. --help ends the run (the parse printed
 * it); a parse failure ends it after telling the player; an informational
 * or batch mode (--config-report, --keys-report, --compile-mods,
 * --extract-recomp-inputs, --dump-sfx-pack, the surface and tooltip
 * dumps) takes the first linked game, whose entry runs it as before. Otherwise the log opens and the launcher resolves the
 * disc over every game's entries; the game recorded on the booted entry is
 * picked, else the first linked game.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param exit_code Receives the process exit status when NULL is returned.
 * @return The game to install, or NULL when the run ends here (--help, a
 *         rejected setting, the player quit the launcher, the library
 *         could not be read). */
const openpete_game_build_t* openpete_game_pick(int argc, char** argv, int* exit_code);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_PICK_H */
