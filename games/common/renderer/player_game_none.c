/* player_game_none.c - the player provider of a game with no player feeder.
 *
 * Every slot is NULL, so the feeder registers no hook and the walk never runs.
 * See renderer/player_game.h. */
#include "renderer/player_game.h"

const player_game_t player_game = { 0 };
