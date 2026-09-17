/* layout_none.c — the empty engine layout for a game whose RAM addresses are
 * not located yet. Every field is 0, so each feeder that reads one refuses to
 * run rather than read through address 0. See core/include/game_layout.h. */

#include "game_layout.h"
#include "renderer/ui_game.h"

const game_layout_t game_layout = { 0 };

/* No 2D UI feeder: the ordering-table chokepoint and the fade emitter are not
 * located, so games/common/renderer/ui_sprite.c registers no hook. */
const ui_game_t ui_game = { 0 };
