// games/common/game_main.cpp - the entry point of a game with no host-side
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
#include "openpete_identify.h"
#include "openpete_log.h"
#include "openpete_boot_timing.h"
#include "openpete_keymap.h"
#include "openpete_surface.h"
#include "game_extras.h"
#include "openpete_psx.h"
#include "psyq/psyq_game_hooks.h"
#include "openpete_recomp_module.h"
#include "renderer/orchestrator.h"   /* game_interp_register, game_reextract_init */

#include <cstdio>

extern "C" void game_state_regions_register(void);
extern "C" void game_disc_boot_order_register(void);
extern "C" const openpete_recomp_module openpete_game_recomp_module;

/** @brief Name the game from the game-neutral options: the --disc image, else --game, else this executable's game.
 *
 * @param opts The game-neutral options.
 * @param id Receives the identification.
 * @return OPENPETE_EXIT_OK or OPENPETE_EXIT_SETUP_ERROR. */
static int identify(const openpete_options_t* opts, openpete_ident_t* id) {
    openpete_ident_in_t in = {};
    in.selector = opts->game_selector;
    in.disc_path = opts->disc_path;
    in.linked_game = game_get_id();
    return openpete_identify(&in, id);
}

/** @brief Run a game that has no host-side services of its own.
 *
 * Resolves the game-neutral settings, identifies the game from the disc
 * (--game, else this executable's game, when no disc names one), refuses a
 * game this executable does not run, then runs the one full parse, opens the
 * log, registers the recompiled module, the gameplay-state regions, and the
 * interpolation and re-extract tables, sets the root counter 2 the game's
 * boot reads, and runs the core loop. Writes the surface dump and exits when
 * OPENPETE_SURFACE_DUMP is set. Returns at once with the option parser's
 * status on a parse failure, after --help, after printing the configuration
 * or keys report when one is requested, or when the executable source is
 * unavailable.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The exit status: openpete_run's result, 0 after --help or a
 *         configuration or keys report, OPENPETE_EXIT_SETUP_ERROR on a refused
 *         identification, 1 when the recompiled module fails to register. */
int main(int argc, char** argv) {
    openpete_boot_mark(OP_BOOT_LAUNCHER_START);
    /* abort() does not flush, and the PSYQ stubs use it to surface an
     * unimplemented surface. Unbuffer so that diagnostic survives. */
    setvbuf(stderr, NULL, _IONBF, 0);

    /* The [disc.SERIAL] scopes' key for the identified disc. */
    openpete_config_set_disc_identity_fn(openpete_disc_identity_serial);

    /* OPENPETE_SURFACE_DUMP: write the surface and exit, before any config
     * or env layer runs. */
    openpete_surface_dump_run_if_requested(argv[0]);

    /* The game-neutral tiers, writing nothing to the process. */
    openpete_options_t neutral;
    openpete_options_init(&neutral);
    int rc = openpete_options_parse_neutral(&neutral, argc, argv);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (neutral.help) return 0;

    openpete_ident_t id;
    rc = identify(&neutral, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    const int report = neutral.config_report || neutral.keys_report;
    if (!report) {
        /* Format and file are device-creation knobs: the game-neutral
         * tiers decide them. */
        openpete_log_init(neutral.log_format, neutral.log_level_spec
                                                  ? neutral.log_level_spec
                                                  : openpete_config_env("OPENPETE_LOG"));
        openpete_log_file_open(neutral.log_file ? neutral.log_file
                                                : openpete_config_env("OPENPETE_LOG_FILE"));
        rc = openpete_identify_require_linked(&id, game_get_id());
        if (rc != OPENPETE_EXIT_OK) return rc;
    }

    /* The one full resolution: the identified game's and disc's scopes
     * join the tiers, and the environment and argv are written once. */
    openpete_options_t opts;
    rc = openpete_options_parse_identified(&opts, argc, argv, &id,
                                           game_get_display_name());
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (opts.config_report) { openpete_config_report(stdout); return 0; }
    if (opts.keys_report) { openpete_keymap_report(stdout); return 0; }

    {
        int src_rc = openpete_options_require_exe_source(&opts);
        if (src_rc != OPENPETE_EXIT_OK) return src_rc;
    }

    /* The runtime reaches recompiled code only through this descriptor. */
    if (openpete_recomp_register(&openpete_game_recomp_module) != 0) return 1;

    /* The gameplay-state region table the oracle hashes (games/common/
     * state_regions.c); explicit, before openpete_run(). */
    game_state_regions_register();

    /* The disc cache loader's boot-order profile (games/common/
     * disc_boot_order.c); before openpete_run() attaches the cache. */
    game_disc_boot_order_register();

    /* The interp ring's region table and the sub-tick re-extract entry, both
     * before openpete_run() so openpete_interp_init() sees the regions; inert
     * on a game whose TUs are the none defaults. */
    game_interp_register();
    game_reextract_init();

    /* Root counter 2 as the game's boot reads it (the RNG seed source). */
    openpete_psx_set_root_counter2(psyq_game_root_counter2());

    return openpete_run(&opts);
}
