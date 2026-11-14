/* games/spyro-3/renderer/moby_game.c - the addresses and the one behavioural
 * switch Spyro 3's moby draw does not share with the other game that runs the
 * same three handwritten routines. The decoders, the cull-record reader, the
 * simple models' pass and the moby_game provider itself are
 * games/common/renderer/moby_game_spyro23.c; every other engine address they
 * read comes from game_layout.
 *
 * Each address is the literal the draw's own lui/addiu pair forms, cited at
 * the asm/mobydraw.s line that forms it. */
#include "renderer/moby_game_spyro23.h"

const moby_game_s23_consts_t spyro3_moby_game_s23 = {
    .vertex_adjust_table = 0x80066274u,  /* mobydraw.s:914, XORed with record word-0 bit 0 */
    .anim_blend_table    = 0x80066530u,  /* mobydraw.s:458-461, indexed by Moby+0x40 */
    .simple_skip_lowpoly = 0x8006C5DCu,  /* mobydraw.s:32,34, parked in GTE data 3 */
    .simple_flat_coltab  = 0x80066998u,  /* mobydraw.s:4353, 16-byte entries by Moby+0x54 */
    /* The light vector the specular material projects the rotated face normal
     * onto, as the three distinct halves of the four words the setup writes to
     * GTE control 16 through 20 (mobydraw.s:2046-2056). */
    .spec_light          = { -2848, -2458, -2517 },
    /* Mode 0 zeroes the source colour for the whole block and derives IR0 per
     * vertex from the sum of that vertex's three colour bytes
     * (mobydraw.s:2216-2260). */
    .mode0_ir0_from_colour_sum = 1,
};
