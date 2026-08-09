#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1
#include "openpete_shader_api.glsl"

/* SCENE-point demo: sepia-tint the 3D scene. The HUD is drawn AFTER
 * this pass, so it stays full-colour and crisp — the property that
 * distinguishes the SCENE point from COMPOSITE (which would tint the
 * HUD too). Depth-independent, so the effect is unmistakable on any
 * frame. op_scene_depth is wired at set 1 (the WORLD-opaque pre-pass)
 * for depth-aware effects, but its coverage/scale needs further work
 * before distance fog is practical — see the mod-shaders plan S5. */
/* Grade strength, pushed by the mod (std140: one float). 0 leaves the
 * scene untouched, 1 is the full sepia tone. */
layout(set = 3, binding = 1, std140) uniform sepia_params {
    float strength;
};

const vec3 SEPIA = vec3(1.07, 0.90, 0.64);
void main() {
    vec3 c = texture(op_prev_pass, op_uv).rgb;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 s = clamp(vec3(l) * SEPIA, 0.0, 1.0);
    op_color = vec4(mix(c, s, clamp(strength, 0.0, 1.0)), 1.0);
}
