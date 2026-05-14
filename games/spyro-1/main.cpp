// games/spyro-1/main.cpp — Spyro 1 binary entry point.
//
// Tiny wrapper: parse opts, set the display name, hand off to core's run loop.
// SDL_main is provided by SDL2main on Windows; SDL.h re-#defines main when
// needed.

#include <SDL.h>
#include "openpete.h"
#include "game_extras.h"
#include "spyro_interp_regions.h"

#include <cstdio>

int main(int argc, char** argv) {
    /* MinGW buffers stderr when it isn't a TTY (true for `>file 2>&1`
     * and for WSL2 interop pipes). abort() — which the PSYQ stubs use to
     * surface unimplemented-surface hits — doesn't flush, so otherwise
     * useful diagnostics get dropped right before exit. Unbuffer once at
     * startup. Linux behaviour is unchanged (line-buffered → unbuffered
     * is fine for stderr). */
    setvbuf(stderr, NULL, _IONBF, 0);

    openpete_options_t opts;
    openpete_options_init(&opts);
    opts.display_name = game_get_display_name();
    /* Hint for the parser's "missing --run-recomp" diagnostic. main()
     * lives at 0x80012204 in spyro-1/build/psx.map; that's what the user
     * almost always wants to dispatch. */
    opts.run_recomp_hint_addr = 0x80012204u;

    int rc = openpete_options_parse(&opts, argc, argv);
    if (rc != OPENPETE_EXIT_OK) return rc;

    /* fps-unlock Phase 2: register the interp ring's region table. Must
     * happen before openpete_run() so openpete_interp_init() (called
     * from inside the run loop) sees the regions. Explicit registration
     * instead of the weak-symbol pattern used by openpete_state_regions
     * — MinGW's PE/COFF linker mis-resolves weak overrides in this
     * configuration. */
    spyro_interp_register();

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
