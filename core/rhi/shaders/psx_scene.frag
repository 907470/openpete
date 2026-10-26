#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_clut.glsl"
#include "psx_texel.glsl"
#include "psx_key_contest.glsl"
#include "psx_hq_fog.glsl"

/* Textured native-scene fragment stage.
 *
 * Same PSX 4/8/16bpp + CLUT VRAM decode as psx_sample.frag, but with the PSX
 * TEXTURE-MODULATION rule for gouraud-textured polys: out = texel * color / 128
 * (8-bit), i.e. a vertex colour of 128 means "unchanged", 255 ~2x, 0 black.
 * In normalised floats that is texel * color_n * (255/128), clamped. Using the
 * /255 of psx_sample.frag here would darken every textured moby by ~half.
 *
 * Determinism-irrelevant: pixel output only. Semi-transparency: the ABR mode
 * is the pipeline's blend state; this stage passes the decoded texel alpha
 * through, and STP_SELECT (below) splits STP-set from STP-clear texels. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16 */
    /* The HQ ladder fade (psx_hq_fog.glsl): the run's fade words and the
     * published ramp. Word a's bit 31 clear leaves every texel stock. */
    uint hqfog_a, hqfog_b;
    float lad_ts, lad_tb;
} pc;

layout(location = 0) in vec2 vUV;     /* texel coords x ratio (divide by vQ) */
layout(location = 1) in vec4 vColor;  /* per-vertex modulate x ratio (divide by vQ) */
layout(location = 2) in float vQ;     /* perspective ratio (see psx_scene.vert) */

layout(location = 0) out vec4 outColor;

/* Untextured lighting view (rhi_set_notex): replace the texel colour with the
 * modulation-neutral grey (128/255) so out = the raw gouraud vertex colour -
 * texel alpha/discard keep real silhouettes. */
layout(constant_id = 2) const bool NOTEX = false;

/* Texel STP selection (rhi_pipeline_desc.texel_stp_select): 0 keeps every
 * texel, 1 only texels with bit 15 clear, 2 only texels with bit 15 set. A
 * semi-transparent primitive of ABR 1-3 draws as a pair of pipes, its
 * STP-clear texels opaque (1) and its STP-set texels through the ABR's blend
 * (2), since those blend equations read no alpha to tell the two apart. */
layout(constant_id = 5) const int STP_SELECT = 0;

/* The scene's gl_FragCoord.z convention (see psx_scene_key.frag): the
 * portal cover test and the HQ fade's view-Z read it. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;
#ifdef OP_PORTAL_COVER
/* The _cover variant (blended terrain): portal_cover_scene.glsl. */
#include "portal_cover_scene.glsl"
#endif

uint sample_word(int u, int v)
{
    return op_texel_word(u_vram, ivec2(pc.tpage_x, pc.tpage_y),
                         ivec2(pc.clut_x, pc.clut_y), pc.bpp, u, v);
}

/* The second fogged-CLUT row of a textured face (op_clut_fog_mix); the
 * untextured lighting view keeps the vertex colour alone. */
vec3 clut_fog_mix(vec3 rgb, vec4 col, int u, int v)
{
    if (NOTEX) return rgb;
    return op_clut_fog_mix(u_vram, rgb, col, ivec2(pc.tpage_x, pc.tpage_y),
                           ivec2(pc.clut_x, pc.clut_y), pc.bpp, u, v);
}

void main()
{
    vec2 uv = vUV / vQ;
    vec2 tg = op_texel_grad(uv);
#ifdef OP_PORTAL_COVER
    /* A blended terrain fragment the portal preview covers. */
    if (op_portal_cover_frag(DEPTH_REVERSED)) discard;
#endif
    vec4 col = vColor / vQ;
    ivec2 ti = op_texel(uv, tg);
    uint word = sample_word(ti.x, ti.y);
    vec4 c = op_decode555(word);
    if (c.a == 0.0) discard;
    if (STP_SELECT == 1 && (word & 0x8000u) != 0u) discard;
    if (STP_SELECT == 2 && (word & 0x8000u) == 0u) discard;
    if (NOTEX) c.rgb = vec3(128.0 / 255.0);
    else c.rgb = op_hq_fog(c.rgb, pc.hqfog_a, pc.hqfog_b,
                           op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED),
                           pc.lad_ts, pc.lad_tb);
    /* PSX texture modulation: texel * vertexColour / 128 (clamped). */
    vec3 rgb = clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    rgb = clut_fog_mix(rgb, col, ti.x, ti.y);
    outColor = vec4(rgb, c.a);
}
