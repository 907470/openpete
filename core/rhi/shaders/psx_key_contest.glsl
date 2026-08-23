/* psx_key_contest.glsl — og's per-pixel painter verdicts, as pure math.
 *
 * ENGINE-INTERNAL, not part of the mod SDK. It deliberately declares NO
 * samplers and no varyings: every caller owns its own bindings (the scene
 * stages, the gouraud stage and the retained-terrain stage all sit at
 * different descriptor sets) and passes in values it has already fetched.
 * That is what lets one copy of the rules serve all of them — the three
 * stages previously carried three transcriptions of the same arithmetic
 * with "keep in sync" comments holding them together.
 *
 * The rules themselves:
 *
 * og decides WHICH surface exists at a pixel by an ORDERING-TABLE key, not
 * by depth. A face's key is its view-Z plus the authored OT slot offsets
 * (LP +0x40+8*bias, HP +4*bias, 128 world per slot); stored depth is TRUE
 * view-Z. So the key rasters decide occupancy and true geometry decides
 * everything against the surface that wins.
 *
 * Three images feed it — terrain og keys, moby og keys, and curated
 * embedded-prop TRUE depth — plus the moby og-RANK raster for the
 * same-moby painter select. Callers that have no meaning for one of them
 * pass the never-wins sentinel OP_KEY_NONE.
 */

#ifndef OPENPETE_PSX_KEY_CONTEST_GLSL
#define OPENPETE_PSX_KEY_CONTEST_GLSL

/* A key so far away it can never win a contest. */
#define OP_KEY_NONE 1e30

/* Window depth -> view-Z, inverting whichever encoding the vertex stage
 * chose. affine: z = 1 - 1/vz (screen-affine, exact under interpolation);
 * else the legacy hyperbolic z = vz/(vz+4096). */
float op_vz_from_depth(float z, bool affine)
{
    return affine ? 1.0 / max(1.0 - z, 1e-9)
                  : 4096.0 * z / max(1.0 - z, 1e-9);
}

/* A key image's stored value -> key view-Z. The images are written by
 * terrain_key.vert's canonical encoding z = kv/(kv+4096) regardless of the
 * scene's own depth mode; 1.0 means "nothing was rasterised here". */
float op_key_from_stored(float stored)
{
    return stored < 0.999999 ? 4096.0 * stored / max(1.0 - stored, 1e-9)
                             : OP_KEY_NONE;
}

/* Same-moby painter select, NEAR-TIE SCOPED. og paints a moby's faces in
 * walk order, so a fragment loses its pixel iff a LATER-ranked face of the
 * SAME moby covers it — regardless of true depth. Scoped to surfaces within
 * interpenetration range of each other (the contour-flicker / decal class):
 * cleanly separated faces keep true depth, because og's deliberate
 * relayering of those was authored for 320x240 and reads as an artifact
 * upscaled.
 *
 * e = the rank texel (r+g*256 = winning face's walk rank, b = that face's
 * present-local moby ordinal, a = its view-Z / 32640). rank = this
 * fragment's own (rank, ordinal). */
bool op_rank_loses(vec4 e, vec2 rank, float own_vz)
{
    float irank = floor(e.r * 255.0 + 0.5) + floor(e.g * 255.0 + 0.5) * 256.0;
    float iord  = floor(e.b * 255.0 + 0.5);
    if (abs(iord - rank.y) >= 0.5 || irank <= rank.x + 0.5) return false;
    return abs(e.a * 32640.0 - own_vz) < 384.0;
}

/* The og key contest. koff = the face's key channel: POSITIVE = its
 * ABSOLUTE og key + 0.5 (OT-band terrain and moby faces); NEGATIVE =
 * near-band terrain, contest-only, where og subdivides into pieces at
 * piece-local means so the fragment's own view-Z is the limit. actor =
 * this is an unranked moby face. truescope = scope moby key yields by the
 * fragment's own TRUE view-Z (the default; false restores og's full
 * compound painter).
 *
 * Under truescope, TERRAIN fragments never yield to general MOBY keys at
 * all: og's authored pulls (-128*dofs) exist to float pickups and decor
 * over whatever the OT sorts them against, and any per-pixel window
 * derived from them re-admits the punch-through classes (a chest through
 * the gnastys_loot wall lip via pull+push compounding; a rotating gem
 * through a near wall it sits 300 world behind). Terrain-over-moby is
 * decided instead by TRUE depth plus the -127 stored tie pull (contact),
 * the PROP EMBED rule below, and the ACTOR_FRONT pass.
 *
 * PROP EMBED: a curated embedded prop (pedestal discs, statue bases — a
 * class list in the moby feeder, because no geometric quantity separates
 * "prop set into its ground" from "actor just behind a crest") within 1024
 * world behind this surface wins the pixel. That is what carries dofs-0
 * pedestals at mid range, where og floated them on the ground's own
 * authored push and the true-depth scope no longer mirrors it. */
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
