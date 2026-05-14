// games/spyro-1/feature_list.c — Spyro 1's registered feature modules.
//
// Phase 1 of the feature-module consolidation plan
// (notes/plans/openpete-feature-modules.md): registry exists, no
// features registered yet. Phase 2 moves Hor+ widescreen here as the
// first migration.

#include "feature_registry.h"

#include <stddef.h>

extern const feature_module_t widescreen_module;
extern const feature_module_t no_lod_module;

const feature_module_t* const openpete_features[] = {
    &widescreen_module,
    &no_lod_module,
};

const size_t openpete_feature_count =
    sizeof(openpete_features) / sizeof(openpete_features[0]);
