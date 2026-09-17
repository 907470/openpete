// games/spyro-3/renderer/layout.c — the Spyro 3 engine layout. Bases come
// from the drift-checked symbol aliases (manifest/spyro3_symbols.toml, each
// row carrying its evidence). An offset is `offsetof` against the typed
// structs header (openpete_game_structs.h, pinned to the probed MIPS layout
// by manifest/struct_layout.toml) where the decomp names the member; every
// other offset is a literal whose comment names the asm site or dump that
// confirmed it. A field with no located counterpart is 0.

#include "game_layout.h"
#include "renderer/ui_game.h"
#include "spyro3_symbols.gen.h"
#include "openpete_game_structs.h"

#include <stddef.h>

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
     * 48-byte tiledef entry at (g_Environment+0x20 table) + index*48 —
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
        .scroll     = 0x8006E350u + 0x08u,
        .block_ptr  = G_ENVIRONMENT + 0x1Cu,
        .live_table = 0x80072330u,    /* D_8006FCF4 + 0x263C */
        .sub = {
            { 0x08u, 0, 8u, 1, ENV_ANIM_KIND_COLOUR },
            { 0x00u, 1, 4u, 0, ENV_ANIM_KIND_VERT },
            { 0x04u, 1, 4u, 0, ENV_ANIM_KIND_VERT },
        },
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
    .terrain = {
        .radius_mask  = 0xFFFu,                     /* world draw: andi $t2, $a0, 0xFFF */
        .flags_mask   = 0xF000u,                    /* world draw: andi $t1, $a0, 0xF000 */
        .flat_flag    = 0x1000u,                    /* environment.s 800225F0: andi 0x5000, bit 12
                                                     * selects the flat HP list */
        .vz_shl       = 1,                          /* Z4 = (v << 3) & 0x1FFC in quarter units */
        .vz_flat_shr  = 2,                          /* flat list: Z4 = v & 0x3FF */
        .uv_ori_table = 0u,                         /* none: GT3 UVs rotate by face word 3 bits 12-13 */
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
        .abe_in_tpage = 1,
        .tile_word1_br = 1,                         /* GT4 emit: uv1 = word1 - 0x1F00, uv3 = word1 */
        .lod_dist_default = 0x8000u,                /* Spyro 1's authored value; the Spyro 3 draw bins
                                                     * with the constants below, not with this */
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
        .sector_cull   = TERRAIN_SECTOR_CULL_SPHERE,
        /* environment.s 80023348-80023374: the corner-SZ sum against 0x2000 and
         * each corner against 0x140, no face-word bits in the test. */
        .near_band       = 0x2000,
        .near_corner     = 0x140,
        .near_flag_excl  = 0u,
        .near_flag_tier2 = 0u,
        .lp_base_slots   = 0x40,  /* the ordinary insert at
                                   * environment.s:4216-4242,4667-4681,4783-4790
                                   * is (SZ sum >> 7) + bias slots with no base
                                   * term, and the faces this feed reports with
                                   * 64 slots sit about 63 slots off the guest
                                   * table's own line. The term is kept because
                                   * it compensates another unmeasured term of
                                   * the same insert: zeroing it alone moves the
                                   * LP-versus-HP contest away from the original
                                   * frame across the corpus. The base and the
                                   * bias multiplier move together or not at
                                   * all. */
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

        .world_ot   = 0u,               /* the OT bucket array is not located */
        .ot_buckets = 0u,

        .hook_cull          = F_MOBY_CULL,
        .hook_draw_regular  = F_MOBY_DRAW_ANIM,
        .hook_draw_specular = 0u,       /* no specular material: the draw issues no
                                         * GTE lighting op at all */
        .hook_draw_shaded   = 0u,
        .hook_draw_simple   = F_MOBY_DRAW_STATIC,
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
        .colour_filter_off = 0x20u,             /* byte 3 selects the pre-blend colour table
                                                 * (spyrodraw.s:740-743) */
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
        .part_stream_flag_bit = { 12, 13 },     /* spyrodraw.s:401,410 */
        .part_matrix_sign  = 0x80000000u,       /* spyrodraw.s:375 bgez skips the reload */
        .blend_mask        = 0x0FFFu,           /* the blend byte << 4, 1.12 (spyrodraw.s:400) */

        .colour_scratch_off = 0x1800u,          /* spyrodraw.s:748 */
        .colour_scratch_len = 0x200u,           /* 7-bit colour indices, four bytes each */
        .sxy_base = 0x1F800000u,                /* the draw writes every projected vertex's
                                                 * int16 screen position here as it goes
                                                 * (spyrodraw.s:361,443-448); the walk does not
                                                 * read it, the probe compares against it */
        .sxy_len  = 0x400u,

        .flame_base  = 0u,                      /* no located flame struct */
        .flame_len   = 0u,
        .shadow_base = D_PLAYERSHADOW,          /* the ground shadow's own state block:
                                                 * position and rotation the player
                                                 * update writes, not fields of g_Spyro */
        .shadow_len  = 0x18u,                   /* mobyfunc.s:625-715 reads +0x00 through
                                                 * +0x17 (flag, position, angles, bias) */

        .pair_bound = 400.0,
    },
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
        .visibility = F_VISIBILITY,   /* called by both draws before their walks */
    },
};

/* The 2D UI layer (games/common/renderer/ui_sprite.c). The sprite, box, fade
 * and letterbox primitives carry the same psyq word order as Spyro 1's and
 * reach the same kind of chokepoint; the text chain does not and is fed by
 * renderer/text_sprt.c instead. */
const ui_game_t ui_game = {
    .ot_insert    = F_UI_OT_INSERT,
    .fade_emitter = F_UI_FADE,

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
};
