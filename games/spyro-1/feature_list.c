// games/spyro-1/feature_list.c — Spyro 1's registered feature modules.
//
// Phase 1 of the feature-module consolidation plan
// (notes/plans/openpete-feature-modules.md): registry exists, no
// features registered yet. Phase 2 moves Hor+ widescreen here as the
// first migration.

#include "feature_registry.h"

#include <stddef.h>

const feature_module_t* const openpete_features[] = {
    /* Phase 2: &widescreen_module, */
    NULL,  /* keep array non-empty for portability */
};

const size_t openpete_feature_count = 0u;
