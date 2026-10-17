/* portal_cover_scene.glsl - the portal cover discard of a scene fragment
 * stage's _cover and _rim variants.
 *
 * Every blended world pipeline of the CPU-fed scene draws through a
 * variant of its stock fragment stage compiled with OP_PORTAL_COVER=1
 * (core/rhi/CMakeLists.txt); the variant includes this file and discards
 * the fragments op_portal_cover_frag reports. The pipeline's layout carries
 * the block below at set 5 (rhi_pipeline_desc.uniform_set5), written by the
 * renderer per pass: the opening planes portal_cover.glsl tests, after this
 * pass's gl_FragCoord -> PSX screen affine (screen x = fc.x * fragx + fc.y,
 * screen y = fc.z * fragy + fc.w). A plane count of 0 turns the test off,
 * so the variant then draws exactly what its stock stage draws.
 *
 * The _cover variant (OP_PORTAL_COVER alone) serves the blended world
 * (terrain, glow, glints, particles, mobys): it
 * discards at or behind plane minus the cover margin (cov_margin.x) while
 * the blended cover is on (cov_cnt.y). The _rim variant (OP_PORTAL_RIM as
 * well) serves the faces the terrain feed accepts as a portal's inner rim:
 * it discards inside the outline of the plane that accepted the face, at
 * any depth (OP_PORTAL_RIM_MARGIN), so the preview paints there and the face
 * stays drawn outside the opening. The rim rule's block carries that plane
 * as cov_cnt.w = 1 + plane index (0 tests every plane); the renderer binds
 * one block per plane and draws each plane's rim faces under its own.
 *
 * The portal profile of a mod material (openpete_psx_material.glsl,
 * OP_PROFILE_PORTAL) defines OP_COVER_SET 7, above the material layout's
 * seven sets, and OP_PORTAL_RUNTIME: one compile serves both rules, the
 * block's cov_cnt.z selecting the rim rule (1) or the cover rule (0); the
 * renderer binds the block written for the rule and plane the draw needs. */

#ifndef OPENPETE_PORTAL_COVER_SCENE_GLSL
#define OPENPETE_PORTAL_COVER_SCENE_GLSL

#include "psx_key_contest.glsl"

#ifndef OP_COVER_SET
#define OP_COVER_SET 5
#endif

layout(std140, set = OP_COVER_SET, binding = 0) uniform PortalCover {
    vec4  cov_fc;
    ivec4 cov_cnt;
    vec4  cov_margin;
    vec4  cov_abc[6];
    vec4  cov_bbox[6];
    vec4  cov_poly[96];
} pcov;
#define OP_COVER_SRC pcov
#include "portal_cover.glsl"

/* Whether this fragment lies inside a portal opening at or behind its plane
 * minus M. reversed is the scene's depth convention (the stage's
 * DEPTH_REVERSED); the fragment's view-Z is decoded from gl_FragCoord.z. */
bool op_portal_cover_frag(bool reversed)
{
#if defined(OP_PORTAL_RIM)
    const float m = OP_PORTAL_RIM_MARGIN;
    const int only = pcov.cov_cnt.w - 1;
#elif defined(OP_PORTAL_RUNTIME)
    if (pcov.cov_cnt.z == 0 && pcov.cov_cnt.y == 0) return false;
    const float m = pcov.cov_cnt.z != 0 ? OP_PORTAL_RIM_MARGIN : pcov.cov_margin.x;
    const int only = pcov.cov_cnt.z != 0 ? pcov.cov_cnt.w - 1 : -1;
#else
    if (pcov.cov_cnt.y == 0) return false;
    const float m = pcov.cov_margin.x;
    const int only = -1;
#endif
    return op_portal_covered(vec2(pcov.cov_fc.x * gl_FragCoord.x + pcov.cov_fc.y,
                                  pcov.cov_fc.z * gl_FragCoord.y + pcov.cov_fc.w),
                             op_vz_from_depth(gl_FragCoord.z, reversed), m, only);
}

#endif
