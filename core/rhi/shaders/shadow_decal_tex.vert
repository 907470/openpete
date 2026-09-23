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

void main()
{
    /* The pipe runs depth-test off; the window depth is the ordering view-Z
     * on the hyperbolic curve, inside [0,1) so the clipper keeps the fan. */
    float z = op_vz_hyperbolic(op_scene_vz(inPos.z));
    float W = max(inW, 1.0);                           /* perspective W (true vz)  */
    gl_Position = vec4(inPos.xy * W, z * W, W);
    vUV = inUV;
    vColor = inColor;
}
