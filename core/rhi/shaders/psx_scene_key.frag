#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"
#include "psx_clut.glsl"

/* Textured OPAQUE scene fragment stage with the terrain KEY-RESOLVE contest.
 *
 * Identical PSX VRAM decode + modulation to psx_scene.frag, plus: a keyed
 * terrain fragment (vKoff > 0, the face's ABSOLUTE og OT key + 0.5 sentinel)
 * survives only where no strictly-nearer key owns the pixel in the key
 * images (moby keys additionally scoped by the fragment's own TRUE view-Z
 * in free-camera mode - see the contest block). The og key of a
 * face is TRUE view-Z + 128*slots (the authored OT offsets); stored depth
 * itself is TRUE view-Z - og's keys decide WHICH terrain surface exists at
 * a pixel, true geometry decides everything against that surface. The
 * face's own key rides the flat vKoff (exact); only the image side is
 * quantised. Non-terrain fragments (vKoff == 0) never touch the sampler. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */
layout(set = 1, binding = 0) uniform sampler2D u_actorkey; /* moby og-key image */
/* The moby identity owning each actor key texel (op_akey_id), the actor key
 * image's colour attachment. */
layout(set = 1, binding = 1) uniform sampler2D u_actorown;
layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
/* Drain sub-cell key records (psx_key_contest.glsl op_drain_record), the
 * terrain key image's set at binding 1. */
layout(set = 2, binding = 1) uniform usampler2D u_drain;
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* prop TRUE-depth image */
/* Moby og-RANK image (RGBA8): r+g*256 = the winning face's OT walk rank at
 * this pixel (max rank wins the raster), b = that face's present-local moby
 * ordinal. Same-moby painter select: og paints a moby's faces in walk order,
 * so a fragment loses its pixel iff a LATER-ranked face of the SAME moby
 * covers it - regardless of true depth. Cross-moby stays true per-pixel z
 * (the ordinal gate). Stored depth is TRUE view-Z either way - the rank
 * decides WHICH same-moby face exists at a pixel, never what depth it has. */
layout(set = 4, binding = 0) uniform sampler2D u_mobyrank;

#ifdef OP_PORTAL_COVER
/* The _cover variant (blended terrain): portal_cover_scene.glsl. */
#include "portal_cover_scene.glsl"
#endif

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;
    int clut_x,  clut_y;
    int bpp;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vQ;
layout(location = 3) flat in float vKoff;
layout(location = 4) flat in vec2 vRank;  /* x = own rank (0 = unranked), y = moby ordinal
                                           * (ranked), moby identity (unranked actor),
                                           * or drain record tag (terrain) */

layout(location = 0) out vec4 outColor;

/* Depth convention of gl_FragCoord.z (the vertex stage's selection, see
 * op_scene_clip): needed to invert the fragment's own window depth back to
 * view-Z for the near-band per-pixel contest key. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;

/* Scope moby og-key yields by the fragment's own TRUE view-Z (see the
 * contest block). Default true; false restores og's compound painter. */
layout(constant_id = 3) const bool MOBY_TRUESCOPE = true;
/* Fragment spec-constant 4 (`SLOT_CONTEST`): the key contest compares
 * 128-world ordering-table slot indices and resolves an equal slot to the
 * terrain. Set per game from game_layout.terrain.slot_ot_contest. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;


/* Untextured lighting view - see psx_scene.frag. */
layout(constant_id = 2) const bool NOTEX = false;

uint sample_word(int u, int v)
{
    return op_texel_word(u_vram, ivec2(pc.tpage_x, pc.tpage_y),
                         ivec2(pc.clut_x, pc.clut_y), pc.bpp, u, v);
}

/* The second fogged-CLUT row of a textured face (op_clut_fog_mix); the
 * untextured lighting view keeps the vertex colour alone. */
vec3 clut_fog_mix(vec3 rgb, vec4 col, int u, int v)
{
    if (NOTEX) return rgb;
    return op_clut_fog_mix(u_vram, rgb, col, ivec2(pc.tpage_x, pc.tpage_y),
                           ivec2(pc.clut_x, pc.clut_y), pc.bpp, u, v);
}

void main()
{
    /* Set 2 = TERRAIN og KEYS (keyed WORLD faces as their ABSOLUTE og key
     * laid flat - og's per-pixel OT occupancy). Set 1 = MOBY og KEYS
     * (keyed opaque ACTOR faces, same encoding). Set 3 = PROP TRUE DEPTH
     * (curated embedded-prop faces only). Set 4 = the moby og-RANK raster.
     * The verdicts themselves live in psx_key_contest.glsl. */
    bool ranked = vRank.x > 0.5;
    float own = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
#ifdef OP_PORTAL_COVER
    /* A blended terrain fragment the portal preview covers. */
    if (op_portal_cover_frag(DEPTH_REVERSED)) discard;
#endif
    if (ranked &&
        op_rank_loses(texelFetch(u_mobyrank, ivec2(gl_FragCoord.xy), 0), vRank, own))
        discard;
    /* A RANKED face skips the positive-koff painter contest: rank IS og's
     * exact same-moby painter. Its rank pair carries the rank-image ordinal,
     * not the moby identity, so the contest's own-moby skip
     * (op_akey_select) cannot apply to it - large same-moby faces (jaw vs
     * chin) sit beyond the 64-world margin, so the contest would kill the
     * rank winner while the select kills everything under it (a hole to the
     * clear). The face still STAMPS its key, so keyed terrain keeps yielding
     * to it. */
    float koff = op_drain_koff(vKoff, vRank.y, vUV / vQ, u_drain);
    if (op_drain_discards(koff)) discard;   /* a sub-cell the drain skips */
    if (!ranked && abs(koff) > 0.25) {
        /* The additive glow fan (rank -2) runs the aura contest instead -
         * see psx_key_contest.glsl. */
        if (vRank.x < -1.5) {
            if (op_aura_loses(texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r,
                              koff - 0.5))
                discard;
        } else if (op_key_loses(
                op_key_from_stored(texelFetch(u_hpkey,    ivec2(gl_FragCoord.xy), 0).r),
                op_akey_contest(op_akey_select(
                    texelFetch(u_actorkey,  ivec2(gl_FragCoord.xy), 0).r,
                    texelFetch(u_actorown,  ivec2(gl_FragCoord.xy), 0),
                    op_akey_owner(vRank))),
                op_key_from_stored(texelFetch(u_mobykey,  ivec2(gl_FragCoord.xy), 0).r),
                own, koff,
                vRank.x < -0.5,     /* unranked moby faces mark rank -1 */
                op_rank_is_prop(vRank.x),
                MOBY_TRUESCOPE, SLOT_CONTEST))
            discard;
    }
    vec2 uv = vUV / vQ;
    vec4 col = vColor / vQ;
    vec4 c = op_decode555(sample_word(int(uv.x), int(uv.y)));
    if (c.a == 0.0) discard;
    if (NOTEX) c.rgb = vec3(128.0 / 255.0);
    vec3 rgb = clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    rgb = clut_fog_mix(rgb, col, int(uv.x), int(uv.y));
    outColor = vec4(rgb, c.a);
}
