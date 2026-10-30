#version 450

/* Face-identity fragment stage of the retained terrain cache's colour
 * pipelines under OPENPETE_TERRAIN_FACEID. It shares the shipped pipelines'
 * vertex stage, layout and rasterisation state, so under the graphics
 * pipeline library the pre-rasterisation part it links against is the one
 * the shipped pipeline links, and the coverage it writes is the shipped
 * vertex stage's. Every rasterised fragment paints, unshaded and without
 * the texel, portal or key-contest discards, so the colour target holds
 * which face covers each pixel and nothing else.
 *
 * The colour is a 24-bit identity, R | G << 8 | B << 16, with bit 23 set:
 * an FNV-1a hash of the draw word and the face's element index within its
 * buffer, folded to 23 bits. The element index is prim_base + gl_PrimitiveID
 * * prim_step: a vertex-list draw pushes its first vertex and step 3 (every
 * triangle starts on a multiple of 3 in its buffer), a cell-record draw is
 * issued one record per draw and pushes record * vertices-per-record and
 * step 1, so the identity does not depend on how the draws split a buffer.
 * gl_PrimitiveID in a fragment stage needs the Geometry capability. */

layout(push_constant) uniform TerrainPC {
    vec4 cbase;
    vec4 cdims;
    int  pass_sel;
    int  full_extent;
    int  mat_pass;
    int  snap;
    int  cover;
    uint draw_word;      /* pass_sel << 16 | arena << 8 | buffer             */
    uint prim_base;      /* element index of the draw's first primitive      */
    uint prim_step;      /* elements per gl_PrimitiveID step                 */
    vec4 fc;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    const uint e = pc.prim_base + uint(gl_PrimitiveID) * pc.prim_step;
    uint h = 2166136261u;
    h = (h ^ (pc.draw_word & 0xFFFFu)) * 16777619u;
    h = (h ^ (pc.draw_word >> 16)) * 16777619u;
    h = (h ^ (e & 0xFFFFu)) * 16777619u;
    h = (h ^ (e >> 16)) * 16777619u;
    h = ((h ^ (h >> 23)) & 0x7FFFFFu) | 0x800000u;
    outColor = vec4(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u), 255.0)
             / 255.0;
}
