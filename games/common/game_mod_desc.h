// games/common/game_mod_desc.h - the per-game mod-layer descriptor entry.
//
// Each game defines game_mod_desc() once: Spyro 1 in
// games/spyro-1/mods/mod_game_desc.c, a game serving the symbol table alone
// through games/common/game_mod_desc_symbols.c. The entry point registers
// the result with openpete_mods_set_game_desc after identifying the game.

#ifndef GAME_MOD_DESC_H
#define GAME_MOD_DESC_H

#include "openpete_mods.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return this game's mod-layer descriptor.
 *
 * Built on the first call into static storage; later calls return the same
 * pointer.
 * @return The descriptor, never NULL. */
const game_mod_desc_t* game_mod_desc(void);

/* The generated SDK symbol table (tools/gen_mod_sdk.py,
 * <game build dir>/mod_sdk_gen/sdk_symbols_table.c). */
extern const openpete_sdk_symbol_t openpete_sdk_symbols[];
extern const size_t                openpete_sdk_symbol_count;

/* The overlays with their sorted entry addresses and the overlay window
 * (tools/gen_overlay_entries.py,
 * <game build dir>/mod_sdk_gen/mod_overlay_entries.c). */
extern const game_mod_overlay_t game_mod_overlays[];
extern const size_t             game_mod_overlay_count;
extern const uint32_t           game_mod_window_lo;
extern const uint32_t           game_mod_window_hi;

#ifdef __cplusplus
}
#endif

#endif
