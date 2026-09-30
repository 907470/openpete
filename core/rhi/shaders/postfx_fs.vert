/* postfx_fs.vert — the engine-owned fullscreen-triangle vertex stage for
 * mod postfx passes. No vertex
 * buffer: three gl_VertexIndex vertices cover the screen; uv lands in
 * [0,1]² at location 0 (op_uv in the mod's fragment shader, see
 * sdk/shaders/openpete_shader_api.glsl). */
#version 450

layout(location = 0) out vec2 uv;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    /* Every engine pass renders under the Y-FLIPPED (GL-convention)
     * viewport, so NDC y=+1 is the screen TOP while texture v=0 is the
     * image's top memory row: uv.y must invert p.y or a single postfx
     * pass flips the frame (caught by eyeball on the first crt shot —
     * an even pass count flips back and hides it). (0,0) = top-left. */
    uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
