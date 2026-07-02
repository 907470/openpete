#version 450

/* Occlusion-grid downsample (moby occlusion culling).
 *
 * Renders into the small (grid_w × grid_h) occlusion RT; each output pixel is
 * one grid cell. Gathers its FULL source block of the WORLD-opaque depth
 * pre-pass and keeps the FARTHEST depth (max — the pre-pass encodes
 * d = 1 - 1/viewZ, monotonic increasing with distance). "Farthest" is the
 * conservative reduction for occlusion: a cell only reports "occluder at
 * depth d" if EVERY covered texel is at least that near, so any sky / gap /
 * far texel in the block prevents culling behind that cell. Block bounds are
 * computed by integer flooring so the union of all cells covers every source
 * texel exactly once (no sampling grid — a skipped far texel would over-cull).
 *
 * Output packs the max depth as 24-bit fixed point into RGB (R = high byte),
 * decoded CPU-side after the ring readback: q/16777215 -> viewZ = 1/(1-d).
 * 24 bits keeps the decode error under ~100 world units at viewZ 40k (16 bits
 * would be ~6000 — useless against a ~1500-unit slack). */

layout(set = 0, binding = 0) uniform sampler2D uDepth;  /* pre-pass depth (depth-aspect view) */

/* gw/gh = grid extent; pvw/pvh = the pre-pass VIEWPORT extent (the rasterized
 * region — the RT allocation may be larger and holds clear-far outside it). */
layout(push_constant) uniform PC { int gw; int gh; int pvw; int pvh; } pc;

layout(location = 0) in vec2 vUV;      /* unused (cell comes from gl_FragCoord) */
layout(location = 1) in vec4 vColor;   /* unused */

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 cell = ivec2(gl_FragCoord.xy);
    int x0 = (cell.x     * pc.pvw) / pc.gw;
    int x1 = ((cell.x+1) * pc.pvw) / pc.gw;
    int y0 = (cell.y     * pc.pvh) / pc.gh;
    int y1 = ((cell.y+1) * pc.pvh) / pc.gh;

    float m = 0.0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            m = max(m, texelFetch(uDepth, ivec2(x, y), 0).r);

    uint q = uint(clamp(m, 0.0, 1.0) * 16777215.0 + 0.5);
    outColor = vec4(float((q >> 16) & 255u) / 255.0,
                    float((q >>  8) & 255u) / 255.0,
                    float( q        & 255u) / 255.0, 1.0);
}
