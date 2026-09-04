/**
 * @file beans_scan_h.frag
 * @brief crt-beans pass 2 of 3: horizontal beam-spot convolution per
 *        scanline. Port of scanlines_fast_horizontal.slang from crt-beans
 *        by Andrew Duffey (MIT, see the mod LICENSE).
 *
 * Each scanline's samples are convolved with the electron-beam spot
 * function along the horizontal axis, piecewise per source texel. All
 * distances are in scanline heights: centre to centre of adjacent
 * scanlines is 1.
 *
 * Differences from upstream:
 *
 * - The source is the virtualised PSX raster from pass 1. Every fragment
 *   samples its row at the row-band centre, so every fragment in a band
 *   computes the identical row profile. Pass 3 relies on that: its
 *   bilinear reads at row centres are exact only if the band is uniform.
 * - delta, the texel width in scanline heights, comes from the on-screen
 *   geometry in op_scene_rect rather than RetroArch's viewport uniforms.
 * - The interlacing branches are dropped. PSX 240p content is
 *   progressive, 224 visible rows.
 * - Intermediates are square-root encoded 8-bit; decode is x*x, and the
 *   raw texel doubles as the sqrt(sample) the spot-width model wants.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1
#include "openpete_shader_api.glsl"

layout(set = 3, binding = 1, std140) uniform beans_params {
    float u_max_spot;           /* max spot size, proportion of scanline    */
    float u_min_spot;           /* min spot size, proportion of max         */
    float u_mask_type;          /* pass 3 only                              */
    float u_subpixel_pattern;   /* pass 3 only                              */
    float u_subpixel_triad_px;  /* pass 3 only                              */
    float u_mask_triads;        /* pass 3 only                              */
    float u_output_gamma;       /* pass 3 only                              */
};

#define PI 3.1415926535897932384626433832795

/**
 * @brief The spot function integrated over one source texel.
 * @param raw The texel's square-root encoded value: raw*raw is the linear
 *            sample and raw is sqrt(sample), which the width model uses.
 * @param x   Position along the scanline, in scanline heights.
 * @param x0  Texel start, same units.
 * @param x1  Texel end, same units.
 *
 * The common factor max_spot / (2*pi) is left out here and applied once
 * by the caller after the sum.
 */
vec3 spot_integral(vec3 raw, float x, float x0, float x1) {
    vec3 sample_value = raw * raw;
    vec3 w_rcp = 1.0 / (u_min_spot * u_max_spot
                        - raw * (u_min_spot * u_max_spot - u_max_spot));
    vec3 u0 = clamp(w_rcp * (x - x0), -1.0, 1.0);
    vec3 u1 = clamp(w_rcp * (x - x1), -1.0, 1.0);
    return sample_value * (PI * u0 + sin(PI * u0) - PI * u1 - sin(PI * u1));
}

void main() {
    if (op_scene_rect.z <= 0.0 || op_scene_rect.w <= 0.0) {
        op_color = vec4(texture(op_prev_pass, op_uv).rgb, 1.0);
        return;
    }
    float vx = (op_uv.x - op_scene_rect.x) / op_scene_rect.z * op_scene_grid.x;
    float vy = (op_uv.y - op_scene_rect.y) / op_scene_rect.w * op_scene_grid.z;

    /* This fragment's row, sampled at the band centre; every fragment in
     * the band computes the same profile. */
    float row_uv_y = op_scene_rect.y
                   + (floor(vy) + 0.5) / op_scene_grid.z * op_scene_rect.w;

    /* Texel width in scanline heights, from on-screen pixel sizes. */
    float line_h_px = op_scene_rect.w * op_resolution.y / op_scene_grid.z;
    float texel_w_px = op_scene_rect.z * op_resolution.x / op_scene_grid.x;
    float delta = texel_w_px / max(line_h_px, 1e-6);

    float x = vx * delta;                 /* position along the scanline   */
    float r = u_max_spot / delta;         /* spot reach, in source texels  */

    vec3 value = vec3(0.0);
    for (float i = floor(vx - r); i <= floor(vx + r); i += 1.0) {
        float su = op_scene_rect.x + (i + 0.5) / op_scene_grid.x * op_scene_rect.z;
        vec3 raw = vec3(0.0);             /* off-frame texels: CRT border  */
        if (su >= 0.0 && su <= 1.0 && row_uv_y >= 0.0 && row_uv_y <= 1.0)
            raw = texture(op_prev_pass, vec2(su, row_uv_y)).rgb;
        value += spot_integral(raw, x, i * delta, (i + 1.0) * delta);
    }
    value /= 2.0 * PI;   /* factored-out spot normalization */

    op_color = vec4(sqrt(clamp(value, 0.0, 1.0)), 1.0);
}
