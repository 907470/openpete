/* games/spyro-2/renderer/moby_game.c — the addresses and the one behavioural
 * switch Spyro 2's moby draw does not share with the other game that runs the
 * same three handwritten routines. The decoders, the cull-record reader, the
 * second model kind's pass and the moby_game provider itself are
 * games/common/renderer/moby_game_spyro23.c; every other engine address they
 * read comes from game_layout.
 *
 * Each address is the literal the draw's own lui/addiu pair forms, cited at
 * the asm/mobydraw.s line that forms it. */
#include "renderer/moby_game_spyro23.h"

const moby_game_s23_consts_t moby_game_s23 = {
    .vertex_adjust_table = 0x80062674u,  /* mobydraw.s:918, XORed with record word-0 bit 0 */
    .anim_blend_table    = 0x80062868u,  /* mobydraw.s:460-462, indexed by Moby+0x40 */
    .simple_skip_lowpoly = 0x80066FA4u,  /* mobydraw.s:34,36, parked in GTE data 3 */
    .simple_flat_coltab  = 0x80064440u,  /* mobydraw.s:4334, 16-byte entries by Moby+0x54 */
    /* The light vector the specular material projects the rotated face normal
     * onto, as the three distinct halves of the four words the setup writes to
     * GTE control 16 through 20 (mobydraw.s:2050-2060). The same vector the
     * other game's setup writes. */
    .spec_light          = { -2848, -2458, -2517 },
    /* Mode 0 loads each vertex's colour word into the GTE source colour and
     * leaves IR0 at the per-moby value the material setup stored, with no
     * per-vertex colour sum (mobydraw.s:2220-2242 against the other game's
     * mtc2 $zero into the source colour and its three-byte sums). */
    .mode0_ir0_from_colour_sum = 0,
};
