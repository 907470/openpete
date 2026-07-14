/* postfx_fs.vert — the engine-owned fullscreen-triangle vertex stage for
 * mod postfx passes (S2, plans/openpete-mod-shaders.md). No vertex
 * buffer: three gl_VertexIndex vertices cover the screen; uv lands in
 * [0,1]² at location 0 (op_uv in the mod's fragment shader, see
 * sdk/shaders/openpete_shader_api.glsl). */
#version 450

layout(location = 0) out vec2 uv;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
