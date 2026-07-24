/* beans_virtualize.frag — crt-beans pass 1/3 (port of linearize.slang from
 * crt-beans by Andrew Duffey, MIT — see the mod LICENSE).
 *
 * The original expects a low-resolution source (the console framebuffer);
 * here the input is the full composed frame, so this pass also VIRTUALIZES:
 * every output pixel takes one sample at the centre of its 512x224 PSX
 * raster cell (op_scene_rect/op_scene_grid — the same grid the engine's
 * vertex snap and raster quantize use), producing a piecewise-constant
 * image the scanline passes can treat as the low-res source. The grid
 * continues beyond the 4:3 rect into the Hor+ widescreen reveal; samples
 * that fall outside the frame are black (the CRT border).
 *
 * Linearizes with the fixed CRT gamma 2.4 (as upstream). The chain's
 * intermediate targets are 8-bit UNORM, not the upstream fp16, so linear
 * light is stored SQRT-ENCODED (gamma-2.0) to keep dark-end precision;
 * later passes decode with x*x. The encoding is doubly convenient: the
 * spot-width model needs sqrt(sample) anyway, so the raw texel IS that. */
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1
#include "openpete_shader_api.glsl"

#define CRT_GAMMA 2.4

void main() {
    /* Degenerate mapping (no scene rect yet): pass the frame through. */
    if (op_scene_rect.z <= 0.0 || op_scene_rect.w <= 0.0) {
        op_color = vec4(texture(op_prev_pass, op_uv).rgb, 1.0);
        return;
    }
    /* Virtual raster cell containing this pixel (x may be < 0 or >= 512 in
     * the widescreen reveal — floor() extends the grid either side). */
    float vx = (op_uv.x - op_scene_rect.x) / op_scene_rect.z * op_scene_grid.x;
    float vy = (op_uv.y - op_scene_rect.y) / op_scene_rect.w * op_scene_grid.z;
    vec2 cuv = vec2(
        op_scene_rect.x + (floor(vx) + 0.5) / op_scene_grid.x * op_scene_rect.z,
        op_scene_rect.y + (floor(vy) + 0.5) / op_scene_grid.z * op_scene_rect.w);

    vec3 rgb = vec3(0.0);
    if (all(greaterThanEqual(cuv, vec2(0.0))) && all(lessThanEqual(cuv, vec2(1.0))))
        rgb = pow(clamp(texture(op_prev_pass, cuv).rgb, 0.0, 1.0), vec3(CRT_GAMMA));

    op_color = vec4(sqrt(rgb), 1.0);
}
