// games/spyro-2/feature_list.c, Spyro 2's registered feature modules.
#include "feature_registry.h"
#include <stddef.h>

extern const feature_module_t render_paths_module;
extern const feature_module_t terrain_module;
extern const feature_module_t cyclorama_module;
extern const feature_module_t shadow_module;
extern const feature_module_t shadow_player_module;
extern const feature_module_t particles_module;
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
    /* The shared moby drop-shadow feeder: hooks the shadow draw from
     * game_layout; inert unless the native scene is enabled or
     * OPENPETE_SHADOW_VERIFY is set. */
    &shadow_module,
    /* The player's own ground shadow, the second descriptor leg of the same
     * feeder: hooks game_layout.shadow.player. */
    &shadow_player_module,
    /* The particle feeder: hooks the particle renderer game_layout.particles
     * names and feeds the primitives it appended, which link into the 3D
     * ordering table and so never reach the 2D chokepoint. */
    &particles_module,
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
