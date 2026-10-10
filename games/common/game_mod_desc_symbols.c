// games/common/game_mod_desc_symbols.c - the mod-layer descriptor of a game
// that serves its generated SDK symbol table and overlay entry arrays and no
// game-specific surface.
//
// Overlay-window overrides bind through override_ovl or a name scoped to one
// overlay only (legacy_window_links 0). Moby ops, level ops, the text rasterizer, the sfx canonicaliser and the
// render-function list stay NULL, so the loader treats those surfaces as
// unsupported on this game; the arena keeps the core default base.

#include "game_mod_desc.h"

const game_mod_desc_t* game_mod_desc(void) {
    static game_mod_desc_t s_desc;
    static int             s_built = 0;
    if (!s_built) {
        s_desc.symbols      = openpete_sdk_symbols;
        s_desc.symbol_count = openpete_sdk_symbol_count;
        s_desc.overlays      = game_mod_overlays;
        s_desc.overlay_count = game_mod_overlay_count;
        s_desc.window_lo     = game_mod_window_lo;
        s_desc.window_hi     = game_mod_window_hi;
        s_built = 1;
    }
    return &s_desc;
}
