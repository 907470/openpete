#version 450

/* Untextured OPAQUE gouraud fragment stage with the terrain KEY-RESOLVE
 * contest — quad.frag plus the LP-vs-HP og-key discard (see
 * psx_scene_key.frag for the model). Used only by the opaque scene pipes
 * whose layout carries the HP key image at set 2. */

layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* moby og-key image */

layout(location = 0) in vec4 vColor;  /* x ratio (divide by vQ) */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;
layout(location = 4) flat in float vKoff;

layout(location = 0) out vec4 outColor;

/* gl_FragCoord.z encoding — see psx_scene_key.frag. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;

void main()
{
    if (abs(vKoff) > 0.25) {   /* keyed face: contest the og key rasters
                                * (rules in psx_scene_key.frag) */
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
    outColor = vColor / vQ;
}
