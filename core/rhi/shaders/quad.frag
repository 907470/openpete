#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_src_tint.glsl"

/* Output the interpolated vertex color, perspective-divided by vQ. The
 * PSX 4/8/16bpp + CLUT VRAM sampler is psx_sample.frag. */

layout(location = 0) in vec4 vColor;  /* x ratio (divide by vQ) */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;     /* perspective ratio (see quad.vert) */

layout(location = 0) out vec4 outColor;
/* 1 on a walked terrain face under the source tint (psx_src_tint.glsl). */
layout(location = 10) flat in float vSrcWalk;

#ifdef OP_PORTAL_COVER
/* The _cover variant (blended terrain): portal_cover_scene.glsl, with the
 * scene's gl_FragCoord.z convention (see psx_scene_key.frag). */
#include "portal_cover_scene.glsl"
layout(constant_id = 0) const bool DEPTH_REVERSED = false;
#endif

void main()
{
#ifdef OP_PORTAL_COVER
    /* A blended terrain fragment the portal preview covers. */
    if (op_portal_cover_frag(DEPTH_REVERSED)) discard;
#endif
    outColor = op_src_tint_scene(vColor / vQ, vSrcWalk);
}
