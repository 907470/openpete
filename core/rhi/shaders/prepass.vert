#version 450

/* WORLD-opaque depth PRE-PASS vertex stage (the shadow-decal occluder source).
 *
 * Same inputs as quad.vert, but the DEPTH written is TRUE PERSPECTIVE:
 *   z_ndc = 1 - 1/vz    (affine in 1/vz)
 * emitted as gl_Position.z = W - 1 with W = vz, so the fixed-function divide
 * yields it and the rasterizer's screen-linear interpolation of z_clip is
 * EXACT for planar surfaces — unlike the canonical scene curve
 * vz/(vz+4096), which is NOT affine in 1/vz: interpolating it screen-linearly
 * across a large terrain triangle keeps the VALUE error small but distorts
 * the local GRADIENT by up to the hyperbola's slope ratio (~70x over a
 * near-to-far floor tri) — which made shadow_decal.frag's neighbour-texel
 * NORMAL reconstruction read flat floors as fronto-parallel (ny ~ 0.08).
 * The pre-pass depth target is PRIVATE to the decal, so its encoding is free
 * to differ from the main scene depth; shadow_decal.frag decodes it with
 * vz = 1/(1-d). Depth-ORDERING is unchanged (1-1/vz is monotonic in vz). */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = canonical NDC depth */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 4) in float inW;    /* true view-Z when z carries an OT shift
                                       * (0 = unshifted; see psx_scene.vert)   */

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) noperspective out vec2 vScreenPSX;

void main()
{
    float z   = clamp(inPos.z, 0.0, 0.9999);
    float vzo = 4096.0 * z / max(1.0 - z, 1e-6);     /* canonical depth -> view-Z */
    vzo = max(vzo, 1.0);
    /* TRUE view-Z, both as the depth encoding and the homogeneous W. The
     * decal reconstructs world geometry from this image, so it must encode
     * the surface's real distance — NOT the OT-shifted ordering value the
     * visible scene sorts by (a slot-shifted floor read as thousands of
     * units deeper drapes the shadow onto phantom ground). And the (z, W)
     * pair must be matched or the clipper corrupts the depth of partially
     * off-screen triangles (see psx_scene.vert). */
    float Wt = inW > 0.0 ? max(inW, 1.0) : vzo;
    gl_Position = vec4(inPos.xy * Wt, Wt - 1.0, Wt);  /* z_ndc = 1 - 1/Wt */
    vColor = inColor;
    vUV = inUV;
    vScreenPSX = inUV;
}
