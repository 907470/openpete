// games/common/game_mod_desc.h - the generated tables of a game's mod-layer
// descriptor.
//
// Each game builds its game_mod_desc_t in a function its runtime descriptor
// names (openpete_game_runtime_t.mod_desc): Spyro 1 in
// games/spyro-1/mods/mod_game_desc.c, Spyro 2 and 3, which serve the
// symbol table alone, in their game_extras_impl.c. The entry point registers
// the result with openpete_mods_set_game_desc after identifying the game.
//
// The tables are generated per game under the game's tag, so every game's
// tables coexist in one executable: tools/gen_mod_sdk.py writes
// <tag>_sdk_symbols and <tag>_sdk_symbol_count
// (<game build dir>/mod_sdk_gen/sdk_symbols_table.c), and
// tools/gen_overlay_entries.py writes <tag>_mod_overlays,
// <tag>_mod_overlay_count, <tag>_mod_window_lo and <tag>_mod_window_hi
// (<game build dir>/mod_sdk_gen/mod_overlay_entries.c).
#ifndef GAME_MOD_DESC_H
#define GAME_MOD_DESC_H

#include "openpete_mods.h"

/* Declares the generated tables of the game whose tag is `tag` (spyro2). */
#define GAME_MOD_TABLES_DECLARE(tag)                              \
    extern const openpete_sdk_symbol_t tag##_sdk_symbols[];       \
    extern const size_t                tag##_sdk_symbol_count;    \
    extern const game_mod_overlay_t    tag##_mod_overlays[];      \
    extern const size_t                tag##_mod_overlay_count;   \
    extern const uint32_t              tag##_mod_window_lo;       \
    extern const uint32_t              tag##_mod_window_hi

/* Points the symbol-table and overlay fields of the game_mod_desc_t at `d`
 * at the generated tables of the game whose tag is `tag`. */
#define GAME_MOD_TABLES_FILL(d, tag)                       \
    do {                                                   \
        (d)->symbols       = tag##_sdk_symbols;            \
        (d)->symbol_count  = tag##_sdk_symbol_count;       \
        (d)->overlays      = tag##_mod_overlays;           \
        (d)->overlay_count = tag##_mod_overlay_count;      \
        (d)->window_lo     = tag##_mod_window_lo;          \
        (d)->window_hi     = tag##_mod_window_hi;          \
    } while (0)

#endif
