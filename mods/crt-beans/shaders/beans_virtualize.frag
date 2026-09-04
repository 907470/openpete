/**
 * @file beans_virtualize.frag
 * @brief crt-beans pass 1 of 3: resample the composed frame onto the
 *        virtual PSX raster and linearise it. Port of linearize.slang
 *        from crt-beans by Andrew Duffey (MIT, see the mod LICENSE).
 *
 * Upstream expects a low-resolution source, the console framebuffer.
 * Here the input is the full composed frame at output resolution, so
 * this pass also virtualises: every output pixel takes one sample at the
 * centre of its 512x224 PSX raster cell. op_scene_rect and op_scene_grid
 * describe that grid; they are the same values the engine's vertex snap
 * and raster quantise use, so the cells line up with the game's own
 * pixels. The result is a piecewise-constant image the scanline passes
 * can treat as the low-resolution source.
 *
 * The grid continues past the 4:3 rect into the widescreen reveal.
 * Samples that fall outside the frame are black, which is the CRT
 * border.
 *
 * Linear light is stored square-root encoded. The chain's intermediate
 * targets are 8-bit UNORM, not upstream's fp16, and a linear 8-bit store
 * loses the dark end. Storing sqrt(linear) spends the codes where the
 * eye needs them; later passes decode with x*x. The spot-width model
 * needs sqrt(sample) anyway, so the raw texel is already the value it
 * wants.
 */
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
    /* Virtual raster cell containing this pixel. x may be below 0 or at
     * or above 512 in the widescreen reveal; floor() extends the grid on
     * either side. */
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
