// games/common/game_pick.cpp - the launcher's pick of the game before any
// game is installed. See game_pick.h.

#include "game_pick.h"

#include "openpete.h"
#include "openpete_fatal.h"
#include "openpete_launcher.h"
#include "openpete_log.h"
#include "openpete_config.h"

#include <cstdlib>
#include <vector>

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

/** @brief The launcher's view before a game is installed: no game id (every entry listed), no name, the known discs of every linked game, no module path.
 *
 * @return The descriptor, built on first call. */
const openpete_launcher_game* neutral_desc() {
    static openpete_launcher_game g;
    static std::vector<openpete_launcher_known_disc> discs;
    if (discs.empty()) {
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

/** @brief Report whether the options ask for a mode the first linked game's entry answers without a launcher.
 *
 * @param o The game-neutral options.
 * @return true for --config-report, --keys-report, --compile-mods,
 *         --extract-recomp-inputs and --dump-sfx-pack. */
bool entry_mode(const openpete_options_t& o) {
    return o.config_report || o.keys_report || o.compile_mods ||
           o.extract_recomp_inputs_dir || o.dump_sfx_pack_dir;
}

}  // namespace

const openpete_game_build_t* openpete_game_pick(int argc, char** argv, int* exit_code) {
    const openpete_game_build_t* first = openpete_game_builds[0];
    *exit_code = 0;
    /* The surface and tooltip dumps run in the entry, for the first game,
     * before any parse. */
    if (env_set("OPENPETE_SURFACE_DUMP") || env_set("OPENPETE_TOOLTIP_VOCAB")) return first;

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
    if (entry_mode(neutral)) return first;

    openpete_fatal_set_interactive(!neutral.headless);
    openpete_log_init(neutral.log_format, neutral.log_level_spec
                                              ? neutral.log_level_spec
                                              : openpete_config_env("OPENPETE_LOG"));
    (void)openpete_log_file_open(neutral.log_file ? neutral.log_file
                                                  : openpete_config_env("OPENPETE_LOG_FILE"));

    openpete_launcher_begin(neutral_desc(), &neutral, !neutral.headless);
    int exit_now = 0;
    const int lrc = openpete_launcher_resolve(&neutral, nullptr, 0, &exit_now);
    if (lrc != OPENPETE_EXIT_OK || exit_now) {
        openpete_launcher_close();
        *exit_code = lrc;
        return nullptr;
    }
    const openpete_game_build_t* b = openpete_game_build_linked(openpete_launcher_selected_game());
    OPL_INFO_F("launcher", "game picked: %s (%s)", b ? b->id : first->id,
               b ? "the booted entry's game" : "no linked game on the entry: the first linked game");
    if (!b) b = first;
    return b;
}
