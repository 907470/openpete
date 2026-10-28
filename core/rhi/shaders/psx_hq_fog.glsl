/* psx_hq_fog.glsl - the continuous ladder fade of a far-drawn HQ terrain
 * texel, shared by the walked scene stages (psx_scene.frag,
 * psx_scene_key.frag) and the retained terrain cache (terrain_shade.glsl),
 * which must agree bit for bit.
 *
 * A terrain face the HQ pin draws past the game's near band samples HQ
 * tiles whose CLUTs carry no fog ladder. The game would draw that face from
 * its far half, one of 16 progressively fogged CLUT rows chosen by the
 * face-mean depth. The fade re-creates the ladder on the HQ texel: the
 * per-channel affine of the face's far CLUT row 0 onto its deepest row 15
 * (texel' = aN * texel + bN, least squares over the palette), weighted by the
 * ladder position t at the fragment's view-Z, t = Δ / 15 of the row select
 * Δ = (key - fogRef + 4096) >> 8, evaluated continuously:
 *   texel' = texel * mix(1, aN, t) + bN * t.
 *
 * The words (the walk's tn_hqfog_words, packed once on the CPU so both paths
 * read identical values):
 *   a: aN.r | aN.g << 10 | aN.b << 20 (signed 10-bit, units 1/256),
 *      bit 30 the face's +0x2000 fog-reference bias, bit 31 enable;
 *   b: bN.r | bN.g << 10 | bN.b << 20 (signed 10-bit, units 1/256); bit 30
 *      (OPENPETE_HQFOG_B30) sets t = 1, the affine being pre-weighted by the
 *      face's own ladder step (the original fog form); bit 31
 *      (OPENPETE_HQFOG_NEUTRAL) replaces the texel colour by the
 *      modulation-neutral 128/255, so the stage's texel * colour / 128
 *      yields the vertex colour while the texel's alpha and STP bit still
 *      decide coverage and blending. Word a is then ignored.
 * ts and tb are the published ramp t = vz * ts + tb (texture_pack.h
 * texture_pack_fog_ladder_set); a bias shifts tb by 8192 / 3840.
 *
 * The per-pixel CLUT ladder (the per-pixel fog form on a face the game
 * ladders) is word a's enable bit clear with word b bit 30 set, word a bit 30
 * the bias: the face carries its base CLUT row and the stage reads the row at
 * the fragment's ladder position (op_hq_ladder_pos) from the palette, lerping
 * the two bracketing rows. */

#ifndef OPENPETE_PSX_HQ_FOG_GLSL
#define OPENPETE_PSX_HQ_FOG_GLSL

/* One signed 10-bit field of a fade word, in units of 1/256. */
float op_hq_fog_s10(uint w, int shift)
{
    return float(bitfieldExtract(int(w), shift, 10)) / 256.0;
}

/* The faded texel colour (0..1): 128/255 when word b's neutral bit is set,
 * else `texel` when word a's enable bit is clear. vz is the fragment's
 * full-world view-Z. */
vec3 op_hq_fog(vec3 texel, uint a, uint b, float vz, float ts, float tb)
{
    if ((b & 0x80000000u) != 0u) return vec3(128.0 / 255.0);
    if ((a & 0x80000000u) == 0u) return texel;
    float t = (b & 0x40000000u) != 0u ? 1.0
            : clamp(vz * ts + (tb - ((a & 0x40000000u) != 0u ? 8192.0 / 3840.0 : 0.0)),
                    0.0, 1.0);
    vec3 aN = vec3(op_hq_fog_s10(a, 0), op_hq_fog_s10(a, 10), op_hq_fog_s10(a, 20));
    vec3 bN = vec3(op_hq_fog_s10(b, 0), op_hq_fog_s10(b, 10), op_hq_fog_s10(b, 20));
    return clamp(texel * mix(vec3(1.0), aN, t) + bN * t, 0.0, 1.0);
}

/* Whether the words select the per-pixel CLUT ladder. */
bool op_hq_ladder_on(uint a, uint b)
{
    return (a & 0x80000000u) == 0u && (b & 0xC0000000u) == 0x40000000u;
}

/* The fragment's continuous CLUT-ladder row, 0..15: 15 t - 0.5 on the
 * published ramp, so a pixel at a face's mean depth (Spyro 1) or nearest
 * corner (Spyro 2/3, whose rows sit one step later on the same ramp) reads
 * the row the game selects there at the centre of that row's depth span. */
float op_hq_ladder_pos(uint a, float vz, float ts, float tb)
{
    float t = vz * ts + (tb - ((a & 0x40000000u) != 0u ? 8192.0 / 3840.0 : 0.0));
    return clamp(15.0 * t - 0.5, 0.0, 15.0);
}

#endif /* OPENPETE_PSX_HQ_FOG_GLSL */
