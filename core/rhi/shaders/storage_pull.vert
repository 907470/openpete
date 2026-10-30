#version 450

/* Vertex stage that takes every input from a read-only storage buffer at
 * set 2 (rhi_storage, pipeline flag ssbo_set2) indexed by gl_VertexIndex:
 * no vertex attribute is read. Each record is a clip-space xy position in
 * pos.xy and an RGBA colour. Pairs with quad.frag (colour / vQ, vQ = 1). */

struct op_storage_vert {
    vec4 pos;    /* xy = clip space; z, w unused */
    vec4 color;  /* RGBA 0..1 */
};

layout(std430, set = 2, binding = 0) readonly buffer OpStorageVerts {
    op_storage_vert v[];
} op_verts;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 3) out float vQ;

void main()
{
    op_storage_vert r = op_verts.v[gl_VertexIndex];
    gl_Position = vec4(r.pos.xy, 0.5, 1.0);
    vColor = r.color;
    vUV = vec2(0.0);
    vQ = 1.0;
}
