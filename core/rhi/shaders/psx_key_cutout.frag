#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"
#include "psx_clut.glsl"
#include "psx_texel.glsl"

/* TEXTURED key-pass fragment stage: alpha-aware og-key rasterisation.
 *
 * A keyed face's og OT occupancy exists only where its OPAQUE texels land -
 * the PSX p==0 sentinel is fully transparent on every prim, so whatever sits
 * behind those texels stays visible in the original painter. Rasterising a
 * textured face's key across its full footprint (the untextured key pipe's
 * behaviour) lets a mostly-transparent face - a cloak edge, a fence lattice -
 * stamp a near key over its whole quad and key-discard everything behind the
 * holes, leaving nothing to draw there but the backdrop.
 *
 * The stored value is written as the fragment depth in the face's form
 * (terrain_key.vert vKeyForm, op_key_stored_depth): the flat og key for the
 * key images, the true view-Z at the fragment for the prop image, the band
 * value for the player band; this stage also cuts transparent texels out of
 * the raster. outColor is the face's flat colour channel,
 * which carries an actor face's moby identity into the actor key image's
 * colour attachment (psx_key_contest.glsl op_akey_id); a terrain face
 * (vCls a class byte) writes its true view-Z and class there instead
 * (op_key_attr_store), and the prop image's colour is never read. Texel decode mirrors psx_scene_key.frag;
 * UVs interpolate against the position's own W (flat key => screen-affine,
 * the PSX rasteriser's own sampling; true-depth prop faces get perspective
 * UV, matching the scene draw). A triangle with a drain record writes the key
 * of the drain sub-cell under the fragment (psx_key_contest.glsl; a folded
 * quadrant's skipped slot through op_drain_fold_rec) as its depth; every
 * flat key, a drain sub-cell's included, is first moved in its bucket's
 * tier order (op_key_tier_key) and stored through op_key_store. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;
    int clut_x,  clut_y;
    int bpp;
} pc;

layout(location = 1) in vec2 vUV;   /* u/w - see terrain_key.vert */
layout(location = 3) in float vQ;   /* 1/w */
layout(location = 4) flat in float vDrain; /* drain record tag */
layout(location = 5) flat in float vKv;    /* flat key view-Z, or the band value */
/* Drain sub-cell key records (psx_key_contest.glsl op_drain_record). */
layout(set = 1, binding = 0) uniform usampler2D u_drain;

layout(location = 6) flat in vec4 vIdent; /* moby identity (op_akey_id) */
layout(location = 7) noperspective in float vIz; /* owner's true 1/view-Z */
layout(location = 8) flat in float vCls;          /* owner's terrain class byte */
layout(location = 9) flat in float vKeyForm;      /* OP_KEY_FORM_* */

layout(location = 0) out vec4 outColor;

/* Fragment spec-constant 4 (`SLOT_CONTEST`): the scene is submitted through
 * one ordering table; a terrain key is stored in its bucket's tier order
 * (op_key_tier_key). Set per game from game_layout.terrain.slot_ot_contest. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;

uint sample_word(int u, int v)
{
    return op_texel_word(u_vram, ivec2(pc.tpage_x, pc.tpage_y),
                         ivec2(pc.clut_x, pc.clut_y), pc.bpp, u, v);
}

void main()
{
    vec2 uv = vUV / vQ;
    ivec2 ti = op_texel(uv, op_texel_grad(uv));
    /* bpp < 0: a material-run overflow tail, keyed full-footprint. */
    if (pc.bpp >= 0 && sample_word(ti.x, ti.y) == 0u) discard;
    uvec4 t0, t1;
    vec2 p;
    float d = op_drain_record(u_drain, vDrain, uv, t0, t1, p) ? op_drain_delta(p, t0, t1, 0.0) : -1.0;
    if (d >= OP_DRAIN_SKIP) d = op_drain_fold_rec(u_drain, vDrain, uv, t0, t1, 0.0);
    if (d >= OP_DRAIN_SKIP) discard;           /* a sub-cell the drain skips */
    float form = d >= 0.0 ? OP_KEY_FORM_FLAT : vKeyForm;
    float kv = form > 0.5 ? op_key_tier_key(vKv + max(d, 0.0), vCls, SLOT_CONTEST) : vKv;
    gl_FragDepth = op_key_stored_depth(form, kv, vIz);
    outColor = vCls < OP_KEY_CLASS_NONE - 0.5 ? op_key_attr_store(1.0 / max(vIz, 1e-9), vCls)
                                              : vIdent;
}
