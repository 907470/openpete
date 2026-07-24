#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1
#include "openpete_shader_api.glsl"

/* Resample the scene on the virtual PSX raster: every output pixel takes
 * the colour at its 512x240-cell centre (nearest — the hard-pixel look).
 * The cell grid comes from op_scene_rect/op_scene_grid, so it is the SAME
 * raster --vertex-precision integer snaps vertices to, under any window
 * size, aspect, or Hor+ widening — snapped polygon edges land ON cell
 * boundaries instead of shimmering across them. Pixels are non-square by
 * construction (512 across the 4:3 width, 224 visible rows), matching the
 * PSX pixel aspect. Outside the 4:3 content rect (Hor+ reveal, overscan
 * bands) the same grid continues at the same pitch. */

layout(set = 3, binding = 1, std140) uniform q_params {
    float blend;   /* 0 = full quantize; 0..1 mixes the sharp frame back in */
};

void main() {
    vec2  r0 = op_scene_rect.xy;
    vec2  rs = op_scene_rect.zw;
    float dispw = op_scene_grid.x;
    float cliph = op_scene_grid.z;
    float band  = (op_scene_grid.y - cliph) * 0.5;

    /* pass uv -> virtual PSX screen coords (the GTE/vertex-snap grid) */
    float sx = (op_uv.x - r0.x) / rs.x * dispw;
    float sy = band + (op_uv.y - r0.y) / rs.y * cliph;

    /* snap to the cell centre, map back, sample */
    vec2 quv = vec2(r0.x + (floor(sx) + 0.5) / dispw * rs.x,
                    r0.y + (floor(sy) + 0.5 - band) / cliph * rs.y);
    vec3 q = texture(op_prev_pass, clamp(quv, vec2(0.0), vec2(1.0))).rgb;
    if (blend > 0.0)
        q = mix(q, texture(op_prev_pass, op_uv).rgb, clamp(blend, 0.0, 1.0));
    op_color = vec4(q, 1.0);
}
