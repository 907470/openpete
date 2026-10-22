// games/spyro-3/feature_list.c - Spyro 3's registered feature modules.
#include "openpete_game_runtime.h"
#include <stddef.h>

extern const feature_module_t render_paths_module;
extern const feature_module_t spyro3_levelwarp_module;
extern const feature_module_t terrain_module;
extern const feature_module_t cyclorama_module;
extern const feature_module_t portal_module;
extern const feature_module_t shadow_module;
extern const feature_module_t shadow_player_module;
extern const feature_module_t particles_module;
extern const feature_module_t player_hide_module;
extern const feature_module_t tracers_module;
extern const feature_module_t glow_module;
extern const feature_module_t glint_module;
extern const feature_module_t zap_module;
extern const feature_module_t moby_module;
extern const feature_module_t player_module;
extern const feature_module_t flame_module;
extern const feature_module_t ui_sprite_module;
extern const feature_module_t text_sprt_module;
extern const feature_module_t terrain_probe_module;
extern const feature_module_t transition_call_module;

static const feature_module_t* const k_modules[] = {
    /* The dev-overlay level warp (M): registers its two rows at
     * apply-options time and writes nothing until one is clicked. */
    &spyro3_levelwarp_module,
    /* The transition queue: drains a queued WarpToLevel / EnterSubArea call
     * at Draw's entry. Inert while the queue is empty. */
    &transition_call_module,
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
    /* --hide-player: tags each particle-pool slot with the owner of the
     * call that allocated it (a game whose layout names no allocator
     * registers no hook) and prints the per-category census. */
    &player_hide_module,
    /* The shared fire-breath feeder: hooks the flame emitter
     * game_layout.flame names and re-derives its additive ribbon from the
     * flame state block; inert unless the native scene is enabled or
     * OPENPETE_FLAME_VERIFY is set. */
    &flame_module,
    /* The tracer feeder: hooks the point-ring ribbon and the indexed mesh,
       both of which link their own primitives into the 3D ordering table. */
    &tracers_module,
    /* The glow-ring feeder: hooks the world draw's call to the glow renderer,
       which chains each ring's draw-mode packet and triangles into one
       ordering-table entry of its own. */
    &glow_module,
    /* The gem-glint feeder: hooks the world draw's call to the glint renderer,
       which chains each star's draw-mode packet and quads, or each far cross's
       lines, into an ordering-table entry of its own. */
    &glint_module,
    /* The jittered-beam feeder: hooks the world draw's call to the ribbon
       renderer, which links each segment's triangles into an ordering-table
       bucket keyed on that segment's own corners. */
    &zap_module,
    /* The shared moby feeder: registers the draw hooks game_layout.moby
     * names, then refuses them because this game supplies no moby_game
     * decoder slots yet - OPENPETE_MOBY_CENSUS prints the reason. */
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
/* The list spyro3_game_runtime carries (game_extras_impl.c). */
const openpete_feature_list_t spyro3_feature_list = {
    k_modules, sizeof(k_modules) / sizeof(k_modules[0])
};
