// games/common/game_mod_desc_symbols.c - the mod-layer descriptor of a game
// that serves its generated SDK symbol table and no game-specific surface.
//
// Moby ops, level ops, the text rasterizer, the sfx canonicaliser and the
// render-function list stay NULL, so the loader treats those surfaces as
// unsupported on this game; the arena keeps the core default base.

#include "game_mod_desc.h"

const game_mod_desc_t* game_mod_desc(void) {
    static game_mod_desc_t s_desc;
    static int             s_built = 0;
    if (!s_built) {
        s_desc.symbols      = openpete_sdk_symbols;
        s_desc.symbol_count = openpete_sdk_symbol_count;
        s_built = 1;
    }
    return &s_desc;
}
