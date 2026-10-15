// games/spyro-1/main.cpp - Spyro 1's entry (spyro1_main), run by the
// process entry (games/common/game_entry.cpp) once Spyro 1 is selected and
// installed.
//
// Parse opts, identify the game, attach the module, hand off to core's run
// loop.

#include "openpete.h"
#include "openpete_fatal.h"
#include "openpete_config.h"
#include "openpete_identify.h"
#include "openpete_log.h"
#include "openpete_boot_timing.h"
#include "openpete_keymap.h"
#include "openpete_launcher.h"
#include "game_extras.h"
#include "openpete_mods.h"
#include "openpete_surface.h"
#include "recomp_attach.h"
#include "mods/mod_wad_overlay.h"
#include "game_build.h"
#include "game_host_desc.h"

extern "C" int spyro1_dump_sfx_pack(const char* cue_path, const char* out_dir,
                                    const char* extra_wads);
extern "C" int spyro1_extract_recomp_inputs(const char* cue_path,
                                            const char* out_dir);
#include "renderer/interp_regions.h"
#include "renderer/reextract.h"

#include <cstdio>

/* The README-pinned retail NTSC-U disc, labelled by name in the library. */
static const openpete_launcher_known_disc k_known_discs[] = {
    {"fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37",
     "Spyro the Dragon (NTSC-U)"},
};

/** @brief The launcher's view of this game: its id, name, known discs and library module path.
 *
 * @return The descriptor, built on first call. */
static const openpete_launcher_game* launcher_game_desc(void) {
    static openpete_launcher_game g;
    g.game_id = game_get_id();
    g.display_name = game_get_display_name();
    g.known_discs = k_known_discs;
    g.known_disc_count = (int)(sizeof k_known_discs / sizeof k_known_discs[0]);
    g.module_path = openpete_game_library_module_path;
    return &g;
}

/** @brief Tell the player the game cannot start, then hand back the exit code.
 *
 * @param rc The exit code main returns.
 * @param why What failed, as a sentence for the player.
 * @return rc. */
static int fatal_start(int rc, const char* why) {
    openpete_fatal_message("OpenPete could not start", why);
    return rc;
}

/** @brief Parse the command line and the config stack into opts, telling the player when the settings are rejected.
 *
 * With no identification the parse is the game-neutral one
 * (openpete_options_parse_neutral), which writes nothing to the process;
 * with one it is the one full parse (openpete_options_parse_identified).
 * A launch with no arguments (a double-click) can only fail on the config
 * file or the environment, so only that case shows the message box; a
 * launch with arguments came from a terminal, which shows the parser's
 * message. After a successful parse the box is allowed for every run that
 * is not --headless.
 * @param opts Receives the options.
 * @param argc Argument count.
 * @param argv Arguments.
 * @param id The identification, or NULL for the game-neutral parse.
 * @return OPENPETE_EXIT_OK or the parser's exit code. */
static int parse_options(openpete_options_t* opts, int argc, char** argv,
                         const openpete_ident_t* id) {
    int rc = id ? openpete_options_parse_identified(opts, argc, argv, id,
                                                    game_get_display_name())
                : openpete_options_parse_neutral(opts, argc, argv);
    openpete_fatal_set_interactive(!opts->headless && argc <= 1);
    if (rc != OPENPETE_EXIT_OK)
        return fatal_start(rc, "A setting in openpete.toml (or an OPENPETE_ "
                               "environment variable) was not accepted.");
    openpete_fatal_set_interactive(!opts->headless);
    return OPENPETE_EXIT_OK;
}

/** @brief Name the game from the game-neutral options and an optional library pick.
 *
 * The disc is --ingest's image or the --disc path; a library boot passes
 * the entry it booted and that entry's recorded game. This executable's own
 * game is the last source.
 * @param opts The options holding --game and the disc.
 * @param library_game The booted library entry's game, or NULL when the library booted nothing.
 * @param id Receives the identification.
 * @return OPENPETE_EXIT_OK or OPENPETE_EXIT_SETUP_ERROR. */
static int identify(const openpete_options_t* opts, const char* library_game,
                    openpete_ident_t* id) {
    openpete_ident_in_t in = {};
    in.selector = opts->game_selector;
    in.disc_path = opts->ingest_path && !library_game ? opts->ingest_path : opts->disc_path;
    in.from_library = library_game != nullptr;
    in.recorded_game = (library_game && *library_game) ? library_game : nullptr;
    in.linked_game = game_get_id();
    return openpete_identify(&in, id);
}

/** @brief Answer an informational mode (--config-report, --keys-report) from the identified resolution.
 *
 * The game comes from the disc named on the command line or a game-neutral
 * tier, from --game, else this executable; the library is not read, so a
 * report writes nothing. Mod keys are bindings too; the keys report is the
 * audit of what an install responds to, so it registers them first.
 * @param argc Argument count.
 * @param argv Arguments.
 * @param neutral The game-neutral options.
 * @return 0 when the report is written, else the identification's or the parser's exit code. */
static int report_mode(int argc, char** argv, const openpete_options_t* neutral) {
    openpete_ident_t id;
    int rc = identify(neutral, nullptr, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    openpete_options_t opts;
    rc = parse_options(&opts, argc, argv, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (opts.config_report) { openpete_config_report(stdout); return 0; }
    openpete_mods_register_key_defaults(&opts);
    openpete_keymap_report(stdout);
    return 0;
}

/** @brief Register this game's mod layer: the runtime descriptor's mod-layer descriptor (mods/mod_game_desc.c) and the WAD overlay.
 *
 * Runs once, after the game is identified and before any mod compile or
 * load, so mods override by name ("CameraUpdate") against the identified
 * game's table. The WAD overlay reads the disc's TOC lazily, at the first
 * CdRead.
 * @param disc_path The identified disc. */
static void register_mod_layer(const char* disc_path) {
    (void)openpete_mods_set_game_desc(openpete_game_runtime()->mod_desc());
    spyro1_mod_wad_configure(disc_path);
}

/** @brief Run a batch exit mode that needs the identified disc: --extract-recomp-inputs or --dump-sfx-pack.
 *
 * --dump-sfx-pack takes the library's most recent entry when no disc is
 * named; the launcher shows no screen, since a batch command must not open a
 * window to ask which version to census.
 * @param argc Argument count.
 * @param argv Arguments.
 * @param neutral The game-neutral options.
 * @return The process exit code. */
static int batch_disc_mode(int argc, char** argv, openpete_options_t* neutral) {
    const char* library_game = nullptr;
    if (neutral->dump_sfx_pack_dir && !neutral->disc_path && !neutral->exe_path) {
        int exit_now = 0;
        openpete_launcher_begin(launcher_game_desc(), neutral, 0);
        (void)openpete_launcher_resolve(neutral, nullptr, 0, &exit_now);
        library_game = openpete_launcher_selected_game();
    }
    openpete_ident_t id;
    int rc = identify(neutral, library_game, &id);
    if (rc == OPENPETE_EXIT_OK) rc = openpete_identify_require_linked(&id, game_get_id());
    if (rc != OPENPETE_EXIT_OK) return rc;
    openpete_options_t opts;
    rc = parse_options(&opts, argc, argv, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    /* Standalone extraction stage - carve PSX.EXE + overlay blobs from the
     * disc and exit (also the first stage of the first-boot pipeline). Runs
     * before module attach: extraction must work when no module exists. */
    if (opts.extract_recomp_inputs_dir)
        return spyro1_extract_recomp_inputs(opts.disc_path,
                                            opts.extract_recomp_inputs_dir) ? 1 : 0;
    return spyro1_dump_sfx_pack(opts.disc_path, opts.dump_sfx_pack_dir,
                                opts.dump_sfx_wads) == 0 ? 0 : 1;
}

/** @brief Pick the disc through the library and the launcher, then identify the game.
 *
 * --ingest copies an image in; no disc at all boots the library's entry, or
 * the version selector picks among several; an empty library opens the drop
 * screen. An explicit --disc bypasses the library (dev/CI path). The
 * launcher's window and device open only when a screen is needed.
 * @param neutral The game-neutral options; receives the library's disc.
 * @param module_path Receives the library entry's module path, "" for none.
 * @param module_cap Its capacity.
 * @param id Receives the identification.
 * @param exit_now Set to 1 when the run ends in the launcher (the player quit).
 * @return OPENPETE_EXIT_OK, or the exit code after the player was told. */
static int launcher_identify(openpete_options_t* neutral, char* module_path,
                             size_t module_cap, openpete_ident_t* id,
                             int* exit_now) {
    openpete_launcher_begin(launcher_game_desc(), neutral, !neutral->headless);
    int rc = openpete_launcher_resolve(neutral, module_path, module_cap, exit_now);
    if (rc != OPENPETE_EXIT_OK || *exit_now) return rc;
    rc = identify(neutral, openpete_launcher_selected_game(), id);
    if (rc == OPENPETE_EXIT_OK) rc = openpete_identify_require_linked(id, game_get_id());
    if (rc != OPENPETE_EXIT_OK)
        return fatal_start(rc, "The disc is not Spyro the Dragon.");
    return OPENPETE_EXIT_OK;
}

/** @brief Take the session from the game-neutral options to the hand-off: the launcher's disc, identification, the full parse, the mod layer, the recompiled module and the mod compile.
 *
 * The launcher draws the waits (the first-boot module build, the mod
 * compile) and its window opens only when one of them has work. The
 * hand-off then gives the game the launcher's window, device and imgui
 * context in a windowed run (creating them when no screen did), so one OS
 * window and one Vulkan device carry launcher and game.
 * @param argc Argument count.
 * @param argv Arguments.
 * @param neutral The game-neutral options; receives the library's disc.
 * @param opts Receives the full resolution.
 * @param exit_now Set to 1 when the run ends in the launcher.
 * @return OPENPETE_EXIT_OK, or the exit code after the player was told. */
static int start_session(int argc, char** argv, openpete_options_t* neutral,
                         openpete_options_t* opts, int* exit_now) {
    static char library_module_path[4096];
    openpete_ident_t id;
    int rc = launcher_identify(neutral, library_module_path, sizeof library_module_path,
                               &id, exit_now);
    if (rc != OPENPETE_EXIT_OK || *exit_now) return rc;

    /* The one full resolution: the identified game's and disc's scopes
     * join the tiers, and the environment and argv are written once. */
    rc = parse_options(opts, argc, argv, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    rc = openpete_options_require_exe_source(opts);
    if (rc != OPENPETE_EXIT_OK)
        return fatal_start(rc, "No Spyro the Dragon disc image was found.");

    /* The mod layer, once, at identification: before the compile-on-boot
     * pass below and the load inside openpete_run(). */
    register_mod_layer(opts->disc_path);
    openpete_boot_mark(OP_BOOT_MODS_STAGED);

    /* The runtime reaches recompiled code only through the recomp-module
     * descriptor. Static builds register the linked-in module;
     * shared-linkage builds load spyro1-recomp and resolve the same
     * descriptor. Must precede any dispatch. Auto-building into the module
     * location (a build with the first-boot pipeline only) is fine for the
     * default path and for library entries; an explicit --recomp-module
     * points at a module the user asserts exists. */
    const char* module_override = opts->recomp_module_path;
    if (!module_override && library_module_path[0])
        module_override = library_module_path;
    const int allow_build = (opts->recomp_module_path == NULL);
    openpete_boot_mark(OP_BOOT_MODULE_ATTACH_START);
    if (openpete_game_recomp_attach(module_override, opts->disc_path, allow_build) != 0)
        return fatal_start(1, "The game code built from the disc could not be loaded or built.");
    openpete_boot_mark(OP_BOOT_MODULE_ATTACHED);

    /* Mod compile-on-boot: compile any stale enabled source mods on a
     * launcher task; the window opens only if there is real work. Loading
     * stays in openpete_run(); this pass only warms the build cache. */
    openpete_mods_prepare(opts, openpete_launcher_progress, openpete_launcher_run_task);
    openpete_boot_mark(OP_BOOT_MODS_COMPILED);
    return openpete_launcher_handoff(opts);
}

/** @brief Spyro 1 entry point: resolve the game-neutral settings, identify the game, apply its settings once, attach the recomp module, and run the game.
 *
 * The process entry has installed Spyro 1's runtime descriptor before this
 * runs.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return The process exit code (OPENPETE_EXIT_*, or 1 on a startup failure). */
extern "C" int spyro1_main(int argc, char** argv) {
    openpete_boot_mark(OP_BOOT_LAUNCHER_START);

    /* MinGW buffers stderr when it isn't a TTY (true for `>file 2>&1`
     * and for WSL2 interop pipes). abort() - which the PSYQ stubs use to
     * surface unimplemented-surface hits - doesn't flush, so otherwise
     * useful diagnostics get dropped right before exit. Unbuffer once at
     * startup. Linux behaviour is unchanged (line-buffered -> unbuffered
     * is fine for stderr). */
    setvbuf(stderr, NULL, _IONBF, 0);

    /* The [disc.*] scopes' keys for the identified disc: its serial, and
     * the hash its library entry recorded. */
    openpete_config_set_disc_identity_fn(openpete_library_disc_identity);

    /* OPENPETE_SURFACE_DUMP: write the surface and exit, before any config
     * or env layer runs. */
    openpete_surface_dump_run_if_requested(argv[0]);

    /* The game-neutral tiers: everything that runs before the game is
     * known reads these options, and nothing is written to the process. */
    openpete_options_t neutral;
    openpete_options_init(&neutral);
    int rc = parse_options(&neutral, argc, argv, nullptr);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (neutral.help) return 0;
    if (neutral.config_report || neutral.keys_report)
        return report_mode(argc, argv, &neutral);

    /* Bring the logger and its file mirror up before the recomp module is
     * attached. First boot builds that module, and a build that fails is
     * exactly the report we need a file for - opening the sink inside
     * openpete_run() would be too late to record it. Format and file are
     * device-creation knobs: the game-neutral tiers decide them. */
    openpete_log_init(neutral.log_format, neutral.log_level_spec
                                              ? neutral.log_level_spec
                                              : openpete_config_env("OPENPETE_LOG"));
    if (openpete_log_file_open(neutral.log_file ? neutral.log_file
                                                : openpete_config_env("OPENPETE_LOG_FILE")) == 0)
        OPL_INFO_F("log", "log file: %s (build %s %s)",
                   openpete_log_file_path(), __DATE__, __TIME__);

    /* Build-time mod compile (--compile-mods): compile every mod in the
     * resolved mods dir against <exe>/sdk and exit - the cmake
     * `spyro1_mods` step for native presets; also pre-warms a dist
     * install's compile-on-boot cache. Runs before any game is identified,
     * so its settings are the game-neutral tiers, materialized once. */
    if (neutral.compile_mods) {
        openpete_ident_t none = {};
        openpete_options_t opts;
        rc = parse_options(&opts, argc, argv, &none);
        if (rc != OPENPETE_EXIT_OK) return rc;
        return openpete_mods_compile_all(&opts) == 0 ? 0 : 1;
    }

    if (neutral.extract_recomp_inputs_dir || neutral.dump_sfx_pack_dir)
        return batch_disc_mode(argc, argv, &neutral);

    openpete_options_t opts;
    int exit_now = 0;
    rc = start_session(argc, argv, &neutral, &opts, &exit_now);
    if (rc != OPENPETE_EXIT_OK || exit_now) {
        openpete_launcher_close();
        return rc;
    }

    /* Register the interp ring's region table. Must
     * happen before openpete_run() so openpete_interp_init() (called
     * from inside the run loop) sees the regions. Explicit registration
     * instead of the weak-symbol pattern used by openpete_state_regions
     * - MinGW's PE/COFF linker mis-resolves weak overrides in this
     * configuration. */
    game_interp_register();
    /* Same explicit-registration rationale as game_interp_register()
     * (see comment above): the state-region table moved off the
     * weak-symbol pattern too. */
    game_state_regions_register();
    /* The disc cache loader's boot-order profile (games/common/
     * disc_boot_order.c); before openpete_run() attaches the cache. */
    game_disc_boot_order_register();

    /* High-FPS spine: register the sub-tick re-feed orchestrator with the
     * native renderer so intermediate presents rebuild the scene against an
     * interpolated camera. Inert unless --renderer vk + --render-fps
     * exceeds the sim rate. */
    game_reextract_init();

    return openpete_run(&opts);
}
