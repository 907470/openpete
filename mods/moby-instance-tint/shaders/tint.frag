/* Per-instance material fragment stage (S8b): output the per-draw tint
 * from the mod uniform block (set 3 binding 1) the callback produced.
 * Fork sdk/shaders/psx_scene.frag for a real material that samples VRAM
 * and modulates by state; this bare version proves the uniform path. */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_shader_api.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(tint.rgb, 1.0); }
