// games/spyro-1/renderer/layout.c — the Spyro 1 engine layout. Bases come
// from the drift-checked symbol aliases; member offsets from the generated
// typed header, whose _Static_asserts pin them to the probed MIPS layout.

#include "game_layout.h"
#include "spyro1_symbols.gen.h"
#include "openpete_game_structs.h"

#include <stddef.h>

/* g_EnvironmentAnimations precedes g_Environment (environment.h); the
 * generated env-anim walker func_8002A6FC indexes it as t9+0x10.. */
#define G_ENVANIM (G_ENVIRONMENT - (uint32_t)sizeof(EnvironmentAnimations))

const game_layout_t game_layout = {
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
    },
    .env_anim = {
        .base    = G_ENVANIM,
        .scroll  = G_ENVANIM + offsetof(EnvironmentAnimations, m_ScrollingTextureCount),
        .lp_poly = G_ENVANIM + offsetof(EnvironmentAnimations, m_LowPolyAnimationCount),
        .hp_poly = G_ENVANIM + offsetof(EnvironmentAnimations, m_HighPolyAnimationCount),
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
        .defer_q      = { G_SCRATCH + 0x2000u, G_SCRATCH + 0x1E00u },
        .face_layout  = TERRAIN_FACES_SPYRO1,
        .tex_stride   = 16u,
        .abe_in_tpage = 0,
        .tile_word1_br = 0,
        .lod_dist_default = 0u,
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
    },
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
    },
};
