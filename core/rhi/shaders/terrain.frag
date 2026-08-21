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
layout(set = 4, binding = 0) uniform sampler2D u_hdatlas; /* HD-fold atlas (NEAREST) */

layout(location = 0)      in vec2  vUV;    /* x q (divide by vQ)             */
layout(location = 1)      in vec4  vColor; /* x q (divide by vQ)             */
layout(location = 2) flat in uvec2 vMat;   /* x: tpx|tpy<<10|bppc<<19|textured<<23,
                                            * y: clx | cly<<16 */
layout(location = 3) flat in float vKoff;  /* og OT key (0 = out of contest) */
layout(location = 4)      in float vQ;
layout(location = 5) flat in uint  vHd;    /* HD-fold entry id | flags (vert stage) */

layout(location = 0) out vec4 outColor;

/* Debug sector tint (globals gflags bit 29): paint by sector id. */
layout(std140, set = 3, binding = 0) uniform TerrainGlobals {
    vec4  mrow0; vec4 mrow1; vec4 mrow2;
    dvec2 dcam_xy; dvec2 dcam_zf; dvec2 dof_xy; dvec2 dH;
    ivec4 fog;
    vec4  pad_[302];     /* scroll_dv + the rim frames (vertex-stage state) */
    vec4  hd_lad;        /* HD fold: CONT-fog ramp ts, tb; count; enable   */
    vec4  hd_ent[1664];  /* 416 entries x 4 vec4 (rect/atlas/fogA/fogB)    */
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
        if (mode == 6) {                        /* exact ladder t0k readout */
            if ((vMat.y & (1u << 16)) == 0u) {  /* ladder never ran here */
                outColor = vec4(0.0, 0.0, 1.0, 1.0);
            } else {
                uint enc = (vMat.y >> 18) & 0x1FFFu;   /* t0k + 4096 */
                outColor = vec4(float((enc >> 8) & 255u) / 255.0,
                                float(enc & 255u) / 255.0,
                                float(((vMat.y >> 17) & 1u) * 128u
                                      + ((vMat.y >> 9) & 0x7Fu)) / 255.0, 1.0);
            }
            return;
        }
        if (mode == 2 || mode == 4 || mode == 5) {  /* raw (or substituted) colour */
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
    /* HD-fold replacement: a baked entry replaces the VRAM fetch —
     * hd_tex_cutout.frag's math (file-box mapping, alpha cutout, the
     * continuous pack-fog ramp) evaluated from the entry table, then the
     * same PSX modulation. Entries exist only on opaque faces. */
    if ((vHd & 0xFFFFu) != 0u) {
        uint ei = ((vHd & 0xFFFFu) - 1u) * 4u;
        vec4 rect  = g.hd_ent[ei];
        vec4 arect = g.hd_ent[ei + 1u];
        vec4 fogA  = g.hd_ent[ei + 2u];
        vec4 fogB  = g.hd_ent[ei + 3u];
        vec2 uvh = vUV / vQ;
        vec2 t = clamp((uvh - rect.xy) / rect.zw, 0.0, 1.0);
        /* Half a texel inside the file's atlas rect: clamp-to-edge
         * without bleeding across the shelf gutter. */
        vec2 ht = 0.5 / arect.zw;
        t = clamp(t, ht, 1.0 - ht);
        vec2 auv = arect.xy + t * arect.zw;
        vec4 hc;
        if (fogA.w > 0.5) {   /* linear-class file: manual bilinear (atlas is NEAREST) */
            vec2 tx = auv - 0.5;
            vec2 fw = fract(tx);
            ivec2 b = ivec2(floor(tx));
            vec4 s00 = texelFetch(u_hdatlas, b, 0);
            vec4 s10 = texelFetch(u_hdatlas, b + ivec2(1, 0), 0);
            vec4 s01 = texelFetch(u_hdatlas, b + ivec2(0, 1), 0);
            vec4 s11 = texelFetch(u_hdatlas, b + ivec2(1, 1), 0);
            hc = mix(mix(s00, s10, fw.x), mix(s01, s11, fw.x), fw.y);
        } else {              /* exact-multiple file: stock-identical fetch */
            hc = texelFetch(u_hdatlas, ivec2(auv), 0);
        }
        if (hc.a < 0.5) discard;
        if ((vHd & 0x10000u) != 0u && g.hd_lad.x != 0.0) {
            float w = DEPTH_AFFINE ? 1.0 / max(1.0 - gl_FragCoord.z, 1e-9)
                                   : 4096.0 * gl_FragCoord.z / max(1.0 - gl_FragCoord.z, 1e-9);
            float tt = clamp(w * g.hd_lad.x +
                             (g.hd_lad.y - ((vHd & 0x20000u) != 0u ? 8192.0 / 3840.0 : 0.0)),
                             0.0, 1.0);
            hc.rgb = clamp(hc.rgb * mix(vec3(1.0), fogA.rgb, tt) + fogB.rgb * tt, 0.0, 1.0);
        }
        outColor = vec4(clamp(hc.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0), 1.0);
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
