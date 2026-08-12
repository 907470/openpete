#version 450

/* Untextured OPAQUE gouraud fragment stage with the terrain KEY-RESOLVE
 * contest — quad.frag plus the LP-vs-HP og-key discard (see
 * psx_scene_key.frag for the model). Used only by the opaque scene pipes
 * whose layout carries the HP key image at set 2. */

layout(set = 1, binding = 0) uniform sampler2D u_actorkey; /* moby og-key image */
layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* prop TRUE-depth image */
/* Moby og-RANK image — same-moby painter select; see psx_scene_key.frag. */
layout(set = 4, binding = 0) uniform sampler2D u_mobyrank;

layout(location = 0) in vec4 vColor;  /* x ratio (divide by vQ) */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;
layout(location = 4) flat in float vKoff;
layout(location = 5) flat in vec2 vRank;  /* x = own rank (0 = unranked), y = moby ordinal */

layout(location = 0) out vec4 outColor;

/* gl_FragCoord.z encoding — see psx_scene_key.frag. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;

/* Free-camera mode: scope moby og-key yields by the fragment's own TRUE
 * view-Z (see the contest block). Canonical camera = false = og painter. */
layout(constant_id = 3) const bool MOBY_TRUESCOPE = false;

void main()
{
    bool ranked = vRank.x > 0.5;
    if (ranked) {   /* ranked moby face: NEAR-TIE-scoped same-moby painter
                     * select (rules in psx_scene_key.frag) */
        vec4 e = texelFetch(u_mobyrank, ivec2(gl_FragCoord.xy), 0);
        float irank = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
        float iord  = floor(e.b * 255.0 + 0.5);
        if (abs(iord - vRank.y) < 0.5 && irank > vRank.x + 0.5) {
            float ownerVz = e.a * 32640.0;
            float ownVz = DEPTH_AFFINE ? 1.0 / max(1.0 - gl_FragCoord.z, 1e-9)
                                       : 4096.0 * gl_FragCoord.z / max(1.0 - gl_FragCoord.z, 1e-9);
            if (abs(ownerVz - ownVz) < 384.0) discard;
        }
    }
    /* Ranked faces skip the positive-koff contest — see psx_scene_key.frag. */
    if (!ranked && abs(vKoff) > 0.25) {   /* keyed face: contest the og key rasters
                                * (rules in psx_scene_key.frag) */
        float hk = texelFetch(u_hpkey,    ivec2(gl_FragCoord.xy), 0).r;
        float ak = texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r;
        float mk = texelFetch(u_mobykey,  ivec2(gl_FragCoord.xy), 0).r;
        float tkey = hk < 0.999999 ? 4096.0 * hk / max(1.0 - hk, 1e-9) : 1e30;
        float akey = ak < 0.999999 ? 4096.0 * ak / max(1.0 - ak, 1e-9) : 1e30;
        float mkey = mk < 0.999999 ? 4096.0 * mk / max(1.0 - mk, 1e-9) : 1e30;
        float own = DEPTH_AFFINE ? 1.0 / max(1.0 - gl_FragCoord.z, 1e-9)
                                 : 4096.0 * gl_FragCoord.z / max(1.0 - gl_FragCoord.z, 1e-9);
        if (vKoff > 0.0) {
            if (tkey < (vKoff - 0.5) - 64.0) discard;
            if (akey < (vKoff - 0.5) - 64.0 &&
                (!MOBY_TRUESCOPE || akey < own - 64.0)) discard;
        } else {   /* near-band: per-pixel own key; +1024 embed rule vs props */
            if (tkey < own - 64.0 || akey < own - 64.0 || mkey < own + 1024.0) discard;
        }
    }
    outColor = vColor / vQ;
}
