#version 450

/* Textured OPAQUE scene fragment stage with the terrain KEY-RESOLVE contest.
 *
 * Identical PSX VRAM decode + modulation to psx_scene.frag, plus: a keyed
 * terrain fragment (vKoff > 0, the face's ABSOLUTE og OT key + 0.5 sentinel)
 * survives only where no strictly-nearer key owns the pixel in the key
 * images (moby keys additionally scoped by the fragment's own TRUE view-Z
 * in free-camera mode — see the contest block). The og key of a
 * face is TRUE view-Z + 128*slots (the authored OT offsets); stored depth
 * itself is TRUE view-Z — og's keys decide WHICH terrain surface exists at
 * a pixel, true geometry decides everything against that surface. The
 * face's own key rides the flat vKoff (exact); only the image side is
 * quantised. Non-terrain fragments (vKoff == 0) never touch the sampler. */

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */
layout(set = 1, binding = 0) uniform sampler2D u_actorkey; /* moby og-key image */
layout(set = 2, binding = 0) uniform sampler2D u_hpkey;   /* terrain og-key image */
layout(set = 3, binding = 0) uniform sampler2D u_mobykey; /* prop TRUE-depth image */
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

/* Scope moby og-key yields by the fragment's own TRUE view-Z (see the
 * contest block). Default true; false restores og's compound painter. */
layout(constant_id = 3) const bool MOBY_TRUESCOPE = true;

/* Untextured lighting view — see psx_scene.frag. */
layout(constant_id = 2) const bool NOTEX = false;

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
    bool ranked = vRank.x > 0.5;
    if (ranked) {   /* ranked moby face: same-moby painter select.
                     * NEAR-TIE SCOPED: og's whole-face verdict overrides
                     * per-pixel depth only where the two same-moby surfaces
                     * are within interpenetration range of each other — the
                     * contour-flicker/decal class. Cleanly separated faces
                     * keep true depth: og's deliberate relayering of those,
                     * authored for 320x240, reads as an artifact upscaled. */
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
    /* A RANKED face skips the positive-koff painter contest: rank IS og's
     * exact same-moby painter, while the key image holds same-moby per-face
     * keys with no ordinal — large same-moby faces (jaw vs chin) sit beyond
     * the 64-world margin, so the contest would kill the rank winner while
     * the select kills everything under it (a hole to the clear). The face
     * still STAMPS its key, so keyed terrain keeps yielding to it. */
    if (!ranked && abs(vKoff) > 0.25) {   /* keyed face: contest the og key rasters */
        /* Three images. Set 2 = TERRAIN og KEYS: keyed WORLD faces as their
         * ABSOLUTE og key laid FLAT — og's per-pixel OT occupancy, the
         * painter select. Set 1 = MOBY og KEYS: keyed opaque ACTOR faces,
         * same flat-key encoding. Set 3 = PROP TRUE DEPTH: curated
         * embedded-prop faces only (pedestal discs, statue bases),
         * per-vertex TRUE view-Z. Decodes match terrain_key.vert's
         * canonical z = kv/(kv+4096) (affine 1-1/kv quantises past the
         * 64-world margin beyond kv~46k — far faces discarded themselves).
         *
         * POSITIVE koff (OT-band terrain + moby faces): strict
         * less-by-half-slot painter select vs both key images; ties inside
         * og's 128-world quantum fall through to true depth. Under
         * MOBY_TRUESCOPE (the default), the select vs MOBY keys is
         * additionally scoped by the fragment's own TRUE view-Z (stored
         * depth is true for every keyed face): a moby's key claims the
         * pixel only if it is also in front of the surface that is
         * actually there. og's OT genuinely COMPOUNDS a moby's authored
         * pull (-128*dofs) with a terrain face's authored push — at
         * gnastys_loot a chest paints through a wall lip 1300 world in
         * front of it (its key beats the wall's +8-slot-pushed key while
         * sitting BEHIND the wall's true surface), and the class shows
         * from ordinary gameplay angles, not just off-rail. The scope
         * keeps every yield within the moby's own authored pull (an
         * embedded pedestal disc's key sits in FRONT of its ground's true
         * surface, so it still paints complete); only the terrain push's
         * widening of the window is removed. Authored inside-terrain
         * cutscene actors (Delvin's wing through the tower) are outside
         * this rule entirely: the ACTOR_FRONT class never emits keys and
         * draws through the scissored front pass. MOBY_TRUESCOPE=false
         * restores og's compound painter for A/B.
         *
         * NEGATIVE koff = NEAR-BAND terrain, contest-only, per-pixel own
         * key (og subdivides the near band into pieces at PIECE-LOCAL
         * means; the fragment's own view-Z — stored depth is TRUE here —
         * is the piece limit). Vs the terrain and moby key images: the
         * same strict rule (og's painter). Vs the PROP image: the
         * EMBED rule, a deliberate divergence from painter roulette — an
         * authored prop PHYSICALLY within 1024 world behind the near
         * surface always wins the pixel, so a prop set into its ground
         * renders whole from every angle where og's view-dependent piece
         * keys show it complete or notched by camera. Prop-ness is a
         * CURATED CLASS LIST in the moby feeder: no geometric quantity
         * separates "prop set into its ground" from "actor just behind a
         * crest" (flat keys, per-pixel keys, biased and true-depth
         * margins all overlap across those classes). */
        float hk = texelFetch(u_hpkey,    ivec2(gl_FragCoord.xy), 0).r;
        float ak = texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r;
        float mk = texelFetch(u_mobykey,  ivec2(gl_FragCoord.xy), 0).r;
        float tkey = hk < 0.999999 ? 4096.0 * hk / max(1.0 - hk, 1e-9) : 1e30;
        float akey = ak < 0.999999 ? 4096.0 * ak / max(1.0 - ak, 1e-9) : 1e30;
        float mkey = mk < 0.999999 ? 4096.0 * mk / max(1.0 - mk, 1e-9) : 1e30;
        float own = DEPTH_AFFINE ? 1.0 / max(1.0 - gl_FragCoord.z, 1e-9)
                                 : 4096.0 * gl_FragCoord.z / max(1.0 - gl_FragCoord.z, 1e-9);
        bool actor = vRank.x < -0.5;   /* unranked moby faces mark rank -1 */
        if (vKoff > 0.0) {
            if (tkey < (vKoff - 0.5) - 64.0) discard;
            if (akey < (vKoff - 0.5) - 64.0 &&
                (!MOBY_TRUESCOPE || akey < own - 64.0)) discard;
            /* Prop EMBED rule on the OT band (terrain fragments only, the
             * near-band rule's twin): a curated embedded prop within 1024
             * world behind this terrain's TRUE surface wins the pixel.
             * Under the true-depth scope this is what carries the
             * dofs-0 pedestal discs at mid/far range — og floated them on
             * the ground's own authored push, which the scope no longer
             * mirrors. */
            if (MOBY_TRUESCOPE && !actor && mkey < own + 1024.0) discard;
        } else {
            if (tkey < own - 64.0 || akey < own - 64.0 || mkey < own + 1024.0) discard;
        }
    }
    vec2 uv = vUV / vQ;
    vec4 col = vColor / vQ;
    vec4 c = decode555(sample_word(int(uv.x), int(uv.y)));
    if (c.a == 0.0) discard;
    if (NOTEX) c.rgb = vec3(128.0 / 255.0);
    vec3 rgb = clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, c.a);
}
