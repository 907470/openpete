#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_unorm8.glsl"
#include "psx_viewz.glsl"
#include "psx_src_tint.glsl"

/* Scene textured vertex stage - PERSPECTIVE-CORRECT UV/colour with
 * CLIP-INVARIANT depth.
 *
 * The CPU feeds post-projection 2D screen coordinates (already in clip space
 * via scene_to_clip) plus two view-Z values (psx_viewz.glsl), which are NOT
 * always the same:
 *
 *   - vzo: the ORDERING view-Z the depth is written from (the depth curve is
 *     is op_scene_clip, selected by DEPTH_REVERSED below). Feeders may shift it to port the PSX
 *     OT's authored ordering (terrain slot offsets, moby tie pulls).
 *   - inW: the TRUE view-Z (perspective W) for UV/colour interpolation.
 *
 * The homogeneous position MUST use W == vzo. gl_Position's (zc, W) pair is
 * linearly interpolated by the hardware CLIPPER; only a matched pair keeps
 * window depth screen-linear through clipping. An earlier version emitted
 * W = inW with zc built from vzo - exact per vertex, but any partially
 * off-screen shifted triangle got its clip-vertex depth perspective-mixed
 * toward the near vertex, which could pull a distant face in FRONT of
 * everything (a screen-huge lake face with one near-camera vertex covered
 * the ground with its own texture wherever the clipper ran).
 *
 * UV/colour still interpolate perspective-correct in inW, exactly: emit
 * attr*q with q = vzo/inW plus the scalar q itself. The rasterizer divides
 * varyings by W=vzo, so attr*q/vzo == attr/inW and q/vzo == 1/inW are both
 * screen-linear; the fragment stage recovers attr = (attr*q)/q - identical
 * to hardware W=inW interpolation on unclipped triangles, and exact on
 * clipped ones. */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = ordering view-Z */
layout(location = 1) in vec2 inUV;    /* texel coords within the texture page */
layout(location = 2) in vec4 inColor; /* R8G8B8A8_UNORM -> 0..1 modulate */
/* True view-Z (equal to inPos.z unless the feeder shifted z to encode OT
 * ordering). The depth WRITTEN always follows inPos.z - only the UV/colour
 * interpolation basis changes. */
layout(location = 4) in float inW;
/* Terrain og OT key (rhi_vertex.koff): the face's ABSOLUTE .s insertion key
 * in world units + 0.5 sentinel (0 = not keyed); keyed fragments run the
 * key-resolve contest against it. Per-face constant -> flat. */
layout(location = 5) in float inKoff;
/* Moby og paint RANK (rhi_vertex.sx,sy): x = the face's OT walk rank (>=1;
 * 0 = unranked), y = the present-local moby ordinal. Ranked fragments run
 * the same-moby painter select against the rank image (set 4). Per-face
 * constant -> flat. */
layout(location = 3) in vec2 inRank;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
/* Interpolation ratio (vzo/inW): fragment divides vUV/vColor by this to
 * recover perspective-correct-in-inW values. 1.0 on unshifted vertices. */
layout(location = 2) out float vQ;
layout(location = 3) flat out float vKoff;
layout(location = 4) flat out vec2 vRank;
/* 1 on a walked terrain face under the source tint (psx_src_tint.glsl). */
layout(location = 10) flat out float vSrcWalk;

/* Scene depth convention, chosen at pipeline creation (RHI spec constant 0,
 * from the pipeline's depth class and the device's depth format): true =
 * reversed z_ndc = 1/vz, false = forward z_ndc = 1 - 1/vz (op_scene_clip). */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;

/* PSX-affine interpolation (--texture-mapping affine, spec-constant 1): emit
 * w=1 so the rasterizer interpolates UV/colour SCREEN-LINEARLY - the PSX GPU
 * had no perspective divide, and this is its authentic texture swim/warp.
 * Screen position is inPos.xy either way, and the window depth is the same
 * value the perspective path's hardware divide produces, so depth
 * testing/writing is identical across the two modes. */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    float vzo = op_scene_vz(inPos.z);                /* ordering view-Z (depth) */
    float Wu  = max(op_src_tint_w(inW), 1.0);         /* UV/colour perspective W */
    float q   = AFFINE_UV ? 1.0 : (vzo / Wu);
    gl_Position = op_scene_clip(inPos.xy, vzo, AFFINE_UV, DEPTH_REVERSED);
    vUV = inUV * q;
    vColor = op_unorm8(inColor) * q;
    vQ = q;
    vKoff = inKoff;
    vRank = inRank;
    vSrcWalk = op_src_tint_walked(inW);
}
