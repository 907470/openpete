#version 450

/* storage_pull.vert over the combined set of a tex_ssbo_set2 pipeline: the
 * records come from the dynamic storage buffer at set 2 binding 1, beside
 * the image at binding 0 that tex_store.frag samples, indexed by
 * gl_VertexIndex from the dynamic offset rhi_bind_texture_storage selects. */

struct op_storage_vert {
    vec4 pos;    /* xy = clip space; z, w unused */
    vec4 color;  /* RGBA 0..1 */
};

layout(std430, set = 2, binding = 1) readonly buffer OpStorageVerts {
    op_storage_vert v[];
} op_verts;

layout(location = 0) out vec4 vColor;

void main()
{
    op_storage_vert r = op_verts.v[gl_VertexIndex];
    gl_Position = vec4(r.pos.xy, 0.5, 1.0);
    vColor = r.color;
}
