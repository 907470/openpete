#version 450

/* HD replacement sampling for OPAQUE (depth-tested, depth-writing) scene
 * geometry: like hd_tex.frag but alpha works as a CUTOUT (discard below 0.5,
 * else opaque) — true alpha blending on opaque groups would break Z ordering.
 * Semi-transparent HD edges harden slightly; the HQ path (Phase 7) revisits. */

layout(set = 0, binding = 0) uniform sampler2D u_hd;

layout(push_constant) uniform HdRect {
    float u0, v0, uw, vh;   /* replaced texel box within the texture page */
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 t = vec2((vUV.x - pc.u0) / pc.uw, (vUV.y - pc.v0) / pc.vh);
    vec4 c = texture(u_hd, clamp(t, 0.0, 1.0));
    if (c.a < 0.5) discard;
    vec3 rgb = clamp(c.rgb * vColor.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, 1.0);
}
