// games/common/game_build.h - what the build system binds per game, and the
// selection of the running game among the games an executable links.
//
// The game's runtime descriptor (core/include/openpete_game_runtime.h) holds
// what its sources define. This record holds what the build derives for the
// game: its entry, its recomp-module name and host->module forwarders, its
// first-boot pipeline, and the tables generated from its manifests (the
// gameplay-state regions, the disc boot order, the save-header location). cmake/openpete_game.cmake
// generates one per game, `<tag>_game_build`, from game_build.c.in, and one
// list per executable, `openpete_game_builds`, of every game the executable
// links: one game for a per-game executable, every built game for the one
// executable of a shared-linkage Linux or MinGW build.
// games/common/game_entry.cpp
// selects a game from the list, installs it and runs its entry.

#ifndef OPENPETE_GAME_BUILD_H
#define OPENPETE_GAME_BUILD_H

#include <stddef.h>

#include "openpete_game_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct openpete_game_build {
    const char* id;          /* "spyro-1": the game's config-scope id */
    const openpete_game_runtime_t* runtime;
    /* The game's entry after selection: parse, identify, attach, run
     * (openpete_game_main for a game with no entry of its own). */
    int (*main)(int argc, char** argv);

    /* Shared linkage: the module's file name without the platform suffix
     * ("spyro1-recomp"), and the binder of the game's host->module
     * forwarders (module_imports.h), which resolves each name through
     * `need` and returns how many it could not. NULL under static
     * linkage. */
    const char* module_name;
    int (*bind_module_funcs)(void* (*need)(const char* name));

    /* The first-boot module build (shared linkage under
     * OPENPETE_RECOMP_FIRST_BOOT, games/common/recomp_build.cpp); every
     * member NULL in a build without one. build_module and
     * library_module_path are the pipeline's entries (recomp_attach.h);
     * extract_recomp_inputs writes PSX.EXE and the manifest's overlays
     * carved from the disc; the window strings are recomp.conf's
     * OVERLAY_RANGE bounds as C literals. */
    int (*build_module)(const char* disc_path, const char* out_module_path);
    int (*library_module_path)(const char* entry_dir, char* out, size_t cap);
    int (*extract_recomp_inputs)(const char* cue_path, const char* out_dir,
                                 const char* manifest_path);
    const char* overlay_window_lo;
    const char* overlay_window_hi;

    /* The tables generated from the game's manifests. */
    void (*state_regions_register)(void);
    void (*disc_boot_order_register)(void);
    void (*save_icon_register)(void);
} openpete_game_build_t;

/* Every game this executable links, in OPENPETE_GAMES order (generated per
 * executable). */
extern const openpete_game_build_t* const openpete_game_builds[];
extern const size_t openpete_game_build_count;

/** @brief Return the linked game the command line names.
 *
 * With one linked game, that game. Otherwise, in order: the game whose
 * serial the SYSTEM.CNF of the image named by --disc or --ingest carries;
 * the --game selector. The executable's name selects no game. A
 * source naming a game this executable does not link is passed over; the
 * selected game's entry then identifies the session and refuses a
 * contradiction.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The game, or NULL when several games are linked and no source
 *         names one of them (the launcher then picks, game_pick.h). */
const openpete_game_build_t* openpete_game_build_named(int argc, char** argv);

/** @brief Select the game this process runs: the game the command line names, else the first linked game.
 *
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The selected game, never NULL while a game is linked. */
const openpete_game_build_t* openpete_game_build_select(int argc, char** argv);

/** @brief Return the linked game with an id.
 *
 * @param id A game id, or NULL.
 * @return The game, or NULL when this executable links none under that id. */
const openpete_game_build_t* openpete_game_build_linked(const char* id);

/** @brief Make a game the running game: record its build and register its runtime descriptor.
 *
 * Called once, before the game's entry runs and before any game table is
 * read.
 * @param b The game; it must outlive the process. */
void openpete_game_build_install(const openpete_game_build_t* b);

/** @brief Return the running game's build.
 *
 * @return The installed build, or NULL before openpete_game_build_install. */
const openpete_game_build_t* openpete_game_build(void);

/** @brief Register the running game's gameplay-state region table with the core's hash and .psxtrace paths.
 *
 * The game's entry calls it before openpete_run(); nothing is registered
 * before a game is installed. */
void game_state_regions_register(void);

/** @brief Install the running game's disc boot-order profile in the disc cache loader.
 *
 * The game's entry calls it before openpete_run() attaches the cache;
 * nothing is installed before a game is installed. */
void game_disc_boot_order_register(void);

/** @brief Register the running game's save-header location, which the window icon is decoded from.
 *
 * The game's entry calls it before openpete_run(); nothing is registered
 * before a game is installed. */
void game_save_icon_register(void);

/** @brief Run a game with no host-side entry of its own: the launcher, identification, the one full parse, the module attach and the core loop.
 *
 * Defined in games/common/game_main.cpp; runs after the process entry has
 * installed the game. Resolves the game-neutral settings, answers --help and
 * the configuration or keys report, opens the log; --compile-mods compiles
 * every mod for each staged game and exits before any game is identified.
 * Otherwise runs the launcher (the disc from the library when none is
 * named), identifies the game and refuses another, runs the one full parse,
 * registers the mod-layer descriptor, attaches the recompiled module,
 * compiles stale mods, hands the launcher's window and device to the game,
 * registers the gameplay-state regions and the interpolation and re-extract
 * tables, sets the root counter 2 the game's boot reads, and runs the core
 * loop. Writes the surface dump and exits when OPENPETE_SURFACE_DUMP is set.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The exit status: openpete_run's result, 0 after --help, a report
 *         or a quit in the launcher, or the startup failure's status; under
 *         --compile-mods 0 when every mod compiled, else 1. */
int openpete_game_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_BUILD_H */
