#version 450

/* Overlay supersample resolve. Box-downsamples a TRANSPARENT-background overlay
 * render target (the FPS glyphs rendered at N× into ovl_ss) by N× per axis and
 * outputs STRAIGHT alpha for RHI_BLEND_ALPHA compositing onto the scene:
 *   - colour  = gamma-correct average of the COVERED texels (un-premultiplied)
 *   - alpha   = coverage = mean of the source alpha over the N×N block
 * The glyphs are opaque (a=1) on a cleared a=0 background, so the mean alpha is
 * the edge coverage and straight-alpha blend (SRC_ALPHA, 1-SRC_ALPHA) composites
 * anti-aliased glyph edges correctly. Sibling of resolve.frag (which is opaque,
 * forces a=1, and is used for the whole-scene SSAA).
 *
 * Outline (pc.outline > 0, in output pixels): black is composited under the
 * layer wherever a source texel lies within pc.outline * ssaa source texels
 * (a disc) of an OUTLINE SOURCE texel, one of alpha exactly 1. A texel of
 * alpha 254/255 (OPENPETE_HUD_ALPHA_NO_OUTLINE, the menu's frame) draws as
 * opaque but seeds no outline. Coverage counts any alpha >= 0.99 as 1, so
 * that marker composites fully opaque. */

layout(set = 0, binding = 0) uniform sampler2D uSrc;   /* supersampled overlay */

layout(push_constant) uniform PC { int ssaa; int outline; } pc;

layout(location = 0) in vec2 vUV;
/* Per-quad modulation. The composite quad's vertex colour scales the resolved
 * result: white (the default every caller built until the OSD) is identity,
 * and the OSD writes a per-toast alpha here so eight toasts can fade
 * independently without eight pipelines or a push constant per draw. */
layout(location = 1) in vec4 vColor;

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
    int r = pc.outline > 0 ? pc.outline * n : 0;   /* outline radius, source texels */
    ivec2 srcsz = textureSize(uSrc, 0);
    ivec2 outsz = srcsz / n;                 /* 1× output extent */
    ivec2 d     = ivec2(vUV * vec2(outsz));  /* this output pixel */
    ivec2 base  = d * n;                      /* top-left of its N×N source block */

    vec3  acc   = vec3(0.0);                  /* premult-weighted linear colour */
    float acc_a = 0.0;                        /* summed coverage */
    float acc_o = 0.0;                        /* summed outline coverage */
    int   cnt   = 0;
    for (int dy = 0; dy < n; ++dy) {
        for (int dx = 0; dx < n; ++dx) {
            ivec2 p = base + ivec2(dx, dy);
            if (p.x >= srcsz.x || p.y >= srcsz.y) continue;
            vec4 s = texelFetch(uSrc, p, 0);
            float a = s.a >= 0.99 ? 1.0 : s.a;
            acc   += srgb_to_linear(s.rgb) * a;
            acc_a += a;
            ++cnt;
            if (r == 0 || a >= 1.0) continue;
            /* Outline: this texel is uncovered (or partly); it takes the
             * outline when an outline-source texel lies within r. */
            float o = 0.0;
            for (int oy = -r; oy <= r && o == 0.0; ++oy) {
                for (int ox = -r; ox <= r; ++ox) {
                    if (ox * ox + oy * oy > r * r) continue;
                    ivec2 q = p + ivec2(ox, oy);
                    if (q.x < 0 || q.y < 0 || q.x >= srcsz.x || q.y >= srcsz.y) continue;
                    if (texelFetch(uSrc, q, 0).a == 1.0) { o = 1.0; break; }
                }
            }
            acc_o += o * (1.0 - a);
        }
    }
    float cov = acc_a / float(max(cnt, 1));
    float ocv = acc_o / float(max(cnt, 1));   /* black under the layer */
    vec3  col = (acc_a > 0.0) ? (acc / acc_a) : vec3(0.0);   /* un-premultiply */
    /* Layer over black: colour weighted by coverage, alpha the union. */
    float tot = cov + ocv;
    vec3  rgb = (tot > 0.0) ? linear_to_srgb(col * (cov / tot)) : vec3(0.0);
    outColor  = vec4(rgb * vColor.rgb, tot * vColor.a);
}
