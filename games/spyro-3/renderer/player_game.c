/* games/spyro-3/renderer/player_game.c - the two tables Spyro 3's player draw
 * reaches through a lui/addiu pair of its own. The provider itself, and every
 * other engine address it reads, are
 * games/common/renderer/player_game_spyro23.c and game_layout.
 *
 * Each address is the literal the draw's own pair forms, cited at the
 * asm/spyrodraw.s line that forms it. */
#include "renderer/player_game_spyro23.h"
#include "spyro3_symbols.gen.h"

const player_game_s23_consts_t spyro3_player_game_s23 = {
    .anim_table  = D_SPYRO_ANIMS,    /* spyrodraw.s:33-34, dereferenced at :35 */
    .blend_table = D_SPYRO_BLEND,    /* spyrodraw.s:39-40, indexed by g_Spyro+0x1C */
};
