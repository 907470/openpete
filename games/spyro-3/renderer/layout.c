// games/spyro-3/renderer/layout.c — the Spyro 3 engine layout. Bases come
// from the drift-checked symbol aliases (manifest/spyro3_symbols.toml, each
// row carrying its evidence); every offset is a literal until the Spyro 3
// typed structs header exists, and its comment names the asm site or dump
// that confirmed it. A field with no located counterpart is 0.

#include "game_layout.h"
#include "spyro3_symbols.gen.h"

const game_layout_t game_layout = {
    .env = {
        .base            = G_ENVIRONMENT,
        .sector_ptr      = G_ENVIRONMENT + 0x00u,   /* world draw: lw $s3, 0x0 */
        .sector_count    = G_ENVIRONMENT + 0x04u,   /* world draw: lw $v0, 0x4; 138 in the dump */
        .occ_groups      = G_ENVIRONMENT + 0x08u,   /* func_8002D0D8(0): lw $t6, 0x8 */
        .occ_group_count = 0u,                      /* no count word; the occlusion data carries its own header */
        .lq_tex          = 0u,                      /* candidates +0x1C, +0x20 (pointers), unconfirmed */
        .hq_tex          = 0u,
        .tex_count       = 0u,                      /* candidate +0x24 (94 in the dump), unconfirmed */
        .lod_dist        = 0u,                      /* candidate +0x2C (0x2C000 in the dump), unconfirmed */
        .cull_dist       = 0u,
    },
    .env_anim = { .base = 0u, .scroll = 0u, .lp_poly = 0u, .hp_poly = 0u },  /* not located */
    .visibility = {
        .base = 0x1F800000u,   /* one byte per sector, written by func_8002D0D8(0), consumed and
                                * cleared by the world draw; the range is general scratch outside
                                * that window */
    },
    .cyc = {
        .base            = G_CYCLORAMA,
        .sector_count    = G_CYCLORAMA + 0x00u,     /* sky draw: lw $ra, 0x0; 37 in the dump */
        .sectors         = G_CYCLORAMA + 0x04u,     /* sky draw: lw $sp, 0x4 */
        .occ_group_count = 0u,
        .occ_groups      = G_ENVIRONMENT + 0x0Cu,   /* func_8002D0D8(1): the sky occlusion data */
        .bg_colour       = G_CYCLORAMA + 0x08u,     /* 0x0044BA52 in the dump = the DRAWENV clear colour */
    },
    .fog = {
        .factor     = 0u,                           /* IR0 source not located */
        .far_colour = G_ENVIRONMENT + 0x34u,        /* three words R, G, B loaded into ctc2 $21..$23 by the
                                                     * world draw, not a packed 0x00BBGGRR word */
    },
    .db = {
        .cur_ptr = G_CUR_DB,
        .base    = G_DB,
        .stride  = 0x74u,                           /* func_8001E640: the other buffer is +116 */
        .drawenv = 0x00u,                           /* dump: clip rect and clear colour at +0x00..+0x1B */
    },
    .polybuf = {
        .cursor = D_POLYBUF_CURSOR,
        .end    = D_POLYBUF_END,
    },
    .cam = {
        .base        = G_CAMERA,
        .proj_matrix = G_CAMERA + 0x00u,            /* sky draw: ctc2 $0..$4 from +0x00..+0x10 */
        .view_matrix = G_CAMERA + 0x14u,            /* world draw: ctc2 $0..$4 from +0x14..+0x24 */
        .position    = G_CAMERA + 0x28u,            /* world draw: lw $s7/$t8/$t9 from +0x28..+0x30 */
        .rotation    = G_CAMERA + 0x44u,            /* func_8004F6C4's a0; pitch +0x46, yaw +0x48 */
        .xform_end   = G_CAMERA + 0x34u,            /* +0x34..+0x3C is the Draw-time position copy the
                                                     * visibility computation reads */
    },
    .level_id = G_LEVELID,
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
    },
};
