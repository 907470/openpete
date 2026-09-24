#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_viewz.glsl"

/* Textured shadow-decal vertex stage — the MOBY drop-shadows.
 *
 * Perspective-correct sprite UV/colour (identical to psx_scene.vert: the
 * homogeneous W is the fed true view-Z). The fragment stage's
 * depth pre-pass lookup takes its PSX screen coordinate from gl_FragCoord
 * through the pass's inverse affine, so no screen coordinate is carried here. */

layout(location = 0) in vec3 inPos;       /* xy = clip-space screen, z = ordering view-Z */
layout(location = 1) in vec2 inUV;        /* sprite texel coords within the page   */
layout(location = 2) in vec4 inColor;     /* R8G8B8A8 -> 0..1 modulate             */
layout(location = 4) in float inW;        /* true view-Z (see psx_scene.vert) */

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

/* Scene depth convention (RHI spec constant 0) — see psx_scene.vert. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;

void main()
{
    /* The pipe runs depth-test off; the window depth is the scene
     * convention's value of the ordering view-Z (op_scene_clip), which the
     * fragment stage decodes to the fan's view-Z. W stays the true view-Z,
     * so clip z is W times that window depth. */
    float vzo = op_scene_vz(inPos.z);
    float W = max(inW, 1.0);                           /* perspective W (true vz)  */
    vec4 sc = op_scene_clip(inPos.xy, vzo, true, DEPTH_REVERSED);
    gl_Position = vec4(inPos.xy * W, sc.z * W, W);
    vUV = inUV;
    vColor = inColor;
}
