/* openpete_shader_api.glsl — the postfx shader contract.
 *
 * Include after the #version line:
 *
 *     #version 450
 *     #extension GL_GOOGLE_include_directive : require
 *     #define OP_POSTFX_STAGE_FRAGMENT 1
 *     #include "openpete_shader_api.glsl"
 *
 * The engine resolves the include to the staged SDK copy and hashes this
 * file into every shader's build key: an edit to it recompiles every
 * cached mod shader, and a prebuilt .spv sidecar refuses until re-issued.
 *
 * Descriptor sets (one sampler per set):
 *   set 0, binding 0  sampler2D  op_prev_pass   previous pass output; for
 *                                               the first pass, the point's
 *                                               input image
 *   set 1, binding 0  sampler2D  op_scene_depth opaque pre-pass depth,
 *                                               terrain only; 1.0 where
 *                                               unavailable
 *   set 2, binding 0  sampler2D  op_prev_frame  previous present's composite
 *   set 3, binding 0  UBO        op_engine      engine frame state, below
 *   set 3, binding 1  UBO        mod-declared   the bytes passed to
 *                                               postfx_set_uniforms, verbatim.
 *                                               Declare it std140 and match
 *                                               the C struct's std140 layout.
 *
 * Defining OP_POSTFX_STAGE_FRAGMENT before the include declares the op_uv
 * input and the op_color output at location 0. */

#ifndef OPENPETE_SHADER_API_GLSL
#define OPENPETE_SHADER_API_GLSL

#define OP_PREV_PASS_SET      0
#define OP_DEPTH_SET          1
#define OP_PREV_FRAME_SET     2
#define OP_UNIFORM_SET        3
#define OP_ENGINE_UBO_BINDING 0
#define OP_MOD_UBO_BINDING    1

layout(set = 0, binding = 0) uniform sampler2D op_prev_pass;
layout(set = 1, binding = 0) uniform sampler2D op_scene_depth;
layout(set = 2, binding = 0) uniform sampler2D op_prev_frame;

/* Engine frame state. std140; entries are appended, never moved. */
layout(set = 3, binding = 0, std140) uniform op_engine_ubo {
    float op_time;        /* host seconds since boot                     */
    float op_alpha;       /* sub-tick phase [0,1); 0 on a canonical present */
    uint  op_tick;        /* sim tick counter                            */
    uint  op_present;     /* VBL stamp of the composed scene; repeats
                           * across the presents of one scene           */
    uint  op_canonical;   /* 1 = canonical present, 0 = intermediate     */
    float op_aspect;      /* render aspect ratio (width / height)        */
    vec2  op_resolution;  /* current pass resolution, pixels             */
    vec2  op_window;      /* window / swapchain resolution, pixels       */
    vec4  op_scene_rect;  /* uv rect of the 4:3 content within the pass
                           * image (xy = top-left, zw = size): where the
                           * 512-wide, overscan-cropped PSX screen maps.
                           * Widescreen reveal continues the same mapping
                           * outside the rect. */
    vec4  op_scene_grid;  /* the PSX raster that rect spans: x = display
                           * width (512), y = display height (240), z =
                           * visible rows (224), w = widen factor (>= 1).
                           * PSX pixel coords of a pass texel:
                           *   sx = (uv.x - rect.x) / rect.z * x
                           *   sy = (y - z)/2 + (uv.y - rect.y) / rect.w * z */
};

#ifdef OP_POSTFX_STAGE_FRAGMENT
layout(location = 0) in  vec2 op_uv;     /* [0,1]^2, (0,0) = top-left;
                                          * texture(op_prev_pass, op_uv) is
                                          * the identity mapping */
layout(location = 0) out vec4 op_color;
#endif

#endif /* OPENPETE_SHADER_API_GLSL */
