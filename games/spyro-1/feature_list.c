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
extern const feature_module_t render_paths_module;
extern const feature_module_t cyclorama_native_module;
extern const feature_module_t moby_native_module;
extern const feature_module_t spyro_native_module;
extern const feature_module_t flame_native_module;
extern const feature_module_t shadow_native_module;
extern const feature_module_t particle_native_module;
extern const feature_module_t terrain_native_module;
extern const feature_module_t ui_sprite_native_module;
extern const feature_module_t portal_native_module;

const feature_module_t* const openpete_features[] = {
    &widescreen_module,
    &no_lod_module,
    /* --render-paths: parse the path-isolation selection at startup (apply_options
     * only; the per-path polybuf brackets live in the native feature modules and
     * the OT-walk filter in psyq_DrawOTag). */
    &render_paths_module,
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
    /* Phase 3b.1 native-renderer flame spike + A/B harness + the first ADDITIVE
     * (ABR mode 1) consumer. Hooks Spyro's flame renderer 0x80058D64 only when
     * OPENPETE_FLAME_VERIFY or a native renderer mode (--renderer vk|ab|diff) is
     * set; inert otherwise. Records the RP_FLAME polybuf range itself (so the
     * render_paths thin bracket for flame is retired — see render_paths.c). */
    &flame_native_module,
    /* Phase 3b.2 native-renderer SHADOWS: moby shadows (0x80059F8C, textured
     * FT3 fan, ABR mode 0) + Spyro silhouette shadow (0x80059A48, untextured
     * G3 fan, ABR mode 0 — the first untextured-alpha consumer). Hooks both
     * only under OPENPETE_SHADOW_VERIFY or a native renderer mode; inert
     * otherwise. Records the RP_SHADOW polybuf range itself (so the
     * render_paths thin brackets for shadow are retired — see render_paths.c). */
    &shadow_native_module,
    /* Phase 3b.3 native-renderer PARTICLES: the particle family — particles
     * (0x800573C8, 7 sprite classes), glows (0x800580F4, additive rings) and
     * sparkles (0x800584C4, twinkle quads). VERDICT-CRITICAL: the .s runs
     * headless (writes m_03 + Sparkle m_Life) and we only mirror geometry.
     * Hooks all three only under OPENPETE_PARTICLE_VERIFY or a native renderer
     * mode; inert otherwise. Records the RP_PARTICLES polybuf range itself (so
     * the render_paths thin bracket on func_800573C8 is retired). */
    &particle_native_module,
    /* Phase 4 native-renderer TERRAIN/world: hooks the env-render dispatch
     * func_8002B9CC (-> func_800258F0 world renderer) only under
     * OPENPETE_TERRAIN_VERIFY or a native renderer mode; inert otherwise.
     * Records the RP_TERRAIN polybuf range itself (so the render_paths thin
     * bracket on func_8002B9CC is retired — see render_paths.c). Capture+dump
     * harness now; native gamestate walk + scene feed are the next increment. */
    &terrain_native_module,
    /* 2D-from-OT native UI: hooks func_800168DC (the 2D addPrim chokepoint) to
     * feed the title logo / "PRESS START" / HUD text sprites into the native
     * scene's HUD overlay layer. Inert unless a native renderer mode or
     * OPENPETE_UISPRITE_DUMP is set. */
    &ui_sprite_native_module,
    /* Native PORTAL skybox previews: re-derives the destination-level dome
     * shown inside homeworld portal arches (driven by RenderPortals
     * func_80050BD0 -> func_8004FEA0/80050240/8004F4BC, into a per-portal clip
     * rect). The main-sky feeder (cyclorama_native) only hooks func_8004EBA8,
     * so portal previews were missing. Capture/dump harness under
     * OPENPETE_PORTAL_DUMP now; float extraction + clip + feed are the next
     * increment. Inert unless a native renderer mode or the dump is set. */
    &portal_native_module,
};

const size_t openpete_feature_count =
    sizeof(openpete_features) / sizeof(openpete_features[0]);
