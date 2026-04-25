// games/spyro-1/main.cpp — Spyro 1 binary entry point.
//
// Tiny wrapper: parse opts, set the display name, hand off to core's run loop.
// SDL_main is provided by SDL2main on Windows; SDL.h re-#defines main when
// needed.

#include <SDL.h>
#include "openpete.h"
#include "game_extras.h"

#include <cstdio>

int main(int argc, char** argv) {
    openpete_options_t opts;
    openpete_options_init(&opts);
    opts.display_name = game_get_display_name();

    int rc = openpete_options_parse(&opts, argc, argv);
    if (rc != OPENPETE_EXIT_OK) return rc;

    return openpete_run(&opts);
}
