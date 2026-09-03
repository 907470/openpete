/* psx_key_contest.glsl — the per-pixel painter verdicts of the original
 * renderer, as pure functions. Included by the engine halves of the
 * material contract; not a mod-facing interface.
 *
 * Declares no samplers and no varyings: each caller owns its own bindings
 * and passes in values it has already fetched, so one copy of the rules
 * serves every stage.
 *
 * The original renderer decides which surface exists at a pixel by an
 * ordering-table key, not by depth. A face's key is its view-Z plus the
 * authored OT slot offsets (LP +0x40+8*bias, HP +4*bias, 128 world per
 * slot); stored depth is true view-Z. The key rasters decide occupancy
 * and true geometry decides everything against the surface that wins.
 *
 * Inputs: terrain keys, moby keys, curated embedded-prop true depth, and
 * the moby rank raster for the same-moby painter select. A caller with no
 * meaning for one of them passes OP_KEY_NONE. */

#ifndef OPENPETE_PSX_KEY_CONTEST_GLSL
#define OPENPETE_PSX_KEY_CONTEST_GLSL

/* A key that never wins a contest. */
#define OP_KEY_NONE 1e30

/* Window depth -> view-Z for the vertex stage's encoding. affine:
 * z = 1 - 1/vz (screen-affine, exact under interpolation); otherwise the
 * hyperbolic z = vz/(vz+4096). */
float op_vz_from_depth(float z, bool affine)
{
    return affine ? 1.0 / max(1.0 - z, 1e-9)
                  : 4096.0 * z / max(1.0 - z, 1e-9);
}

/* A key image's stored value -> key view-Z. Key images are written with
 * z = kv/(kv+4096) regardless of the scene's depth mode; 1.0 means nothing
 * was rasterised at the pixel. */
float op_key_from_stored(float stored)
{
    return stored < 0.999999 ? 4096.0 * stored / max(1.0 - stored, 1e-9)
                             : OP_KEY_NONE;
}

/* Same-moby painter select, scoped to near ties. The original renderer
 * paints a moby's faces in walk order, so a fragment loses its pixel when
 * a later-ranked face of the same moby covers it, regardless of depth.
 * Applied only within interpenetration range (384 world): cleanly
 * separated faces keep true depth, since the relayering was authored for
 * 320x240 and reads as an artifact upscaled.
 *
 * e = the rank texel (r + g*256 = the winning face's walk rank, b = its
 * present-local moby ordinal, a = its view-Z / 32640). rank = this
 * fragment's own (rank, ordinal). */
bool op_rank_loses(vec4 e, vec2 rank, float own_vz)
{
    float irank = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
    float iord  = floor(e.b * 255.0 + 0.5);
    if (abs(iord - rank.y) >= 0.5 || irank <= rank.x + 0.5) return false;
    return abs(e.a * 32640.0 - own_vz) < 384.0;
}

/* The key contest. koff is the face's key channel: positive = its absolute
 * OT key + 0.5 (OT-band terrain and moby faces); negative = near-band
 * terrain, contest-only, where the fragment's own view-Z is the limit.
 * actor = an unranked moby face. truescope = moby keys yield by the
 * fragment's own true view-Z (the default; false restores the full
 * compound painter).
 *
 * Under truescope, terrain never yields to general moby keys: the authored
 * pulls (-128*dofs) float pickups and decor over whatever the OT sorts them
 * against, and a per-pixel window derived from them re-admits punch-through
 * (a chest through a wall lip, a gem through a near wall). Terrain over
 * moby is decided by true depth plus the -127 stored tie pull, the prop
 * embed rule, and the actor-front pass.
 *
 * Prop embed: a curated embedded prop (pedestal discs, statue bases; a
 * class list in the moby feeder) within 1024 world behind this surface
 * wins the pixel. This carries dofs-0 pedestals at mid range, where the
 * original floated them on the ground's own push. */
bool op_key_loses(float tkey, float akey, float mkey, float own,
                  float koff, bool actor, bool truescope)
{
    if (koff > 0.0) {
        float band = (koff - 0.5) - 64.0;      /* strict, by half an OT slot */
        if (tkey < band) return true;
        if ((actor || !truescope) && akey < band &&
            (!truescope || akey < own - 64.0)) return true;
        return truescope && !actor && mkey < own + 1024.0;
    }
    /* Near band: per-pixel own key, same strict rule, same embed rule. */
    return tkey < own - 64.0 ||
           (!truescope && akey < own - 64.0) ||
           mkey < own + 1024.0;
}

#endif /* OPENPETE_PSX_KEY_CONTEST_GLSL */
