/* games/spyro-3/renderer/portal_game.c - the addresses Spyro 3's portal
 * preview feeder reaches. The feeder itself is
 * games/common/renderer/portal_spyro23.c; the contract is
 * renderer/portal_spyro23.h. */
#include "renderer/portal_spyro23.h"
#include "spyro-3/gen/spyro3_symbols.gen.h"

const portal_game_s23_consts_t spyro3_portal_game_s23 = {
    .block   = D_PORTAL_BLOCK, /* written per portal by the pass, read by the leaf */
    .pass    = F_PORTAL_PASS,  /* func_8001EDEC */
    .leaf    = F_PORTAL_LEAF,  /* func_8001C8C8 (cyclorama.s:378) */
    .records = D_PORTALS,      /* the record array pointer the pass indexes */
    .count   = D_PORTALCOUNT,  /* the record count the pass loops over */
    .anim    = 1,              /* the +0x43 opening animation */
    .atan_table  = 0x80065B24u,   /* func_8004E880's lhu table (stdutil.s:256-257) */
    .sqrt_table  = 0x80069F40u,   /* func_8004EDE8's lh table (stdutil.s:645-646) */
    .far_counter = 0x8006C644u,   /* drawutil/func_8001EDEC.s:505-506 */
    .env_draw_ret = 0x8001EC4Cu,  /* drawutil/func_8001EC24.s:13-15: after the world draw's jal */
    .env_wrap_ret = 0x8001E4A8u,  /* draw/func_8001E460.s:22-24: after the wrapper's jal */
};
