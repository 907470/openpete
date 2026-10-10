// games/spyro-1/renderer/layout.c - the Spyro 1 engine layout. Bases come
// from the drift-checked symbol aliases; member offsets from the generated
// typed header, whose _Static_asserts pin them to the probed MIPS layout.

#include "game_layout.h"
#include "renderer/ui_game.h"
#include "spyro1_symbols.gen.h"
#include "openpete_game_structs.h"
#include "psx_runtime_internal.h"

#include <stddef.h>

/** @brief Returns the byte offset of the flame's first-cell cap colour set for this tick.
 *
 * The cap colours at D_FLAME_UV hold two four-word sets; the flame draw
 * reads the second while g_SpyroFlame's wide-form word (+0x9C) is nonzero
 * (r_flame.s:80059040-80059048).
 * @param cpu The guest CPU state.
 * @return 0x10 while the wide form is set, else 0. */
static uint32_t flame_cap_colour_off(CPUState* cpu) {
    return cpu->read_word(G_SPYROFLAME + 0x9Cu) ? 0x10u : 0u;
}

/* g_EnvironmentAnimations precedes g_Environment (environment.h); the
 * generated env-anim walker func_8002A6FC indexes it as t9+0x10.. */
#define G_ENVANIM (G_ENVIRONMENT - (uint32_t)sizeof(EnvironmentAnimations))

/* Moby placements, read off each writer: classes 0x15A/0x15B going from state
 * 1 to 2 have had their world position projected to screen space in place
 * (level_55 0x800803B0-0x80080408: func_80017AA4(&m_Position, &m_Position),
 * then sb 2 to m_State). */
static const game_moby_place_t s_moby_place[] = {
    { 0x15Au, 1u, 2u },
    { 0x15Bu, 1u, 2u },
    /* Toasty (level_14 0x80080074-0x800800E4) stores m_State 0xB into the
     * record g_LevelMobys[props+0x20] names and VecCopy's its own position
     * over it. */
    { 0x13Au, 0u, 0xBu },
};

/* Props-word placements:
 * - 0x0A3 (level_55 0x8007FB74-0x8007FBFC): the state-0 bob stores
 *   z = props+0x14 + 0x800 + D_8006CC78[t * 8 / 15] / 2 with the timer
 *   t = props+0x8 += g_DeltaTime; the first update after the record is
 *   admitted moves z from the level-data value (props+0x14) by about 4095.
 * - 0x11E (level_11 0x800861B4-0x800863BC): with props+0x10 == 1, a
 *   countdown props+0x24 at or below 0 stores the next path point into the
 *   position and a new countdown.
 * - 0x00D (level_11 0x8007E790-0x8007E9C8, level_14 0x8007B760): the
 *   attach handler's first update (props+0x10 == 0) runs the init, stores
 *   props+0x10 = 1 and copies the parent's position. */
static const game_moby_props_t s_moby_props[] = {
    { 0x0A3u, 0x08u, 0x14u, 2u, "level_55" },
    { 0x11Eu, 0x24u, 0u,    0u, "level_11" },
    { 0x00Du, 0x10u, 0u,    0u, "level_"   },
};

/* The 0x00D attach handler copies the position of g_LevelMobys[props+0x0]
 * every update (VecCopy after the init). */
static const game_moby_attach_t s_moby_attach[] = {
    { 0x00Du, 0x00u, "level_" },
};

/* Regimes whose moby stores are placements:
 * - g_LoadStage (0x80075864, LoadLevel's stage machine) 1..12: the CD DMA
 *   writes the records, also while the title demo path loads (gs 13).
 * - GS_LevelTransition (1): LoadLevel stores the new level's records over
 *   the old ones while the state stays 1 on both ticks.
 * - GS_Respawn (4): the reload rewrites the records.
 * - GS_EntranceAnimation (9) on the snapshot before the pair: g_UpdateMoby
 *   does not run in state 9, so the first update after the entrance runs
 *   each record's init placement (0x143 copies g_Spyro, level_11
 *   0x80086C14-0x80086C24). */
static const game_moby_regime_t s_moby_regime[] = {
    { "g_LoadStage", 1u, 12u, GAME_REGIME_EITHER },
    { "g_GameState", 1u, 1u,  GAME_REGIME_EITHER },
    { "g_GameState", 4u, 4u,  GAME_REGIME_EITHER },
    { "g_GameState", 9u, 9u,  GAME_REGIME_BEFORE },
};

/* The balloonist ride (D_800777E8): on its m_State 4 -> 5 edge,
 * GS_Balloonist (func_800324D8, 0x80032810-0x8003285C) stores m_State = 5
 * and VecCopy's the destination homeworld's arrival point
 * (D_8006EA4C[m_Homeworld]) over the position of m_BalloonMoby (+0x68), the
 * balloon record the ride copied out of g_LevelMobys before LoadLevel. */
static const game_moby_director_t s_moby_director[] = {
    { "g_BalloonState", "g_BalloonMoby", 4u, 5u },
};

const game_layout_t spyro1_game_layout = {
    .display = {
        /* initialization.c GraphicsInitialize: SetDefDispEnv(..., 0, 240, 512, 240)
         * and SetDefDrawEnv(..., 0, 8, 512, 224) - 8 undrawn rows per side. */
        .disp_w = 512, .disp_h = 240, .clip_h = 224,
    },
    .sin_table = D_SIN_TABLE,
    .env = {
        .base            = G_ENVIRONMENT,
        .sector_ptr      = G_ENVIRONMENT + offsetof(Environment, m_SectorPointer),
        .sector_count    = G_ENVIRONMENT + offsetof(Environment, m_SectorCount),
        .occ_groups      = G_ENVIRONMENT + offsetof(Environment, m_OcclusionGroups),
        .occ_group_count = G_ENVIRONMENT + offsetof(Environment, m_OcclusionGroupCount),
        .lq_tex          = G_ENVIRONMENT + offsetof(Environment, m_LQTexturePointer),
        .hq_tex          = G_ENVIRONMENT + offsetof(Environment, m_HQTexturePointer),
        .tex_count       = G_ENVIRONMENT + offsetof(Environment, m_TextureCount),
        .lod_dist        = G_ENVIRONMENT + offsetof(Environment, m_LodDistance),
        .cull_dist       = G_ENVIRONMENT + offsetof(Environment, m_CullingDistance),
        .vis_aspect_independent = 0u,               /* the verdict byte is stored inside the draw's
                                                     * sector loop after the MVMVA sphere-frustum cull
                                                     * (r_environment.s:151-185), so it carries the 4:3
                                                     * frustum; the aspect-independent superset is the
                                                     * occlusion-group sublist. */
    },
    .env_anim = {
        .form    = ENV_ANIM_LISTS,
        .base    = G_ENVANIM,
        .scroll  = G_ENVANIM + offsetof(EnvironmentAnimations, m_ScrollingTextureCount),
        .lp_poly = G_ENVANIM + offsetof(EnvironmentAnimations, m_LowPolyAnimationCount),
        .hp_poly = G_ENVANIM + offsetof(EnvironmentAnimations, m_HighPolyAnimationCount),
        /* Walked by func_8002A6FC (r_environment.s:8002AAE8 LP, 8002AC88 HP),
         * which only advances each descriptor's keyframe and stamps its list
         * index into sector+0x19 / +0x1B; the world draw then INTPLs or
         * copies the keyframe colours into the sector's LP colour array
         * (r_environment.s:80025D14-80025E58) and HP colour arrays
         * (80025FD0 onward) in place. */
        .lp_col  = G_ENVANIM + offsetof(EnvironmentAnimations, m_LowColorAnimationCount),
        .hp_col  = G_ENVANIM + offsetof(EnvironmentAnimations, m_HighColorAnimationCount),
    },
    .visibility = {
        .base = D_VISIBILITY,
    },
    .cyc = {
        .base            = G_CYCLORAMA,
        .sector_count    = G_CYCLORAMA + offsetof(Cyclorama, m_SectorCount),
        .sectors         = G_CYCLORAMA + offsetof(Cyclorama, m_Sectors),
        .occ_group_count = G_CYCLORAMA + offsetof(Cyclorama, m_OcclusionGroupsCount),
        .occ_groups      = G_CYCLORAMA + offsetof(Cyclorama, m_OcclusionGroups),
        .bg_colour       = G_CYCLORAMA + offsetof(Cyclorama, m_BackgroundColor),
        .vertex_base     = 0x18u,
        .face_layout     = CYC_FACES_SPYRO1,
        .visibility      = 0u,           /* the occlusion group list the draw's a0 names */
        .hook_args       = 1,            /* (group, cull matrix, draw matrix) */
        .fade_in_draw    = 0,            /* the cross-fade renderer fades, the sky draw does not */
    },
    .fog = {
        .factor     = D_FOG_FACTOR,
        .far_colour = D_BLEND_TARGET,
        .far_colour_words = 0,
    },
    .db = {
        .cur_ptr = G_CUR_DB,
        .base    = G_DB,
        .stride  = (uint32_t)sizeof(DB),
        .drawenv = offsetof(DB, m_DrawEnv),
    },
    .polybuf = {
        .cursor = D_POLYBUF_CURSOR,
        .end    = D_POLYBUF_END,
    },
    .cam = {
        .base        = G_CAMERA,
        .proj_matrix = G_CAMERA + offsetof(Camera, m_ProjectionMatrix),
        .view_matrix = G_CAMERA + offsetof(Camera, m_ViewMatrix),
        .position    = G_CAMERA + offsetof(Camera, m_Position),
        .rotation    = G_CAMERA + offsetof(Camera, m_Rotation),
        .xform_end   = G_CAMERA + offsetof(Camera, m_DestinationPosition),
    },
    .level_id = G_LEVELID,
    .terrain = {
        .radius_mask  = 0x1FFFu,
        .flags_mask   = 0xE000u,
        .flat_flag    = 0u,
        .vz_shl       = 0,
        .vz_flat_shr  = 0,
        .uv_ori_table = D_HQ_UV_ORI_TABLE,
        .uv_ori_mask  = 0x7Fu,                      /* r_environment.s 80027E9C and 8002928C take the
                                                     * index as a bare `srl 25`, already bounded to
                                                     * 0x7F, over a 128-byte table (D_8006D058: the
                                                     * 16-texel half at +0, the 32-texel half at
                                                     * +0x40) */
        .defer_q      = { G_SCRATCH + 0x2000u, G_SCRATCH + 0x1E00u },
        .face_layout  = TERRAIN_FACES_SPYRO1,
        .tex_stride   = 16u,
        .hq_stride    = 168u,                       /* the sibling HQ record */
        .hq_sub0      = 8u,                         /* entry 0 is the detail-path base;
                                                     * the 2x2 quadrants are entries 1..4 */
        .hq_cells4    = 0x28u,                      /* the 16 cell tiles are entries 5..20:
                                                     * r_environment.s 0x80029210 steps the
                                                     * record pointer past entry 0 and the
                                                     * quadrants before the cell loop */
        .hq_tri4_nodes = D_HQ_TRI4_NODES,
        .hq_tri4_cells = D_HQ_TRI4_CELLS,
        .hq_tri_uv     = D_HQ_TRI_UV,
        .hq_tri_uv_mask = 0x78u,                    /* `srl 25; andi 0x78` at
                                                     * r_environment.s 0x80029E98 */
        .hq_tri2_nodes = D_HQ_TRI2_NODES,
        .hq_tri2_tiles = D_HQ_TRI2_TILES,
        .abe_in_tpage = 0,
        .tile_word1_br = 0,
        .lod_dist_default = 0u,
        .no_lp_flag    = 0x2000u,
        .no_hp_flag    = 0x4000u,
        .force_lp_flag = 0x8000u,
        .lp_near_shift_flag = 0u,
        .lp_near_shift = 0,
        .hp_far        = 0,       /* the draw bins by flags alone: no distance test */
        .lp_near       = 0,
        .hp_far_tight  = 0,
        .tight_flags   = 0u,
        .sector_cull   = TERRAIN_SECTOR_CULL_NONE,
        .near_band       = 0x2000,
        .near_corner     = 0x140,
        .near_flag_excl  = 0x80u,
        .near_flag_tier2 = 0x40u,
        .lp_base_slots   = 0x40,  /* unmeasured against this game's own ordering
                                   * table; r_environment.s:1436-1448 computes
                                   * (SZ sum >> 7) + 4*bias with no base term at
                                   * one insert site, so the base wants its own
                                   * table census before it moves */
        .lp_bias_slots   = 8,
        .hp_bias_slots   = 4,
        .nearband_ot_keys = 1,    /* the near/HQ drain (func_800276A0) inserts
                                   * every sub-quad into g_WorldOT at
                                   * (ΣSZ >> 7) + ((word3 & 0x38) >> 1):
                                   * r_environment.s 0x80027D4C, 0x80027EF8 */
        .slot_ot_contest  = 0,
    },
    .shadow = {
        .desc        = G_MOBYSHADOWS,     /* {Tiledef shadow; int* shadow_list @+0x08} */
        .hook        = F_MOBY_SHADOWS,
        .list_source = SHADOW_LIST_ADDR,
        .list_base   = G_SCRATCH + 0x2800u, /* r_shadows.s:389 addi 0x2800 off the scratch base */
        .list_reg    = 0,
        .list_offset = 0u,
        .moby_pos    = 0x0Cu,             /* Vector3D m_Position */
        .moby_ground = 0x1Cu,             /* int m_ShadowDistance + the two normal angles */
        .size_shl    = 0,                 /* r_shadows.s:389+ feeds the record word unshifted */
        .uv1_bottom_right = 0,            /* +0x04 is already the top-right corner */
        .matrix_fold_a = 1,               /* r_shadows.s:8005A150-8005A158: R12 dropped,
                                           * idxA << 3 added into R11 */
        .cull_layout = SHADOW_CULL_SPYRO1,
        .ground_plane = D_SPYROSHADOW + 0x18u,   /* SpyroShadow ground height */
        .moby_ot_bias = 0x47u,             /* r_shadows.s 8005A294 lb 0x47, 8005A320 addi -1 */
        .moby_ot_bias_sub = 1,
        .player = { .hook = 0u },         /* the player silhouette is its own
                                           * renderer, func_80059A48, fed by
                                           * renderer/shadow_spyro.c */
    },
    .moby = {
        .array_head   = G_LEVELMOBYS,     /* points at the first Moby */
        .array_direct = 0,
        .array_stride = (uint32_t)sizeof(Moby),
        .state_off    = offsetof(Moby, m_State),
        .state_term   = 0xFFu,
        .pos_off      = offsetof(Moby, m_Position),
        .angle_off    = offsetof(Moby, m_Rotation),   /* m_DepthOffset rides +3 */
        .class_off    = offsetof(Moby, m_Class),
        .anim_state_off = offsetof(Moby, m_AnimationState),
        .colour_off   = 0u,               /* no per-moby colour word */
        .size_off     = offsetof(Moby, m_ScaleOverride),
        .drawn_off    = offsetof(Moby, m_WasDrawn),
        /* No distance bound (jump_bound 0): Sparx (class 120) closes on
         * Spyro in steps of up to about 49000 units a tick while respawned
         * and relocated mobys jump as little as about 900, so no bound
         * separates the two; a moby the cutscene path sampler does not drive
         * (its actors take spyro_cutpath_jump) is held only on a placement
         * marker or the load-regime rule. */
        .jump_bound   = 0u,
        .place        = s_moby_place,
        .place_n      = sizeof s_moby_place / sizeof s_moby_place[0],
        .director     = s_moby_director,
        .director_n   = sizeof s_moby_director / sizeof s_moby_director[0],
        .props_mark   = s_moby_props,
        .props_mark_n = sizeof s_moby_props / sizeof s_moby_props[0],
        .attach       = s_moby_attach,
        .attach_n     = sizeof s_moby_attach / sizeof s_moby_attach[0],
        .regime       = s_moby_regime,
        .regime_n     = sizeof s_moby_regime / sizeof s_moby_regime[0],

        .model_table     = G_MODELS,
        .model_anims_off = offsetof(Model, m_Animations),
        .model_data_off  = offsetof(Model, m_Data),

        .anim_ptr_block   = offsetof(AnimationHeader, m_AnimationVertices),
        .anim_scale_off   = offsetof(AnimationHeader, m_Scale),
        .anim_shift_off   = offsetof(AnimationHeader, m_ShortEncodeShift),
        .anim_ccount_off  = offsetof(AnimationHeader, m_NumColors),
        .anim_vcount_hi   = offsetof(AnimationHeader, m_VertCountHigh),
        .anim_vcount_lo   = offsetof(AnimationHeader, m_VertCountLow),
        .anim_radius      = offsetof(AnimationHeader, m_Radius),
        .anim_frames      = offsetof(AnimationHeader, m_Frames),
        .anim_frame_stride = (uint32_t)sizeof(AnimationFrame),

        /* The cull builders (regular func_8001F158 and specular func_800208FC
         * both) compute the queue base as g_SonyImage + 0x1600. */
        .rec_base         = G_SCRATCH + 0x1600u,
        .rec_stride       = 0x38u,
        .rec_header_off   = 0x04u,
        .rec_frame_a_off  = 0x08u,
        .rec_frame_b_off  = 0x0Cu,
        .rec_tr_off       = 0x10u,
        .rec_matrix_off   = 0x1Cu,
        .rec_tier_off     = 0x2Cu,   /* ZSF4 in the matrix word's high half */
        .rec_material_off = 0x30u,   /* m_SpecularMetalColor, copied by the cull */
        .rec_moby_off     = 0x34u,
        .blend_shift      = 2,       /* IR0 = (w0 & 0xFF00) >> 2 */
        .blend_mask       = 0x3FC0u,

        .cal_world_ot   = D_WORLDOT,
        .cal_ot_buckets = 0x800u,    /* r_moby.s links into 2048 buckets */

        /* The dragon-pedestal statue and its base disc. */
        .props  = { { 250u, -1 }, { 332u, -1 } },
        .prop_n = 2,

        .hook_cull          = 0u,    /* the cull is not hooked: it writes
                                      * m_WasDrawn, so the feeder reads its
                                      * records after the draw instead */
        .hook_draw_regular  = F_MOBY_DRAW_REGULAR,
        .hook_draw_specular = F_MOBY_DRAW_SPECULAR,
        .hook_draw_shaded   = F_MOBY_DRAW_SHADED,
        .hook_draw_simple   = 0u,    /* the shaded draw is the second kind's pass */
    },
    .player = {
        .base    = G_SPYRO,
        .rec_len = 0x2A4u,               /* the span the interp ring captures */
        .pos_off = 0x00u,                /* Vector3D m_Position */
        .rot_off = { 0x0Cu, 0x10u, 0x14u },   /* body / head / tail packed byte euler */
        .anim_off = { 0x18u, 0x1Au, 0x1Cu },  /* per-part animation index byte */
        .sort_depth_off    = 0x27u,      /* r_pete.s 80023F8C lbu 0x27: the ordering-table bias */
        .colour_filter_off = 0x28u,      /* byte 3 non-zero selects the pre-blend colour table */
        .emit_mode_off     = 0x30u,      /* non-zero selects the interpolated emit path */
        .hook_draw = F_PLAYER_DRAW,

        /* The setup pass builds one 0x44-byte record per part at
         * g_SonyImage + 0x1900 (r_pete.s 80023B?? addi 0x1900). */
        .part_base   = G_SCRATCH,
        .part_base_polybuf_off = 0u,
        .part_off    = 0x1900u,
        .part_stride = 0x44u,
        .part_count  = 3,
        .part_block_len = 0x100u,

        .part_flags_off    = 0x00u,
        .part_matrix_off   = 0x10u,      /* SHORTMATRIX R11R12..R33 */
        .part_tr_off       = 0x04u,
        .part_stream_a_off = 0x24u,      /* correction / 1-byte / 2-byte cursors */
        .part_stream_b_off = 0x34u,
        .part_stream_flag_bit = { 12, 13 },
        .part_matrix_sign  = 0x80000000u,/* the record carries its own matrix */
        .blend_mask        = 0x0FFFu,    /* the keyframe blend weight, 1.12 */

        .colour_scratch_off = 0x1D00u,   /* the pre-blend colour table, read to +0x1EEC */
        .colour_scratch_len = 0x300u,
        .sxy_base = 0x1F800000u,         /* the PSX scratchpad: every projected vertex's SXY */
        .sxy_len  = 0x400u,

        .flame_base  = G_SPYROFLAME,
        .flame_len   = 0xD0u,
        .shadow_base = D_SPYROSHADOW,
        .shadow_len  = 0x28u,

        .pair_bound = 400.0,             /* world units one record may move in a tick */
    },
    .flame = {
        .hook       = F_FLAME_DRAW,
        .attach_off = 0xB8u,             /* r_flame.s:80058E4C-80058E5C loads five words */
        .super_off  = 0x9Cu,             /* r_flame.s:80058E74 */
        .geom           = D_FLAME_GEOM,
        .geom_stride_lo = 0xC0u,         /* r_flame.s:80058EB8 */
        .geom_stride_hi = 0x100u,        /* r_flame.s:80058EC8 */
        .geom_hi_base   = 0x300u,        /* r_flame.s:80058ED0 */
        .conn           = D_FLAME_CONN,
        .conn_hi_off    = 0x80u,         /* r_flame.s:80058ED4 */
        .cap_colours    = D_FLAME_UV,
        .cap_amp        = 0x2Cu, .cap_amp_super   = 0x40u,  /* r_flame.s:80058F7C-80058F80 */
        .strip_amp      = 0x2Cu, .strip_amp_super = 0x40u,  /* r_flame.s:80059300-80059304 */
        .uv1_bias       = 0,             /* the strip's second texture word is read as stored */
        .cap_colour_off = flame_cap_colour_off,
    },
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
    },

    /* The HUD font: one moby class per glyph, digits 260-269, letters
     * 426-451, punctuation scattered; there is no colon model. Glyphs span
     * y -76..76. The game lights them with a dark back-colour plus a white
     * directional term added on top: measured on the Artisans HUD, back
     * colour (96,96,0) on unlit edges up to (214,214,118) at full facing, so
     * the add is 118 on every channel and there is no highlight term. The
     * period model sits at y 24..77, which the colon recentres. */
    .font = {
        .form        = GAME_FONT_CLASS_PER_GLYPH,
        .model_table = G_MODELS,
        .digit_base  = 260,
        .letter_base = 426,
        .punct       = { {'%', 272}, {'/', 277}, {'?', 278}, {'+', 317},
                         {'^', 321}, {'.', 327}, {'\'', 76} },
        .colon_recenter = -50.0f, .colon_spread = 44.0f, .colon_scale = 0.72f,
        .model_h     = 152.0f,
        .light_add   = { 118.0f, 118.0f, 118.0f },
        .hi_threshold = 2.0f, .hi_gain = 0.0f,      /* t <= 1: never opens */
        .fixed_light = { -0.34f, 0.50f, 0.80f },    /* unit, front-dominant */
        .fixed_tint  = 0x606000u,
    },
};

/* The 2D UI layer (games/common/renderer/ui_sprite.c). */
const ui_game_t spyro1_ui_game = {
    .ot_insert    = F_UI_OT_INSERT,
    .fade_emitter = F_UI_FADE,

    /* The pause / inventory / flight-results freeze quad: setRGB0(0x4C, 0x80,
     * 0x40) packed B,G,R, sampling the grayscale CLUT the desaturation writes
     * to VRAM (512, 224). */
    .freeze_tint   = 0x40804Cu,
    .freeze_clut_x = 512,
    .freeze_clut_y = 224,

    /* The title-screen UI upload: logo, "PRESS START" and the banner glyphs,
     * one 512x256 block at VRAM (512, 256). */
    .atlas_x = 512, .atlas_y = 256, .atlas_w = 512, .atlas_h = 256,
    .atlas_name = "title_ui",

    /* Text is drawn as text-moby models (renderer/text_moby.c), not as an SPRT
     * chain, so there is no self-linking emitter to hook. */
    .sprt_chain_emitter = { 0u, 0u, 0u },
};
