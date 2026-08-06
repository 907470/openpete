#version 450

/* Untextured OPAQUE gouraud fragment stage with the terrain KEY-RESOLVE
 * contest — quad.frag plus the LP-vs-HP og-key discard (see
 * psx_scene_key.frag for the model). Used only by the opaque scene pipes
 * whose layout carries the HP key image at set 2. */

layout(set = 2, binding = 0) uniform sampler2D u_hpkey;

layout(location = 0) in vec4 vColor;  /* x ratio (divide by vQ) */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;
layout(location = 4) flat in float vKoff;

layout(location = 0) out vec4 outColor;

void main()
{
    if (vKoff > 0.25) {   /* terrain face: contest the og key raster
                           * (flat per-face keys — see psx_scene_key.frag) */
        float hk = texelFetch(u_hpkey, ivec2(gl_FragCoord.xy), 0).r;
        if (hk < 0.999999) {
            if (1.0 / max(1.0 - hk, 1e-9) < (vKoff - 0.5) - 64.0) discard;
        }
    }
    outColor = vColor / vQ;
}
