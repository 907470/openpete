#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_src_tint.glsl"

/* HD texture replacement: samples a host RGBA8 replacement texture instead
 * of the PSX VRAM decode. UVs arrive in PSX texel units (same vertex path as
 * psx_scene); the push-constant rect maps the replaced texel box onto
 * [0,1]^2 of the HD image. Modulation mirrors psx_scene.frag (texel *
 * colour / 128, rgb only); alpha is the replacement's own (real per-texel
 * alpha, drawn on an RHI_BLEND_ALPHA pipe instead of the PSX black-sentinel
 * discard). */

layout(set = 0, binding = 0) uniform sampler2D u_hd;

layout(push_constant) uniform HdRect {
    float u0, v0, uw, vh;   /* replaced texel box within the texture page */
    vec4 fogA, fogB;        /* per-channel fog transform; identity = (1,1,1)/(0,0,0) */
    vec4 fogAN, fogBN;      /* deepest-ladder-row affine; fogAN.a > 0 enables the
                             * CONTINUOUS model (see fog block in main) */
    vec4 fogLad;            /* x,y: per-fragment ramp t = clamp(w*x + y, 0, 1) */
} pc;

layout(location = 0) in vec2 vUVq;    /* texel coords x ratio (divide by vQ) */
layout(location = 1) in vec4 vColorq; /* per-vertex modulate x ratio (divide by vQ) */
layout(location = 2) in float vQ;     /* perspective ratio (see psx_scene.vert) */

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
    vec2 vUV = vUVq / vQ;
    vec4 vColor = vColorq / vQ;
    vec2 t = vec2((vUV.x - pc.u0) / pc.uw, (vUV.y - pc.v0) / pc.vh);
    vec4 c = texture(u_hd, clamp(t, 0.0, 1.0));
    if (c.a < 0.004) discard;
    /* Terrain fog: unreplaced draws get it from a greyer palette row, which a
     * host RGBA replacement has no way to reach - so re-apply it here as the
     * affine transform derived from that texture's own two CLUT rows. Applied
     * before the vertex modulate, matching the palette-swap ordering. The
     * continuous fogAN/fogLad model is cutout-pipe-only (see hd_tex_cutout):
     * ladder-eligible faces are opaque terrain and never route here - the
     * fields exist so both pipes share one push layout. */
    c.rgb = clamp(c.rgb * pc.fogA.rgb + pc.fogB.rgb, 0.0, 1.0);
    vec3 rgb = clamp(c.rgb * vColor.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = op_src_tint_scene(vec4(rgb, c.a), vSrcWalk);
}
