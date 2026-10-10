/* games/common/renderer/player_game_spyro23.c's interface: the player provider
 * shared by the two games whose player draw is one handwritten routine at
 * different addresses.
 *
 * The two routines are instruction-identical over all 1703 instructions: the
 * only encoding that differs in the disassembly is a `lhu $a0, 0($a0)` the
 * two listings render differently from the same word 0x00008494
 * (spyrodraw.s:1196 against spyrodraw.s:1198). Every field offset, literal,
 * scratch carve and GTE op therefore transfers, so the provider is one file
 * and each game supplies only the two tables the draw reaches through a
 * lui/addiu pair of its own. */
#ifndef OPENPETE_COMMON_RENDERER_PLAYER_GAME_SPYRO23_H
#define OPENPETE_COMMON_RENDERER_PLAYER_GAME_SPYRO23_H

#include <stdint.h>
#include "renderer/player_game.h"

/* What the two games do not share. Every other engine address the provider
 * reads comes from game_layout. Each game's renderer/player_game.c defines its
 * instance.
 *
 *   anim_table   pointer to the 256-entry Animation table the draw indexes by
 *                the animation word's low byte (spyrodraw.s:33-35)
 *   blend_table  the 256-byte table the keyframe blend byte is looked up in
 *                before its result is shifted left four into the part
 *                record's IR0 field (spyrodraw.s:39-44) */
typedef struct player_game_s23_consts {
    uint32_t anim_table;
    uint32_t blend_table;
} player_game_s23_consts_t;

/* The running game's table: game_runtime_install (game_host_desc.h) copies
 * the registered descriptor's table here before any feeder runs. */
extern player_game_s23_consts_t player_game_s23;

/* The player provider the two games share, built over player_game_s23. */
extern const player_game_t player_game_spyro23;

#endif /* OPENPETE_COMMON_RENDERER_PLAYER_GAME_SPYRO23_H */
