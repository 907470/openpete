/* crt.frag — scanlines + slot mask + barrel curvature over the
 * composed frame (OP_POSTFX_COMPOSITE point).
 *
 * Parameters are compile-time constants until the mod-uniform block
 * lands (S4 wires config.toml -> postfx_set_uniforms -> set 1
 * binding 1); keep the shader self-contained meanwhile. */
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1
#include "openpete_shader_api.glsl"

const float CURVATURE   = 0.12;  /* barrel amount; 0 = flat            */
const float SCAN_DEPTH  = 0.28;  /* scanline darkening [0,1]           */
const float MASK_DEPTH  = 0.18;  /* slot-mask darkening [0,1]          */
const float CORNER_FADE = 0.02;  /* vignette width at the curved edge  */

vec2 curve(vec2 uv) {
    vec2 c = uv * 2.0 - 1.0;                 /* [-1,1]² */
    c *= 1.0 + CURVATURE * dot(c, c) * 0.25;
    c *= 1.0 - CURVATURE * 0.18;             /* refit into frame */
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
    float scan = 1.0 - SCAN_DEPTH * (0.5 + 0.5 * cos(line * 6.28318530718));

    /* Simple RGB slot mask across window pixels. */
    float px = uv.x * op_resolution.x;
    int phase = int(mod(px, 3.0));
    vec3 mask = vec3(1.0 - MASK_DEPTH);
    if (phase == 0) mask.r = 1.0;
    else if (phase == 1) mask.g = 1.0;
    else mask.b = 1.0;

    /* Edge vignette so the curved corners fade instead of clipping. */
    vec2 edge = min(uv, 1.0 - uv);
    float vig = smoothstep(0.0, CORNER_FADE, min(edge.x, edge.y));

    op_color = vec4(rgb * scan * mask * vig, 1.0);
}
