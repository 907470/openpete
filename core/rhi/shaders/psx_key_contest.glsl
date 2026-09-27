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

/* Window depth -> view-Z for the scene stages' encoding (psx_viewz.glsl
 * op_scene_clip): reversed z = 1/vz, forward z = 1 - 1/vz. */
float op_vz_from_depth(float z, bool reversed)
{
    return reversed ? 1.0 / max(z, 1e-12)
                    : 1.0 / max(1.0 - z, 1e-9);
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

/* ---- drain sub-cell keys ---------------------------------------------
 *
 * The game's near drain inserts a near-band terrain face into the ordering
 * table as sub-cells, each at its own key: a quad as a 4x4 grid of cells, a
 * triangle as the 16 sub-triangles of a 4-subdivided grid (tier 4) or the 4
 * of a 2-subdivided one (tier 2). Where the port draws such a face coarser
 * (a quad as its 2x2, a tier-4 triangle as its tier-2 split, a triangle
 * whole), one drawn triangle spans sub-cells of different keys, and a
 * fragment's key is the key of the sub-cell the game draws at its pixel.
 *
 * A drawn triangle of such a face has a coordinate p over it and a table
 * of 16 entries of 16 bits in two uvec4 (entry i in word i >> 1, halfword
 * i & 1): bits 0-13 the sub-cell's key above the face's contest key (koff),
 * in entry 0 bits 14-15 the grid (OP_DRAIN_*), in entry 1 bits 14-15 the
 * quadrant of a quad's drawn triangle. p interpolates as the triangle's
 * texture coordinates do (both are affine over the triangle in camera
 * space): the retained cache carries it per vertex (terrain.vert), the
 * walked scene evaluates it from the fragment's texture coordinate through
 * the triangle's record (op_drain_record).
 *
 * A triangle is planar and its sub-triangles are pieces of its plane, so p
 * is the drain grid's face-local coordinate in quarter-edge units (x = 4 x
 * the weight of corner B, y = 4 x the weight of corner C) and the
 * sub-triangle containing p is the one at the pixel. Table order: the
 * upward sub-triangle at column i and row j (nodes (j, i), (j, i + 1),
 * (j + 1, i)) at 4j + i, the downward one (nodes (j, i + 1), (j + 1, i),
 * (j + 1, i + 1)) at 4(3 - j) + 3 - i.
 *
 * A quad's 4x4 cells lie on its bilinear surface, which the 2x2 drawn
 * triangles depart from wherever a quadrant is not a parallelogram. The
 * cells of one quadrant meet along the quadrant's two interior isolines
 * (the straight segments joining its edge midpoints), and a quadrant's
 * outline is its drawn triangles' outline, so the cell at a pixel of a
 * drawn triangle is fixed by the side of each isoline's eye plane the
 * pixel's view ray lies on. p holds those two sides, (column, row), each
 * the dot product of the eye plane's normal with the vertex's camera-space
 * position scaled to 1 at the quadrant's far corner: positive on the far
 * column (row) of the quadrant. The cell is table entry 4r + c.
 *
 * A sub-cell the drain skips carries 0, the face's minimum. The keys come
 * from the drain's own sums (terrain.c tn_quad_cell_keys and
 * tn_tri_cell_keys on the walk, the terrain.vert mirrors on the retained
 * cache); these functions only locate the entry. */
#define OP_DRAIN_NONE  0u
#define OP_DRAIN_QUAD  1u   /* 4x4 cells                    */
#define OP_DRAIN_TRI4  2u   /* 16 sub-triangles, cell 1     */
#define OP_DRAIN_TRI2  3u   /* 4 sub-triangles, cell 2      */

uint op_drain_grid(uvec4 t0)
{
    return (t0.x >> 14) & 3u;
}

/* The table entry of the sub-cell at coordinate p (quadrant q of a quad). */
int op_drain_slot(vec2 p, uint grid, uint q)
{
    if (grid == OP_DRAIN_QUAD) {
        int r = int(q >> 1) * 2 + (p.y > 0.0 ? 1 : 0);
        int c = int(q & 1u) * 2 + (p.x > 0.0 ? 1 : 0);
        return r * 4 + c;
    }
    float h = grid == OP_DRAIN_TRI4 ? 1.0 : 2.0;
    int n = grid == OP_DRAIN_TRI4 ? 4 : 2;
    vec2 g = p / h;
    int i = clamp(int(floor(g.x)), 0, n - 1), j = clamp(int(floor(g.y)), 0, n - 1);
    bool up = (g.x - float(i)) + (g.y - float(j)) < 1.0;
    if (i + j > n - 1) {            /* past the hypotenuse: its nearest row cell */
        if (i >= j) i = n - 1 - j; else j = n - 1 - i;
        up = true;
    } else if (!up && i + j > n - 2) {
        up = true;                   /* no downward sub-triangle on the edge row */
    }
    return up ? j * 4 + i : (3 - j) * 4 + (3 - i);
}

float op_drain_entry(int slot, uvec4 t0, uvec4 t1)
{
    uint w = slot < 8 ? t0[slot >> 1] : t1[(slot - 8) >> 1];
    return float((w >> (uint(slot & 1) * 16u)) & 0x3FFFu);
}

/* The key of the sub-cell at p above the face's contest key; 0 for a face
 * without a table. The rasters of the colour and the key passes
 * interpolate p through different clip-space W, so on a pixel whose centre
 * lies on a sub-cell border the two can pick adjacent sub-cells. The key
 * pass writes the exact sub-cell (margin 0); the colour pass contests at
 * the least key within `margin` of p, so a fragment never contests at a
 * key above the one its own key raster wrote there. */
float op_drain_delta(vec2 p, uvec4 t0, uvec4 t1, float margin)
{
    uint grid = op_drain_grid(t0);
    if (grid == OP_DRAIN_NONE) return 0.0;
    uint q = (t0.x >> 30) & 3u;
    if (margin <= 0.0) return op_drain_entry(op_drain_slot(p, grid, q), t0, t1);
    float d = op_drain_entry(op_drain_slot(p + vec2(-margin, -margin), grid, q), t0, t1);
    d = min(d, op_drain_entry(op_drain_slot(p + vec2( margin, -margin), grid, q), t0, t1));
    d = min(d, op_drain_entry(op_drain_slot(p + vec2(-margin,  margin), grid, q), t0, t1));
    d = min(d, op_drain_entry(op_drain_slot(p + vec2( margin,  margin), grid, q), t0, t1));
    return d;
}

/* The colour pass's border margin in p's units (quarter edges on a
 * triangle, 1 at a quad quadrant's far corner): above the float error of
 * an interpolated coordinate, far below a pixel of any sub-cell the camera
 * can resolve. */
#define OP_DRAIN_MARGIN 0.0001

/* A contesting fragment's key channel: koff plus its sub-cell's key above
 * the face's contest key. Only a positive (ordering-table) key moves; a
 * near-band contest-only key (negative) and an unkeyed face (0) keep koff. */
float op_drain_koff_p(float koff, vec2 p, uvec4 t0, uvec4 t1)
{
    return koff > 0.25 ? koff + op_drain_delta(p, t0, t1, OP_DRAIN_MARGIN) : koff;
}

/* A walked triangle's drain record. The scene vertex's rank ordinal (tag)
 * holds -(index + 1) on a triangle with one, >= 0 on every other. Record
 * `index` is four texels of the records image at row index >> 6, columns
 * 4 (index & 63) + 0..3: the table (t0, t1), then the affine map from the
 * triangle's texture coordinate to p, (ax, bx, cx, ay) and (by, cy,
 * valid, 0), p = (ax u + bx v + cx, ay u + by v + cy). A triangle whose
 * texture coordinates are collinear (an untextured face) has no map and
 * keys at the face minimum. Returns false when the fragment has no record. */
bool op_drain_record(usampler2D rec, float tag, vec2 uv,
                     out uvec4 t0, out uvec4 t1, out vec2 p)
{
    t0 = uvec4(0u); t1 = uvec4(0u); p = vec2(0.0);
    if (tag > -0.5) return false;
    int r = int(-tag + 0.5) - 1;
    ivec2 b = ivec2((r & 63) * 4, r >> 6);
    uvec4 m1 = texelFetch(rec, b + ivec2(3, 0), 0);
    if ((m1.z & 1u) == 0u) return false;
    vec4 a = uintBitsToFloat(texelFetch(rec, b + ivec2(2, 0), 0));
    vec2 c = uintBitsToFloat(m1.xy);
    t0 = texelFetch(rec, b, 0);
    t1 = texelFetch(rec, b + ivec2(1, 0), 0);
    precise float px = a.x * uv.x + a.y * uv.y + a.z;
    precise float py = a.w * uv.x + c.x * uv.y + c.y;
    p = vec2(px, py);
    return true;
}

/* op_drain_koff_p for a walked triangle: its record by the vertex tag,
 * evaluated at texture coordinate uv. */
float op_drain_koff(float koff, float tag, vec2 uv, usampler2D rec)
{
    uvec4 t0, t1;
    vec2 p;
    if (koff <= 0.25 || !op_drain_record(rec, tag, uv, t0, t1, p)) return koff;
    return koff + op_drain_delta(p, t0, t1, OP_DRAIN_MARGIN);
}

/* The key pass's stored value for key view-Z kv (the key images' encoding,
 * op_key_from_stored's inverse). */
float op_key_store(float kv)
{
    return kv / (kv + 4096.0);
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
 * than decoding it as an impossibly near key. The key feed carries a band
 * vertex's stored value itself as its z, and the key stage writes it
 * unchanged (op_key_clip): a band value is never clamped or encoded as a
 * view-Z, which would store the one-world-unit floor's 2.4e-4 and read
 * back as a real key 1. The player's faces raster
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

/* The key pass's clip position for a key-feed vertex at clip-space screen
 * cxy. z is the feed's z: a value inside the player band is the stored band
 * value, written as z_ndc = z at w = 1 so it reaches the image bit-exact;
 * any other z is a view-Z, and the vertex stores its key view-Z kv as
 * kv/(kv+4096) with w = kv, a (z, w) pair the hardware clipper
 * interpolates without disturbing the window depth. A true view-Z inside
 * the band's range (0, 4.2e-5] lies at the eye and would clamp to the near
 * guard anyway. */
vec4 op_key_clip(vec2 cxy, float z, float kv)
{
    if (op_pkey_is(z)) return vec4(cxy, z, 1.0);
    return vec4(cxy * kv, kv * kv / (kv + 4096.0), kv);
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

/* A shadow decal against the terrain painted after it. Every game's shadow
 * routines insert their fans into the ordering table before the terrain
 * pass, so a terrain face keyed into the fan's slot or a nearer one is
 * appended later and paints over the fan, and a farther one lies under it.
 * tkey = the decoded terrain key at the pixel; own_koff = the fan
 * triangle's slot key + 0.5 (0 = not keyed, never covered). */
bool op_decal_covered(float tkey, float own_koff)
{
    return own_koff > 0.25 &&
           op_key_bucket(tkey) <= floor((own_koff - 0.5) / OP_KEY_SLOT);
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
