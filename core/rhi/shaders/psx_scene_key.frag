#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

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

/* Depth convention of gl_FragCoord.z (the vertex stage's selection, see
 * op_scene_clip): needed to invert the fragment's own window depth back to
 * view-Z for the near-band per-pixel contest key. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;

/* Scope moby og-key yields by the fragment's own TRUE view-Z (see the
 * contest block). Default true; false restores og's compound painter. */
layout(constant_id = 3) const bool MOBY_TRUESCOPE = true;
/* Fragment spec-constant 4 (`SLOT_CONTEST`): the key contest compares
 * 128-world ordering-table slot indices and resolves an equal slot to the
 * terrain. Set per game from game_layout.terrain.slot_ot_contest. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;


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

/* The CLUT index of a paletted texel (4 or 8 bpp). */
uint clut_index(int u, int v)
{
    if (pc.bpp == 8) {
        uint word = vram_at(pc.tpage_x + (u >> 1), pc.tpage_y + v);
        return (word >> (8 * (u & 1))) & 0xFFu;
    }
    uint word = vram_at(pc.tpage_x + (u >> 2), pc.tpage_y + v);
    return (word >> (4 * (u & 3))) & 0xFu;
}

uint sample_word(int u, int v)
{
    if (pc.bpp == 16) return vram_at(pc.tpage_x + u, pc.tpage_y + v);
    return vram_at(pc.clut_x + int(clut_index(u, v)), pc.clut_y);
}

/* Second fogged-CLUT row of a textured face: the vertex alpha code of
 * psx_scene.frag's clut_fog_mix, decoded identically. */
vec3 clut_fog_mix(vec3 rgb, vec4 col, int u, int v)
{
    uint code = uint(round(clamp(col.a, 0.0, 1.0) * 255.0));
    uint q = code & 15u, hi = code >> 4;
    if (NOTEX || pc.bpp == 16 || hi == 15u || q == 0u) return rgb;
    uint w2 = vram_at(pc.clut_x + int(clut_index(u, v)), pc.clut_y + int(hi) - 7);
    if (w2 == 0u) return rgb;
    vec3 rgb2 = clamp(decode555(w2).rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    return mix(rgb, rgb2, float(q) / 30.0);
}

void main()
{
    /* Set 2 = TERRAIN og KEYS (keyed WORLD faces as their ABSOLUTE og key
     * laid flat — og's per-pixel OT occupancy). Set 1 = MOBY og KEYS
     * (keyed opaque ACTOR faces, same encoding). Set 3 = PROP TRUE DEPTH
     * (curated embedded-prop faces only). Set 4 = the moby og-RANK raster.
     * The verdicts themselves live in psx_key_contest.glsl. */
    bool ranked = vRank.x > 0.5;
    float own = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
    if (ranked &&
        op_rank_loses(texelFetch(u_mobyrank, ivec2(gl_FragCoord.xy), 0), vRank, own))
        discard;
    /* A RANKED face skips the positive-koff painter contest: rank IS og's
     * exact same-moby painter, while the key image holds same-moby per-face
     * keys with no ordinal — large same-moby faces (jaw vs chin) sit beyond
     * the 64-world margin, so the contest would kill the rank winner while
     * the select kills everything under it (a hole to the clear). The face
     * still STAMPS its key, so keyed terrain keeps yielding to it. */
    if (!ranked && abs(vKoff) > 0.25) {
        /* The additive glow fan (rank -2) runs the aura contest instead —
         * see psx_key_contest.glsl. */
        if (vRank.x < -1.5) {
            if (op_aura_loses(texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r,
                              vKoff - 0.5))
                discard;
        } else if (op_key_loses(
                op_key_from_stored(texelFetch(u_hpkey,    ivec2(gl_FragCoord.xy), 0).r),
                op_akey_contest(texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r),
                op_key_from_stored(texelFetch(u_mobykey,  ivec2(gl_FragCoord.xy), 0).r),
                own, vKoff,
                vRank.x < -0.5,     /* unranked moby faces mark rank -1 */
                MOBY_TRUESCOPE, SLOT_CONTEST))
            discard;
    }
    vec2 uv = vUV / vQ;
    vec4 col = vColor / vQ;
    vec4 c = decode555(sample_word(int(uv.x), int(uv.y)));
    if (c.a == 0.0) discard;
    if (NOTEX) c.rgb = vec3(128.0 / 255.0);
    vec3 rgb = clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0);
    rgb = clut_fog_mix(rgb, col, int(uv.x), int(uv.y));
    outColor = vec4(rgb, c.a);
}
