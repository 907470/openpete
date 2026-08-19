#version 450
#extension GL_ARB_gpu_shader_fp64 : require

/* Terrain sector-mesh fragment stage — psx_scene.frag / psx_scene_key.frag
 * with the per-draw push-constant material replaced by FLAT varyings from
 * terrain.vert (the material is per FACE in the retained mesh: the
 * CLUT-ladder fog row and the near-tile select are camera-dependent face
 * verdicts computed in the vertex stage). Untextured faces (LP, flat-gouraud
 * HP) branch to the plain vertex colour. Decode + modulation + the og
 * key-resolve contest are identical to the scene stages — keep in sync.
 *
 * Contest scope: terrain is never ranked and never an actor, and the moby
 * true-scope is enforced ON for the retained path (OPENPETE_MOBY_TRUESCOPE=0
 * claims the whole walk back to the CPU), so only the terrain-key and
 * prop-key rules apply here. Key images bind at sets 1/2 (the terrain
 * pipeline layout's continuity sets; set 3 is the UBO pair). */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */
layout(set = 1, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
layout(set = 2, binding = 0) uniform sampler2D u_mobykey; /* prop TRUE-depth image */

layout(location = 0)      in vec2  vUV;    /* x q (divide by vQ)             */
layout(location = 1)      in vec4  vColor; /* x q (divide by vQ)             */
layout(location = 2) flat in uvec2 vMat;   /* x: tpx|tpy<<10|bppc<<19|textured<<23,
                                            * y: clx | cly<<16 */
layout(location = 3) flat in float vKoff;  /* og OT key (0 = out of contest) */
layout(location = 4)      in float vQ;

layout(location = 0) out vec4 outColor;

/* Debug sector tint (globals gflags bit 29): paint by sector id. */
layout(std140, set = 3, binding = 0) uniform TerrainGlobals {
    vec4  mrow0; vec4 mrow1; vec4 mrow2;
    dvec2 dcam_xy; dvec2 dcam_zf; dvec2 dof_xy; dvec2 dH;
    ivec4 fog;
} g;

/* gl_FragCoord.z encoding — matches the vertex stage's selection. */
layout(constant_id = 0) const bool DEPTH_AFFINE = false;

uint vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x, y), 0).r; }

float expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }

vec4 decode555(uint p)
{
    if (p == 0u) return vec4(0.0);            /* PSX transparent sentinel */
    return vec4(expand5( p        & 31u),
                expand5((p >> 5)  & 31u),
                expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}

void main()
{
    if ((g.fog.y & (1 << 29)) != 0) {          /* debug tint modes */
        int mode = (g.fog.y >> 26) & 7;
        if (mode == 2 || mode >= 4) {           /* raw (or substituted) colour */
            outColor = vec4((vColor / vQ).rgb, 1.0);
        } else if (mode == 3) {                 /* UV visualizer (texel/256) */
            vec2 uvq = vUV / vQ;
            outColor = vec4(fract(uvq.x / 256.0), fract(uvq.y / 256.0), 0.0, 1.0);
        } else {                                /* ladder verdict */
            uint half_ = (vMat.y >> 30) & 1u;
            uint dlt   = (vMat.y >> 25) & 31u;
            outColor = vec4(float(half_), float(dlt) / 15.0,
                            (vMat.x & (1u << 23)) != 0u ? 0.0 : 1.0, 1.0);
        }
        return;
    }
    /* og key-resolve contest (rules in psx_scene_key.frag; terrain subset:
     * not ranked, not an actor, true-scope on). */
    if (abs(vKoff) > 0.25) {
        float hk = texelFetch(u_hpkey,   ivec2(gl_FragCoord.xy), 0).r;
        float mk = texelFetch(u_mobykey, ivec2(gl_FragCoord.xy), 0).r;
        float tkey = hk < 0.999999 ? 4096.0 * hk / max(1.0 - hk, 1e-9) : 1e30;
        float mkey = mk < 0.999999 ? 4096.0 * mk / max(1.0 - mk, 1e-9) : 1e30;
        float own = DEPTH_AFFINE ? 1.0 / max(1.0 - gl_FragCoord.z, 1e-9)
                                 : 4096.0 * gl_FragCoord.z / max(1.0 - gl_FragCoord.z, 1e-9);
        if (vKoff > 0.0) {
            if (tkey < (vKoff - 0.5) - 64.0) discard;
            /* Prop EMBED rule (true-scope): a curated embedded prop within
             * 1024 world behind this terrain's TRUE surface wins the pixel. */
            if (mkey < own + 1024.0) discard;
        } else {   /* near-band: per-pixel own key; +1024 embed rule vs props */
            if (tkey < own - 64.0 || mkey < own + 1024.0) discard;
        }
    }

    uint mat0 = vMat.x;
    vec4 col = vColor / vQ;
    if ((mat0 & (1u << 23)) == 0u) {          /* untextured: plain gouraud */
        outColor = vec4(col.rgb, 1.0);
        return;
    }
    int tpx  = int(mat0 & 0x3FFu);
    int tpy  = int((mat0 >> 10) & 0x1FFu);
    uint bppc = (mat0 >> 19) & 3u;
    int clx  = int(vMat.y & 0xFFFFu);
    int cly  = int(vMat.y >> 16);

    vec2 uvq = vUV / vQ;
    int u = int(uvq.x), v = int(uvq.y);
    uint p;
    if (bppc == 2u) {          /* 16bpp direct */
        p = vram_at(tpx + u, tpy + v);
    } else if (bppc == 1u) {   /* 8bpp CLUT */
        uint word = vram_at(tpx + (u >> 1), tpy + v);
        uint idx  = (word >> (8 * (u & 1))) & 0xFFu;
        p = vram_at(clx + int(idx), cly);
    } else {                   /* 4bpp CLUT */
        uint word = vram_at(tpx + (u >> 2), tpy + v);
        uint nib  = (word >> (4 * (u & 3))) & 0xFu;
        p = vram_at(clx + int(nib), cly);
    }
    vec4 c = decode555(p);
    if (c.a == 0.0) discard;
    /* PSX texture modulation: texel * vertexColour / 128 (clamped). */
    vec3 rgb = clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, c.a);
}
