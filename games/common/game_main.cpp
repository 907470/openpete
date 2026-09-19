// games/common/game_main.cpp — the entry point of a game with no host-side
// services of its own.
//
// Parse options, register the recompiled module, hand off to core's run loop.
// A game that resolves a disc library, drives a splash, or registers mod
// services supplies its own main TU instead (games/spyro-1/main.cpp).
// SDL3's SDL_main.h re-#defines main -> SDL_main and supplies the platform
// entry shim; it must be included by the TU defining main().

#include <SDL3/SDL_main.h>
#include "openpete.h"
#include "openpete_config.h"
#include "openpete_log.h"
#include "game_extras.h"
#include "openpete_psx.h"
#include "psyq/psyq_game_hooks.h"
#include "openpete_recomp_module.h"
#include "renderer/orchestrator.h"   /* game_interp_register, game_reextract_init */

#include <cstdio>

extern "C" void game_state_regions_register(void);
extern "C" const openpete_recomp_module openpete_game_recomp_module;

int main(int argc, char** argv) {
    /* abort() does not flush, and the PSYQ stubs use it to surface an
     * unimplemented surface. Unbuffer so that diagnostic survives. */
    setvbuf(stderr, NULL, _IONBF, 0);

    openpete_options_t opts;
    openpete_options_init(&opts);
    opts.display_name = game_get_display_name();
    openpete_config_set_game_id(game_get_id());

    int rc = openpete_options_parse(&opts, argc, argv);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (opts.config_report) { openpete_config_report(stdout); return 0; }

    openpete_log_init(opts.log_format, opts.log_level_spec);
    openpete_log_file_open(opts.log_file);

    {
        int src_rc = openpete_options_require_exe_source(&opts);
        if (src_rc != OPENPETE_EXIT_OK) return src_rc;
    }

    /* The runtime reaches recompiled code only through this descriptor. */
    if (openpete_recomp_register(&openpete_game_recomp_module) != 0) return 1;

    /* The gameplay-state region table the oracle hashes (games/common/
     * state_regions.c); explicit, before openpete_run(). */
    game_state_regions_register();

    /* The interp ring's region table and the sub-tick re-extract entry, both
     * before openpete_run() so openpete_interp_init() sees the regions; inert
     * on a game whose TUs are the none defaults. */
    game_interp_register();
    game_reextract_init();

    /* Root counter 2 as the game's boot reads it (the RNG seed source). */
    openpete_psx_set_root_counter2(psyq_game_root_counter2());

    return openpete_run(&opts);
}
