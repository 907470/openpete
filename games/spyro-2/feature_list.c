// games/spyro-2/feature_list.c, Spyro 2's registered feature modules.
#include "feature_registry.h"
#include <stddef.h>

extern const feature_module_t render_paths_module;
extern const feature_module_t terrain_module;
extern const feature_module_t cyclorama_module;
extern const feature_module_t moby_module;
extern const feature_module_t player_module;

const feature_module_t* const openpete_features[] = {
    /* --render-paths: the path-isolation selection (apply_options only). */
    &render_paths_module,
    /* The shared terrain feeder: hooks the world draw and the visibility
     * function from game_layout; inert unless the native scene is enabled. */
    &terrain_module,
    /* The shared cyclorama feeder: hooks the sky draw from game_layout;
     * inert unless the native scene is enabled or OPENPETE_CYC_VERIFY is set. */
    &cyclorama_module,
    /* The shared moby feeder: registers the cull and the two draw passes
     * game_layout.moby names and decodes their records through the
     * moby_game_s23 provider; inert unless the native scene is enabled. */
    &moby_module,
    /* The shared player-avatar feeder: hooks the player draw game_layout.player
     * names and decodes its skeleton through the player_game_s23 provider;
     * inert unless the native scene is enabled or OPENPETE_PLAYER_VERIFY is
     * set. */
    &player_module,
};
const size_t openpete_feature_count = sizeof(openpete_features) / sizeof(openpete_features[0]);
