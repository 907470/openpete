/* games/spyro-2/renderer/player_game.c — the two tables Spyro 2's player draw
 * reaches through a lui/addiu pair of its own. The provider itself, and every
 * other engine address it reads, are
 * games/common/renderer/player_game_spyro23.c and game_layout.
 *
 * Each address is the literal the draw's own pair forms, cited at the
 * asm/spyrodraw.s line that forms it. */
#include "renderer/player_game_spyro23.h"
#include "spyro2_symbols.gen.h"

const player_game_s23_consts_t player_game_s23 = {
    .anim_table  = D_SPYRO_ANIMS,    /* spyrodraw.s:35-36, dereferenced at :37 */
    .blend_table = D_SPYRO_BLEND,    /* spyrodraw.s:41-42, indexed by g_Spyro+0x1C */
};
