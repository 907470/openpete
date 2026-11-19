#version 450
#extension GL_GOOGLE_include_directive : require
#include "prepass_pack.glsl"

/* Depth pre-pass fragment for faces that cover their whole footprint: the
 * scene pre-pass's far backstop and hole-fill backing (prepass.vert) and the
 * retained-terrain hole-fill pre-pass (unconsumed vertex-stage varyings are
 * legal and simply dropped). The depth attachment
 * takes the fixed-function depth; the colour attachment takes the same depth
 * packed (op_prepass_pack_depth). Pipelines that draw the hole-fill backing
 * disable colour writes, so the colour attachment holds the depth of the
 * WORLD occluders alone. */

layout(location = 0) out vec4 outColor;

void main() { outColor = op_prepass_pack_depth(gl_FragCoord.z); }
