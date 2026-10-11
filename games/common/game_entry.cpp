// games/common/game_entry.cpp - the process entry of every executable.
//
// Selects the game among the games the executable links (game_build.h),
// installs it, and runs its entry. SDL3's SDL_main.h re-#defines main ->
// SDL_main and supplies the platform entry shim; it must be included by the
// TU defining main().

#include <SDL3/SDL_main.h>

#include "game_build.h"

#include <cstdio>

/** @brief Select and install the game this process runs, then run its entry.
 *
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The game entry's exit status, or 1 when the executable links no game. */
int main(int argc, char** argv) {
    const openpete_game_build_t* b = openpete_game_build_select(argc, argv);
    if (!b) {
        std::fprintf(stderr, "openpete: this executable links no game\n");
        return 1;
    }
    openpete_game_build_install(b);
    return b->main(argc, argv);
}
