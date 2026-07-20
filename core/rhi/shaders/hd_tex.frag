#version 450

/* HD texture-replacement PoC (openpete-texture-replacement plan, Phase 4
 * spike): samples a host RGBA8 replacement texture instead of the PSX VRAM
 * decode. UVs arrive in PSX texel units (same vertex path as psx_scene); the
 * push-constant rect maps the replaced texel box onto [0,1]^2 of the HD
 * image. Modulation mirrors psx_scene.frag (texel * colour / 128, rgb only);
 * alpha is the replacement's own (real per-texel alpha, drawn on an
 * RHI_BLEND_ALPHA pipe instead of the PSX black-sentinel discard). */

layout(set = 0, binding = 0) uniform sampler2D u_hd;

layout(push_constant) uniform HdRect {
    float u0, v0, uw, vh;   /* replaced texel box within the texture page */
    vec4 fogA, fogB;        /* per-channel fog transform; identity = (1,1,1)/(0,0,0) */
} pc;

layout(location = 0) in vec2 vUV;     /* texel coords within the texture page */
layout(location = 1) in vec4 vColor;  /* per-vertex modulate (R8G8B8A8 -> 0..1) */

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 t = vec2((vUV.x - pc.u0) / pc.uw, (vUV.y - pc.v0) / pc.vh);
    vec4 c = texture(u_hd, clamp(t, 0.0, 1.0));
    if (c.a < 0.004) discard;
    /* Terrain fog: unreplaced draws get it from a greyer palette row, which a
     * host RGBA replacement has no way to reach — so re-apply it here as the
     * affine transform derived from that texture's own two CLUT rows. Applied
     * before the vertex modulate, matching the palette-swap ordering. */
    c.rgb = clamp(c.rgb * pc.fogA.rgb + pc.fogB.rgb, 0.0, 1.0);
    vec3 rgb = clamp(c.rgb * vColor.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, c.a);
}
