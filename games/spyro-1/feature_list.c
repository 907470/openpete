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
extern const feature_module_t cyclorama_native_module;
extern const feature_module_t moby_native_module;
extern const feature_module_t spyro_native_module;

const feature_module_t* const openpete_features[] = {
    &widescreen_module,
    &no_lod_module,
    /* Phase 2 native-renderer cyclorama spike + A/B verify harness.
     * Hooks 0x8004EBA8 only when OPENPETE_CYC_VERIFY is set; otherwise an
     * inert fall-through. Registered after widescreen so its cyclorama-360
     * arm keeps first dibs at wide aspect (it defers at native 4:3). */
    &cyclorama_native_module,
    /* Phase 3 native-renderer moby spike + A/B verify harness. Hooks the
     * REGULAR material renderer 0x8001F798 only when OPENPETE_MOBY_VERIFY or
     * a native renderer mode (--renderer vk|ab|diff) is set; inert otherwise. */
    &moby_native_module,
    /* Phase 3b.4 native-renderer Spyro spike + A/B verify harness. Hooks the
     * player-avatar renderer 0x80023AC4 only when OPENPETE_SPYRO_VERIFY or
     * a native renderer mode (--renderer vk|ab|diff) is set; inert otherwise. */
    &spyro_native_module,
};

const size_t openpete_feature_count =
    sizeof(openpete_features) / sizeof(openpete_features[0]);
