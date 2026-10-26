#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_viewz.glsl"
#include "psx_key_contest.glsl"

/* Terrain KEY pass vertex stage: rasterise opaque keyed terrain with depth =
 * the og OT KEY (true view-Z + 128*slots world) instead of true view-Z, into
 * the depth image the scene fragments contest against
 * (psx_scene_key.frag / quad_key.frag / hd_tex_cutout.frag).
 *
 * Every fragment stage writes the stored value itself as its fragment depth
 * (vKeyForm): a flat key as op_key_store of the face's key (psx_key_contest.glsl,
 * an exact power-of-two scale), the prop class as op_key_store of its true
 * view-Z at the fragment (1/vIz, perspective-correct), the player band as
 * the band value the feed carries. The rasterised window depth is used for
 * clipping only (op_key_clip), so no stored key depends on a GPU's
 * rasteriser arithmetic. Keys are FLAT per face (all three verts carry the
 * same kv); the (zc, W) pair uses W == the key view-Z so the clipper
 * interpolates the face without disturbing it (the scene stages'
 * clip-invariance rule), and the owner's 1/view-Z (vIz) interpolates
 * screen-linearly to its exact value at the fragment. A PLAYER face in the
 * actor batch carries the player band's stored value as its z instead of a
 * view-Z. A textured triangle with a drain record writes its fragment depth
 * from the key of the drain sub-cell under the fragment instead
 * (psx_key_cutout.frag). */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = flat key view-Z or player band value */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 4) in float inW;    /* TRUE per-vertex view-Z (textured cutout UV) */
/* |inKoff| - 0.5: the key offset; the sign is the depth form (positive: a
 * flat key the fragment stage stores from vKv; negative: the prop class's
 * per-vertex true depth, stored from the fragment's true view-Z). */
layout(location = 5) in float inKoff;
/* y: the drain record tag (psx_key_contest.glsl op_drain_record). */
layout(location = 3) in vec2 inRank;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) noperspective out vec2 vScreenPSX;
layout(location = 3) out float vQ;
layout(location = 4) flat out float vDrain;   /* the drain record tag */
layout(location = 5) flat out float vKv;      /* the flat key view-Z, or the band value */
/* The face's colour channel, flat: an actor face's moby identity in r, g, b
 * (op_akey_id), written beside its key into the key image's colour. */
layout(location = 6) flat out vec4 vIdent;
/* The owner's true 1/view-Z, screen-linear (exact for a plane under the
 * flat key W), and its terrain class byte (op_key_class_walk of the rank
 * channel; OP_KEY_CLASS_NONE off the terrain class): a terrain key's
 * fragment writes both beside the key (op_key_attr_store). */
layout(location = 7) noperspective out float vIz;
layout(location = 8) flat out float vCls;
/* The stored value's form (OP_KEY_FORM_*, psx_key_contest.glsl
 * op_key_stored_depth): a flat key, the player band, or the prop class's
 * true depth. */
layout(location = 9) flat out float vKeyForm;

/* PSX-affine UV (--texture-mapping affine, spec-constant 1) - must track the
 * SCENE stages' selection: the cutout fragment's texel lookups have to land on
 * the same texels the scene draw samples, or a key raster stamps where the
 * face's own paint discards (a one-texel hole to the backdrop). The position W
 * here is the FLAT key (per-face), so hardware interpolation is screen-linear;
 * perspective UV is recovered manually via the classic (u/w, 1/w) pair in
 * vUV/vQ (w = the TRUE per-vertex view-Z in inW). */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    float vz = op_scene_vz(inPos.z);
    float kv = vz + max(abs(inKoff) - 0.5, 0.0);   /* og key view-Z */
    gl_Position = op_key_clip(inPos.xy, inPos.z, kv);   /* z_ndc = op_key_store(kv), or the band value */
    float wq = AFFINE_UV ? 1.0 : max(inW, 1.0);
    vColor = inColor;
    vUV = inUV / wq;
    vScreenPSX = inUV;
    vQ = 1.0 / wq;
    vDrain = inRank.y;
    bool band = op_pkey_is(inPos.z);
    vKv = band ? inPos.z : kv;
    vIdent = inColor;
    vIz = 1.0 / max(inW, 1.0);
    vCls = op_key_class_walk(inRank.x);
    vKeyForm = band ? OP_KEY_FORM_BAND : inKoff > 0.0 ? OP_KEY_FORM_FLAT : OP_KEY_FORM_TRUE;
}
