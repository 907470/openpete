/* A fragment shader that defines op_material and its own main without the
 * contract header. */
#version 450

layout(location = 0) out vec4 color;

vec4 op_material(vec4 base) { return base.bgra; }

void main() { color = op_material(vec4(gl_FragCoord.xy, 0.0, 1.0)); }
