#version 450

/* Occlusion-grid reduce (moby occlusion culling), one axis per draw.
 *
 * The grid holds, per cell, the FARTHEST depth of the pre-pass COLOUR
 * attachment over the cell's source block. That attachment holds the WORLD
 * occluders' depth packed as 24-bit fixed point (terrain_prepass.frag; the
 * hole-fill backing draws there with colour writes off, since the main pass
 * paints it only where no WORLD fragment covered, so it occludes nothing);
 * the pre-pass encodes d = 1 - 1/viewZ, monotonic increasing with distance,
 * so farthest is the largest packed value. "Farthest" is the conservative
 * reduction for occlusion: a cell only reports "occluder at depth d" if
 * EVERY covered texel is at least that near, so any sky / gap / far texel in
 * the block prevents culling behind that cell.
 *
 * Cell c of an axis with `cells` cells over `extent` source texels spans
 * [c*extent/cells, (c+1)*extent/cells) by integer flooring, so the union of
 * the cells covers every source texel exactly once (no sampling grid - a
 * skipped far texel would over-cull); a cell is empty when extent < cells,
 * and an empty cell reduces to 0.
 *
 * The grid is two draws of this shader. The first (axis 0) renders a
 * cells-wide, extent-tall target from the pre-pass: texel (cx, y) is the
 * maximum over row y of column block cx. The second (axis 1) renders the
 * grid from that target: texel (cx, cy) is the maximum over rows block cy
 * of column cx. Maximum is associative and commutative and both draws use
 * the same block bounds, so the grid equals the maximum over each 2D block
 * taken in one loop, bit for bit, while each fragment reads one block edge
 * (about 45 texels at 2880x1920) instead of the whole block (about 1800).
 *
 * Input and output pack the 24-bit value into RGB (R = high byte) of an
 * 8-bit UNORM target, so a value round-trips exactly through the
 * intermediate target. The CPU decodes the grid after the ring readback:
 * q/16777215 -> viewZ = 1/(1-d). 24 bits keeps the decode error under ~100
 * world units at viewZ 40k (16 bits would be ~6000 - useless against a
 * ~1500-unit slack). */

layout(set = 0, binding = 0) uniform sampler2D uSrc;  /* packed depth: pre-pass colour, or the axis-0 result */

/* axis = the axis this draw reduces (0 = x, 1 = y); cells = the grid's cell
 * count on it; extent = the source texels on it (the pre-pass VIEWPORT
 * extent, the rasterized region - the allocation may be larger and holds
 * clear-far or stale texels outside it). */
layout(push_constant) uniform PC { int axis; int cells; int extent; int pad; } pc;

layout(location = 0) in vec2 vUV;      /* unused (cell comes from gl_FragCoord) */
layout(location = 1) in vec4 vColor;   /* unused */

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int c = (pc.axis == 0) ? p.x : p.y;
    int t0 = (c       * pc.extent) / pc.cells;
    int t1 = ((c + 1) * pc.extent) / pc.cells;
    ivec2 step = (pc.axis == 0) ? ivec2(1, 0) : ivec2(0, 1);
    ivec2 src = (pc.axis == 0) ? ivec2(t0, p.y) : ivec2(p.x, t0);

    uint m = 0u;
    for (int t = t0; t < t1; ++t, src += step) {
        uvec3 b = uvec3(texelFetch(uSrc, src, 0).rgb * 255.0 + 0.5);
        m = max(m, (b.r << 16) | (b.g << 8) | b.b);
    }

    outColor = vec4(float((m >> 16) & 255u) / 255.0,
                    float((m >>  8) & 255u) / 255.0,
                    float( m        & 255u) / 255.0, 1.0);
}
