// games/spyro-1/main.cpp - Spyro 1 binary entry point.
//
// Tiny wrapper: parse opts, set the display name, hand off to core's run loop.
// SDL3's SDL_main.h re-#defines main -> SDL_main and supplies the platform
// entry shim (links SDL3::SDL3main); must be included by the TU with main().

#include <SDL3/SDL_main.h>
#include "openpete.h"
#include "openpete_fatal.h"
#include "openpete_config.h"
#include "openpete_identify.h"
#include "openpete_log.h"
#include "openpete_boot_timing.h"
#include "openpete_keymap.h"
#include "game_extras.h"
#include "openpete_mods.h"
#include "openpete_surface.h"
#include "recomp_attach.h"
#include "mods/mod_wad_overlay.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
/* Distribution Windows builds link as a GUI subsystem app (-mwindows) so
 * no console window pops up when an end user double-clicks. But when the
 * exe is launched from a terminal or with redirected output - cmd.exe, a
 * `> log` redirect, or WSL2 interop (which passes inherited *pipe*
 * handles, not a console) - we still want logs to flow. So: attach to a
 * parent console if there is one, then bind the C stdio streams to
 * whatever the standard OS handles actually point at (console, pipe, or
 * file). Harmless no-op for console-subsystem (dev) builds. */
static void reconnect_std(DWORD which, FILE* stream) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    int target = _fileno(stream);

    /* Leave a stream that is ALREADY wired to this handle alone. A
     * console-subsystem build - and any build whose CRT inherited its
     * handles, which is every WSL-interop launch - arrives here with
     * working stdio, and "reconnecting" it is destructive: _open_osfhandle
     * takes a SECOND CRT reference to the same HANDLE, then _dup2 closes
     * the destination fd first, which shuts that one handle for both
     * references. The stream is left pointing at a closed handle and every
     * write is silently dropped. That is what made stdout-based output
     * (--config-report, --keys-report) vanish while stderr-based output
     * (--help, the log) kept working. */
    if (target >= 0 && (HANDLE)_get_osfhandle(target) == h) {
        setvbuf(stream, NULL, _IONBF, 0);
        return;
    }

    int fd = _open_osfhandle((intptr_t)h, _O_TEXT);
    if (fd == -1) return;
    if (fd != target) { _dup2(fd, target); _close(fd); }
    setvbuf(stream, NULL, _IONBF, 0);
}
/** @brief Attaches the process to its parent console and rebinds stdout, stderr and stdin to the OS handles.
 *
 * Windows build: AttachConsole failure (parent has no console, or stdio is a
 * pipe or file) is ignored; each stream is then wired by reconnect_std to
 * whatever its standard handle refers to. */
static void openpete_attach_parent_console(void) {
    AttachConsole(ATTACH_PARENT_PROCESS);   /* ok if it fails (pipe/file) */
    reconnect_std(STD_OUTPUT_HANDLE, stdout);
    reconnect_std(STD_ERROR_HANDLE,  stderr);
    reconnect_std(STD_INPUT_HANDLE,  stdin);
}
#else
/** @brief Does nothing on a non-Windows build, where stdio is already connected. */
static void openpete_attach_parent_console(void) {}
#endif

extern "C" int spyro1_dump_sfx_pack(const char* cue_path, const char* out_dir,
                                    const char* extra_wads);
extern "C" int spyro1_extract_recomp_inputs(const char* cue_path,
                                            const char* out_dir);
extern "C" int spyro1_config_disc_identity(const char* disc_path,
                                           char* serial, size_t serial_cap,
                                           char* sha, size_t sha_cap);
extern "C" int spyro1_library_resolve(openpete_options_t* opts,
                                      char* module_path_out,
                                      size_t module_path_cap);
extern "C" const char* spyro1_library_selected_game(void);
extern "C" void game_state_regions_register(void);
extern "C" void game_disc_boot_order_register(void);
/* Mod SDK name->vaddr table, generated from manifest/symbols.toml by
 * tools/gen_mod_sdk.py into the build dir (sdk_symbols_table.c). */
extern "C" {
extern const openpete_sdk_symbol_t openpete_sdk_symbols[];
extern const size_t                openpete_sdk_symbol_count;
/* Draw-service text rasterizer (renderer/text_moby.c - that header
 * has no C++ guards, so declare the one symbol we need directly). */
int text_moby_native_mod_emit(const char* str, float x, float y,
                              float cap_h, unsigned rgb,
                              void* out_verts, int cap);
/* Moby-type registry game ops (mods/mod_moby_ops.c). */
void spyro1_moby_ops_register(void);
/* Level-slot registry game ops (mods/mod_level_ops.c). */
void spyro1_level_ops_register(void);
}
#include "dist/splash.h"
extern "C" {
extern void (*spyro1_progress_cb)(const char* stage, int done, int total);
}
#include "renderer/interp_regions.h"
#include "renderer/reextract.h"

#include <cstdio>

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

/** @brief Register this game's mod layer: the symbol table, the draw text rasterizer, the content registries' game ops and the WAD overlay.
 *
 * Runs once, after the game is identified and before any mod compile or
 * load, so mods override by name ("CameraUpdate") against the identified
 * game's table. The WAD overlay reads the disc's TOC lazily, at the first
 * CdRead.
 * @param disc_path The identified disc. */
static void register_mod_layer(const char* disc_path) {
    openpete_mods_set_symbol_table(openpete_sdk_symbols,
                                   openpete_sdk_symbol_count);
    /* Spyro's 3D gold text-moby glyphs - the same font as the FPS overlay. */
    openpete_mods_draw_set_text_fn(text_moby_native_mod_emit);
    spyro1_moby_ops_register();
    spyro1_level_ops_register();
    spyro1_mod_wad_configure(disc_path);
}

/** @brief Run a batch exit mode that needs the identified disc: --extract-recomp-inputs or --dump-sfx-pack.
 *
 * --dump-sfx-pack takes the library's most recent entry when no disc is
 * named; the splash stays off, since a batch command must not open a window
 * to ask which version to census.
 * @param argc Argument count.
 * @param argv Arguments.
 * @param neutral The game-neutral options.
 * @return The process exit code. */
static int batch_disc_mode(int argc, char** argv, openpete_options_t* neutral) {
    const char* library_game = nullptr;
    if (neutral->dump_sfx_pack_dir && !neutral->disc_path && !neutral->exe_path) {
        spyro1_splash_set_enabled(0);
        (void)spyro1_library_resolve(neutral, nullptr, 0);
        if (neutral->disc_path) library_game = spyro1_library_selected_game();
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

/** @brief Pick the disc through the library and the launcher's splash, then identify the game.
 *
 * --ingest copies an image in; no disc at all boots the most recent entry,
 * or the version selector picks among several; an empty library opens the
 * drop screen. An explicit --disc bypasses the library (dev/CI path). The
 * splash renders the waits (ingest copy), the selector and the drop screen,
 * lazy-opening only when one of them is needed.
 * @param neutral The game-neutral options; receives the library's disc.
 * @param module_path Receives the library entry's module path, "" for none.
 * @param module_cap Its capacity.
 * @param id Receives the identification.
 * @param exit_now Set to 1 when the player closed the drop screen.
 * @return OPENPETE_EXIT_OK, or the exit code after the player was told. */
static int launcher_identify(openpete_options_t* neutral, char* module_path,
                             size_t module_cap, openpete_ident_t* id,
                             int* exit_now) {
    *exit_now = 0;
    const bool named = neutral->disc_path || neutral->exe_path;
    spyro1_splash_set_enabled(!neutral->headless);
    /* Open at the game window's size (an explicit --window carries through)
     * so the vk-mode window adoption needs no resize. */
    spyro1_splash_set_window_size(neutral->window_w, neutral->window_h);
    spyro1_progress_cb = spyro1_splash_progress;

    if (spyro1_library_resolve(neutral, module_path, module_cap) != 0)
        return fatal_start(1, "The game library could not be read.");
    if (!neutral->disc_path && !neutral->exe_path && !neutral->headless) {
        /* Nothing to boot anywhere: first run. DROP mode. */
        if (spyro1_splash_open() == 0) {
            const char* dropped = spyro1_splash_wait_for_disc();
            if (!dropped) { *exit_now = 1; return OPENPETE_EXIT_OK; }   /* window closed */
            neutral->ingest_path = dropped;
            if (spyro1_library_resolve(neutral, module_path, module_cap) != 0)
                return fatal_start(1, "The disc image could not be added to the game library.");
        }
    }
    const char* library_game = (!named && neutral->disc_path)
                               ? spyro1_library_selected_game() : nullptr;
    int rc = identify(neutral, library_game, id);
    if (rc == OPENPETE_EXIT_OK) rc = openpete_identify_require_linked(id, game_get_id());
    if (rc != OPENPETE_EXIT_OK)
        return fatal_start(rc, "The disc is not Spyro the Dragon.");
    return OPENPETE_EXIT_OK;
}

/** @brief Spyro 1 entry point: resolve the game-neutral settings, identify the game, apply its settings once, attach the recomp module, and run the game.
 *
 * @param argc Argument count.
 * @param argv Arguments.
 * @return The process exit code (OPENPETE_EXIT_*, or 1 on a startup failure). */
int main(int argc, char** argv) {
    openpete_boot_mark(OP_BOOT_LAUNCHER_START);
    /* Reconnect stdout/stderr to the parent terminal on a GUI-subsystem
     * (distribution) Windows build; no-op elsewhere. Before any output. */
    openpete_attach_parent_console();

    /* MinGW buffers stderr when it isn't a TTY (true for `>file 2>&1`
     * and for WSL2 interop pipes). abort() - which the PSYQ stubs use to
     * surface unimplemented-surface hits - doesn't flush, so otherwise
     * useful diagnostics get dropped right before exit. Unbuffer once at
     * startup. Linux behaviour is unchanged (line-buffered -> unbuffered
     * is fine for stderr). */
    setvbuf(stderr, NULL, _IONBF, 0);

    /* The [disc.*] scopes' keys for the identified disc: its serial, and
     * the hash its library entry recorded. */
    openpete_config_set_disc_identity_fn(spyro1_config_disc_identity);

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

    static char library_module_path[4096];
    openpete_ident_t id;
    int exit_now = 0;
    rc = launcher_identify(&neutral, library_module_path, sizeof library_module_path,
                           &id, &exit_now);
    if (rc != OPENPETE_EXIT_OK || exit_now) return rc;

    /* The one full resolution: the identified game's and disc's scopes
     * join the tiers, and the environment and argv are written once. */
    openpete_options_t opts;
    rc = parse_options(&opts, argc, argv, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;

    {
        int src_rc = openpete_options_require_exe_source(&opts);
        if (src_rc != OPENPETE_EXIT_OK)
            return fatal_start(src_rc, "No Spyro the Dragon disc image was found.");
    }

    /* The mod layer, once, at identification: before the compile-on-boot
     * pass below and the load inside openpete_run(). */
    register_mod_layer(opts.disc_path);

    /* The runtime reaches recompiled code only
     * through the recomp-module descriptor. Static builds register the
     * linked-in module; shared-linkage builds dlopen spyro1-recomp and
     * resolve the same descriptor. Must precede any dispatch. */
    const char* module_override = opts.recomp_module_path;
    if (!module_override && library_module_path[0])
        module_override = library_module_path;
    /* Auto-building into the module location is fine for the default
     * path and for library entries; an explicit --recomp-module points
     * at a module the user asserts exists. */
    int allow_build = (opts.recomp_module_path == NULL);
    if (spyro1_recomp_attach(module_override, opts.disc_path, allow_build) != 0)
        return fatal_start(1, "The game code built from the disc could not be loaded or built.");

    /* Mod compile-on-boot: compile any
     * stale enabled source mods behind the splash while it is still
     * alive - run_task lazy-opens the window only if there is real work,
     * so a boot with nothing stale never flashes it. Loading stays in
     * openpete_run(); this pass only warms the build cache. */
    openpete_mods_prepare(&opts, spyro1_splash_progress,
                          spyro1_splash_run_task);

    /* In-process continuation: the splash's job ends where the game
     * window begins. Every windowed run ADOPTS the splash window
     * (native_render retitles + reuses it) so the window never closes and
     * reopens - both renderer modes display through the native window, so
     * both want the handover. Headless closes it. Progress is unhooked
     * either way - nothing may lazy-reopen a splash window mid-game. */
    spyro1_progress_cb = NULL;
    if (!opts.headless)
        opts.adopted_window = spyro1_splash_detach_window();
    else
        spyro1_splash_close();

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
