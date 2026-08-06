#version 450

/* HD replacement sampling for OPAQUE (depth-tested, depth-writing) scene
 * geometry: like hd_tex.frag but alpha works as a CUTOUT (discard below 0.5,
 * else opaque) — true alpha blending on opaque groups would break Z ordering.
 * Semi-transparent HD edges harden slightly; the HQ path (Phase 7) revisits. */

layout(set = 0, binding = 0) uniform sampler2D u_hd;
layout(set = 2, binding = 0) uniform sampler2D u_hpkey; /* HP og-key image */

layout(push_constant) uniform HdRect {
    float u0, v0, uw, vh;   /* replaced texel box within the texture page */
    vec4 fogA, fogB;        /* per-channel fog transform; identity = (1,1,1)/(0,0,0) */
} pc;

layout(location = 0) in vec2 vUVq;    /* x ratio (divide by vQ) */
layout(location = 1) in vec4 vColorq; /* x ratio (divide by vQ) */
layout(location = 2) in float vQ;     /* perspective ratio (see psx_scene.vert) */
layout(location = 3) flat in float vKoff; /* terrain og-key offset (see psx_scene_key.frag) */

layout(location = 0) out vec4 outColor;

void main()
{
    /* Terrain KEY-RESOLVE: same contest as psx_scene_key.frag so
     * HD-replaced terrain keeps og's per-pixel face select. */
    if (vKoff > 0.25) {
        float hk = texelFetch(u_hpkey, ivec2(gl_FragCoord.xy), 0).r;
        if (hk < 0.999999) {
            if (1.0 / max(1.0 - hk, 1e-9) < (vKoff - 0.5) - 64.0) discard;
        }
    }
    vec2 vUV = vUVq / vQ;
    vec4 vColor = vColorq / vQ;
    vec2 t = vec2((vUV.x - pc.u0) / pc.uw, (vUV.y - pc.v0) / pc.vh);
    vec4 c = texture(u_hd, clamp(t, 0.0, 1.0));
    if (c.a < 0.5) discard;
    /* See hd_tex.frag: re-apply the CLUT-ladder fog a replacement can't get
     * from the palette. This is the pipe that terrain actually draws through. */
    c.rgb = clamp(c.rgb * pc.fogA.rgb + pc.fogB.rgb, 0.0, 1.0);
    vec3 rgb = clamp(c.rgb * vColor.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, 1.0);
}
