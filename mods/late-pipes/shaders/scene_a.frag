#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1
#include "openpete_shader_api.glsl"

/* Faint per-channel scale: an installed pass changes the image, a pass
 * that never ran leaves it as the previous pass wrote it. */
void main() {
    vec3 c = texture(op_prev_pass, op_uv).rgb;
    op_color = vec4(c * vec3(0.92,1.0,0.94), 1.0);
}
