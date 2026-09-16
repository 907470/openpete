/* moby_game_none.c — the moby provider of a game with no moby feeder.
 *
 * Every decoder slot is NULL, so the feeder's apply_options refuses to register
 * a hook and the walk never runs. See renderer/moby_game.h. */
#include "renderer/moby_game.h"

const moby_game_t moby_game = { 0 };
