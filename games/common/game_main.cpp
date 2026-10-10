// games/common/game_main.cpp - the entry point of a game with no host-side
// services of its own.
//
// Register the game's runtime descriptor, parse options, resolve the disc
// through the common launcher, register the mod-layer descriptor and the
// recompiled module, compile stale mods on a
// launcher wait, hand off to core's run loop. A game that loads its module
// from the library or configures game-specific mod services (the Spyro 1 WAD
// overlay) supplies its own main TU instead (games/spyro-1/main.cpp).
// SDL3's SDL_main.h re-#defines main -> SDL_main and supplies the platform
// entry shim; it must be included by the TU defining main().

#include <SDL3/SDL_main.h>
#include "openpete.h"
#include "openpete_config.h"
#include "openpete_identify.h"
#include "openpete_log.h"
#include "openpete_boot_timing.h"
#include "openpete_keymap.h"
#include "openpete_launcher.h"
#include "openpete_surface.h"
#include "game_extras.h"
#include "game_host_desc.h"
#include "openpete_mods.h"
#include "openpete_psx.h"
#include "psyq/psyq_game_hooks.h"
#include "recomp_attach.h"
#include "renderer/orchestrator.h"   /* game_interp_register, game_reextract_init */

#include <cstdio>

extern "C" void game_state_regions_register(void);
extern "C" void game_disc_boot_order_register(void);
/* This executable's game descriptor, `<tag>_game_runtime` in the game's
 * game_extras_impl.c (OPENPETE_GAME_RUNTIME, cmake/openpete_game.cmake). */
extern "C" const openpete_game_runtime_t OPENPETE_GAME_RUNTIME;

/** @brief Name the game from the game-neutral options and an optional library pick: the disc image, else --game, else this executable's game.
 *
 * @param opts The game-neutral options.
 * @param library_game The booted library entry's game ("" when unidentified), or NULL when the library booted nothing.
 * @param id Receives the identification.
 * @return OPENPETE_EXIT_OK or OPENPETE_EXIT_SETUP_ERROR. */
static int identify(const openpete_options_t* opts, const char* library_game,
                    openpete_ident_t* id) {
    openpete_ident_in_t in = {};
    in.selector = opts->game_selector;
    in.disc_path = opts->disc_path;
    in.from_library = library_game != nullptr;
    in.recorded_game = (library_game && *library_game) ? library_game : nullptr;
    in.linked_game = game_get_id();
    return openpete_identify(&in, id);
}

/** @brief The launcher's view of this game: its id, name and library module path; no known discs.
 *
 * @return The descriptor, built on first call. */
static const openpete_launcher_game* launcher_game_desc(void) {
    static openpete_launcher_game g;
    g.game_id = game_get_id();
    g.display_name = game_get_display_name();
    g.module_path = openpete_game_library_module_path;
    return &g;
}

/** @brief Answer --config-report or --keys-report from the identified resolution, without reading the library.
 *
 * @param argc The argument count.
 * @param argv The arguments.
 * @param neutral The game-neutral options.
 * @return 0 when the report is written, else the identification's or the parser's exit code. */
static int report_mode(int argc, char** argv, const openpete_options_t* neutral) {
    openpete_ident_t id;
    int rc = identify(neutral, nullptr, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    openpete_options_t opts;
    rc = openpete_options_parse_identified(&opts, argc, argv, &id, game_get_display_name());
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (opts.config_report) openpete_config_report(stdout);
    else openpete_keymap_report(stdout);
    return 0;
}

/** @brief Take the session from the game-neutral options to the hand-off: the launcher's disc, identification, the full parse, the mod layer, the recompiled module and the mod compile.
 *
 * The launcher resolves the disc through the library (the drop screen, the
 * version selector) when none is named and draws the mod compile's wait,
 * opening its window only when a stale mod has to compile. The hand-off
 * gives the game its window, Vulkan device and imgui context in a windowed
 * run.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param neutral The game-neutral options; receives the library's disc.
 * @param opts Receives the full resolution.
 * @param exit_now Set to 1 when the run ends in the launcher.
 * @return OPENPETE_EXIT_OK, OPENPETE_EXIT_SETUP_ERROR on a refused
 *         identification, the parser's status, or 1 when the recompiled
 *         module cannot be attached. */
static int start_session(int argc, char** argv, openpete_options_t* neutral,
                         openpete_options_t* opts, int* exit_now) {
    openpete_launcher_begin(launcher_game_desc(), neutral, !neutral->headless);
    static char library_module_path[4096];
    int rc = openpete_launcher_resolve(neutral, library_module_path,
                                       sizeof library_module_path, exit_now);
    if (rc != OPENPETE_EXIT_OK || *exit_now) return rc;
    openpete_ident_t id;
    rc = identify(neutral, openpete_launcher_selected_game(), &id);
    if (rc == OPENPETE_EXIT_OK) rc = openpete_identify_require_linked(&id, game_get_id());
    if (rc != OPENPETE_EXIT_OK) return rc;

    /* The one full resolution: the identified game's and disc's scopes
     * join the tiers, and the environment and argv are written once. */
    rc = openpete_options_parse_identified(opts, argc, argv, &id, game_get_display_name());
    if (rc != OPENPETE_EXIT_OK) return rc;
    rc = openpete_options_require_exe_source(opts);
    if (rc != OPENPETE_EXIT_OK) return rc;

    /* The mod layer, once, at identification: before the compile below
     * and the load inside openpete_run(). */
    if (openpete_game_runtime()->mod_desc)
        (void)openpete_mods_set_game_desc(openpete_game_runtime()->mod_desc());
    openpete_boot_mark(OP_BOOT_MODS_STAGED);

    /* The runtime reaches recompiled code only through the recomp-module
     * descriptor: the linked-in module (static linkage) or the one loaded
     * from --recomp-module, the library entry's module, or <tag>-recomp
     * beside the executable (shared linkage). Must precede any dispatch.
     * Only a build with the first-boot pipeline builds a missing module, and
     * never into an explicit --recomp-module path. */
    const char* module_override = opts->recomp_module_path;
    if (!module_override && library_module_path[0])
        module_override = library_module_path;
    openpete_boot_mark(OP_BOOT_MODULE_ATTACH_START);
    if (openpete_game_recomp_attach(module_override, opts->disc_path,
                                    opts->recomp_module_path == NULL) != 0)
        return 1;
    openpete_boot_mark(OP_BOOT_MODULE_ATTACHED);

    /* Mod compile-on-boot: every stale enabled source mod compiles on a
     * launcher task, so openpete_mods_init only loads. */
    openpete_mods_prepare(opts, openpete_launcher_progress, openpete_launcher_run_task);
    openpete_boot_mark(OP_BOOT_MODS_COMPILED);
    return openpete_launcher_handoff(opts);
}

/** @brief Run a game that has no host-side services of its own.
 *
 * Registers the game's runtime descriptor, resolves the game-neutral
 * settings, answers --help and the configuration
 * or keys report, opens the log; --compile-mods compiles every mod for each
 * staged game and exits before any game is identified. Otherwise runs the
 * launcher (the disc from the library when none is named), identifies the
 * game and refuses one this executable does not run, runs the one full
 * parse, registers the mod-layer descriptor, attaches the recompiled module,
 * compiles stale mods, hands the launcher's window and device to the game,
 * registers the gameplay-state regions and the interpolation and
 * re-extract tables, sets the root counter 2 the game's boot reads, and
 * runs the core loop. Writes the surface dump and exits when
 * OPENPETE_SURFACE_DUMP is set.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The exit status: openpete_run's result, 0 after --help, a report
 *         or a quit in the launcher, or start_session's failure status;
 *         under --compile-mods 0 when every mod compiled, else 1. */
int main(int argc, char** argv) {
    /* The game's descriptor binds every per-game hook and table; nothing
     * that reads one runs before it. */
    game_runtime_install(&OPENPETE_GAME_RUNTIME);
    openpete_boot_mark(OP_BOOT_LAUNCHER_START);
    /* abort() does not flush, and the PSYQ stubs use it to surface an
     * unimplemented surface. Unbuffer so that diagnostic survives. */
    setvbuf(stderr, NULL, _IONBF, 0);

    /* The [disc.*] scopes' keys for the identified disc: its serial, and
     * the hash its library entry recorded. */
    openpete_config_set_disc_identity_fn(openpete_library_disc_identity);

    /* OPENPETE_SURFACE_DUMP: write the surface and exit, before any config
     * or env layer runs. */
    openpete_surface_dump_run_if_requested(argv[0]);

    /* The game-neutral tiers, writing nothing to the process. */
    openpete_options_t neutral;
    openpete_options_init(&neutral);
    int rc = openpete_options_parse_neutral(&neutral, argc, argv);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (neutral.help) return 0;
    if (neutral.config_report || neutral.keys_report)
        return report_mode(argc, argv, &neutral);

    /* Format and file are device-creation knobs: the game-neutral tiers
     * decide them. */
    openpete_log_init(neutral.log_format, neutral.log_level_spec
                                              ? neutral.log_level_spec
                                              : openpete_config_env("OPENPETE_LOG"));
    openpete_log_file_open(neutral.log_file ? neutral.log_file
                                            : openpete_config_env("OPENPETE_LOG_FILE"));

    /* --compile-mods: compile every mod for each game staged under the
     * SDK and exit. No game is identified, so the settings are the
     * game-neutral tiers, materialized once. */
    if (neutral.compile_mods) {
        openpete_ident_t none = {};
        openpete_options_t batch;
        rc = openpete_options_parse_identified(&batch, argc, argv, &none,
                                               game_get_display_name());
        if (rc != OPENPETE_EXIT_OK) return rc;
        return openpete_mods_compile_all(&batch) == 0 ? 0 : 1;
    }

    openpete_options_t opts;
    int exit_now = 0;
    rc = start_session(argc, argv, &neutral, &opts, &exit_now);
    if (rc != OPENPETE_EXIT_OK || exit_now) {
        openpete_launcher_close();
        return rc;
    }

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
