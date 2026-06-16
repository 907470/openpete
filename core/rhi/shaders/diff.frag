#version 450

/* Diff pass (--renderer diff): per-pixel absolute colour difference between the
 * native render and the PsyCross reference. set 0 = the native render target
 * (rendered top-down this frame), set 1 = the reference frame read back from
 * PsyCross's present FBO (GL origin = bottom-up, so its V is flipped). Output is
 * |native - ref| per channel — black where they agree, bright where they differ.
 * Pairs with psx_sample.vert (passthrough pos + UV + colour). */

layout(set = 0, binding = 0) uniform sampler2D uNative;
layout(set = 1, binding = 0) uniform sampler2D uRef;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor; /* unused */

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 n = texture(uNative, vUV).rgb;
    vec3 r = texture(uRef, vec2(vUV.x, 1.0 - vUV.y)).rgb;
    outColor = vec4(abs(n - r), 1.0);
}
