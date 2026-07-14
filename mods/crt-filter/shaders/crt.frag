/* crt.frag — scanlines + slot mask + barrel curvature over the
 * composed frame (OP_POSTFX_COMPOSITE point).
 *
 * Parameters arrive through the mod uniform block (set 3, binding 1 —
 * bytes from postfx_set_uniforms, fed from config.toml by the C side).
 * std140: four floats, offsets 0/4/8/12. */
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1
#include "openpete_shader_api.glsl"

layout(set = 3, binding = 1, std140) uniform crt_params {
    float u_curvature;    /* barrel amount; 0 = flat            */
    float u_scan_depth;   /* scanline darkening [0,1]           */
    float u_mask_depth;   /* slot-mask darkening [0,1]          */
    float u_corner_fade;  /* vignette width at the curved edge  */
};

vec2 curve(vec2 uv) {
    vec2 c = uv * 2.0 - 1.0;                 /* [-1,1]² */
    c *= 1.0 + u_curvature * dot(c, c) * 0.25;
    c *= 1.0 - u_curvature * 0.18;           /* refit into frame */
    return c * 0.5 + 0.5;
}

void main() {
    vec2 uv = curve(op_uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        op_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 rgb = texture(op_prev_pass, uv).rgb;

    /* Scanlines track the ~224 visible PSX lines, not window pixels. */
    float line = uv.y * 224.0;
    float scan = 1.0 - u_scan_depth * (0.5 + 0.5 * cos(line * 6.28318530718));

    /* Simple RGB slot mask across window pixels. */
    float px = uv.x * op_resolution.x;
    int phase = int(mod(px, 3.0));
    vec3 mask = vec3(1.0 - u_mask_depth);
    if (phase == 0) mask.r = 1.0;
    else if (phase == 1) mask.g = 1.0;
    else mask.b = 1.0;

    /* Edge vignette so the curved corners fade instead of clipping. */
    vec2 edge = min(uv, 1.0 - uv);
    float vig = (u_corner_fade > 0.0)
        ? smoothstep(0.0, u_corner_fade, min(edge.x, edge.y))
        : 1.0;

    op_color = vec4(rgb * scan * mask * vig, 1.0);
}
