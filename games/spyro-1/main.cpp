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

    /* Spyro's gp comes from PSYQ's _96_init boot stub, which we don't
     * recompile (psyq___main is a no-op). main()'s prologue assumes gp is
     * already set; without it, every gp-relative store (g_PadMutex flips,
     * g_DeltaTime writes, …) goes to PSX address 0x6XX instead of the real
     * global. Hardcode Spyro's gp from spyro-1/build/psx.map (g_PadMutex
     * is at 0x80075868, encoded as gp+1540 in main, so gp = 0x80075264).
     * The user can still override via --test-recomp-gp. */
    if (opts.run_recomp_addr != 0 && opts.test_recomp_gp == 0) {
        opts.test_recomp_gp = 0x80075264u;
    }

    return openpete_run(&opts);
}
