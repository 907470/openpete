#version 450

/* Overlay supersample resolve. Box-downsamples a TRANSPARENT-background overlay
 * render target (the FPS glyphs rendered at N× into ovl_ss) by N× per axis and
 * outputs STRAIGHT alpha for RHI_BLEND_ALPHA compositing onto the scene:
 *   - colour  = gamma-correct average of the COVERED texels (un-premultiplied)
 *   - alpha   = coverage = mean of the source alpha over the N×N block
 * The glyphs are opaque (a=1) on a cleared a=0 background, so the mean alpha is
 * the edge coverage and straight-alpha blend (SRC_ALPHA, 1-SRC_ALPHA) composites
 * anti-aliased glyph edges correctly. Sibling of resolve.frag (which is opaque,
 * forces a=1, and is used for the whole-scene SSAA). */

layout(set = 0, binding = 0) uniform sampler2D uSrc;   /* supersampled overlay */

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
    ivec2 outsz = srcsz / n;                 /* 1× output extent */
    ivec2 d     = ivec2(vUV * vec2(outsz));  /* this output pixel */
    ivec2 base  = d * n;                      /* top-left of its N×N source block */

    vec3  acc   = vec3(0.0);                  /* premult-weighted linear colour */
    float acc_a = 0.0;                        /* summed coverage */
    int   cnt   = 0;
    for (int dy = 0; dy < n; ++dy) {
        for (int dx = 0; dx < n; ++dx) {
            ivec2 p = base + ivec2(dx, dy);
            if (p.x >= srcsz.x || p.y >= srcsz.y) continue;
            vec4 s = texelFetch(uSrc, p, 0);
            acc   += srgb_to_linear(s.rgb) * s.a;
            acc_a += s.a;
            ++cnt;
        }
    }
    float cov = acc_a / float(max(cnt, 1));
    vec3  col = (acc_a > 0.0) ? (acc / acc_a) : vec3(0.0);   /* un-premultiply */
    outColor  = vec4(linear_to_srgb(col), cov);
}
