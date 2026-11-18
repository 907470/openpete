#version 450

/* Base-image resample (--resolution N | WxH). Maps the source rect of the
 * base image (set 0) onto the picture area of the window-size target; the
 * pixels outside the picture area are written black, the colour the target
 * is cleared to. Pairs with psx_sample.vert over the full-target quad; the
 * output pixel is read from gl_FragCoord, so the quad's UVs are unused.
 *
 * Per axis the filter is chosen on the CPU (render_res.c):
 *   0 copy, 1 nearest  - integer fetch, origin and span whole texels;
 *   2 area             - integer downscale ratio, whole-texel origin;
 *   3 sharp-bilinear   - upscale: nearest to the integer multiple, linear
 *                        across the remainder;
 *   4 area (float)     - downscale or equal size at a fractional ratio or
 *                        origin: the footprint's texels weighted by overlap.
 * Both axes copy or nearest fetch one texel and write it unconverted. Both
 * axes area at one ratio run the SSAA resolve's box loop (resolve.frag) term
 * for term, so an integer-ratio average equals the resolve bit for bit. Every
 * other combination sums separable float weights over texelFetch taps in
 * linear light; the sampler's filter is never used, since its weights may be
 * quantized to a few subtexel bits. Filtered taps are clamped to the centres
 * of the outermost texels the source rect touches, so a fractional edge never
 * blends in the texel past it. */

layout(set = 0, binding = 0) uniform sampler2D uSrc;   /* base image */

layout(push_constant) uniform PC {
    vec4  src;    /* source rect in texels: x0, y0, x1, y1 */
    ivec4 dst;    /* picture area in target pixels: x, y, w, h */
    ivec4 mode;   /* filter x, filter y, integer ratio x, integer ratio y */
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;                   /* unused */

layout(location = 0) out vec4 outColor;

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 linear_to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

const int MAX_TAPS = 8;

/* One axis's taps: texel indices and weights. Returns the tap count. */
int axis_taps(int f, int r, float s0, float s1, int p, int d, out int idx[MAX_TAPS], out float w[MAX_TAPS])
{
    float len = s1 - s0;
    int lo = int(floor(s0));
    int hi = int(ceil(s1)) - 1;
    if (f <= 1) {                      /* copy / nearest: one whole texel */
        idx[0] = lo + (p * int(len + 0.5)) / d;
        w[0] = 1.0;
        return 1;
    }
    if (f == 2) {                      /* area at an integer ratio */
        int n = min(r, MAX_TAPS);
        for (int i = 0; i < n; ++i) { idx[i] = lo + p * r + i; w[i] = 1.0 / float(r); }
        return n;
    }
    float scale = len / float(d);
    if (f == 3) {                      /* sharp-bilinear */
        float k = max(floor(float(d) / len), 1.0);
        float t = s0 + (float(p) + 0.5) * scale;          /* texel space, centres at i + 0.5 */
        float fl = floor(t);
        float cd = (t - fl) - 0.5;
        float rr = 0.5 - 0.5 / k;
        float m = fl + (cd - clamp(cd, -rr, rr)) * k + 0.5;
        m = clamp(m, float(lo) + 0.5, float(hi) + 0.5);
        float u = m - 0.5;
        float i0 = floor(u);
        float a = u - i0;
        idx[0] = int(i0);          w[0] = 1.0 - a;
        idx[1] = min(int(i0) + 1, hi); w[1] = a;
        return 2;
    }
    float a0 = s0 + float(p) * scale;  /* area over the footprint [a0, a1) */
    float a1 = s0 + float(p + 1) * scale;
    int i = int(floor(a0));
    int n = 0;
    for (; n < MAX_TAPS && float(i) < a1; ++n, ++i) {
        float ov = min(a1, float(i + 1)) - max(a0, float(i));
        idx[n] = clamp(i, lo, hi);
        w[n] = max(ov, 0.0) / scale;
    }
    return n;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - pc.dst.xy;
    if (p.x < 0 || p.y < 0 || p.x >= pc.dst.z || p.y >= pc.dst.w) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (pc.mode.x <= 1 && pc.mode.y <= 1) {
        int ix = int(floor(pc.src.x)) + (p.x * int(pc.src.z - pc.src.x + 0.5)) / pc.dst.z;
        int iy = int(floor(pc.src.y)) + (p.y * int(pc.src.w - pc.src.y + 0.5)) / pc.dst.w;
        outColor = vec4(texelFetch(uSrc, ivec2(ix, iy), 0).rgb, 1.0);
        return;
    }
    if (pc.mode.x == 2 && pc.mode.y == 2 && pc.mode.z == pc.mode.w) {
        /* resolve.frag's box, n = the ratio, offset by the source origin. */
        int n = pc.mode.z;
        ivec2 srcsz = textureSize(uSrc, 0);
        ivec2 base = ivec2(floor(pc.src.xy)) + p * n;
        vec3 acc = vec3(0.0);
        int cnt = 0;
        for (int dy = 0; dy < n; ++dy) {
            for (int dx = 0; dx < n; ++dx) {
                ivec2 q = base + ivec2(dx, dy);
                if (q.x >= srcsz.x || q.y >= srcsz.y) continue;
                acc += srgb_to_linear(texelFetch(uSrc, q, 0).rgb);
                ++cnt;
            }
        }
        outColor = vec4(linear_to_srgb(acc / float(max(cnt, 1))), 1.0);
        return;
    }
    int ix[MAX_TAPS], iy[MAX_TAPS];
    float wx[MAX_TAPS], wy[MAX_TAPS];
    int nx = axis_taps(pc.mode.x, pc.mode.z, pc.src.x, pc.src.z, p.x, pc.dst.z, ix, wx);
    int ny = axis_taps(pc.mode.y, pc.mode.w, pc.src.y, pc.src.w, p.y, pc.dst.w, iy, wy);
    vec3 acc = vec3(0.0);
    for (int j = 0; j < ny; ++j) {
        vec3 row = vec3(0.0);
        for (int i = 0; i < nx; ++i)
            row += wx[i] * srgb_to_linear(texelFetch(uSrc, ivec2(ix[i], iy[j]), 0).rgb);
        acc += wy[j] * row;
    }
    outColor = vec4(linear_to_srgb(acc), 1.0);
}
