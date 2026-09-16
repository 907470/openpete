/* layout_none.c — the empty engine layout for a game whose RAM addresses are
 * not located yet. Every field is 0, so each feeder that reads one refuses to
 * run rather than read through address 0. See core/include/game_layout.h. */

#include "game_layout.h"

const game_layout_t game_layout = { 0 };
