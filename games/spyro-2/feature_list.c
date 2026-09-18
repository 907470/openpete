// games/spyro-2/feature_list.c, Spyro 2's registered feature modules.
#include "feature_registry.h"
#include <stddef.h>

extern const feature_module_t render_paths_module;
extern const feature_module_t terrain_module;

const feature_module_t* const openpete_features[] = {
    /* --render-paths: the path-isolation selection (apply_options only). */
    &render_paths_module,
    /* The shared terrain feeder: hooks the world draw and the visibility
     * function from game_layout; inert unless the native scene is enabled. */
    &terrain_module,
};
const size_t openpete_feature_count = sizeof(openpete_features) / sizeof(openpete_features[0]);
