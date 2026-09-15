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
    },
    .fog = {
        .factor     = D_FOG_FACTOR,
        .far_colour = D_BLEND_TARGET,
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
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
    },
};
