#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_src_tint.glsl"

/* WORLD-opaque depth PRE-PASS vertex stage (the shadow-decal occluder source).
 *
 * Same inputs as quad.vert, but the DEPTH written is TRUE PERSPECTIVE:
 *   z_ndc = 1 - 1/vz    (affine in 1/vz)
 * emitted as gl_Position.z = W - 1 with W = vz, so the fixed-function divide
 * yields it and the rasterizer's screen-linear interpolation of z_clip is
 * EXACT for planar surfaces - unlike the canonical scene curve
 * vz/(vz+4096), which is NOT affine in 1/vz: interpolating it screen-linearly
 * across a large terrain triangle keeps the VALUE error small but distorts
 * the local GRADIENT by up to the hyperbola's slope ratio (~70x over a
 * near-to-far floor tri) - which made shadow_decal.frag's neighbour-texel
 * NORMAL reconstruction read flat floors as fronto-parallel (ny ~ 0.08).
 * The pre-pass depth target is PRIVATE to the decal, so its encoding is free
 * to differ from the main scene depth; shadow_decal.frag decodes it with
 * vz = 1/(1-d). Depth-ORDERING is unchanged (1-1/vz is monotonic in vz). */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = ordering view-Z */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 4) in float inW;    /* true view-Z (see psx_scene.vert) */

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;    /* texel coords x vQ (divide by vQ) */
layout(location = 2) noperspective out vec2 vScreenPSX;
layout(location = 3) out float vQ;

/* PSX-affine texture mapping (rhi_set_affine_uv), as psx_scene.vert: the
 * textured cutout stage samples the texel the scene draw does. */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    /* TRUE view-Z, both as the depth encoding and the homogeneous W. The
     * decal reconstructs world geometry from this image, so it must encode
     * the surface's real distance - NOT the OT-shifted ordering value the
     * visible scene sorts by (a slot-shifted floor read as thousands of
     * units deeper drapes the shadow onto phantom ground). And the (z, W)
     * pair must be matched or the clipper corrupts the depth of partially
     * off-screen triangles (see psx_scene.vert). */
    float Wt = max(op_src_tint_w(inW), 1.0);
    gl_Position = vec4(inPos.xy * Wt, Wt - 1.0, Wt);  /* z_ndc = 1 - 1/Wt */
    vColor = inColor;
    /* W is the true view-Z, psx_scene.vert's UV perspective W, so plain
     * interpolation is its perspective mapping; UV and the ratio scaled by
     * W interpolate screen-linearly, its affine mapping. */
    float q = AFFINE_UV ? Wt : 1.0;
    vUV = inUV * q;
    vQ = q;
    vScreenPSX = inUV;
}
