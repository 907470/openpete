#version 450

/* SSAA resolve pass (--ssaa N). Box-downsamples the supersampled scene render
 * target (set 0) by N× per axis into the 1× offscreen. Pairs with
 * psx_sample.vert (passthrough pos + UV).
 *
 * Two correctness points the previous vkCmdBlitImage(VK_FILTER_LINEAR) path
 * got wrong at N>=3:
 *   1. FOOTPRINT - a linear blit takes a single 2x2 tap regardless of the
 *      downscale ratio, so most supersampled texels were discarded. This
 *      averages the FULL NxN block (a true box filter).
 *   2. GAMMA - the stored values are perceptual (sRGB-ish in a UNORM target);
 *      averaging them directly darkens edges. This decodes each sample to
 *      linear light, averages, then re-encodes - gamma-correct downsampling. */

layout(set = 0, binding = 0) uniform sampler2D uSrc;   /* supersampled scene */

layout(push_constant) uniform PC { int ssaa; } pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;                   /* unused */

layout(location = 0) out vec4 outColor;

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 linear_to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main()
{
    int n = pc.ssaa < 1 ? 1 : pc.ssaa;
    ivec2 srcsz = textureSize(uSrc, 0);
    ivec2 outsz = srcsz / n;                 /* 1x output extent */
    ivec2 d     = ivec2(vUV * vec2(outsz));  /* this output pixel (UV centre -> index) */
    ivec2 base  = d * n;                      /* top-left of its NxN source block */

    vec3 acc = vec3(0.0);
    int cnt = 0;
    for (int dy = 0; dy < n; ++dy) {
        for (int dx = 0; dx < n; ++dx) {
            ivec2 p = base + ivec2(dx, dy);
            if (p.x >= srcsz.x || p.y >= srcsz.y) continue;
            acc += srgb_to_linear(texelFetch(uSrc, p, 0).rgb);
            ++cnt;
        }
    }
    outColor = vec4(linear_to_srgb(acc / float(max(cnt, 1))), 1.0);
}
