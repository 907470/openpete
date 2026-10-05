/* games/spyro-2/renderer/portal_game.c - the addresses Spyro 2's portal
 * preview feeder reaches. The feeder itself is
 * games/common/renderer/portal_spyro23.c; the contract is
 * renderer/portal_spyro23.h. */
#include "renderer/portal_spyro23.h"
#include "spyro2_symbols.gen.h"

const portal_game_s23_consts_t portal_game_s23 = {
    .block   = D_PORTAL_BLOCK, /* written per portal by the pass, read by the leaf */
    .pass    = F_PORTAL_PASS,  /* func_8004C66C */
    .leaf    = F_PORTAL_LEAF,  /* func_800296D0 (cyclorama.s:382) */
    .records = D_PORTALS,      /* the record array pointer the pass indexes */
    .count   = D_PORTALCOUNT,  /* the record count the pass loops over */
    .anim    = 0,              /* no opening animation: the pass never reads +0x43 */
    .atan_table  = 0x80061E5Cu,   /* func_8001B4B8's lhu table (stdutil.s:267-268) */
    .sqrt_table  = 0x80066478u,   /* func_8001BA20's lh table (stdutil.s:670-671) */
    .far_counter = 0x8006700Cu,   /* drawutil/func_8004C66C.s:359-360 */
};
