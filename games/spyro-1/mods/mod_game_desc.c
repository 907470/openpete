// games/spyro-1/mods/mod_game_desc.c - Spyro 1's mod-layer descriptor.
//
// Every game-specific surface the mod layer reaches: the generated SDK
// symbol table and overlay entry arrays, the unscoped overlay-window links
// of mods pinned below the api adding override_ovl, the moby and level ops
// (mod_moby_ops.c, mod_level_ops.c), the gold text-moby rasterizer, the sfx alias canonicaliser
// (psyq_hooks.cpp) and the PSX render functions the native renderer does
// not rasterize.

#include "game_mod_desc.h"
#include "renderer/text_moby.h" /* text_moby_native_mod_emit */

const openpete_moby_ops_t*  spyro1_moby_ops(void);
const openpete_level_ops_t* spyro1_level_ops(void);
const char* spyro1_sfx_canon(const char* name);

/* Original PSX render and GPU-submission functions whose rasterized output
 * the native renderer replaces. */
static const char* const k_psx_render_fns[] = {
    "GamestateDraw",                                   /* top-level world draw */
    "DrawOTag", "DrawSync",                            /* OT submit / GPU sync */
    "PutDrawEnv", "SetDefDrawEnv", "SetDrawMode",      /* PsyQ GPU draw state   */
    "TitlescreenDraw", "TitlescreenDrawSprite",        /* titlescreen render    */
    NULL,
};

const game_mod_desc_t* game_mod_desc(void) {
    static game_mod_desc_t s_desc;
    static int             s_built = 0;
    if (!s_built) {
        s_desc.symbols        = openpete_sdk_symbols;
        s_desc.symbol_count   = openpete_sdk_symbol_count;
        s_desc.moby_ops       = spyro1_moby_ops();
        s_desc.level_ops      = spyro1_level_ops();
        s_desc.text_emit      = text_moby_native_mod_emit;
        s_desc.sfx_canon      = spyro1_sfx_canon;
        s_desc.psx_render_fns = k_psx_render_fns;
        s_desc.overlays       = game_mod_overlays;
        s_desc.overlay_count  = game_mod_overlay_count;
        s_desc.window_lo      = game_mod_window_lo;
        s_desc.window_hi      = game_mod_window_hi;
        s_desc.legacy_window_links = 1;
        s_built = 1;
    }
    return &s_desc;
}
