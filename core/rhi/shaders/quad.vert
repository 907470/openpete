#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_viewz.glsl"

/* Vertex-coloured scene stage — PERSPECTIVE-CORRECT (matches psx_scene.vert).
 * The CPU feeds post-projection 2D clip-space screen xy + the ordering view-Z
 * (psx_viewz.glsl). W is that view-Z, re-homogenised so the rasterizer
 * interpolates the vertex colour PERSPECTIVE-CORRECT instead of affine:
 *   W = vz ;  gl_Position = (clip.xy*W, z_ndc*W, W)
 * Screen position and window-space depth are unchanged (clip.xy*W/W;
 * z_ndc*W/W = z_ndc, screen-linear in the clip pair), so depth sorting is
 * identical — only the gouraud colour interpolation gains perspective. Used
 * only by the depth-tested scene pipes (terrain LP, Spyro body, shadows,
 * glows); the fullscreen blit/diff passes use their own shaders. */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = ordering view-Z */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;  /* R8G8B8A8_UNORM -> 0..1 */
/* True view-Z (equal to inPos.z unless the feeder shifted z to encode OT
 * ordering). See psx_scene.vert — same rule, so both scene pipes share one
 * vertex buffer. */
layout(location = 4) in float inW;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
/* Interpolation ratio (vzo/inW) — see psx_scene.vert: position W must equal
 * the depth's own view-Z (clip-invariance), so perspective-in-inW colour is
 * recovered by dividing by this in the fragment stage. */
layout(location = 3) out float vQ;
/* Terrain og OT key (rhi_vertex.koff) — see psx_scene.vert. */
layout(location = 4) flat out float vKoff;
layout(location = 5) in float inKoff;
/* Moby og paint RANK (rhi_vertex.sx,sy) — see psx_scene.vert: x = OT walk
 * rank (>=1; 0 = unranked), y = present-local moby ordinal. */
layout(location = 3) in vec2 inRank;
layout(location = 5) flat out vec2 vRank;

/* Depth curve, chosen at pipeline creation (RHI vertex spec-constant 0) —
 * see psx_scene.vert: false = legacy z=vz/(vz+4096), true = screen-affine
 * z_ndc=1-1/vz. Must match psx_scene.vert so both scene pipes share one buffer. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;

/* PSX-affine interpolation (--texture-mapping affine, spec-constant 1): w=1
 * makes the gouraud colour interpolate SCREEN-LINEARLY like the PSX GPU
 * (no perspective divide). Same screen position; the window depth zc/W is
 * the value the perspective path's divide produces, so depth behaviour is
 * identical. Must match psx_scene.vert so both scene pipes stay consistent. */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    /* Position W MUST be the depth's own view-Z (vzo), not the true-vz inW:
     * the clipper interpolates (zc, W) linearly, and only a matched pair
     * keeps window depth screen-linear through clipping (see psx_scene.vert
     * for the failure this prevents). Colour perspective in inW is recovered
     * via the vQ ratio in the fragment stage. */
    float vzo = op_scene_vz(inPos.z);                /* ordering view-Z (depth) */
    float Wu  = max(inW, 1.0);                        /* colour perspective W   */
    float q   = AFFINE_UV ? 1.0 : (vzo / Wu);
    float z   = op_vz_hyperbolic(vzo);                /* legacy curve's z_ndc   */
    float zc  = DEPTH_AFFINE ? (vzo - 1.0) : (z * vzo);   /* z_ndc = zc/vzo */
    gl_Position = AFFINE_UV ? vec4(inPos.xy, DEPTH_AFFINE ? (vzo - 1.0) / vzo : z, 1.0)
                            : vec4(inPos.xy * vzo, zc, vzo);
    vColor = inColor * q;
    vUV = inUV * q;
    vQ = q;
    vKoff = inKoff;
    vRank = inRank;
}
