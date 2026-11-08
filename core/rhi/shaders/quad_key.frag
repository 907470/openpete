#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"
#include "psx_src_tint.glsl"
#ifdef OP_MOBY_SHELL
/* The _shell variant (opaque ACTOR faces): moby_shell.glsl. */
#include "moby_shell.glsl"
#endif
#ifdef OP_MOBY_SNAP
/* The _snap variants (ACTOR faces): the sunk-pickup depth snap. */
#include "psx_moby_snap.glsl"
#endif

/* Untextured OPAQUE gouraud fragment stage with the terrain KEY-RESOLVE
 * contest - quad.frag plus the LP-vs-HP og-key discard (see
 * psx_scene_key.frag for the model). Used only by the opaque scene pipes
 * whose layout carries the HP key image at set 2. */

layout(set = 1, binding = 0) uniform sampler2D u_actorkey; /* moby og-key image */
/* Owner identities of the actor key image; see psx_scene_key.frag. */
layout(set = 1, binding = 1) uniform sampler2D u_actorown;
/* The per-pixel actor key image (each opaque actor face at its per-pixel
 * moby-against-moby key, op_actor_key) and its owner colour (op_akey_id). */
layout(set = 1, binding = 2) uniform sampler2D u_actorpkey;
layout(set = 1, binding = 3) uniform sampler2D u_actorpown;
layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
/* The terrain key owner's true view-Z and class (op_key_attr_store), the
 * terrain key image's colour, at the set's binding 2. */
layout(set = 2, binding = 2) uniform sampler2D u_hpattr;
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* prop TRUE-depth image */
/* Moby og-RANK image - same-moby painter select; see psx_scene_key.frag. */
layout(set = 4, binding = 0) uniform sampler2D u_mobyrank;

layout(location = 0) in vec4 vColor;  /* x ratio (divide by vQ) */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;
layout(location = 4) flat in float vKoff;
layout(location = 5) flat in vec2 vRank;  /* x = own rank (0 = unranked), y = moby ordinal
                                           * (ranked) or moby identity (unranked actor) */

layout(location = 6) flat in float vAkc; /* moby-against-moby key offset (op_actor_key) */
layout(location = 0) out vec4 outColor;
/* 1 on a walked terrain face under the source tint (psx_src_tint.glsl). */
layout(location = 10) flat in float vSrcWalk;

#ifdef OP_PORTAL_COVER
/* The _cover variant (blended terrain): portal_cover_scene.glsl. */
#include "portal_cover_scene.glsl"
#endif

/* gl_FragCoord.z convention - see psx_scene_key.frag. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;

/* Scope moby og-key yields by the fragment's own TRUE view-Z (see the
 * contest block). Default true; false restores og's compound painter. */
layout(constant_id = 3) const bool MOBY_TRUESCOPE = true;
/* Fragment spec-constant 4 (`SLOT_CONTEST`): the key contest compares
 * 128-world ordering-table slot indices for terrain against terrain, an
 * equal slot keeping the fragment, and leaves terrain against an actor face
 * to true depth. Set per game from game_layout.terrain.slot_ot_contest. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;


void main()
{
#if defined(OP_MOBY_SHELL) || defined(OP_ACTOR_TIE) || defined(OP_MOBY_SNAP)
    /* Written before any discard; a discarded fragment writes nothing. */
    float zout = gl_FragCoord.z;
#ifdef OP_MOBY_SHELL
    zout = op_shell_depth(vRank, DEPTH_REVERSED);
#endif
#ifdef OP_ACTOR_TIE
    zout = op_actor_tie_depth(zout, vRank, vKoff,
        op_key_from_stored(texelFetch(u_hpkey, ivec2(gl_FragCoord.xy), 0).r),
        SLOT_CONTEST, DEPTH_REVERSED);
#endif
#ifdef OP_MOBY_SNAP
    zout = op_snap_depth(zout, vRank, vKoff,
        op_key_from_stored(texelFetch(u_hpkey, ivec2(gl_FragCoord.xy), 0).r),
        texelFetch(u_hpattr, ivec2(gl_FragCoord.xy), 0), DEPTH_REVERSED);
#endif
    gl_FragDepth = zout;
#endif
#if defined(OP_MOBY_SNAP) && defined(OP_PORTAL_COVER)
    float ckoff = op_snapb_contest_koff(vKoff, vRank);   /* the blended snap twin */
#else
    float ckoff = vKoff;
#endif
    bool ranked = vRank.x > 0.5;
    float own = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
#ifdef OP_PORTAL_COVER
    /* A blended terrain fragment the portal preview covers. */
    if (op_portal_cover_frag(DEPTH_REVERSED)) discard;
#endif
    if (ranked &&
        op_rank_loses(texelFetch(u_mobyrank, ivec2(gl_FragCoord.xy), 0), vRank, own))
        discard;
    /* Ranked faces skip the positive-koff contest - see psx_key_contest.glsl. */
#if defined(OP_PORTAL_COVER) && !defined(OP_PORTAL_RIM) && !defined(OP_MOBY_SNAP)
    /* The blended world variant: an unranked non-actor face is terrain. */
    op_key_blend_frag = !ranked && vRank.x > -0.5;
#endif
    if (!ranked && abs(ckoff) > 0.25) {
        /* The additive glow fan (rank -2) runs the aura contest instead: it
         * orders against actor and player keys by ordering-table slot and
         * against terrain by its own true depth. */
        if (vRank.x < -1.5) {
            if (op_aura_loses(texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r,
                              ckoff - 0.5))
                discard;
        } else {
          if (op_key_loses(
                op_key_from_stored(texelFetch(u_hpkey,    ivec2(gl_FragCoord.xy), 0).r),
                texelFetch(u_hpattr, ivec2(gl_FragCoord.xy), 0),
                op_akey_contest(op_akey_select(
                    texelFetch(u_actorkey,  ivec2(gl_FragCoord.xy), 0).r,
                    texelFetch(u_actorown,  ivec2(gl_FragCoord.xy), 0),
                    op_akey_owner(vRank))),
                op_akey_contest(op_akey_select(
                    texelFetch(u_actorpkey, ivec2(gl_FragCoord.xy), 0).r,
                    texelFetch(u_actorpown, ivec2(gl_FragCoord.xy), 0),
                    op_akey_owner(vRank))),
                op_key_from_stored(texelFetch(u_mobykey,  ivec2(gl_FragCoord.xy), 0).r),
                own, ckoff, vAkc, op_key_fattr_walk(vRank.x),
                vRank.x < -0.5,     /* unranked moby faces mark rank -1 */
                op_rank_is_prop(vRank.x),
                MOBY_TRUESCOPE, SLOT_CONTEST))
            discard;
        }
    }
    /* A terrain-order moby fragment yields to terrain keyed into its slot
     * or nearer (op_terrain_order_loses). */
    if (op_terrain_order_loses(op_key_from_stored(texelFetch(u_hpkey, ivec2(gl_FragCoord.xy), 0).r),
                               ckoff, vRank.x, SLOT_CONTEST))
        discard;
    /* A terrain fragment (unranked, not an actor or glow-fan face) keyed
     * behind a key-order moby's key yields to it (op_key_order_yields). */
    int ko = (!ranked && vRank.x > -0.5)
        ? op_key_order_yields(texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r,
                              texelFetch(u_actorown, ivec2(gl_FragCoord.xy), 0),
                              ckoff, SLOT_CONTEST)
        : 0;
    if (ko == 1) discard;
    outColor = ko == 2 ? vec4(1.0, 0.0, 1.0, 1.0) : vColor / vQ;
    outColor = op_src_tint_scene(outColor, vSrcWalk);
    outColor = op_key_blend_paint(outColor);
}
