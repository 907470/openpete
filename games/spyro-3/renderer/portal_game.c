/* games/spyro-3/renderer/portal_game.c - the addresses Spyro 3's portal
 * preview feeder reaches. The feeder itself is
 * games/common/renderer/portal_spyro23.c; the contract is
 * renderer/portal_spyro23.h. */
#include "renderer/portal_spyro23.h"
#include "spyro3_symbols.gen.h"

const portal_game_s23_consts_t portal_game_s23 = {
    .block   = D_PORTAL_BLOCK, /* written per portal by the pass, read by the leaf */
    .pass    = F_PORTAL_PASS,  /* func_8001EDEC */
    .leaf    = F_PORTAL_LEAF,  /* func_8001C8C8 (cyclorama.s:378) */
    .records = D_PORTALS,      /* the record array pointer the pass indexes */
    .count   = D_PORTALCOUNT,  /* the record count the pass loops over */
    .anim    = 1,              /* the +0x43 opening animation */
};
