// games/spyro-3/renderer/layout.c — the Spyro 3 engine layout. Bases come
// from the drift-checked symbol aliases (manifest/spyro3_symbols.toml, each
// row carrying its evidence); every offset is a literal until the Spyro 3
// typed structs header exists, and its comment names the asm site or dump
// that confirmed it. A field with no located counterpart is 0.

#include "game_layout.h"
#include "spyro3_symbols.gen.h"

const game_layout_t game_layout = {
    .sin_table = G_SIN,     /* g_Sin (psx.map); cos = g_Cos 0x80065920 at +0x80.
                             * asm/stdutil.s func_8004E9E4 (sin) and func_8004EA2C
                             * (cos) `lh` from those two bases; func_8004F6C4 builds
                             * the camera rotation matrix from them with 0x1000 as
                             * the unit, so the entries are s16 1.12 */
    .env = {
        .base            = G_ENVIRONMENT,
        .sector_ptr      = G_ENVIRONMENT + 0x00u,   /* world draw: lw $s3, 0x0 */
        .sector_count    = G_ENVIRONMENT + 0x04u,   /* world draw: lw $v0, 0x4; 138 in the dump */
        .occ_groups      = G_ENVIRONMENT + 0x08u,   /* func_8002D0D8(0): lw $t6, 0x8 */
        .occ_group_count = 0u,                      /* no count word; the occlusion data carries its own header */
        .lq_tex          = G_ENVIRONMENT + 0x20u,   /* world draw: lw $s3, 0x20($at); one table of
                                                     * 48-byte entries serves LQ and HQ tiles */
        .hq_tex          = 0u,                      /* no separate HQ table: the four HQ sub-tiles sit
                                                     * at +0x10 of the LQ entry; the near-subdivide
                                                     * pass is not fed on this game yet */
        .tex_count       = G_ENVIRONMENT + 0x24u,   /* 94 in the dump; max face texture index 93 */
        .lod_dist        = 0u,                      /* no header field: the visibility pre-pass bins
                                                     * LP/HP with constants (terrain.lod_dist_default) */
        .cull_dist       = G_ENVIRONMENT + 0x2Cu,   /* 0x2C000; the LP far cull, (v >> 7) - 1 against
                                                     * the LP OT key */
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
    .terrain = {
        .radius_mask  = 0xFFFu,                     /* world draw: andi $t2, $a0, 0xFFF */
        .flags_mask   = 0xE000u,                    /* bit 14 skips the HP pass as in Spyro 1;
                                                     * bits 13 and 15 unmeasured */
        .flat_flag    = 0x1000u,                    /* environment.s 800225F0: andi 0x5000, bit 12
                                                     * selects the flat HP list */
        .vz_shl       = 1,                          /* Z4 = (v << 3) & 0x1FFC in quarter units */
        .vz_flat_shr  = 2,                          /* flat list: Z4 = v & 0x3FF */
        .uv_ori_table = 0u,                         /* none: GT3 UVs rotate by face word 3 bits 12-13 */
        .defer_q      = { 0u, 0u },                 /* the near lists live in the scratchpad; not read */
        .face_layout  = TERRAIN_FACES_SPYRO3,
        .tex_stride   = 48u,
        .abe_in_tpage = 1,
        .tile_word1_br = 1,                         /* GT4 emit: uv1 = word1 - 0x1F00, uv3 = word1 */
        .lod_dist_default = 0x8000u,                /* Spyro 1's authored value; the Spyro 3 pre-pass
                                                     * constants are not mirrored yet */
    },
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
        .visibility = F_VISIBILITY,   /* called by both draws before their walks */
    },
};
