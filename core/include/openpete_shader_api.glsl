/* openpete_shader_api.glsl — the mod-shader binding contract.
 *
 * Include from a mod shader (after its #version line) with:
 *
 *     #extension GL_GOOGLE_include_directive : require
 *     #include "openpete_shader_api.glsl"
 *
 * The engine resolves this include to the staged SDK copy and hashes
 * these bytes into every shader's build key — editing this contract
 * auto-invalidates every cached mod shader (source builds recompile;
 * prebuilt .spv sidecars refuse until re-issued).
 *
 * Descriptor sets, postfx tier (one sampler per set — the engine binds
 * inputs through its per-texture descriptor sets):
 *   set 0, binding 0  sampler2D  op_prev_pass   — previous pass output
 *                                                 (first pass: the point's
 *                                                 input image)
 *   set 1, binding 0  sampler2D  op_scene_depth — opaque pre-pass depth
 *                                                 (WORLD opaque only; 1.0
 *                                                 where unavailable)
 *   set 2, binding 0  sampler2D  op_prev_frame  — previous present's
 *                                                 composite (temporal FX)
 *   set 3, binding 0  UBO        op_engine      — engine frame state
 *                                                 (declared below)
 *   set 3, binding 1  UBO        (mod-declared) — the mod's own block;
 *                                                 bytes come verbatim from
 *                                                 postfx_set_uniforms.
 *                                                 Declare it std140 and
 *                                                 match your C struct's
 *                                                 std140 layout by hand.
 *
 * A postfx fragment shader declares its role by defining
 * OP_POSTFX_STAGE_FRAGMENT before the include — that brings in the
 * op_uv input and the op_color output (location 0):
 *
 *     #version 450
 *     #extension GL_GOOGLE_include_directive : require
 *     #define OP_POSTFX_STAGE_FRAGMENT 1
 *     #include "openpete_shader_api.glsl"
 */

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

/* Engine frame state (std140; the C mirror is openpete_shader_engine_ubo_t
 * in the engine — offsets are load-bearing, append only). */
layout(set = 3, binding = 0, std140) uniform op_engine_ubo {
    float op_time;        /* host seconds since boot                     */
    float op_alpha;       /* sub-tick interp phase [0,1); 0 on canonical */
    uint  op_tick;        /* canonical sim tick (openpete_frame, 60 Hz)  */
    uint  op_present;     /* monotonic present counter                   */
    uint  op_canonical;   /* 1 = canonical present, 0 = intermediate     */
    float op_aspect;      /* render aspect ratio (width / height)        */
    vec2  op_resolution;  /* current pass resolution, pixels             */
    vec2  op_window;      /* window / swapchain resolution, pixels       */
};

#ifdef OP_POSTFX_STAGE_FRAGMENT
layout(location = 0) in  vec2 op_uv;     /* [0,1]²; (0,0) = TOP-left —
                                          * sample op_prev_pass at op_uv
                                          * for the identity mapping */
layout(location = 0) out vec4 op_color;
#endif

#endif /* OPENPETE_SHADER_API_GLSL */
