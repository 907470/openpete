/**
 * @file beans_scan_v.frag
 * @brief crt-beans pass 3 of 3: vertical spot profile, phosphor mask and
 *        output encode. Port of scanlines_fast_vertical.slang and
 *        mask.inc from crt-beans by Andrew Duffey (MIT, see the mod
 *        LICENSE).
 *
 * Each output pixel takes the two nearest scanline centres' horizontal
 * beam profiles from pass 2 and applies the vertical spot falloff: a
 * cubic profile whose width varies with the signal, wide for bright and
 * narrow for dim. Because the width is a function of the signal and not
 * of the output pixel size, the look holds at any output resolution.
 * The phosphor mask follows, either a subpixel-aware static grille or a
 * band-limited dynamic grille, then the output gamma encode.
 *
 * Differences from upstream: row geometry comes from op_scene_rect and
 * op_scene_grid (224 visible PSX rows; the engine has already cropped
 * overscan, so the Overscan parameters are dropped); the interlacing
 * branches are dropped (240p progressive); the pass-2 input is
 * square-root encoded 8-bit, decoded with x*x.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1
#include "openpete_shader_api.glsl"

layout(set = 3, binding = 1, std140) uniform beans_params {
    float u_max_spot;           /* max spot size, proportion of scanline    */
    float u_min_spot;           /* min spot size, proportion of max         */
    float u_mask_type;          /* 0 off, 1 subpixel grille, 2 dynamic      */
    float u_subpixel_pattern;   /* 0 RGB, 1 BGR                             */
    float u_subpixel_triad_px;  /* monitor px per phosphor triad (2..5)     */
    float u_mask_triads;        /* dynamic-mask triads across screen width  */
    float u_output_gamma;       /* 0 = sRGB encode, 1 = 2.2 gamma           */
};

#define PI 3.1415926535897932384626433832795

/**
 * @name Phosphor masks (mask.inc, ported unchanged)
 * @{
 */

/**
 * @brief Static aperture grille aligned to the monitor's physical
 *        subpixels.
 * @param pixels_per_triad Monitor pixels per phosphor triad, 2..5. Each
 *        tile below lights the subpixels that fall in one phosphor's
 *        column at that pitch, so the grille is drawn by the monitor's
 *        own subpixels rather than by blending.
 * @return rgb = per-channel mask, a = coverage, the ratio the mask
 *         dims the picture by, which blend_mask compensates.
 */
vec4 subpixel_aperture_grille(vec2 coord, vec4 output_size,
                              float subpixel_pattern, float pixels_per_triad) {
    float x = coord.x * output_size.x;
    vec3 mask = vec3(1.0);
    float mask_coverage = 1.0;
    if (subpixel_pattern == 0.0) { // RGB subpixels
        if (pixels_per_triad == 2.0) { // <=1080p
            vec3[2] mask_tile = { vec3(1.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0) };
            mask = mask_tile[int(mod(x, 2.0))];
            mask_coverage = 2.0;
        } else if (pixels_per_triad == 3.0) { // 1080p/1440p
            vec3[3] mask_tile = { vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0),
                                  vec3(1.0, 0.0, 0.0) };
            mask = mask_tile[int(mod(x, 3.0))];
            mask_coverage = 3.0;
        } else if (pixels_per_triad == 4.0) { // 1440p/4k
            vec3[4] mask_tile = { vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0),
                                  vec3(0.0, 1.0, 1.0), vec3(0.0, 0.0, 1.0) };
            mask = mask_tile[int(mod(x, 4.0))];
            mask_coverage = 2.0;
        } else if (pixels_per_triad == 5.0) { // 4k, lower TVL
            vec3[5] mask_tile = { vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 1.0),
                                  vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0),
                                  vec3(0.0, 1.0, 0.0) };
            mask = mask_tile[int(mod(x, 5.0))];
            mask_coverage = 15.0 / 6.0;
        }
    } else { // BGR subpixels, same as above but red and blue transposed
        if (pixels_per_triad == 2.0) {
            vec3[2] mask_tile = { vec3(1.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0) };
            mask = mask_tile[int(mod(x, 2.0))];
            mask_coverage = 2.0;
        } else if (pixels_per_triad == 3.0) {
            vec3[3] mask_tile = { vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0),
                                  vec3(0.0, 0.0, 1.0) };
            mask = mask_tile[int(mod(x, 3.0))];
            mask_coverage = 3.0;
        } else if (pixels_per_triad == 4.0) {
            vec3[4] mask_tile = { vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0),
                                  vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0) };
            mask = mask_tile[int(mod(x, 4.0))];
            mask_coverage = 2.0;
        } else if (pixels_per_triad == 5.0) {
            vec3[5] mask_tile = { vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 1.0),
                                  vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0),
                                  vec3(0.0, 1.0, 0.0) };
            mask = mask_tile[int(mod(x, 5.0))];
            mask_coverage = 15.0 / 6.0;
        }
    }
    return vec4(mask, mask_coverage);
}

/**
 * @brief Band-limited aperture grille at an arbitrary triad count.
 *
 * A raised-cosine low-pass of radius one output pixel is convolved with
 * a piecewise-constant RGB grille and sampled at the output pixel
 * centre. Filtering before sampling is what keeps the grille alias-free
 * when the triad pitch is not a whole number of pixels.
 */
vec4 dynamic_aperture_grille(vec2 coord, vec4 output_size, float mask_triads) {
    const vec3[3] phosphors = { vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0),
                                vec3(0.0, 0.0, 1.0) };
    // All units are in phosphor widths (1 == one phosphor).
    float kernel_radius = 3.0 * mask_triads * output_size.z;
    float x = 3.0 * mask_triads * coord.x;
    vec3 mask = vec3(0.0);
    if (kernel_radius < 0.5) {
        float x1 = clamp((x - round(x)) / kernel_radius, -1.0, 1.0);
        // mod(x, 3.0) isn't exact in floating point; nudge before truncating.
        vec3 p0 = phosphors[int(mod(round(x) - 1.0, 3.0) + 1e-3)];
        vec3 p1 = p0.brg;
        mask = p0 + p1 + (p1 - p0) * (x1 + sin(PI * x1) / PI);
        mask *= 0.5;
    } else if (kernel_radius < 1.0) {
        float x1 = clamp((x - (floor(x) + 0.0)) / kernel_radius, -1.0, 1.0);
        float x2 = clamp((x - (floor(x) + 1.0)) / kernel_radius, -1.0, 1.0);
        vec3 p0 = phosphors[int(mod(floor(x) - 1.0, 3.0) + 1e-3)];
        vec3 p1 = p0.brg;
        vec3 p2 = p1.brg;
        mask = p0 + p2 +
               (p1 - p0) * (x1 + sin(PI * x1) / PI) +
               (p2 - p1) * (x2 + sin(PI * x2) / PI);
        mask *= 0.5;
    } else { // kernel_radius < 1.5 (mask fades toward flat beyond)
        float x1 = clamp((x - (round(x) - 1.0)) / kernel_radius, -1.0, 1.0);
        float x2 = clamp((x - (round(x) + 0.0)) / kernel_radius, -1.0, 1.0);
        float x3 = clamp((x - (round(x) + 1.0)) / kernel_radius, -1.0, 1.0);
        vec3 p0 = phosphors[int(mod(round(x) - 2.0, 3.0) + 1e-3)];
        vec3 p1 = p0.brg;
        vec3 p2 = p1.brg;
        vec3 p3 = p2.brg;
        mask = p0 + p3 +
               (p1 - p0) * (x1 + sin(PI * x1) / PI) +
               (p2 - p1) * (x2 + sin(PI * x2) / PI) +
               (p3 - p2) * (x3 + sin(PI * x3) / PI);
        mask *= 0.5;
    }
    return vec4(mask, 3.0);
}

/**
 * @brief Apply a mask while holding overall brightness.
 *
 * The mask is kept at full strength wherever the masked value can still
 * integrate to the unmasked value, and faded only where it cannot, so a
 * bright pixel does not dim just because a grille is on.
 */
vec3 blend_mask(vec3 mask, vec3 pixel_value, float mask_coverage) {
    vec3 a = clamp((pixel_value - 1) / (1 - mask_coverage), vec3(0.0), pixel_value);
    vec3 b = clamp((1 - mask_coverage * pixel_value) / (1 - mask_coverage), vec3(0.0), pixel_value);
    return mask_coverage * a * mask + b;
}

/** @} */

/** @brief Linear to sRGB encode; the clamp guards pow against fp error. */
vec3 linear_to_srgb(vec3 rgb) {
    rgb = clamp(rgb, 0.0, 1.0);   /* guard fp error before pow */
    bvec3 cutoff = lessThan(rgb, vec3(0.0031308));
    vec3 higher = vec3(1.055) * pow(rgb, vec3(1.0 / 2.4)) - vec3(0.055);
    vec3 lower = rgb * vec3(12.92);
    return mix(higher, lower, cutoff);
}

/** @brief Linear to gamma 2.2 encode. */
vec3 linear_to_gamma(vec3 rgb) {
    rgb = clamp(rgb, 0.0, 1.0);
    return pow(rgb, vec3(1.0 / 2.2));
}

/**
 * @brief One scanline's contribution at a vertical distance from its
 *        centre.
 * @param row_c The row centre, in row units.
 * @param dist  Distance from that centre, in scanline heights.
 *
 * The spot width w depends on the sample: w_rcp is 1/w, and w runs from
 * min_spot*max_spot at black to max_spot at white. The cubic
 * (y*y)*(2y-3)+1 is the falloff, 1 at the centre and 0 at one width.
 */
vec3 line_contrib(float row_c, float dist) {
    if (row_c < 0.0 || row_c > op_scene_grid.z) return vec3(0.0); /* off-raster */
    float uv_y = op_scene_rect.y + row_c / op_scene_grid.z * op_scene_rect.w;
    if (uv_y < 0.0 || uv_y > 1.0) return vec3(0.0);
    vec3 raw = texture(op_prev_pass, vec2(op_uv.x, uv_y)).rgb;
    vec3 s = raw * raw;                     /* decode sqrt storage */
    vec3 w_rcp = 1.0 / (u_min_spot * u_max_spot
                        - raw * (u_min_spot * u_max_spot - u_max_spot));
    vec3 y = clamp(abs(dist) * w_rcp, 0.0, 1.0);
    return s * w_rcp * ((y * y) * (2.0 * y - 3.0) + 1.0);
}

void main() {
    if (op_scene_rect.z <= 0.0 || op_scene_rect.w <= 0.0) {
        op_color = vec4(texture(op_prev_pass, op_uv).rgb, 1.0);
        return;
    }
    float vy = (op_uv.y - op_scene_rect.y) / op_scene_rect.w * op_scene_grid.z;

    /* Two nearest scanline centres (rows peak at integer + 0.5). */
    float upper_c = round(vy) + 0.5;
    float lower_c = upper_c - 1.0;
    float upper_d = upper_c - vy;
    float lower_d = upper_d - 1.0;
    vec3 value = u_max_spot * (line_contrib(upper_c, upper_d) +
                               line_contrib(lower_c, lower_d));

    if (u_mask_type == 1.0) {
        vec4 mask = subpixel_aperture_grille(
            op_uv, vec4(op_resolution, 1.0 / op_resolution),
            u_subpixel_pattern, u_subpixel_triad_px);
        value = blend_mask(mask.rgb, value, mask.a);
    } else if (u_mask_type == 2.0) {
        vec4 mask = dynamic_aperture_grille(
            op_uv, vec4(op_resolution, 1.0 / op_resolution), u_mask_triads);
        value = blend_mask(mask.rgb, value, mask.a);
    } // Otherwise, no mask.

    value = (u_output_gamma < 0.5) ? linear_to_srgb(value)
                                   : linear_to_gamma(value);
    op_color = vec4(value, 1.0);
}
