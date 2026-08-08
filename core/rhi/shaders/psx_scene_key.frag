#version 450

/* Textured OPAQUE scene fragment stage with the terrain KEY-RESOLVE contest.
 *
 * Identical PSX VRAM decode + modulation to psx_scene.frag, plus: a keyed
 * terrain fragment (vKoff > 0, the face's ABSOLUTE og OT key + 0.5 sentinel)
 * survives only where no strictly-nearer key owns the pixel in the key
 * image. The og key of a face is TRUE view-Z + 128*slots (the authored OT
 * offsets); stored depth itself is TRUE view-Z — og's keys decide WHICH
 * terrain surface exists at a pixel, true geometry decides everything
 * against that surface. The face's own key rides the flat vKoff (exact);
 * only the image side is quantised. Non-terrain fragments (vKoff == 0)
 * never touch the sampler. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */
layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* moby og-key image */
/* Moby og-RANK image (RGBA8): r+g*256 = the winning face's OT walk rank at
 * this pixel (max rank wins the raster), b = that face's present-local moby
 * ordinal. Same-moby painter select: og paints a moby's faces in walk order,
 * so a fragment loses its pixel iff a LATER-ranked face of the SAME moby
 * covers it — regardless of true depth. Cross-moby stays true per-pixel z
 * (the ordinal gate). Stored depth is TRUE view-Z either way — the rank
 * decides WHICH same-moby face exists at a pixel, never what depth it has. */
layout(set = 4, binding = 0) uniform sampler2D u_mobyrank;

layout(push_constant) uniform PsxMaterial {
    int tpage_x, tpage_y;
    int clut_x,  clut_y;
    int bpp;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vQ;
layout(location = 3) flat in float vKoff;
layout(location = 4) flat in vec2 vRank;  /* x = own rank (0 = unranked), y = moby ordinal */

layout(location = 0) out vec4 outColor;

/* Depth encoding of gl_FragCoord.z (matches the vertex stage's selection):
 * false = canonical z=vz/(vz+4096), true = screen-affine z=1-1/vz. Needed
 * to invert the fragment's own window depth back to view-Z for the
 * near-band per-pixel contest key. */
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

uint sample_word(int u, int v)
{
    if (pc.bpp == 16) {
        return vram_at(pc.tpage_x + u, pc.tpage_y + v);
    } else if (pc.bpp == 8) {
        uint word = vram_at(pc.tpage_x + (u >> 1), pc.tpage_y + v);
        uint idx  = (word >> (8 * (u & 1))) & 0xFFu;
        return vram_at(pc.clut_x + int(idx), pc.clut_y);
    } else { /* 4bpp */
        uint word = vram_at(pc.tpage_x + (u >> 2), pc.tpage_y + v);
        uint nib  = (word >> (4 * (u & 3))) & 0xFu;
        return vram_at(pc.clut_x + int(nib), pc.clut_y);
    }
}

void main()
{
    if (vRank.x > 0.5) {   /* ranked moby face: same-moby painter select */
        vec4 e = texelFetch(u_mobyrank, ivec2(gl_FragCoord.xy), 0);
        float irank = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
        float iord  = floor(e.b * 255.0 + 0.5);
        if (abs(iord - vRank.y) < 0.5 && irank > vRank.x + 0.5) discard;
    }
    if (abs(vKoff) > 0.25) {   /* keyed face: contest the og key rasters */
        /* Two images. Set 2 = og KEYS: every keyed face (terrain, sky
         * fans, moby faces) as its ABSOLUTE og key laid FLAT — og's
         * per-pixel OT occupancy, the painter select. Set 3 = PROP TRUE
         * DEPTH: curated embedded-prop faces only (pedestal discs, statue
         * bases), per-vertex TRUE view-Z. Decodes match terrain_key.vert's
         * canonical z = kv/(kv+4096) (affine 1-1/kv quantises past the
         * 64-world margin beyond kv~46k — far faces discarded themselves).
         *
         * POSITIVE koff (OT-band terrain + moby faces): strict
         * less-by-half-slot painter select vs the key image; ties inside
         * og's 128-world quantum fall through to true depth.
         *
         * NEGATIVE koff = NEAR-BAND terrain, contest-only, per-pixel own
         * key (og subdivides the near band into pieces at PIECE-LOCAL
         * means; the fragment's own view-Z — stored depth is TRUE here —
         * is the piece limit). Vs the key image: the same strict rule
         * (og's painter, including moby keys). Vs the PROP image: the
         * EMBED rule, a deliberate divergence from painter roulette — an
         * authored prop PHYSICALLY within 1024 world behind the near
         * surface always wins the pixel, so a prop set into its ground
         * renders whole from every angle where og's view-dependent piece
         * keys show it complete or notched by camera. Prop-ness is a
         * CURATED CLASS LIST in the moby feeder: no geometric quantity
         * separates "prop set into its ground" from "actor just behind a
         * crest" (flat keys, per-pixel keys, biased and true-depth
         * margins all overlap across those classes). */
        float hk = texelFetch(u_hpkey,   ivec2(gl_FragCoord.xy), 0).r;
        float mk = texelFetch(u_mobykey, ivec2(gl_FragCoord.xy), 0).r;
        float tkey = hk < 0.999999 ? 4096.0 * hk / max(1.0 - hk, 1e-9) : 1e30;
        float mkey = mk < 0.999999 ? 4096.0 * mk / max(1.0 - mk, 1e-9) : 1e30;
        if (vKoff > 0.0) {
            if (tkey < (vKoff - 0.5) - 64.0) discard;
        } else {
            float own = DEPTH_AFFINE ? 1.0 / max(1.0 - gl_FragCoord.z, 1e-9)
                                     : 4096.0 * gl_FragCoord.z / max(1.0 - gl_FragCoord.z, 1e-9);
            if (tkey < own - 64.0 || mkey < own + 1024.0) discard;
        }
    }
    vec2 uv = vUV / vQ;
    vec4 col = vColor / vQ;
    vec4 c = decode555(sample_word(int(uv.x), int(uv.y)));
    if (c.a == 0.0) discard;
    vec3 rgb = clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, c.a);
}
