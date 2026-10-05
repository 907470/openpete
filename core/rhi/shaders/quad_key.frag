#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

/* Untextured OPAQUE gouraud fragment stage with the terrain KEY-RESOLVE
 * contest - quad.frag plus the LP-vs-HP og-key discard (see
 * psx_scene_key.frag for the model). Used only by the opaque scene pipes
 * whose layout carries the HP key image at set 2. */

layout(set = 1, binding = 0) uniform sampler2D u_actorkey; /* moby og-key image */
layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* prop TRUE-depth image */
/* Moby og-RANK image - same-moby painter select; see psx_scene_key.frag. */
layout(set = 4, binding = 0) uniform sampler2D u_mobyrank;

layout(location = 0) in vec4 vColor;  /* x ratio (divide by vQ) */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;
layout(location = 4) flat in float vKoff;
layout(location = 5) flat in vec2 vRank;  /* x = own rank (0 = unranked), y = moby ordinal */

layout(location = 0) out vec4 outColor;

#ifdef OP_PORTAL_COVER
/* The _cover variant (blended terrain): the portal opening planes
 * portal_cover.glsl tests, after this pass's gl_FragCoord -> PSX screen
 * affine (screen x = fc.x * fragx + fc.y, screen y = fc.z * fragy + fc.w). */
layout(std140, set = 5, binding = 0) uniform PortalCover {
    vec4  cov_fc;
    ivec4 cov_cnt;
    dvec4 cov_margin;
    dvec4 cov_abc[6];
    vec4  cov_bbox[6];
    vec4  cov_poly[96];
} pcov;
#define OP_COVER_SRC pcov
#include "portal_cover.glsl"
#endif

/* gl_FragCoord.z convention - see psx_scene_key.frag. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;

/* Scope moby og-key yields by the fragment's own TRUE view-Z (see the
 * contest block). Default true; false restores og's compound painter. */
layout(constant_id = 3) const bool MOBY_TRUESCOPE = true;
/* Fragment spec-constant 4 (`SLOT_CONTEST`): the key contest compares
 * 128-world ordering-table slot indices and resolves an equal slot to the
 * terrain. Set per game from game_layout.terrain.slot_ot_contest. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;


void main()
{
    bool ranked = vRank.x > 0.5;
    float own = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
#ifdef OP_PORTAL_COVER
    /* A blended terrain fragment the portal preview covers (portal_cover.glsl). */
    if (op_portal_covered(vec2(pcov.cov_fc.x * gl_FragCoord.x + pcov.cov_fc.y,
                               pcov.cov_fc.z * gl_FragCoord.y + pcov.cov_fc.w), own))
        discard;
#endif
    if (ranked &&
        op_rank_loses(texelFetch(u_mobyrank, ivec2(gl_FragCoord.xy), 0), vRank, own))
        discard;
    /* Ranked faces skip the positive-koff contest - see psx_key_contest.glsl. */
    if (!ranked && abs(vKoff) > 0.25) {
        /* The additive glow fan (rank -2) runs the aura contest instead: it
         * orders against actor and player keys by ordering-table slot and
         * against terrain by its own true depth. */
        if (vRank.x < -1.5) {
            if (op_aura_loses(texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r,
                              vKoff - 0.5))
                discard;
        } else if (op_key_loses(
                op_key_from_stored(texelFetch(u_hpkey,    ivec2(gl_FragCoord.xy), 0).r),
                op_akey_contest(texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r),
                op_key_from_stored(texelFetch(u_mobykey,  ivec2(gl_FragCoord.xy), 0).r),
                own, vKoff,
                vRank.x < -0.5,     /* unranked moby faces mark rank -1 */
                op_rank_is_prop(vRank.x),
                MOBY_TRUESCOPE, SLOT_CONTEST))
            discard;
    }
    outColor = vColor / vQ;
}
