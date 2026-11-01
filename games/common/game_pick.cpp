// games/common/game_pick.cpp - the launcher's pick of the game before the
// final install, and each linked game's launcher entry. See game_pick.h.

#include "game_pick.h"

#include "openpete.h"
#include "openpete_fatal.h"
#include "openpete_launcher.h"
#include "openpete_log.h"
#include "openpete_config.h"
#include "recomp_attach.h"
#include "launcher_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/** @brief Bring a per-game entry's launcher asset cache up to date for a library disc (launcher_cache_update).
 *
 * @param g The per-game entry; its game is installed.
 * @param disc_path The library entry's disc.
 * @param disc_sha256 The disc's recorded content hash.
 * @param context What started the job.
 * @return launcher_cache_update's status. */
static int entry_assets(const openpete_launcher_game* g, const char* disc_path, const char* disc_sha256,
                        const char* context) OP_REQUIRES(ingest) {
    return launcher_cache_update(static_cast<const openpete_game_build_t*>(g->build), disc_path, disc_sha256,
                                 context);
}

/** @brief Read a per-game entry's glyph models from its launcher asset cache (launcher_cache_title_load).
 *
 * @param g The per-game entry.
 * @return 0 when ready, -1 when the cache holds none. */
static int entry_title_load(const openpete_launcher_game* g) {
    return launcher_cache_title_load(static_cast<const openpete_game_build_t*>(g->build));
}

/** @brief Emit a string as a per-game entry's 3D title (launcher_cache_title_emit).
 *
 * @param g The per-game entry.
 * @param text The string.
 * @param t The placement.
 * @param out The openpete_hud_vert buffer.
 * @param cap Its capacity, in vertices.
 * @param cap_h Receives the cap height.
 * @param glyphs Receives the posed glyphs, or NULL.
 * @param glyph_cap The records @p glyphs holds.
 * @return The vertex count. */
static int entry_title_emit(const openpete_launcher_game* g, const char* text, const openpete_launcher_title* t,
                            void* out, int cap, float* cap_h, openpete_launcher_title_glyph* glyphs, int glyph_cap) {
    return launcher_cache_title_emit(static_cast<const openpete_game_build_t*>(g->build), text, t, out, cap, cap_h,
                                     glyphs, glyph_cap);
}

namespace {

/** @brief Return the display name of a linked game, the version selector's prefix of an entry recorded for it.
 *
 * @param id A game id.
 * @return The name, or NULL when no linked game has that id. */
const char* linked_game_name(const char* id) {
    const openpete_game_build_t* b = openpete_game_build_linked(id);
    if (!b || !b->runtime || !b->runtime->identity) return nullptr;
    return b->runtime->identity->display_name;
}

/** @brief Return the build record a per-game launcher entry carries.
 *
 * @param g The entry.
 * @return The build. */
const openpete_game_build_t* build_of(const openpete_launcher_game* g) {
    return static_cast<const openpete_game_build_t*>(g->build);
}

/** @brief The launcher's module path of a library entry for the entry's game (openpete_game_library_module_path).
 *
 * @param g The per-game entry.
 * @param entry_dir The library entry's directory.
 * @param out Receives the path.
 * @param cap Its capacity.
 * @return 0, or -1 when the game loads no module from the library. */
int entry_module_path(const openpete_launcher_game* g, const char* entry_dir, char* out, size_t cap) {
    return openpete_game_library_module_path(build_of(g), entry_dir, out, cap);
}

/** @brief Install a per-game entry's game for the launcher jobs about to run.
 *
 * @param g The per-game entry. */
void entry_install(const openpete_launcher_game* g) {
    openpete_game_build_install(build_of(g));
}

/** @brief Build a per-game entry's module from a disc, announcing the build as the bind does (openpete_recomp_announce_build).
 *
 * @param g The per-game entry; its game is installed.
 * @param disc_path The disc.
 * @param out_module_path Where the module is written.
 * @return The build's status: 0, -2 when stopped, -1 on failure. */
int entry_build(const openpete_launcher_game* g, const char* disc_path, const char* out_module_path) {
    openpete_recomp_announce_build("is not there");
    return build_of(g)->build_module(disc_path, out_module_path);
}

/** @brief Return the per-game launcher entries, one per linked game, in link order.
 *
 * @return The entries, built on first call. */
std::vector<openpete_launcher_game>& game_entries() {
    static std::vector<openpete_launcher_game> entries;
    if (!entries.empty()) return entries;
    entries.resize(openpete_game_build_count);
    for (size_t i = 0; i < openpete_game_build_count; i++) {
        const openpete_game_build_t* b = openpete_game_builds[i];
        openpete_launcher_game& g = entries[i];
        g.game_id = b->id;
        const openpete_game_runtime_t* rt = b->runtime;
        g.display_name = rt && rt->identity ? rt->identity->display_name : nullptr;
        g.known_discs = rt ? rt->known_discs : nullptr;
        g.known_disc_count = rt ? rt->known_disc_count : 0;
        g.module_path = entry_module_path;
        g.build = b;
        g.install = entry_install;
        g.build_module = b->build_module ? entry_build : nullptr;
        if (launcher_cache_game_has_assets(b)) {
            g.update_assets = entry_assets;
            g.title_load = entry_title_load;
            g.title_emit = entry_title_emit;
        }
    }
    return entries;
}

/** @brief The launcher's view before a game is installed: no game id (every entry listed), no name, the known discs of every linked game, no module path, and the per-game entries.
 *
 * @return The descriptor, built on first call. */
const openpete_launcher_game* neutral_desc() {
    static openpete_launcher_game g;
    static std::vector<openpete_launcher_known_disc> discs;
    static std::vector<const openpete_launcher_game*> games;
    if (games.empty()) {
        for (auto& e : game_entries()) games.push_back(&e);
        g.games = games.data();
        g.game_count = (int)games.size();
        for (size_t i = 0; i < openpete_game_build_count; i++) {
            const openpete_game_runtime_t* rt = openpete_game_builds[i]->runtime;
            for (int k = 0; rt && k < rt->known_disc_count; k++)
                discs.push_back(rt->known_discs[k]);
        }
        g.known_discs = discs.empty() ? nullptr : discs.data();
        g.known_disc_count = (int)discs.size();
        g.game_name = linked_game_name;
    }
    return &g;
}

/** @brief Report whether an environment variable is set to a non-empty value.
 *
 * @param name The variable.
 * @return true when set and non-empty. */
bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

/** @brief Return the linked game the command line's --game names, read from argv before any parse.
 *
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The game, or NULL when --game is absent or names no linked game. */
const openpete_game_build_t* game_flag(int argc, char** argv) {
    const char* v = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) v = argv[i + 1];
        else if (!strncmp(argv[i], "--game=", 7)) v = argv[i] + 7;
    }
    return openpete_game_build_linked(v);
}

/** @brief Report whether the options ask for a mode a game's entry answers without a launcher.
 *
 * @param o The game-neutral options.
 * @return true for --config-report, --keys-report, --compile-mods,
 *         --extract-recomp-inputs and --dump-sfx-pack. */
bool entry_mode(const openpete_options_t& o) {
    return o.config_report || o.keys_report || o.compile_mods ||
           o.extract_recomp_inputs_dir || o.dump_sfx_pack_dir;
}

}  // namespace

const openpete_launcher_game* openpete_game_launcher_entry(const openpete_game_build_t* b) {
    for (auto& e : game_entries())
        if (e.build == b) return &e;
    return nullptr;
}

const openpete_game_build_t* openpete_game_pick(int argc, char** argv, int* exit_code) {
    const openpete_game_build_t* first = openpete_game_builds[0];
    /* The game a mode that runs without the launcher answers for: --game's
     * when it names a linked game, else the first linked game. */
    const openpete_game_build_t* named = game_flag(argc, argv);
    const openpete_game_build_t* mode_game = named ? named : first;
    *exit_code = 0;
    /* The surface and tooltip dumps run in the entry before any parse. */
    if (env_set("OPENPETE_SURFACE_DUMP") || env_set("OPENPETE_TOOLTIP_VOCAB")) return mode_game;

    /* The launcher keeps a pointer to the options: static storage. */
    static openpete_options_t neutral;
    openpete_options_init(&neutral);
    const int rc = openpete_options_parse_neutral(&neutral, argc, argv);
    if (rc != OPENPETE_EXIT_OK) {
        /* A launch with no arguments (a double-click) fails only on the
         * config file or the environment; only it shows the box. */
        openpete_fatal_set_interactive(!neutral.headless && argc <= 1);
        openpete_fatal_message(OP_E_CONFIG, "OpenPete could not start",
                               "A setting in openpete.toml (or an OPENPETE_ "
                               "environment variable) was not accepted.");
        *exit_code = rc;
        return nullptr;
    }
    if (neutral.help) return nullptr;
    if (entry_mode(neutral)) return mode_game;

    openpete_fatal_set_interactive(!neutral.headless);
    openpete_log_init(neutral.log_format, neutral.log_level_spec
                                              ? neutral.log_level_spec
                                              : openpete_config_env("OPENPETE_LOG"));
    (void)openpete_log_file_open(neutral.log_file ? neutral.log_file
                                                  : openpete_config_env("OPENPETE_LOG_FILE"));

    openpete_launcher_begin(neutral_desc(), &neutral, !neutral.headless);
    if (const int lock = openpete_launcher_lock(); lock != 0) {
        if (lock == -1) {
            OPL_ERROR_F("launcher", "%s", "another OpenPete is preparing a game: the library is "
                        "locked by another process; nothing was done");
            std::fprintf(stderr, "openpete: another OpenPete is preparing a game; try again "
                                 "when it has started\n");
        } else {
            OPL_ERROR_F("launcher", "%s", "the library lock file cannot be taken (the reason is logged "
                        "above); nothing was done");
            std::fprintf(stderr, "openpete: the library folder cannot be locked; see the log\n");
        }
        *exit_code = OPENPETE_EXIT_SETUP_ERROR;
        return nullptr;
    }
    int exit_now = 0;
    const int lrc = openpete_launcher_resolve(&neutral, nullptr, 0, &exit_now);
    if (lrc != OPENPETE_EXIT_OK || exit_now) {
        openpete_launcher_close();
        *exit_code = lrc;
        return nullptr;
    }
    const openpete_game_build_t* b = openpete_game_build_linked(openpete_launcher_selected_game());
    OPL_INFO_F("launcher", "game picked: %s (%s)", b ? b->id : mode_game->id,
               b ? "the booted entry's game"
                 : named ? "no entry booted: the --game game" : "no entry booted: the first linked game");
    return b ? b : mode_game;
}
