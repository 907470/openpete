// games/spyro-3/feature_list.c — Spyro 3's registered feature modules.
#include "feature_registry.h"
#include <stddef.h>

extern const feature_module_t terrain_probe_module;

const feature_module_t* const openpete_features[] = {
    /* The calibrated terrain probe: inert unless OPENPETE_TERRAIN_PROBE is set. */
    &terrain_probe_module,
};
const size_t openpete_feature_count = sizeof(openpete_features) / sizeof(openpete_features[0]);
