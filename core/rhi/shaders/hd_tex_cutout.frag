#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"
#ifdef OP_MOBY_SHELL
/* The _shell variant (opaque ACTOR faces): moby_shell.glsl. */
#include "moby_shell.glsl"
#endif

/* HD replacement sampling for OPAQUE (depth-tested, depth-writing) scene
 * geometry: like hd_tex.frag but alpha works as a CUTOUT (discard below 0.5,
 * else opaque) - true alpha blending on opaque groups would break Z ordering.
 * Semi-transparent HD edges harden slightly. */

layout(set = 0, binding = 0) uniform sampler2D u_hd;
layout(set = 1, binding = 0) uniform sampler2D u_actorkey; /* moby og-key image */
/* Owner identities of the actor key image; see psx_scene_key.frag. */
layout(set = 1, binding = 1) uniform sampler2D u_actorown;
layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
/* Drain sub-cell key records (psx_key_contest.glsl op_drain_record), the
 * terrain key image's set at binding 1. */
layout(set = 2, binding = 1) uniform usampler2D u_drain;
/* The terrain key owner's true view-Z and class (op_key_attr_store), the
 * terrain key image's colour, at the set's binding 2. */
layout(set = 2, binding = 2) uniform sampler2D u_hpattr;
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* prop TRUE-depth image */
/* Moby og-RANK image - same-moby painter select; see psx_scene_key.frag. */
layout(set = 4, binding = 0) uniform sampler2D u_mobyrank;

layout(push_constant) uniform HdRect {
    float u0, v0, uw, vh;   /* replaced texel box within the texture page */
    vec4 fogA, fogB;        /* per-channel fog transform; identity = (1,1,1)/(0,0,0) */
    vec4 fogAN, fogBN;      /* deepest-ladder-row affine; fogAN.a > 0 enables the
                             * CONTINUOUS model (see fog block in main) */
    vec4 fogLad;            /* x,y: per-fragment ramp t = clamp(w*x + y, 0, 1) */
} pc;

layout(location = 0) in vec2 vUVq;    /* x ratio (divide by vQ) */
layout(location = 1) in vec4 vColorq; /* x ratio (divide by vQ) */
layout(location = 2) in float vQ;     /* perspective ratio (see psx_scene.vert) */
layout(location = 3) flat in float vKoff; /* terrain og-key offset (see psx_scene_key.frag) */
layout(location = 4) flat in vec2 vRank;  /* x = own rank (0 = unranked), y = moby ordinal */

layout(location = 0) out vec4 outColor;

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

#ifdef OP_PORTAL_COVER
/* The _rim variant (portal inner-rim terrain): portal_cover_scene.glsl. */
#include "portal_cover_scene.glsl"
#endif

void main()
{
#if defined(OP_MOBY_SHELL) || defined(OP_ACTOR_TIE)
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
    gl_FragDepth = zout;
#endif
#ifdef OP_PORTAL_COVER
    if (op_portal_cover_frag(DEPTH_REVERSED)) discard;
#endif
    bool ranked = vRank.x > 0.5;
    if (ranked) {   /* ranked moby face: NEAR-TIE-scoped same-moby painter
                     * select (rules in psx_scene_key.frag) */
        vec4 e = texelFetch(u_mobyrank, ivec2(gl_FragCoord.xy), 0);
        float irank = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
        float iord  = floor(e.b * 255.0 + 0.5);
        if (abs(iord - vRank.y) < 0.5 && irank > vRank.x + 0.5) {
            float ownerVz = e.a * 32640.0;
            float ownVz = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
            if (abs(ownerVz - ownVz) < 384.0) discard;
        }
    }
    /* Terrain KEY-RESOLVE: same contest as psx_scene_key.frag so
     * HD-replaced terrain keeps og's per-pixel face select. Ranked faces
     * skip the positive-koff contest - see psx_scene_key.frag. */
    float koff = op_drain_koff(vKoff, vRank.y, vUVq / vQ, u_drain);
    if (op_drain_discards(koff)) discard;   /* a sub-cell the drain skips */
    if (!ranked && abs(koff) > 0.25 &&
        op_key_loses(
            op_key_from_stored(texelFetch(u_hpkey, ivec2(gl_FragCoord.xy), 0).r),
            texelFetch(u_hpattr, ivec2(gl_FragCoord.xy), 0),
            op_akey_contest(op_akey_select(
                texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r,
                texelFetch(u_actorown, ivec2(gl_FragCoord.xy), 0),
                op_akey_owner(vRank))),
            op_key_from_stored(texelFetch(u_mobykey, ivec2(gl_FragCoord.xy), 0).r),
            op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED), koff,
            op_key_fattr_walk(vRank.x),
            vRank.x < -0.5,     /* unranked moby faces mark rank -1 */
            op_rank_is_prop(vRank.x),
            MOBY_TRUESCOPE, SLOT_CONTEST))
        discard;
    vec2 vUV = vUVq / vQ;
    vec4 vColor = vColorq / vQ;
    vec2 t = vec2((vUV.x - pc.u0) / pc.uw, (vUV.y - pc.v0) / pc.vh);
    vec4 c = texture(u_hd, clamp(t, 0.0, 1.0));
    if (c.a < 0.5) discard;
    /* See hd_tex.frag: re-apply the CLUT-ladder fog a replacement can't get
     * from the palette. This is the pipe that terrain actually draws through.
     * Continuous model (fogAN.a > 0): the game selects ONE fogged palette row
     * per FACE from the face-mean fog key - on stock art adjacent rows fade
     * gently, but reproduced as per-face affines on replacement art the row
     * steps read as toothed bands marching along the fog contour. Evaluate
     * the same ramp per FRAGMENT instead: fade identity -> the deepest-row
     * affine by t from the true view-Z (decoded from the depth this fragment
     * wrote - terrain stores TRUE depth), which is the fade the rows
     * quantize, minus the steps. */
    if (pc.fogAN.a > 0.0) {
        float w = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
        float t = clamp(w * pc.fogLad.x + pc.fogLad.y, 0.0, 1.0);
        c.rgb = clamp(c.rgb * mix(vec3(1.0), pc.fogAN.rgb, t) + pc.fogBN.rgb * t,
                      0.0, 1.0);
    } else {
        c.rgb = clamp(c.rgb * pc.fogA.rgb + pc.fogB.rgb, 0.0, 1.0);
    }
    vec3 rgb = clamp(c.rgb * vColor.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, 1.0);
}
