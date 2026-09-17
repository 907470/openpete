// games/spyro-3/feature_list.c — Spyro 3's registered feature modules.
#include "feature_registry.h"
#include <stddef.h>

extern const feature_module_t render_paths_module;
extern const feature_module_t terrain_module;
extern const feature_module_t cyclorama_module;
extern const feature_module_t portal_module;
extern const feature_module_t shadow_module;
extern const feature_module_t shadow_player_module;
extern const feature_module_t particles_module;
extern const feature_module_t tracers_module;
extern const feature_module_t glow_module;
extern const feature_module_t moby_module;
extern const feature_module_t player_module;
extern const feature_module_t ui_sprite_module;
extern const feature_module_t text_sprt_module;
extern const feature_module_t terrain_probe_module;

const feature_module_t* const openpete_features[] = {
    /* --render-paths: the path-isolation selection (apply_options only). */
    &render_paths_module,
    /* The shared terrain feeder: hooks the world draw and the visibility
     * function from game_layout; inert unless the native scene is enabled. */
    &terrain_module,
    /* The shared cyclorama feeder: hooks the sky draw from game_layout;
     * inert unless the native scene is enabled or OPENPETE_CYC_VERIFY is set. */
    &cyclorama_module,
    /* The portal preview feeder: hooks the portal pass and its leaf; inert
     * unless the native scene is enabled or OPENPETE_PORTAL3_DBG is set. */
    &portal_module,
    /* The shared moby drop-shadow feeder: hooks the shadow draw from
     * game_layout; inert unless the native scene is enabled or
     * OPENPETE_SHADOW_VERIFY is set. */
    &shadow_module,
    &shadow_player_module,
    /* The particle feeder: hooks the particle renderer game_layout.particles
       names and feeds the primitives it appended, which link into the 3D
       ordering table and so never reach the 2D chokepoint. */
    &particles_module,
    /* The tracer feeder: hooks the point-ring ribbon and the indexed mesh,
       both of which link their own primitives into the 3D ordering table. */
    &tracers_module,
    /* The glow-ring feeder: hooks the world draw's call to the glow renderer,
       which chains each ring's draw-mode packet and triangles into one
       ordering-table entry of its own. */
    &glow_module,
    /* The shared moby feeder: registers the draw hooks game_layout.moby
     * names, then refuses them because this game supplies no moby_game
     * decoder slots yet — OPENPETE_MOBY_CENSUS prints the reason. */
    &moby_module,
    /* The shared player-avatar feeder: hooks the player draw game_layout.player
     * names; inert unless the native scene is enabled. */
    &player_module,
    /* The shared 2D UI feeder: hooks the ordering-table chokepoint and the
     * fade emitter ui_game names, feeding the HUD sprites, dialog boxes,
     * separator lines, letterbox bars and screen fades into the native scene's
     * overlay layers; inert unless the native scene is enabled. */
    &ui_sprite_module,
    /* The SPRT text feeder: hooks the two glyph emitters, which link their own
     * primitive chain and so never reach the chokepoint. */
    &text_sprt_module,
    /* The calibrated terrain probe: inert unless OPENPETE_TERRAIN_PROBE is set. */
    &terrain_probe_module,
};
const size_t openpete_feature_count = sizeof(openpete_features) / sizeof(openpete_features[0]);
