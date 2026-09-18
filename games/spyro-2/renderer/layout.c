// games/spyro-2/renderer/layout.c, the Spyro 2 engine layout. Bases come from
// the drift-checked symbol aliases (manifest/spyro2_symbols.toml, each row
// carrying its evidence). Every offset is a literal whose comment names the
// asm site in games/spyro-2/spyro-2/asm that reads or writes it. A field with
// no located counterpart is 0, and the feeder that reads one refuses to run
// rather than read through address 0 (core/include/game_layout.h).
//
// Filled: the environment header, the terrain decode and binning switches, the
// texture tables, the environment animations, the visibility verdict, the fog
// terms, the draw buffers, the primitive allocator, the camera, the level id
// the world-draw, sky-draw and visibility hooks and the cyclorama header. The
// moby, shadow, particle, portal and player-draw groups are still 0.

#include "game_layout.h"
#include "renderer/ui_game.h"
#include "spyro2_symbols.gen.h"

const game_layout_t game_layout = {
    .sin_table = G_SIN,      /* 256 s16 1.12 entries; func_8001B61C interpolates
                              * it from a 12-bit angle (stdutil.s:308-318) and
                              * func_8001B6C8 pairs it with the cos table
                              * D_80061C58 at +0x80 (stdutil.s:375-378) */
    .env = {
        .base            = G_ENVIRONMENT,
        .sector_ptr      = G_ENVIRONMENT + 0x00u,   /* world draw: lw $s3, 0x0 (environment.s:1594) */
        .sector_count    = G_ENVIRONMENT + 0x04u,   /* world draw: lw $v0, 0x4 (environment.s:1593) */
        .occ_groups      = G_ENVIRONMENT + 0x08u,   /* func_80048D18(0): lw $t6, 0x8 (mobyfunc.s:29) */
        .occ_group_count = 0u,                      /* no count word; the occlusion data carries its own header */
        .lq_tex          = G_ENVIRONMENT + 0x20u,   /* world draw: lw $s3, 0x20 (environment.s:2212); one
                                                     * table of 48-byte entries serves LQ and HQ tiles */
        .hq_tex          = G_ENVIRONMENT + 0x20u,   /* no separate HQ table: the four HQ sub-tiles sit
                                                     * inline at +0x10 of the same 48-byte entry, so the
                                                     * near subdivide reads the LQ table with
                                                     * terrain.hq_stride / terrain.hq_sub0 */
        .tex_count       = G_ENVIRONMENT + 0x24u,   /* the tile count the loader writes beside the table
                                                     * pointer; read by the texture census only */
        .lod_dist        = 0u,                      /* no header field: the draw bins LP/HP with the
                                                     * constants below */
        .cull_dist       = G_ENVIRONMENT + 0x2Cu,   /* the LP far cull, (v >> 7) - 1 against the LP OT
                                                     * key (environment.s:6924-6935) */
        .vis_aspect_independent = 1u,               /* func_80048D18 (mobyfunc.s:13-40) is a PVS lookup
                                                     * keyed on the camera position alone: it reads the
                                                     * Draw-time position copy shifted right by 11 and
                                                     * walks the byte-packed octree at
                                                     * g_Environment+0x08, with no matrix, frustum,
                                                     * aspect or FOV term */
    },
    /* The environment animations live in one inline-word block reached through
     * the word at g_Environment+0x1C. Four routines walk it, each entering with
     * `lw $t5, 0x1C($at)`, $at = g_Environment:
     *   func_80023778  blk+0x08              writes the sector colour array,
     *                                        skipping an entry with bit 31 set
     *                                        (environment.s:1236-1271)
     *   func_800238DC  blk+0x08              takes a sector index and a flag and
     *                                        toggles bit 31 of the matching
     *                                        entry (environment.s:1335-1352)
     *   func_80023994  blk+rd(blk+0x00)      writes the sector vertex bank from
     *                                        the cos table (environment.s:1390-1408)
     *   func_80023A9C  blk+rd(blk+0x04)      writes the same bank from the sin
     *                                        table (environment.s:1465-1483)
     * All four gate on the liveness byte table D_ENV_ANIM_LIVE, indexed by the
     * entry's low byte. The three `sub[]` rows are the three that rewrite
     * geometry or colour; func_800238DC only flips a skip bit in the list
     * sub[0] already walks, so it needs no row.
     *
     * The scrolling-texture animations live in a second registry, seven
     * {count, pointer to a descriptor-pointer array} pairs at +0x00 through
     * +0x34, walked in order by func_800225F8 and func_800231F4. Pair 0 memcpys
     * whole 48-byte texture entries between tiles, a discrete swap the tile
     * generation counter already snaps. Pair 1 is the UV scroll: a descriptor
     * holds a u16 tile index at +4 and a u16 accumulator at +6, and on a
     * keyframe advance the walker adds the keyframe's signed delta, masks the
     * accumulator to 7 bits and commits whole texels into the 48-byte tiledef
     * entry, acc>>2 and that +31 into the V bytes of both LQ halves (+1/+5,
     * +9/+13) and acc>>1 with its +31 into the HQ sub-tiles
     * (environment.s:138-155). `scroll` therefore names pair 1. */
    .env_anim = {
        .form       = ENV_ANIM_ARENA,
        .base       = D_ENV_ANIM_REGISTRY,
        .scroll     = D_ENV_ANIM_REGISTRY + 0x08u,
        .block_ptr  = G_ENVIRONMENT + 0x1Cu,
        .live_table = D_ENV_ANIM_LIVE,
        .sub = {
            { 0x08u, 0, 8u, 1, ENV_ANIM_KIND_COLOUR },
            { 0x00u, 1, 4u, 0, ENV_ANIM_KIND_VERT },
            { 0x04u, 1, 4u, 0, ENV_ANIM_KIND_VERT },
        },
    },
    .visibility = {
        .base = 0x1F800000u,   /* one byte per sector, written by func_80048D18(0), consumed and
                                * cleared by the world draw's sector loop
                                * (environment.s:1603-1610); the range is general scratch outside
                                * that window */
    },
    .cyc = {
        .base            = G_CYCLORAMA,
        .sector_count    = G_CYCLORAMA + 0x00u,     /* sky draw: lw $ra, 0x0 (cyclorama.s:48); the walk
                                                     * ends when the visibility cursor reaches
                                                     * 0x1F8003C0 + count (cyclorama.s:52,66) */
        .sectors         = G_CYCLORAMA + 0x04u,     /* sky draw: lw $sp, 0x4 (cyclorama.s:49) */
        .occ_group_count = 0u,
        .occ_groups      = G_ENVIRONMENT + 0x0Cu,   /* func_80048D18(1): the argument shifts the header
                                                     * cursor by 4 before lw $t6, 0x8 (mobyfunc.s:19,24) */
        .bg_colour       = G_CYCLORAMA + 0x08u,     /* 0x00BBGGRR; the moby depth cue splits the same word
                                                     * into three 0xFF0 far-colour fields (mobydraw.s:2597-2604) */
        .vertex_base     = 0x14u,                   /* sky draw: addi $t4, $gp, 0x14 (cyclorama.s:89) */
        .face_layout     = CYC_FACES_SPYRO3,        /* vertex count = byte 0 of +0x0C (andi 0xFF,
                                                     * cyclorama.s:91), colour count = byte 1 (srl 6,
                                                     * andi 0x3FC, cyclorama.s:157-158); +0x10 = face-word
                                                     * list length << 16 | index-halfword list length
                                                     * (cyclorama.s:156,161,164); strips */
        .visibility      = 0x1F8003C0u,             /* one byte per sky sector, func_80048D18(1): 0xFF drawn,
                                                     * 0 skipped, 0x40 bytes zeroed first
                                                     * (cyclorama.s:50-51; mobyfunc.s:19,23-24,63,77) */
        .hook_args       = 0,                       /* the sky draw loads the camera +0x00 matrix for both
                                                     * the cull and the projection and zeroes the
                                                     * translation registers (cyclorama.s:30-45) */
        .fade_in_draw    = 1,                       /* DPCS over the colour table when the camera +0x40 word
                                                     * is nonzero, with the environment header's three
                                                     * far-colour words as FC (cyclorama.s:37,171,181-188) */
    },
    .fog = {
        .factor     = G_CAMERA + 0x40u,             /* the sky draw's IR0, loaded beside the
                                                     * projection matrix (cyclorama.s:39) */
        .far_colour = G_ENVIRONMENT + 0x34u,        /* three words R, G, B loaded into ctc2 $21..$23
                                                     * (environment.s:2479-2484), not a packed
                                                     * 0x00BBGGRR word */
        .far_colour_words = 1,
    },
    .db = {
        .cur_ptr = G_CUR_DB,
        .base    = G_DB,
        .stride  = 0x74u,                           /* func_8004D810 flips between G_DB and G_DB+0x74
                                                     * (drawutil/func_8004D810.s:46-52) */
        .drawenv = 0x00u,                           /* DRAWENV at +0x00; the primitive buffer base the
                                                     * cursor is reset from sits at +0x70
                                                     * (drawutil/func_8004D810.s:57) */
    },
    .polybuf = {
        .cursor = D_POLYBUF_CURSOR,
        .end    = D_POLYBUF_END,
    },
    .cam = {
        .base        = G_CAMERA,
        .proj_matrix = G_CAMERA + 0x00u,            /* sky draw: ctc2 $0..$4 from +0x00..+0x10
                                                     * (cyclorama.s:32-40) */
        .view_matrix = G_CAMERA + 0x14u,            /* world draw: ctc2 $0..$4 from +0x14..+0x24
                                                     * (environment.s:1572-1581) */
        .position    = G_CAMERA + 0x28u,            /* world draw: lw $s7/$t8/$t9 from +0x28..+0x30,
                                                     * each shifted right 4 for the sector cull
                                                     * (environment.s:1585-1588) */
        .rotation    = G_CAMERA + 0x44u,            /* roll +0x44, pitch +0x46, yaw +0x48 */
        .xform_end   = G_CAMERA + 0x34u,            /* the Draw-time position copy the visibility
                                                     * pre-pass reads shifted right 11 (mobyfunc.s:32-39) */
    },
    .level_id = G_LEVELID,
    .terrain = {
        .radius_mask  = 0xFFFu,                     /* world draw: andi $t2, $a0, 0xFFF (environment.s:1624) */
        .flags_mask   = 0xF000u,                    /* world draw: andi $t1, $a0, 0xF000 (environment.s:1623) */
        .flat_flag    = 0x1000u,                    /* bit 12 of the header flags selects the flat HP
                                                     * list; the binning tightens on andi 0x5000
                                                     * (environment.s:1711) */
        .vz_shl       = 1,                          /* Z4 = (v << 3) & 0x1FFC in quarter units */
        .vz_flat_shr  = 2,                          /* flat list: Z4 = v & 0x3FF */
        .uv_ori_table = 0u,                         /* none: GT3 UVs rotate by face word 3 bits 12-13 */
        .defer_q      = { 0u, 0u },                 /* the near lists live in the scratchpad; not read */
        .face_layout  = TERRAIN_FACES_SPYRO3,
        .tex_stride   = 48u,                        /* the tile index times (1 << 5) + (1 << 4)
                                                     * (environment.s:2674-2677) */
        .hq_stride    = 48u,                        /* the HQ index (face word3 & 0x7F) times 48 into
                                                     * the same texture table (environment.s:5192-5196) */
        .hq_sub0      = 0x10u,                      /* the entry pointer is bumped 8 in the loop's
                                                     * delay slot and the sub-quad tile read at
                                                     * +0x8/+0xC of it, so the four quadrant halves
                                                     * are +0x10, +0x18, +0x20, +0x28; quadrant order
                                                     * TL, TR, BL, BR over the 3x3 subdivide grid */
        .abe_in_tpage = 1,
        .tile_word1_br = 1,                         /* GT4 emit: uv1 = word1 - 0x1F00, uv3 = word1
                                                     * (environment.s:2680,2669-2671) */
        .lod_dist_default = 0x8000u,                /* unused: the draw bins with the constants below */
        /* The per-sector list binning, environment.s:1686-1720. */
        .no_lp_flag    = 0x2000u,                   /* 1687: andi 0x2000 skips the LP block */
        .no_hp_flag    = 0x4000u,                   /* 1716: andi 0x4000 emits no HP entry */
        .force_lp_flag = 0u,                        /* the LP list is distance-gated only */
        .lp_near_shift_flag = 0x8000u,              /* 1690: andi 0x8000 */
        .lp_near_shift = 0x600,                     /* 1693: addi 0x600 */
        .hp_far        = 0x7C0,                     /* 1709: addi -0x7C0, bgez skips HP */
        .lp_near       = 0x760,                     /* 1692: addi -0x760, blez skips LP */
        .hp_far_tight  = 0x600,                     /* 1713/1714: addi -0x100 then -0x500 */
        .tight_flags   = 0x5000u,                   /* 1711: andi 0x5000 */
        .sector_cull   = TERRAIN_SECTOR_CULL_SPHERE,
        /* The HP defer classifier, environment.s:2621-2630: the four corner
         * SZ3s summed against 0x2000 and each corner against 0x140, with no
         * face-word bits in the test. */
        .near_band       = 0x2000,
        .near_corner     = 0x140,
        .near_flag_excl  = 0u,
        .near_flag_tier2 = 0u,
        .lp_base_slots   = 0x40,  /* the ordinary insert at environment.s:4302-4307
                                   * is (SZ sum >> 7) + (face bits 0x380 >> 5)
                                   * slots with no base term. The term is kept
                                   * because it compensates another unmeasured
                                   * term of the same insert: the base and the
                                   * bias multiplier move together or not at
                                   * all. */
        .lp_bias_slots   = 8,
        .hp_bias_slots   = 4,
        .nearband_ot_keys = 1,    /* the faces the near/HQ classifier selects key
                                   * through that same ordinary insert
                                   * (environment.s:2674-2679) */
        .slot_ot_contest  = 1,    /* the scene is submitted through one ordering
                                   * table, and a later insert into a bucket
                                   * paints later, with the moby pass run before
                                   * the terrain pass */
    },
    /* The player state block opens with the world position, three int32. Only
     * `base` and `pos_off` are located: the player feeder needs the part
     * records and the draw hook as well, so it still refuses to run, and the
     * position probe (game_get_player_position) reads these two alone. */
    .player = {
        .base    = G_SPYRO,
        .pos_off = 0x00u,
    },
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
        .visibility = F_VISIBILITY,   /* called by both draws before their walks */
    },
};

/* No 2D UI feeder: the ordering-table chokepoint and the fade emitter are not
 * located, so games/common/renderer/ui_sprite.c registers no hook. */
const ui_game_t ui_game = { 0 };
