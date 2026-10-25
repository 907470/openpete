// games/spyro-3/renderer/layout.c - the Spyro 3 engine layout. Bases come
// from the drift-checked symbol aliases (manifest/spyro3_symbols.toml, each
// row carrying its evidence). An offset is `offsetof` against the typed
// structs header (openpete_game_structs.h, pinned to the probed MIPS layout
// by manifest/struct_layout.toml) where the decomp names the member; every
// other offset is a literal whose comment names the asm site or dump that
// confirmed it. A field with no located counterpart is 0.

#include "game_layout.h"
#include "renderer/ui_game.h"
#include "spyro-3/gen/spyro3_symbols.gen.h"
#include "spyro-3/mod_sdk_gen/openpete_game_structs.h"
#include "psx_runtime_internal.h"

#include <stddef.h>

/** @brief Returns the byte offset of the flame's first-cell cap colour set for this tick.
 *
 * The emitter reads the second four-word set of D_80069C38 when currentLevel
 * 0x8006C5BC is 0x1F, else the first (asm/mobyfunc.s:334-341).
 * @param cpu The guest CPU state.
 * @return 0x10 on level 0x1F, else 0. */
static uint32_t flame_cap_colour_off(CPUState* cpu) {
    return cpu->read_word(G_LEVELID) == 0x1Fu ? 0x10u : 0u;
}

/* Moby placements, read off each writer:
 * - class 0x1A2, state 30 -> 4: state 30 hid the record (level_33 0x800806BC
 *   clears +0x4C/+0x4D); level_33 0x800806D0-0x800807F4 copies a source
 *   moby's position into it (func_8004F178) and stores state 4.
 * - class 0x0C7, state 2 -> 3: level_13 0x8007FA64-0x8007FB3C stores state 3
 *   and sets the position to the props base plus (r sin, r cos) at a new
 *   angle. */
static const game_moby_place_t s_moby_place[] = {
    { 0x1A2u, 30u, 4u },
    { 0x0C7u,  2u, 3u },
};

/* func_8003E968 0x8003EDF8-0x8003EE44 copies the moby Spyro+0x250 names to
 * Spyro's hand each tick; the tick the word comes to name a moby places it. */
static const game_moby_director_t s_moby_director[] = {
    { NULL, "g_Spyro", 0u, 0u, 0x250u },
};

const game_layout_t spyro3_game_layout = {
    .display = {
        /* warp.c func_80059038: SetDefDrawEnv(..., 0, 12, 512, 216) paired with
         * SetDefDispEnv(..., 0, 0, 512, 240) - 12 undrawn rows per side. */
        .disp_w = 512, .disp_h = 240, .clip_h = 216,
    },
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
        .hq_tex          = G_ENVIRONMENT + 0x20u,   /* no separate HQ table: the four HQ sub-tiles sit
                                                     * inline at +0x10 of the same 48-byte entry, so
                                                     * the near subdivide reads the LQ table with
                                                     * terrain.hq_stride / terrain.hq_sub0 */
        .tex_count       = G_ENVIRONMENT + 0x24u,   /* 94 in the dump; max face texture index 93 */
        .lod_dist        = 0u,                      /* no header field: the visibility pre-pass bins
                                                     * LP/HP with constants (terrain.lod_dist_default) */
        .cull_dist       = G_ENVIRONMENT + 0x2Cu,   /* 0x2C000; the LP far cull, (v >> 7) - 1 against
                                                     * the LP OT key */
        .vis_aspect_independent = 1u,               /* the verdict comes from the pre-pass
                                                     * func_8002D0D8 (mobyfunc.s:11-150), a PVS lookup
                                                     * keyed on the camera position alone: it reads
                                                     * g_Camera+0x34/0x38/0x3C shifted right by 11 and
                                                     * walks the byte-packed octree at
                                                     * g_Environment+0x08, with no matrix, frustum,
                                                     * aspect or FOV term. The draw's own sphere cull
                                                     * (environment.s:800225A0-80022640) never writes
                                                     * this byte. */
    },
    /* The environment animations live in one inline-word block reached through
     * the word at g_Environment+0x1C, not in Spyro 1's descriptor-pointer
     * lists. The block header is +0x00 the byte offset of sub-list 2, +0x04
     * the byte offset of sub-list 3, +0x08 sub-list 1's count, +0x0C a packed
     * colour the walker loads into ctc2 $21..$23, +0x10 sub-list 1's entries.
     * Three routines walk it, each entering with `lw $t5, 0x1C($at)`,
     * $at = g_Environment:
     *   func_80021FF4  blk+0x08              writes the sector colour array
     *                                        (sector+28+(byte[20]+byte[23])*4)
     *   func_80022158  blk+rd(blk+0x00)      writes the sector vertex bank
     *                                        (sector+28+byte[23]*4)
     *   func_80022260  blk+rd(blk+0x04)      writes the same vertex bank from
     *                                        a sine table, two words per entry
     * Only func_80021FF4 tests bit 31 of the entry (0x80022074, `bltz`); the
     * other two walk every entry. All three gate on the liveness byte table,
     * `lbu` at g_SonyImage+0x263C + (entry & 0xFF), bit 0. None of the three
     * touches a tiledef.
     *
     * The scrolling-texture animations live in a second registry at
     * 0x8006E350, walked by func_80020E74 (called from func_80055294
     * immediately before those three). It is three {count, pointer to a
     * descriptor-pointer array} pairs at +0x00/+0x04, +0x08/+0x0C and
     * +0x10/+0x14, the same record shape as Spyro 1's EnvironmentAnimations,
     * so `scroll` names the second pair and the shared UV-scroll mirror reads
     * it under the ARENA form. A scroll descriptor holds a u16 tile index at
     * +4 and a u16 accumulator at +6; on a keyframe advance
     * (0x80021034-0x800210B8) the walker adds the keyframe's signed int8
     * delta, masks the accumulator to 7 bits and commits whole texels into the
     * 48-byte tiledef entry at (g_Environment+0x20 table) + index*48 -
     * acc>>2 and acc>>2+31 into the V bytes of both LQ halves (+1/+5, +9/+13),
     * acc>>1 and that +31 into the first two HQ sub-tiles (+17/+21, +25/+29),
     * and ((acc>>1)+32)&0x3F with its +31 into the last two (+33/+37,
     * +41/+45). The first pair (+0x00/+0x04) memcpys whole 48-byte entries
     * between tiles, which is a discrete swap the tile-generation counter
     * already snaps. */
    .env_anim = {
        .form       = ENV_ANIM_ARENA,
        /* The scroll registry's base; no symbol names it, and the geometry
         * registry above is a different structure with no header of its own. */
        .base       = 0x8006E350u,
        .swap       = 0x8006E350u + 0x00u,           /* func_80020E74 (80020E80) */
        .scroll     = 0x8006E350u + 0x08u,
        /* The registry func_80020E74 walks: LP geometry, LP colour, HP geometry,
         * HP colour; the geometry walkers read the sector with lbu +4, the
         * colour walkers with lhu +4. */
        .lp_poly    = 0x8006E350u + 0x10u,
        .lp_col     = 0x8006E350u + 0x18u,
        .hp_poly    = 0x8006E350u + 0x20u,
        .hp_col     = 0x8006E350u + 0x28u,
        .poly_sec_u8 = 1,
        .block_ptr  = G_ENVIRONMENT + 0x1Cu,
        .live_table = 0x80072330u,    /* D_8006FCF4 + 0x263C */
        /* Registry pairs 2 and 4. func_80020E74 advances each descriptor's
         * keyframe and stamps its list index into sector+0x18 / +0x1A
         * (80021188, 80021328); func_80022378 packs the posted pose and
         * stores 0xFF there only for a tier it lists (8002265C LP, 80022938
         * HP). */
        .kf_lp_poly = 0x8006E350u + 0x10u,
        .kf_hp_poly = 0x8006E350u + 0x20u,
        .kf_hide    = 1,             /* 800226A0 (LP, ori 0x2000), 8002297C (HP, ori 0x4000) */
        .sub = {
            { 0x08u, 0, 8u, 1, ENV_ANIM_KIND_COLOUR, ENV_ANIM_MODEL_COLOUR },
            { 0x00u, 1, 4u, 0, ENV_ANIM_KIND_VERT,   ENV_ANIM_MODEL_WAVE1 },
            { 0x04u, 1, 4u, 0, ENV_ANIM_KIND_VERT,   ENV_ANIM_MODEL_WAVE2 },
        },
        .wave_table = G_SIN,          /* the walkers' sine table; cosine at +0x80 */
        .timer      = 0x8006C644u,   /* the timer word the walkers read (environment.s:1237, 1334, 1407) */
    },
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
        .vertex_base     = 0x14u,                   /* sky draw: addi $t4, $gp, 0x14 (cyclorama.s:87) */
        .face_layout     = CYC_FACES_SPYRO3,        /* vertex count = byte 0 of +0x0C, colour count =
                                                     * byte 1; +0x10 = face-word list length << 16 |
                                                     * index-halfword list length; strips */
        .visibility      = 0x1F8003C0u,             /* one byte per sky sector, func_8002D0D8(1):
                                                     * 0xFF drawn, 0 skipped (cyclorama.s:63-67) */
        .hook_args       = 0,                       /* the sky draw loads the camera +0x00 matrix
                                                     * for both the cull and the projection,
                                                     * translation zero (cyclorama.s:30-43) */
        .fade_in_draw    = 1,                       /* DPCS over the colour table when the factor
                                                     * word is nonzero (cyclorama.s:161-187) */
    },
    .fog = {
        .factor     = G_CAMERA + 0x40u,             /* the sky draw's IR0 (cyclorama.s:35,177); the world
                                                     * draw computes its own IR0 per entry from a table
                                                     * (environment.s:1298-1303) */
        .far_colour = G_ENVIRONMENT + 0x34u,        /* three words R, G, B loaded into ctc2 $21..$23 by the
                                                     * world and sky draws, not a packed 0x00BBGGRR word */
        .far_colour_words = 1,
    },
    .db = {
        .cur_ptr = G_CUR_DB,
        .base    = G_DB,
        .stride  = 0x74u,                           /* func_8001E640: the other buffer is +116 */
        .drawenv = 0x00u,                           /* dump: clip rect and clear colour at +0x00..+0x1B */
        .repeat_site = {
            /* func_80020168 entry: MoveImage of the screen, then isbg zeroed
             * on both buffers (D_8006FC14, D_8006FC88) after the wait
             * (drawutil/func_80020168.s:10-39). */
            { 0x80020184u, REPEAT_SITE_HOLD },
            /* func_8001EBAC: DrawSync, VSync, ClearImage of both buffers
             * black (drawutil.c:20-33); the caller's next draw has isbg set
             * again (func_80020168 restores it after its loop,
             * drawutil/func_80020168.s:94-98) and clears to the colour the
             * buffer's DRAWENV holds. */
            { 0x8001EBC4u, REPEAT_SITE_CLEAR_DB },
        },
    },
    .polybuf = {
        .cursor = D_POLYBUF_CURSOR,
        .end    = D_POLYBUF_END,
    },
    .cam = {
        .base        = G_CAMERA,
        .proj_matrix = G_CAMERA + 0x00u,            /* sky draw: ctc2 $0..$4 from +0x00..+0x10 */
        .view_matrix = G_CAMERA + 0x14u,            /* world draw: ctc2 $0..$4 from +0x14..+0x24 */
        .position    = G_CAMERA + offsetof(Camera, nextCameraPosCartesian),   /* world draw: lw $s7/$t8/$t9 */
        .rotation    = G_CAMERA + 0x44u,            /* func_8004F6C4's a0; pitch +0x46, yaw +0x48 */
        .xform_end   = G_CAMERA + offsetof(Camera, currentCameraPosCartesian),   /* the Draw-time position
                                                     * copy the visibility computation reads */
    },
    .level_id = G_LEVELID,
    .game_state = G_GAMESTATE,
    .terrain = {
        .radius_mask  = 0xFFFu,                     /* world draw: andi $t2, $a0, 0xFFF */
        .flags_mask   = 0xF000u,                    /* world draw: andi $t1, $a0, 0xF000 */
        .flat_flag    = 0x1000u,                    /* environment.s 800225F0: andi 0x5000, bit 12
                                                     * selects the flat HP list */
        .vz_shl       = 1,                          /* Z4 = (v << 3) & 0x1FFC in quarter units */
        .vz_flat_shr  = 2,                          /* flat list: Z4 = v & 0x3FF */
        .uv_ori_table = 0x80065D08u,                /* D_80065D08: the 8-entry per-corner packed UV
                                                     * delta table the near/HQ 2x2 emit adds
                                                     * to the sub-tile's base UVs
                                                     * (environment.s 80024D18-80024D58 (near/HQ emit) and
                                                     * 80025FD8-80026018 (detail sub-path)) */
        .uv_ori_mask  = 0x38u,                      /* environment.s: `srl 25` then `andi 0x38`;
                                                     * ABE occupies tpage bit 15 here, and the
                                                     * unmasked index would reach past the
                                                     * 64-byte table */
        .defer_q      = { 0u, 0u },                 /* the near lists live in the scratchpad; not read */
        .face_layout  = TERRAIN_FACES_SPYRO3,
        .tex_stride   = 48u,
        .hq_stride    = 48u,                        /* environment.s 80025904-80025914: the HQ index
                                                     * (face word3 & 0x7F) times 48 into the same
                                                     * texture table */
        .hq_sub0      = 0x10u,                      /* environment.s .L80024C08: the entry pointer is
                                                     * bumped 8 in the loop's delay slot and the
                                                     * sub-quad tile read at +0x8/+0xC of it, so the
                                                     * four quadrant halves are +0x10, +0x18, +0x20,
                                                     * +0x28; quadrant order TL, TR, BL, BR from the
                                                     * vertex-base table D_80065C28 over the 3x3
                                                     * subdivide grid */
        .hq_tri4_nodes = 0x80065D68u,               /* the tier-4 triangle pass's sub-triangle table
                                                     * (environment.s 80026A0C); the textures are not
                                                     * located, so the port draws the triangle whole */
        .hq_tri2_nodes = 0x80065D58u,               /* the tier-2 triangle pass's (environment.s 80025294) */
        .abe_in_tpage = 1,
        .tile_word1_br = 1,                         /* GT4 emit: uv1 = word1 - 0x1F00, uv3 = word1 */
        .lod_dist_default = 0x7C00u,                /* no header field. The feeder derives its fog
                                                     * reference as this value >> 2 = 0x1F00, the
                                                     * constant the HP walk loads before every
                                                     * depth-cue blend (environment.s 80023444: addi
                                                     * $t5, $zero, 0x1F00 on the quad walk; 80023928:
                                                     * addi $a0, $zero, 0x1F00 on the triangle walk;
                                                     * IR0 = 0x1F00 - SZ3), the HP face drop's sum
                                                     * bound as this value (80023350: addi -0x7C00;
                                                     * the walk drops a face whose four corner SZ3s
                                                     * sum to 0x8800 or more, or to 0x7C00 or more
                                                     * with every corner at 0x1F00 or more,
                                                     * 800233E0-80023408), and the in-LOD bound as
                                                     * this value >> 4 = 0x7C0 = hp_far. The draw
                                                     * bins with the constants below. */
        .ladder_nearest_corner = 1,                 /* the HP walk blends each corner with IR0 =
                                                     * 0x1F00 - SZ3, clamps it to [0, 0xFFF], keeps
                                                     * the largest, and takes the CLUT row 15 -
                                                     * (IR0 >> 8), row 0 selecting the +8 tile half
                                                     * (environment.s 800235E4-800236EC) */
        /* environment.s 800225A0-80022640, sector loop .L80022460-.L8002264C. */
        .no_lp_flag    = 0x2000u,                   /* 800225A0: andi 0x2000 skips the LP block */
        .no_hp_flag    = 0x4000u,                   /* 80022604: andi 0x4000 emits no HP entry */
        .force_lp_flag = 0u,                        /* the LP list is distance-gated only */
        .lp_near_shift_flag = 0x8000u,              /* 800225AC: andi 0x8000 */
        .lp_near_shift = 0x600,                     /* 800225B8: addi 0x600 */
        .hp_far        = 0x7C0,                     /* 800225E8: addi -0x7C0, bgez skips HP */
        .lp_near       = 0x760,                     /* 800225B4: addi -0x760, blez skips LP */
        .hp_far_tight  = 0x600,                     /* 800225F8/800225FC: addi -0x100 then -0x500 */
        .tight_flags   = 0x5000u,                   /* 800225F0: andi 0x5000 */
        /* The LP near outcode, the Spyro 2 rule. */
        .lp_near_band    = 0x760,                   /* 800225C0-800225D0: vz - r - 0x760 <= 0
                                                     * sets entry bit 1 */
        .lp_near_sz      = 0x760,                   /* 8002739C-800273A4: t1 = ((w10 >> 15) &
                                                     * 0x600) - 0x760; 80027494, 80027520: SZ3
                                                     * + t1 < 0 tags the vertex 8; 80027644:
                                                     * a face whose corners share a tag drops */
        .lp_near_sz_step = 0x200,
        .sector_cull   = TERRAIN_SECTOR_CULL_SPHERE,
        /* environment.s 80023348-80023374: the corner-SZ sum against 0x2000 and
         * each corner against 0x140, no face-word bits in the test. */
        .near_band       = 0x2000,
        .near_corner     = 0x140,
        .near_flag_excl  = 0u,
        .near_flag_tier2 = 0u,
        .lp_base_slots   = 0x30,  /* the LP insert's bucket index is
                                   * (4-corner SZ sum >> 5) + (word1 & 0xF) * 8
                                   * + 0x30 (environment.s:7136-7138); the
                                   * inserts at environment.s:4216-4242 and
                                   * 4667-4681 are the HP textured triangle's,
                                   * with no base */
        .lp_farcull_keyed = 1,    /* that whole index is dropped at or beyond
                                   * (G_ENVIRONMENT+0x2C >> 7) - 1
                                   * (environment.s:6851-6858, 7140-7141) */
        .lp_bias_slots   = 8,
        .hp_bias_slots   = 4,
        .nearband_ot_keys = 1,    /* environment.s:4216-4242: the faces the
                                   * near/HQ classifier selects key through the
                                   * ordinary insert; the 0x30-based drain at
                                   * environment.s:7104,7138-7145 takes none of
                                   * them */
        .slot_ot_contest  = 1,    /* draw/Draw.s:403 submits the whole scene
                                   * through one table and one DrawOTag;
                                   * stdutil.s:84-99 makes a later insert into a
                                   * bucket paint later, and
                                   * draw/func_8001E460.s:14,22 runs the moby
                                   * pass before the terrain pass */
        .hp_source_wad_entry = 139, /* Fireworks Factory: the title scene's
                                     * sectors are cut-down copies of its
                                     * sectors (WAD entry 6, sub-file 1) */
        .standin_lp_key = 1,  /* stand-ins adjoin no whole-mesh borrow */
    },
    .shadow = {
        .desc        = G_MOBYSHADOWS,     /* mobyfunc.s:921-923: +0x00/+0x04 the sprite UV
                                           * words, +0x08 the drawn-moby list terminator */
        .hook        = F_MOBY_SHADOWS,
        .list_source = SHADOW_LIST_GTE_DATA, /* mobyfunc.s:914-916: mfc2 from data reg 2,
                                              * the base the moby draw mobydraw.s:36 left
                                              * there (polybuf end - 0x3000) */
        .list_base   = 0u,
        .list_reg    = 2,
        .list_offset = 0x1400u,           /* mobyfunc.s:916 addi 0x1400 */
        .moby_pos    = offsetof(Moby, position),   /* mobyfunc.s:932-933 lw 0xC / lw 0x10 */
        .moby_ground = 0x1Cu,             /* mobyfunc.s:930 lw 0x1C; packed by func_8005629C
                                           * (updatemobys.s:984-993) */
        .size_shl    = 1,                 /* mobyfunc.s:1058-1060 sll 1 before the corners */
        .uv1_bottom_right = 1,            /* mobyfunc.s:922 addi -0x1F00 on the second UV word */
        .matrix_fold_a = 0,               /* mobyfunc.s:987-1004 writes R12 from the first
                                           * MVMVA and folds no angle index into R11 */
        .cull_layout = SHADOW_CULL_SPYRO3, /* mobyfunc.s:1105-1127: four-corner extent tests */
        .ground_plane = D_PLAYERSHADOW + 0x0Cu,  /* the player shadow's world height,
                                           * mobyfunc.s:630 lw 0xC */
        .moby_ot_bias = 0x47u,             /* mobyfunc.s 8002DEBC lb 0x47, 8002E078 addi -3 */
        .moby_ot_bias_sub = 3,
        .frame_radius_off = 0x07u, /* mobydraw.s 800306D8-800306E8 lbu 0x24 of header + frame*8 + 7 */
        .player = {
            .hook         = F_PLAYER_SHADOW,
            .state        = D_PLAYERSHADOW,
            .anim_table   = D_SPYRO_ANIMS,    /* mobyfunc.s:706-708 lw through the
                                               * table pointer, as spyrodraw.s:33 */
            .blend_table  = D_SPYRO_BLEND,    /* mobyfunc.s:699-704 lbu g_Spyro+0x1C
                                               * into the table, result << 4 = IR0 */
            .ring_dirs    = D_SHADOW_RING,    /* mobyfunc.s:775-790 */
            .avatar_anim  = 0x14u,            /* mobyfunc.s:705 lw 0x14: bodyAnimation */
            .avatar_blend = 0x1Cu,            /* mobyfunc.s:699 lbu 0x1C */
        },
    },
    .particles = {
        .hook       = F_PARTICLES_DRAW,
        .array_ptr  = D_PARTS_ARRAY,      /* partsdraw.s:57-59 loads the pool base
                                           * through this word into its walk register */
        .rec_stride = 0x20u,              /* partsdraw.s:104-105 addi 0x20 after reading
                                           * the next record's selector at 0x20 */
        .sel_off    = 0x00u,              /* partsdraw.s:67 lh 0x0: bltz skips, and the
                                           * arithmetic shift by 8 tested at :76-87
                                           * dispatches the class */
        .pos_off    = 0x04u,              /* partsdraw.s:709-712 and :219-226 read 0x4
                                           * (X low, Y high) and 0x8 (Z low) and subtract
                                           * the camera position shifted right by 2 */
        .drawn_off  = 0x03u,              /* partsdraw.s:223 sb zero at record start,
                                           * :358/:415/:528/:693/:745 sb 1 once the record
                                           * passes its screen and depth window */
        .fixed_layer_class = 5u,          /* partsdraw.s:76-87 dispatches classes 0-4 and
                                           * falls through for 5 and up; the fallthrough
                                           * links at D_OT_BASE - 8 (:202) instead of a
                                           * depth bucket */
        .type_table = D_PARTS_TYPES,      /* partsdraw.s:68-69 loads it into $s1, indexed
                                           * and read as in the other game's routine */
        .stamp_skip = {
            /* class 0: :745 stamps, :744 has shifted SZ3 right by 5, :751-752
             * subtracts 2 and skips a key <= 0; the size byte is subtracted
             * after (:753) and the key clamped to 0 (:754-756) */
            [0] = { .on = 1, .size_off = 0u, .key_min = 3 },
            /* class 3: :415 stamps, :414 has shifted SZ3 right by 5, :408/:417-421
             * subtracts the byte at +0x11 (lhu +0x10, srl 8) and skips a
             * negative key */
            [3] = { .on = 1, .size_off = 0x11u, .key_min = 0 },
            /* class 2 clamps its key to 0 (:579-582) and appends; classes 1
             * (:690 tests, :693 stamps) and 4 (:355 tests, :358 stamps)
             * append after the stamp unconditionally */
        },
    },
    .moby = {
        /* Offsets read from asm/mobydraw.s, whose three routines Spyro 2
         * duplicates instruction-for-instruction at other addresses. The
         * record base is NOT an address: the cull computes the draw scratch
         * as D_POLYBUF_END - 0x3000 and parks the animated record list at
         * +0x2000 of it (mobydraw.s:26-44), so it has to be derived per pass
         * rather than named here. */
        .array_head   = G_MOBYARRAY,
        .array_direct = 0,
        .array_stride = 0x58u,          /* mobydraw.s:63 addi $ra, $ra, 0x58 */
        .state_off    = 0x48u,          /* mobydraw.s:64-72 lb; >= 0 processes */
        .state_term   = 0xFFu,          /* mobydraw.s:70 state == -1 ends the list */
        .pos_off      = 0x0Cu,          /* mobydraw.s:81 the box cull's three words */
        .angle_off    = 0x44u,          /* mobydraw.s:218 the three byte angles */
        .class_off    = 0x36u,          /* mobydraw.s:47-48 lhu, model table index */
        .anim_state_off = 0x3Cu,        /* mobydraw.s:116 {id, nextId, frame, nextFrame} */
        .colour_off   = 0x54u,          /* mobydraw.s:217,229 copied into the record at +0x3C */
        .size_off     = 0x4Fu,          /* mobydraw.s:453-467 high byte of the +0x4E halfword */
        .drawn_off    = 0x4Du,          /* mobydraw.s:73 cleared, :224 written 1 or 2 */
        .rot_off      = 0x20u,          /* mobydraw.s:2655-2659 the five rotation words
                                         * the water-plane split loads */
        /* Every cutscene overlay's sampler and the title overlay's
         * (func_cutscene_62_80077C70, func_title_8007A1C0) walk the first
         * D_8006EDFC records of the array and, where two consecutive track
         * points lie 0x800 or more apart (func_8004EDE8 length), hold the
         * first until the sub-step passes half and then place the actor on
         * the second, where shorter segments interpolate in sixteenths
         * (cutscene_62 camera.s 0x80078044-0x80078180). */
        .cutpath_count    = 0x8006EDFCu,
        .cutpath_snap     = 0x800u,
        .cutpath_overlays = "cutscene_ title",
        /* No distance bound (jump_bound 0): Sparx (class 0x78) moves up to
         * about 31000 units a tick while other mobys jump as little as about
         * 1000, so a moby the sampler does not drive is held only on a
         * placement marker. */
        .place   = s_moby_place,
        .place_n = sizeof s_moby_place / sizeof s_moby_place[0],
        .director   = s_moby_director,
        .director_n = sizeof s_moby_director / sizeof s_moby_director[0],

        .model_table     = G_MOBYMODELS,
        .model_anims_off = 0x3Cu,       /* mobydraw.s:116-118 model + 0x3C + id*4 */
        .model_data_off  = 0x38u,       /* mobydraw.s:897 */

        .anim_ptr_block   = 0x0Cu,      /* mobydraw.s:975 vertices; faces :939 at +0x10 */
        .anim_scale_off   = 0x02u,      /* mobydraw.s:510,537-539 the cull shifts the
                                         * quarter-world centre left 2 then right by this
                                         * byte before storing it as the translation, so
                                         * the model's unit is world >> byte (0 on most
                                         * Sunrise Spring models: full world units) */
        .anim_shift_off   = 0x02u,      /* short-encode shift */
        .anim_ccount_off  = 0x01u,      /* colour count, one byte where Spyro 1 has two */
        .anim_vcount_hi   = 0x04u,      /* mobydraw.s:917-921 HP vertex count */
        .anim_vcount_lo   = 0x05u,      /* mobydraw.s:917-920 selected by the record flag sign */
        .anim_radius      = 0x07u,      /* mobydraw.s:124 lbu, << 4 for the sphere cull */
        .anim_frames      = 0x24u,      /* mobydraw.s:170-174 8-byte frame records */
        .anim_frame_stride = 8u,

        .rec_base         = 0u,         /* derived from the polybuf end, see above */
        .rec_stride       = 0x44u,      /* mobydraw.s:880-889 the animated record list */
        .rec_header_off   = 0x04u,      /* mobydraw.s:531 AnimationHeader* (frame A) */
        .rec_frame_a_off  = 0x0Cu,      /* mobydraw.s:535 frame A's record word 0 */
        .rec_frame_b_off  = 0x10u,      /* mobydraw.s:490, read :989 frame B's word 0 */
        .rec_tr_off       = 0x1Cu,      /* mobydraw.s:572-574 view-space translation */
        .rec_matrix_off   = 0x28u,      /* mobydraw.s:355-359 the composed 3x3 */
        .rec_tier_off     = 0x00u,      /* word 0 bits 16-31, sign picks HP/LP */
        .rec_material_off = 0x3Cu,      /* mobydraw.s:229 the Moby+0x54 colour word */
        .special_tiledef  = 0x8006C4F4u, /* mobydraw.s:2085-2087,2098-2099: the material
                                          * mode-1 setup loads this word, biased by the
                                          * record's CLUT row, into GTE control 16, and
                                          * the per-face emit writes its halves over the
                                          * primitive's texture page and CLUT. The address
                                          * is the middle of an unnamed data block, so it
                                          * is written as the literal the lui/addiu pair
                                          * forms. */
        .env_sqrt_table   = 0x80069F40u, /* mobydraw.s:2448-2450 */
        .env_atan_table   = 0x800664ECu, /* mobydraw.s:2497-2499 */
        .env_radius_table = 0x80066468u, /* mobydraw.s:2518-2522 */
        .env_bias_word    = 0x8006E040u, /* mobydraw.s:2100-2102 (D_8006DFF8 + 0x48) */
        .rec_moby_off     = 0x40u,      /* mobydraw.s:360 */
        .blend_shift      = 4,          /* mobydraw.s:922-923,986 (w0 >> 4) & 0xFF0 */
        .blend_mask       = 0xFF0u,

        .rec_base_polybuf_off = 0x1000u,  /* scratch = polybuf end - 0x3000, queue at
                                           * +0x2000 of it (mobydraw.s:26-44) */
        .rec_tr_final     = 1,          /* the cull GPF-rescales the translation by the
                                         * size byte and drops the records that pushes
                                         * past 63000 (mobydraw.s:546-551,572-574); the
                                         * draw loads it as stored (mobydraw.s:898-900) */
        .rec_frame_is_word = 1,         /* mobydraw.s:532-535 the keyframe's own word */
        .rec_header_b_off = 0x08u,      /* mobydraw.s:988 keyframe B's AnimationHeader* */

        /* The world ordering table: the pointer at D_8006C634, 0x580 entries of
         * {tail, head}, linked highest entry first by func_8004E664
         * (stdutil.s:11-61; draw/Draw.s:271 passes the count). */
        .cal_world_ot   = 0x8006C634u,
        .cal_ot_buckets = 0x580u,

        /* Class 3 (m_Class at +0x36): the effect moby beside every sub-area
         * door, drawn by func_80031124 from the func_80030478 records. Its
         * key sorts it in front of the door's plate, which lies in front of
         * it in true depth. */
        .key_order_cls  = 3u,
        /* Update (0x80055400) dispatches game.state 6 to UpdateCutscene and
         * 11 to the title overlay (func_title_8007566C); both draw through
         * the same model table filled with their own models, where index 3
         * is Spyro, not the door effect. */
        .key_order_off_states = (1u << 6) | (1u << 11),
        /* Seashell Shore's barrel-lifting Rhynoc (0x101) and its barrel, a
         * moby of its own (0x166): the barrel's m_DepthOffset keys it about
         * 500 world in front of its true depth, in front of the Rhynoc's
         * head where the head is nearer. */
        .true_depth_cls = { 0x101u, 0x166u },
        .true_depth_n   = 2,

        .hook_cull          = F_MOBY_CULL,
        .hook_draw_regular  = F_MOBY_DRAW_ANIM,
        .hook_draw_specular = 0u,       /* no specular material: the draw issues no
                                         * GTE lighting op at all */
        .hook_draw_shaded   = 0u,
        .hook_draw_simple   = F_MOBY_DRAW_STATIC,
        /* D_8006C65C: in game state 4, draw/Draw.s:268-337 stores it into
         * D_8006C550, writes 0xFF to its +0xA0 (the next element's state
         * byte), runs func_80030478 and func_80031124 into a cleared OT and
         * links that chain after the frame's, 2D layer included. */
        .overlay_moby       = 0x8006C65Cu,
    },
    .player = {
        .base    = G_SPYRO,
        .rec_len = (uint32_t)sizeof(Spyro),     /* 0x2E8; the interp ring's g_Spyro row */
        .pos_off = offsetof(Spyro, position),
        .rot_off = { 0x0Cu, 0x10u },            /* bodyRotation, headRotation
                                                 * (spyrodraw.s:269,327) */
        .anim_off = { 0x14u, 0x18u },           /* bodyAnimation, headAnimation
                                                 * (spyrodraw.s:15,118) */
        .sort_depth_off    = 0x1Eu,             /* spyrodraw.s:250 lb 0x1E: the ordering bias */
        .colour_filter_off = 0x20u,             /* the tint word: byte 3 non-zero cues the colour
                                                 * block toward bytes 0-2 (spyrodraw.s:740-743) */
        .emit_mode_off     = 0u,                /* one emit path */
        .hook_draw = F_PLAYER_DRAW,

        /* The routine carves its scratch out of the polybuf every tick, as the
         * moby draw does: base = *polybuf.end - 0x3000, the two 0x3C-byte part
         * records at +0x1400 and a zero flags word after them
         * (spyrodraw.s:18-20,115,175,201). */
        .part_base   = 0u,
        .part_base_polybuf_off = 0x3000u,
        .part_off    = 0x1400u,
        .part_stride = 0x3Cu,
        .part_count  = 2,
        .part_block_len = 0xC0u,                /* the two records and the terminator */

        .part_flags_off    = 0x00u,             /* spyrodraw.s:114 */
        .part_matrix_off   = 0x10u,             /* spyrodraw.s:270 func_8004EA90's destination */
        .part_tr_off       = 0x04u,             /* spyrodraw.s:265-267 */
        .part_stream_a_off = 0x24u,             /* static / adjust / block cursors
                                                 * (spyrodraw.s:98-100,394-396) */
        .part_stream_b_off = 0x30u,             /* spyrodraw.s:108-110,404-406 */
        .part_seam_off     = 0x7Cu,             /* host block only: the body's two
                                                 * seam words, after the zero flags
                                                 * word at 0x78 (spyrodraw.s:66,92) */
        .part_stream_flag_bit = { 12, 13 },     /* spyrodraw.s:401,410 */
        .part_matrix_sign  = 0x80000000u,       /* spyrodraw.s:375 bgez skips the reload */
        .blend_mask        = 0x0FFFu,           /* the blend byte << 4, 1.12 (spyrodraw.s:400) */

        .sxy_base = 0x1F800000u,                /* the draw writes every projected vertex's
                                                 * int16 screen position here as it goes
                                                 * (spyrodraw.s:361,443-448); the walk does not
                                                 * read it, the probe compares against it */
        .sxy_len  = 0x400u,

        .flame_base  = D_FLAME_STATE,
        .flame_len   = 0xC4u,                   /* through the attach matrix's fifth word */
        .shadow_base = D_PLAYERSHADOW,          /* the ground shadow's own state block:
                                                 * position and rotation the player
                                                 * update writes, not fields of g_Spyro */
        .shadow_len  = 0x18u,                   /* mobyfunc.s:625-715 reads +0x00 through
                                                 * +0x17 (flag, position, angles, bias) */

        .pair_bound = 400.0,
    },
    .flame = {
        .hook       = F_FLAME_DRAW,
        .attach_off = 0xB0u,             /* asm/mobyfunc.s:212-216 loads five words */
        .super_off  = 0u,                /* no wide second form */
        .geom           = D_FLAME_GEOM,
        .geom_stride_lo = 0xB0u,         /* asm/mobyfunc.s:239 */
        .geom_stride_hi = 0xE0u,         /* asm/mobyfunc.s:244 */
        .geom_hi_base   = 0x2C0u,        /* asm/mobyfunc.s:246 */
        .conn           = D_FLAME_CONN,
        .conn_hi_off    = 0x78u,         /* asm/mobyfunc.s:247 */
        .cap_colours    = D_FLAME_CAP_COLOURS,
        .cap_amp        = 0x28u, .cap_amp_super   = 0x28u,  /* asm/mobyfunc.s:289 */
        .strip_amp      = 0x2Fu, .strip_amp_super = 0x2Fu,  /* asm/mobyfunc.s:523 */
        .uv1_bias       = -0x7F00,       /* asm/mobyfunc.s:489, before the per-step ramp */
        .cap_colour_off = flame_cap_colour_off,
    },
    .glow = {
        .hook       = F_GLOW_DRAW,
        /* The world draw's call returns here (asm/nonmatchings/draw/
         * func_8001E460.s:28, the delay slot after the jal). The pause screen
         * calls the same routine from func_80058408 (:43); that instance draws
         * over a frozen frame and is not this feeder's. */
        .ra_guard   = 0x8001E4BCu,
        .pool       = D_GLOW_POOL,
        .slots      = 16u,               /* asm/mobyfunc.s:1933, base +0x140 */
        .stride     = 0x14u,
        .pos_off    = 0x00u,             /* asm/mobyfunc.s:1989 */
        .packed_off = 0x10u,             /* asm/mobyfunc.s:1959 */
        .box        = 0x7800,            /* asm/mobyfunc.s:2001-2012 */
        .near_z     = 0x80,              /* asm/mobyfunc.s:2023 */
        .far_z      = 0x7800,            /* asm/mobyfunc.s:2025 */
        .pyr_x      = 0x400,             /* asm/mobyfunc.s:2039 */
        .pyr_y      = 0x300,             /* asm/mobyfunc.s:2049 */
        .front_bias = 0x7F,              /* asm/mobyfunc.s:2056-2059 */
        /* The depth-offset byte (+0x47, the top byte of the angle word the
         * moby cull subtracts from its slot) of the moby each glow sits on:
         * 4 on Sparx, the moby nearest every glow record that sits on one.
         * The allocator links the record back to nothing (it stores the
         * shape, size, a random phase and the bias 4), so the value is layout
         * data. */
        .owner_offset = 4,
        .shape_table  = 0x80066864u,     /* asm/mobyfunc.s:2013-2018 */
        .cam_yaw_off  = 0x48u,           /* asm/mobyfunc.s:1949, >> 4 at :1953 */
        /* asm/mobyfunc.s:1964-1980: game state 4 with D_80070104 set draws
         * only the front-entry records. */
        .solo_state_addr = 0x8006E344u, .solo_state = 4u, .solo_flag = 0x80070104u,
    },
    .glint = {
        .hook       = F_GLINT_DRAW,
        .pool       = D_GLINT_POOL,
        .slots      = 16u,               /* asm/mobyfunc.s:2210, base +0x100 */
        .stride     = 0x10u,
        .pos_off    = 0x00u,             /* asm/mobyfunc.s:2249-2251 */
        .packed_off = 0x0Cu,             /* asm/mobyfunc.s:2233 */
        .box        = 0x7800,            /* asm/mobyfunc.s:2265-2276, half-world */
        .near_z     = 0x80,              /* asm/mobyfunc.s:2293 */
        .far_z      = 0x7800,            /* asm/mobyfunc.s:2295 */
        .pyr_x      = 0x100,             /* asm/mobyfunc.s:2309 */
        .pyr_y      = 0xC0,              /* asm/mobyfunc.s:2319 */
        .near_vz    = 0x2000,            /* asm/mobyfunc.s:2347, the branch depth */
        .outline    = D_GLINT_OUTLINE,      /* asm/mobyfunc.s:2360, 2457 */
        .quads      = D_GLINT_QUADS,        /* asm/mobyfunc.s:2375 */
        .colours    = D_GLINT_COLOURS,      /* asm/mobyfunc.s:2328 */
    },
    .zap = {
        .hook       = F_GLINT2_DRAW,
        .pool       = D_GLINT2_POOL,
        .slots      = 8u,                /* asm/mobyfunc.s:2563, base +0x180 */
        .stride     = 0x30u,
        .head_off   = 0x00u,             /* asm/mobyfunc.s:2608 */
        .packed_off = 0x1Cu,             /* asm/mobyfunc.s:2588 */
        .box        = 0x5400,            /* asm/mobyfunc.s:2612-2627 */
        .far_sz     = 0x5000,            /* asm/mobyfunc.s:2825 */
        .spad       = 0x1F800100u,       /* asm/mobyfunc.s:2747 */
    },
    .tracers = {
        .ribbon_hook = F_TRACER_RIBBON,
        .mesh_hook   = F_TRACER_MESH,
        .ring        = D_TRACER_RING,
        .slots       = 0x20u,            /* asm/mobyfunc.s:2943, base +0xD80 */
        .stride      = 0x6Cu,            /* asm/mobyfunc.s:2944 */
        .points_off  = 0x04u,            /* asm/mobyfunc.s:2951 */
        .packed_off  = 0x64u,            /* asm/mobyfunc.s:2949 */
        .ring_n      = 16,               /* +0x04 through +0x64, 6 bytes per point */
        .box         = 0x6000,           /* asm/mobyfunc.s:2985-2993 */
        .far_sz      = 0x4000,           /* asm/mobyfunc.s:3065 */
        .spad        = 0x1F800000u,      /* asm/mobyfunc.s:3257 */
    },
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
        .visibility = F_VISIBILITY,   /* called by both draws before their walks */
    },

    /* The credits font: WAD entry 183, sub-file 2, the model the credits
     * overlay reads as class 59 (credits.ovl, the spawn at 0x80076258 stores
     * the class and the glyph into the moby's animation-id bytes). Its 41
     * blocks: letters 0-25, digits 26-35, '!' 36, ',' 37, '.' 38, '-' 39,
     * ':' 40 (the overlay's character map at 0x80075590-0x8007563C). Letters
     * span y -50..54. The punctuation blocks sit about the model's centre
     * line; '.' and ',' are lowered onto the letters' baseline (the credits
     * draw them on the baseline of their smaller lowercase row), ':' and '-'
     * stay centred.
     *
     * Every face is flat-lit against entry 2 of the flat colour table at
     * 0x80066998 (the overlay stores Moby+0x54 = 2): base word 0x00004040,
     * lit word 0x0000A0A0, red in the low byte: base (64,64,0), lit
     * (160,160,0). The draw's light is (-1916,-3065,-3072) against a
     * view-space normal of length 127, shifted right 8 (mobydraw.s:4758-4813):
     * at unit N.L = t the projection is 2353 * t, the lit colour scales by it
     * over 4096 (160 * 2353 / 4096 = 92) and the highlight
     * (projection - 1472) << 3 in 12-bit colour opens at t = 1472 / 2353 with
     * slope 2353 * 8 / 16. The fixed light is that vector in the host form,
     * z negated toward the reader. */
    .font = {
        .form            = GAME_FONT_WAD_MODEL,
        .wad_entry       = 183,
        .wad_subfile     = 2,
        .digit_base      = 26,
        .letter_base     = 0,
        .punct           = { {'!', 36, 0}, {',', 37, 32}, {'.', 38, 36},
                             {'-', 39, 0}, {':', 40, 0} },
        .model_h         = 100.0f,
        .light_add       = { 92.0f, 92.0f, 0.0f },
        .hi_threshold    = 0.6256f, .hi_gain = 1176.5f,
        .fixed_light     = { -0.404f, -0.646f, 0.648f },
        .fixed_tint      = 0x404000u,
    },
    /* The credits letter roll: game state 10 draws through func_8001E374
     * (Draw.s's table jtbl_800102F0), which calls func_credits_80076118.
     * While D_8006C510 is not -1 it walks the D_8006C5A4 (lh) CreditsLetter
     * records at *D_8006C5F0 (stride 0x1C, state byte +0x04) and writes one
     * class-0x3B moby per record in state 1, 2 or 3 into consecutive slots up
     * from *D_8006C550, then the 0xFF sentinel
     * (func_credits_80076118.s:48-100); state 1 is flying in, 2 settled, 3
     * departing, 4 once its y reaches 0x119, 0 free. The update
     * (func_credits_80074BA0) clears 100 records and gives a new letter the
     * first state-0 record or appends one. */
    .credits = {
        .gamestate = 0x8006E344u, .gamestate_value = 10u,
        .stage = 0u, .stage_below = 0,
        .draw_gate = 0x8006C510u,
        .recs = 0x8006C5F0u, .count = 0x8006C5A4u, .count_width = 2u,
        .rec_stride = 0x1Cu, .state_off = 0x04u, .rec_max = 100u,
        .drawn_states = (1u << 1) | (1u << 2) | (1u << 3), .flyin_state = 1u,
        .slot_base = 0x8006C550u, .slot_dir = 1, .slot_stride = 0x58u,
        .letter_class = 0x3Bu,
    },
    /* The loading screen's treasure tally: in builder state 3 (D_8006FBC8)
     * func_loading_80075B74 and func_loading_80077964 rewrite the moby array
     * from *D_8006C550 every tick; the second first writes one moby of class
     * *D_8006C528 into slot 0 (0x800779F8-0x80077BE0). While (D_8006FBC4 -
     * 0x21) < 0x48 unsigned each writes one class-1 moby per record of the 24
     * at *D_8006C764 (stride 8) whose byte +4 is nonzero (0x80075C60-
     * 0x80075EB8), then the counter gem (+0x0C 0x154, +0x14 0xC00;
     * 0x80076074) and the total gem (+0x0C 0xB4; 0x800762E0). */
    .tally = {
        .state = 0x8006FBC8u, .state_value = 3u,
        .timer = 0x8006FBC4u, .timer_lo = 0x21u, .timer_span = 0x48u,
        .recs = 0x8006C764u, .rec_stride = 8u, .active_off = 4u, .rec_count = 24u,
        .slot_base = 0x8006C550u, .slot_stride = 0x58u, .prefix_max = 1u,
        .gem_class = 1u, .hud_x = { 0x154, 0xB4 }, .hud_z = 0xC00,
    },
    .owner = {
        /* MobyUpdate_120, Sparx (src/overlays/level_17/moby.c,
         * func_level_17_80075384; entry 120 of each level overlay's
         * update table). */
        .sparx_class   = 0x78u,
        /* UpdateSpyro (src/update.c func_80055294 calls it under
         * UPDATE_SPYRO): every particle spawn of the player's update module
         * (asm/nonmatchings/spyroupdate) and of the overlays' animation
         * module it calls runs inside this call. */
        .player_update = 0x8003E83Cu,
        /* ovlHeader.MobyUpdate (include/ovl_header.h, 0x800742D8); the moby
         * update loop func_8003038C calls entry [moby+0x36] with a0 = moby
         * (asm/nonmatchings/mobyupdate/func_8003038C.s). */
        .moby_update   = 0x800742D8u,
        /* The slot allocator every overlay's SpawnParticle calls with
         * a0 = 2 (asm/updatemobys.s func_80056434): it returns the slot
         * at the pool cursor D_8006C614, or a random slot without the
         * class byte's bit 4 when the 0x2000-byte pool is full. */
        .part_alloc    = 0x80056434u,
        /* Sparx's update keeps its glow record at tag+0x10: it stores the
         * func_8002EC50 result there and clears the record's shape byte
         * through it (func_level_17_80075384, 0x80075560-0x80075584). */
        .sparx_glow_off = 0x10u,
    },
};

/* The 2D UI layer (games/common/renderer/ui_sprite.c). The sprite, box, fade
 * and letterbox primitives carry the same psyq word order as Spyro 1's and
 * reach the same kind of chokepoint; the text chain does not and is fed by
 * renderer/text_sprt.c instead. */
const ui_game_t spyro3_ui_game = {
    .ot_insert    = F_UI_OT_INSERT,
    .fade_emitter = F_UI_FADE,
    /* The credits overlay (magic 7, recomp manifest) fades the credits in from
     * black and through black at each scene change with its own copy of the
     * fade emitter, linked at slot 8 of the slotted table
     * (credits.c:10-33), driven by D_8006C598 (func_credits_80076118.s, 0x800762A8-0x800762C8). */
    .fade_emitter_ovl     = F_CREDITS_FADE,
    .fade_emitter_overlay = 7u,

    /* The cutscene letterbox: func_8001FF44 steps D_8006C64C and draws the
     * bars over rows 12..12+h and 228-h..228 (drawutil/func_8001FF44.s:4-99). */
    .letterbox = { .emitter = F_UI_LETTERBOX, .height = 0x8006C64Cu,
                   .top_y = 12, .bottom_y = 228 },

    /* No freeze-frame backdrop quad is located on this game: the pause screen
     * is not reproduced from the native renderer's last world frame yet. */
    .freeze_tint   = 0u,
    .freeze_clut_x = 0,
    .freeze_clut_y = 0,
    /* The state-4 draw func_8001DD1C keeps the last rendered frame behind the
     * save prompt, the atlas sub-screens and the sub-screen that follows them by
     * writing DRAWENV.isbg = 0 on both draw buffers (draw/func_8001DD1C.s:278-281,
     * :331-334, :393-396) and drawing no world; nothing writes the byte back to 1
     * until a screen that clears runs (:98-105). The ordinary six-entry pause
     * menu leaves isbg at 1 and clears to the level background colour. */
    .freeze_retained = 1,

    /* The UI texture block: tpage word 0x618 in the text chain's DR_MODE is
     * page (512, 256) at 4bpp, the same block the HUD and dialog sprites
     * sample. */
    .atlas_x = 512, .atlas_y = 256, .atlas_w = 512, .atlas_h = 256,
    .atlas_name = "ui_atlas",

    /* Both glyph emitters build a tag-to-tag SPRT chain in the primitive
     * buffer and link it into the 2D ordering table themselves, so neither
     * reaches the chokepoint. */
    /* The loading-screen picture is the third chain of this shape: the image is
     * LoadImage'd to VRAM (512,0) once and re-drawn every frame as four
     * 128x216 SPRTs whose rgb carries the fade level, so the walk that reads a
     * glyph chain out of the primitive buffer reads it unchanged. */
    .sprt_chain_emitter = { F_TEXT_SPRT, F_TEXT_SPRT_PER_GLYPH, F_LOADING_SPRT },
    /* All three emitters are base-binary functions, resident whatever overlay
     * is loaded, so none is overlay-qualified. */
    .sprt_chain_overlay = { 0u, 0u, 0u },

    /* The table is 0x580 {tail, head} slots off D_8006C634 with the 2D entry
     * D_8006C638 one slot below slot 0 (init/func_8002AA34.s:16-50: base
     * X - 0x2C00, entry X - 0x2C08). func_8004E758 and func_8004E71C append
     * at a slot's tail (stdutil.s:65-100) and func_8004E664, called by Draw,
     * stitches the slots highest first and the 2D entry last
     * (stdutil.s:11-62), after the canonical capture. */
    .rank_by_link = 1,
    /* func_8004E71C links the loading picture's DR_MODE and SPRT pairs at its
     * slot argument; the SPRT chain hook above feeds them. */
    .ot_rank_slot_insert = F_UI_OT_INSERT_SLOT,
    .ot_stitch           = F_UI_OT_STITCH,
    /* The two callers of func_8004E664, both in the base binary: Draw
     * (draw.s, VSync at 0x8001E88C-0x8001E8F8, stitch at 0x8001E994-
     * 0x8001EB74) and func_80020168 (drawutil.s, VSync at 0x80020268, stitch
     * at 0x80020290). Every other wait is outside them, such as
     * func_80059038's load screen, which links the strip copy
     * (func_800289C8), waits VSync(0) and links the labels
     * (func_loading_80075114) into the same table (warp.c:524-531). */
    .stitch_routine = { { 0x8001E638u, 0x8001EBACu, 0u },
                        { 0x80020168u, 0x800202DCu, 0u } },
};
