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
 * authored OT slot offsets the game's own insert applies (the per-game
 * terms live in game_layout.terrain; 128 world per slot); stored depth is
 * true view-Z. The key rasters decide occupancy and true geometry decides
 * everything against the surface that wins.
 *
 * Inputs: terrain keys, moby keys, curated embedded-prop true depth, and
 * the moby rank raster for the same-moby painter select. A caller with no
 * meaning for one of them passes OP_KEY_NONE. */

#ifndef OPENPETE_PSX_KEY_CONTEST_GLSL
#define OPENPETE_PSX_KEY_CONTEST_GLSL

/* A key that never wins a contest. */
#define OP_KEY_NONE 1e30

/* World view-Z per ordering-table bucket. */
#define OP_KEY_SLOT 128.0

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

/* The bucket a stored terrain key falls in. The key rasters hold
 * z = kv/(kv+4096) in a 32-bit float depth image, and the rasteriser's
 * perspective divide and plane interpolation land that value within a few
 * ulps of the encoding the writer computed (one ulp low measured on NVIDIA
 * against a shader-side encode of the same key). Decoding a value one ulp
 * low moves the key by (kv+4096)^2/4096 * 2^-24 world (0.02 at 33792),
 * which crosses a bucket boundary whenever the true key sits on one; a
 * terrain fragment then loses the pixel to its own key and the pixel
 * shows whatever lies behind. The stored key is floored into its bucket
 * with two ulps of that resolution added, so a value inside the encoding's
 * error below a boundary reads as the boundary's bucket. OP_KEY_NONE
 * overflows to +inf and stays a bucket nothing sits below. */
float op_key_bucket(float tkey)
{
    float res = (tkey + 4096.0) * (tkey + 4096.0) * (2.0 / 16777216.0 / 4096.0);
    return floor((tkey + res) / OP_KEY_SLOT);
}

/* ---- the player band of the actor key image ----------------------------
 *
 * The avatar's draw keys every part at max(0, (part-origin view-Z >> 7) -
 * sortDepth) (r_pete.s 80023F8C, spyrodraw.s 8004215C/8003D148), an
 * ordering key the native path does not mirror in stored depth: a body
 * pulled slots nearer in world units would show through any wall within
 * that distance. Only the additive glow fan needs the key, to be painted
 * under the avatar the way the table paints it.
 *
 * The key rides the ACTOR key image in a reserved value band instead of a
 * sixth image: its slot index is encoded linearly from OP_PKEY_BASE in
 * steps of OP_PKEY_STEP. The band sits below every reachable real key —
 * a key is floored at one world unit, which stores at 2.4e-4, five times
 * the band's top — so the depth test that composes the image resolves a
 * pixel both classes cover to the PLAYER, and every reader that is not
 * the aura contest filters the band out through op_akey_contest() rather
 * than decoding it as an impossibly near key. The player's faces raster
 * only where an aura could read them (the key pass clips them to the aura
 * footprint), so the actor key they displace is one no other contest
 * reaches.
 *
 * OP_PKEY_STEP is four orders of magnitude above a 32-bit float's ulp at
 * the band, so the rasteriser's plane interpolation of a flat value
 * (see op_key_bucket) rounds back to the slot the writer encoded. */
#define OP_PKEY_BASE 0.000001
#define OP_PKEY_STEP 0.00000001
#define OP_PKEY_MAX_SLOT 4095.0

bool op_pkey_is(float stored)
{
    return stored > 0.0 &&
           stored <= OP_PKEY_BASE + (OP_PKEY_MAX_SLOT + 0.5) * OP_PKEY_STEP;
}

float op_pkey_slot(float stored)
{
    return floor((stored - OP_PKEY_BASE) / OP_PKEY_STEP + 0.5);
}

/* The actor key image as every contest but the aura's sees it: a player
 * texel is not a key, so it never wins a pixel from an actor or a terrain
 * fragment. */
float op_akey_contest(float stored)
{
    return op_pkey_is(stored) ? OP_KEY_NONE : op_key_from_stored(stored);
}

/* The additive glow fan's order against the bodies it overlaps. The fan is
 * a flat billboard inserted at its own view-Z bucket and it writes no
 * depth, so it occludes nothing: the only question is which of the two the
 * table paints last. Both sides carry their ordering-table slot, so the
 * verdict is the table's own — the fan loses a pixel whose actor or player
 * key sits in its bucket or nearer, and keeps it otherwise. Terrain is not
 * consulted: the fan's stored depth is true, and true depth is what decides
 * it against the ground.
 *
 * akey_stored = the raw actor-key texel, the one reader that decodes the
 * player band; own_key = the fan's own absolute key. */
bool op_aura_loses(float akey_stored, float own_key)
{
    float own_slot = floor(own_key / OP_KEY_SLOT);
    if (op_pkey_is(akey_stored)) return op_pkey_slot(akey_stored) <= own_slot;
    return op_key_bucket(op_key_from_stored(akey_stored)) <= own_slot;
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
                  float koff, bool actor, bool truescope, bool slot_contest)
{
    if (koff > 0.0) {
        float band = (koff - 0.5) - 64.0;      /* strict, by half an OT slot */
        if (slot_contest) {
            /* One ordering table, OP_KEY_SLOT world per bucket. Both classes
             * carry their absolute key, so quantizing each to its bucket
             * reproduces the table's own granularity: a difference smaller
             * than a bucket is no difference at all. The moby pass is
             * submitted before the terrain pass, so a terrain face in the
             * same bucket is appended after an actor face and paints over
             * it — the actor loses a tie, the terrain keeps one (a terrain
             * fragment must never lose to its own key). */
            float own_slot = floor((koff - 0.5) / OP_KEY_SLOT);
            float t_slot   = op_key_bucket(tkey);
            if (actor ? (t_slot <= own_slot) : (t_slot < own_slot)) return true;
        } else if (tkey < band) return true;
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
