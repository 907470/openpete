#version 450

/* HD replacement sampling for OPAQUE (depth-tested, depth-writing) scene
 * geometry: like hd_tex.frag but alpha works as a CUTOUT (discard below 0.5,
 * else opaque) — true alpha blending on opaque groups would break Z ordering.
 * Semi-transparent HD edges harden slightly; the HQ path (Phase 7) revisits. */

layout(set = 0, binding = 0) uniform sampler2D u_hd;
layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* moby og-key image */
/* Moby og-RANK image — same-moby painter select; see psx_scene_key.frag. */
layout(set = 4, binding = 0) uniform sampler2D u_mobyrank;

layout(push_constant) uniform HdRect {
    float u0, v0, uw, vh;   /* replaced texel box within the texture page */
    vec4 fogA, fogB;        /* per-channel fog transform; identity = (1,1,1)/(0,0,0) */
} pc;

layout(location = 0) in vec2 vUVq;    /* x ratio (divide by vQ) */
layout(location = 1) in vec4 vColorq; /* x ratio (divide by vQ) */
layout(location = 2) in float vQ;     /* perspective ratio (see psx_scene.vert) */
layout(location = 3) flat in float vKoff; /* terrain og-key offset (see psx_scene_key.frag) */
layout(location = 4) flat in vec2 vRank;  /* x = own rank (0 = unranked), y = moby ordinal */

layout(location = 0) out vec4 outColor;

/* gl_FragCoord.z encoding — see psx_scene_key.frag. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;

void main()
{
    if (vRank.x > 0.5) {   /* ranked moby face: same-moby painter select
                            * (rules in psx_scene_key.frag) */
        vec4 e = texelFetch(u_mobyrank, ivec2(gl_FragCoord.xy), 0);
        float irank = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
        float iord  = floor(e.b * 255.0 + 0.5);
        if (abs(iord - vRank.y) < 0.5 && irank > vRank.x + 0.5) discard;
    }
    /* Terrain KEY-RESOLVE: same contest as psx_scene_key.frag so
     * HD-replaced terrain keeps og's per-pixel face select. */
    if (abs(vKoff) > 0.25) {   /* contest rules in psx_scene_key.frag */
        float hk = texelFetch(u_hpkey,   ivec2(gl_FragCoord.xy), 0).r;
        float mk = texelFetch(u_mobykey, ivec2(gl_FragCoord.xy), 0).r;
        float tkey = hk < 0.999999 ? 4096.0 * hk / max(1.0 - hk, 1e-9) : 1e30;
        float mkey = mk < 0.999999 ? 4096.0 * mk / max(1.0 - mk, 1e-9) : 1e30;
        if (vKoff > 0.0) {
            if (tkey < (vKoff - 0.5) - 64.0) discard;
        } else {   /* near-band: per-pixel own key; +512 embed rule vs mobys */
            float own = DEPTH_AFFINE ? 1.0 / max(1.0 - gl_FragCoord.z, 1e-9)
                                     : 4096.0 * gl_FragCoord.z / max(1.0 - gl_FragCoord.z, 1e-9);
            if (tkey < own - 64.0 || mkey < own + 1024.0) discard;
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
