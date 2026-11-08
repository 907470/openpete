/* psx_key_contest.glsl - the per-pixel painter verdicts of the original
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

/* The key images' encoding: stored = kv * 2^-26, the key view-Z scaled by a
 * power of two. The scale only moves the exponent, so a key from 1 to
 * OP_VZ_FAR (4.1e7, under 2^26) stores exactly, decodes exactly, and keeps
 * its order: two keys store in the order of their values, and a key in a
 * bucket decodes to a value in that bucket, on any GPU, whatever arithmetic
 * produced the key. A rational map such as kv/(kv+4096) has neither
 * property in float: its divide rounds, so a key a few ulps above a bucket
 * boundary can store one ulp below the boundary's own encoding and read
 * back in the bucket before it (31232 + 3 * 2^-9 against 31232). The key
 * values reach the stored range [2^-26, 0.61]; 1.0 is the clear, nothing
 * rasterised at the pixel. */
#define OP_KEY_STORE_SCALE 1.4901161193847656e-8   /* 2^-26 */
#define OP_KEY_LOAD_SCALE  67108864.0              /* 2^26 */

/* A key image's stored value -> key view-Z (op_key_store's inverse, exact);
 * the clear reads OP_KEY_NONE. */
float op_key_from_stored(float stored)
{
    return stored < 0.999999 ? stored * OP_KEY_LOAD_SCALE : OP_KEY_NONE;
}

/* The key pass's stored value for key view-Z kv. Every key stage writes its
 * fragment depth through this function, a flat key from the key and the
 * prop class from its true view-Z at the fragment. */
float op_key_store(float kv)
{
    return kv * OP_KEY_STORE_SCALE;
}

/* Whether a decoded key tkey (op_key_from_stored of a key image texel) lies
 * in an ordering-table bucket nearer than slot: below the slot's boundary,
 * slot * OP_KEY_SLOT. The decoded key is the stored key exactly, so a key on
 * the boundary or above it sits in the slot. OP_KEY_NONE is never nearer. */
bool op_key_slot_nearer(float tkey, float slot)
{
    return tkey < slot * OP_KEY_SLOT;
}

/* Whether a decoded key tkey lies at or nearer than the band edge band
 * (a face's key less half an ordering-table slot), the terrain-against-
 * terrain test where the scene is not submitted through one ordering table.
 * A key exactly half a slot nearer than the face decodes to exactly the edge
 * and counts as nearer. Such a pair shares a bucket when the nearer key lies
 * in the bucket's first half, where the table's insertion order decides and
 * this test does not model it. OP_KEY_NONE is never nearer. */
bool op_key_band_nearer(float tkey, float band)
{
    return tkey <= band;
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
 * quadrant of a quad's drawn triangle, in entry 2 + q bit 14 the fold flag
 * of quad quadrant q (op_drain_folded). p interpolates as the triangle's
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
 * The isolines partition the quadrant's screen image only while no cell
 * crosses them there. A quadrant seen near edge-on can fold: a drawn cell
 * reaches across an isoline's eye plane into the slot of a skipped cell,
 * and the isoline sides name the skipped slot on pixels the drawn cell
 * covers. Such a quadrant carries the fold flag (terrain.c tn_quad_folds,
 * the terrain.vert mirror), and a fragment of it whose slot is skipped is
 * located by the drain's own cell polygons instead (op_drain_fold_delta).
 *
 * A sub-cell the drain skips (a straddler's back-facing cell, a cell whose
 * key is not positive) carries OP_DRAIN_SKIP: the game draws nothing there,
 * so a fragment of the coarser drawn mesh in that sub-cell is discarded in
 * the key and the colour passes. A real entry is at most 0x3FFE. The keys
 * come from the drain's own sums (terrain.c tn_quad_cell_keys and
 * tn_tri_cell_keys on the walk, the terrain.vert mirrors on the retained
 * cache); these functions only locate the entry. */
#define OP_DRAIN_NONE  0u
#define OP_DRAIN_QUAD  1u   /* 4x4 cells                    */
#define OP_DRAIN_TRI4  2u   /* 16 sub-triangles, cell 1     */
#define OP_DRAIN_TRI2  3u   /* 4 sub-triangles, cell 2      */

/* The entry of a sub-cell the drain skips (14-bit field, all ones). */
#define OP_DRAIN_SKIP  16383.0

/* The key channel op_drain_koff / op_drain_koff_pf return for a fragment in
 * a skipped sub-cell; the caller discards it (op_drain_discards). */
#define OP_DRAIN_DISCARD -1e30

bool op_drain_discards(float koff)
{
    return koff < -1e29;
}

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
 * without a table, OP_DRAIN_SKIP when the sub-cell at p is skipped. The
 * rasters of the colour and the key passes interpolate p through different
 * clip-space W, so on a pixel whose centre lies on a sub-cell border the
 * two can pick adjacent sub-cells. The key pass writes the exact sub-cell
 * (margin 0); the colour pass contests at the least key of the drawn
 * sub-cells within `margin` of p, so a fragment never contests at a key
 * above the one its own key raster wrote there. */
float op_drain_delta(vec2 p, uvec4 t0, uvec4 t1, float margin)
{
    uint grid = op_drain_grid(t0);
    if (grid == OP_DRAIN_NONE) return 0.0;
    uint q = (t0.x >> 30) & 3u;
    float e = op_drain_entry(op_drain_slot(p, grid, q), t0, t1);
    if (margin <= 0.0 || e >= OP_DRAIN_SKIP) return e;
    float d = OP_DRAIN_SKIP;
    for (int k = 0; k < 4; k++) {
        vec2 o = vec2((k & 1) != 0 ? margin : -margin, k >= 2 ? margin : -margin);
        float n = op_drain_entry(op_drain_slot(p + o, grid, q), t0, t1);
        if (n < OP_DRAIN_SKIP) d = min(d, n);
    }
    return d < OP_DRAIN_SKIP ? d : e;
}

/* The colour pass's border margin in p's units (quarter edges on a
 * triangle, 1 at a quad quadrant's far corner): above the float error of
 * an interpolated coordinate, far below a pixel of any sub-cell the camera
 * can resolve. */
#define OP_DRAIN_MARGIN 0.0001

/* ---- folded quadrants ------------------------------------------------
 *
 * A fold fragment carries its position D and the face's corners TL, TR,
 * BL, BR in one linear image of camera space, the homogeneous screen
 * space (OFX z + H x, OFY z + H y, z): D projects to the fragment's pixel
 * and the corners to the face's corners, and the map preserves the sign
 * of every triple product, so a ray test there is the camera-space one.
 * The node (r, c) of the drain's 5x5 grid is the walk's halving-order mean
 * of the corners (terrain.c nc4_grid); the drain draws cell 4r + c as the
 * triangles (TL, TR, BL) and (BR, BL, TR) on its nodes. */

/* The drain grid's quarter point k of the segment a-b, by halving. */
vec3 op_fold_pt(vec3 a, vec3 b, int k)
{
    if (k == 0) return a;
    if (k == 4) return b;
    precise vec3 m = (a + b) * 0.5;
    if (k == 2) return m;
    precise vec3 q = ((k == 1 ? a : b) + m) * 0.5;
    return q;
}

/* Node (r, c) of the 5x5 grid over the corners (terrain.c nc4_grid). */
vec3 op_fold_node(int r, int c, vec3 TL, vec3 TR, vec3 BL, vec3 BR)
{
    return op_fold_pt(op_fold_pt(TL, BL, r), op_fold_pt(TR, BR, r), c);
}

/* Whether ray D passes through triangle (a, b, c) in front of the eye:
 * D's coordinates over a, b, c (triple-product ratios) are each at least
 * -margin of their sum, and the sum is positive. */
bool op_fold_ray_in(vec3 a, vec3 b, vec3 c, vec3 D, float margin)
{
    precise float t  = dot(a, cross(b, c));
    if (t == 0.0) return false;
    precise float l0 = dot(D, cross(b, c)) / t;
    precise float l1 = dot(a, cross(D, c)) / t;
    precise float l2 = dot(a, cross(b, D)) / t;
    precise float s  = l0 + l1 + l2;
    float m = margin * s;
    return s > 0.0 && l0 >= -m && l1 >= -m && l2 >= -m;
}

/* Whether the fragment's quadrant (a quad table's quadrant bits) folds. */
bool op_drain_folded(uvec4 t0)
{
    if (op_drain_grid(t0) != OP_DRAIN_QUAD) return false;
    uint q = (t0.x >> 30) & 3u;
    uint w = q < 2u ? t0.y : t0.z;
    return ((w >> ((q & 1u) * 16u + 14u)) & 1u) != 0u;
}

/* The key above the face's contest key of the least drawn cell whose
 * drain triangles contain ray D (each widened by `margin` of its
 * coordinate sum, as op_drain_delta's margin), OP_DRAIN_SKIP when none
 * does. */
float op_drain_fold_delta(vec3 D, vec3 TL, vec3 TR, vec3 BL, vec3 BR,
                          uvec4 t0, uvec4 t1, float margin)
{
    float d = OP_DRAIN_SKIP;
    for (int i = 0; i < 16; i++) {
        float e = op_drain_entry(i, t0, t1);
        if (e >= OP_DRAIN_SKIP || e >= d) continue;
        int r = i >> 2, c = i & 3;
        vec3 tl = op_fold_node(r, c, TL, TR, BL, BR);
        vec3 tr = op_fold_node(r, c + 1, TL, TR, BL, BR);
        vec3 bl = op_fold_node(r + 1, c, TL, TR, BL, BR);
        vec3 br = op_fold_node(r + 1, c + 1, TL, TR, BL, BR);
        if (op_fold_ray_in(tl, tr, bl, D, margin) || op_fold_ray_in(br, bl, tr, D, margin))
            d = e;
    }
    return d;
}

/* A contesting fragment's key channel: koff plus its sub-cell's key above
 * the face's contest key, OP_DRAIN_DISCARD in a skipped sub-cell. Only a
 * positive (ordering-table) key moves; a near-band contest-only key
 * (negative) and an unkeyed face (0) keep koff. The sub-cell is the one at
 * p (op_drain_delta); where that one is skipped and the quadrant folds, the
 * least drawn cell containing ray D (op_drain_fold_delta, the fold data D
 * and the corners). */
float op_drain_koff_pf(float koff, vec2 p, uvec4 t0, uvec4 t1,
                       vec3 D, vec3 TL, vec3 TR, vec3 BL, vec3 BR)
{
    if (koff <= 0.25) return koff;
    float d = op_drain_delta(p, t0, t1, OP_DRAIN_MARGIN);
    if (d >= OP_DRAIN_SKIP && op_drain_folded(t0))
        d = op_drain_fold_delta(D, TL, TR, BL, BR, t0, t1, OP_DRAIN_MARGIN);
    return d >= OP_DRAIN_SKIP ? OP_DRAIN_DISCARD : koff + d;
}

/* A walked triangle's drain record. The scene vertex's rank ordinal (tag)
 * holds -(index + 1) on a triangle with one, >= 0 on every other. Record
 * `index` is four texels of the records image at row index >> 6, columns
 * 4 (index & 63) + 0..3: the table (t0, t1), then the affine map from the
 * triangle's texture coordinate to p, (ax, bx, cx, ay) and (by, cy,
 * valid, fold), p = (ax u + bx v + cx, ay u + by v + cy). A triangle whose
 * texture coordinates are collinear (an untextured face) has no map and
 * keys at the face minimum. Returns false when the fragment has no record.
 * A triangle of a folded quadrant (fold 1) is followed by records index + 1
 * and index + 2 (op_drain_record_fold). */
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

/* Texel k (0..7) of the two records after walked record r. */
uvec4 op_drain_ext(usampler2D rec, int r, int k)
{
    int n = r + 1 + (k >> 2);
    return texelFetch(rec, ivec2((n & 63) * 4 + (k & 3), n >> 6), 0);
}

/* The fold data of a walked triangle at texture coordinate uv: the face's
 * homogeneous-screen corners (texels 0-2 of the extension: TL.xyz TR.x,
 * TR.yz BL.xy, BL.z BR.xyz) and the fragment's position D, the affine map
 * from uv in texels 3-5 (Dx = e3.x u + e3.y v + e3.z, Dy = e3.w u + e4.x v
 * + e4.y, Dz = e4.z u + e4.w v + e5.x). Returns false when the record
 * carries none. */
bool op_drain_record_fold(usampler2D rec, float tag, vec2 uv, out vec3 D,
                          out vec3 TL, out vec3 TR, out vec3 BL, out vec3 BR)
{
    D = vec3(0.0); TL = vec3(0.0); TR = vec3(0.0); BL = vec3(0.0); BR = vec3(0.0);
    if (tag > -0.5) return false;
    int r = int(-tag + 0.5) - 1;
    if ((texelFetch(rec, ivec2((r & 63) * 4 + 3, r >> 6), 0).w & 1u) == 0u) return false;
    vec4 e0 = uintBitsToFloat(op_drain_ext(rec, r, 0));
    vec4 e1 = uintBitsToFloat(op_drain_ext(rec, r, 1));
    vec4 e2 = uintBitsToFloat(op_drain_ext(rec, r, 2));
    vec4 e3 = uintBitsToFloat(op_drain_ext(rec, r, 3));
    vec4 e4 = uintBitsToFloat(op_drain_ext(rec, r, 4));
    vec4 e5 = uintBitsToFloat(op_drain_ext(rec, r, 5));
    TL = e0.xyz; TR = vec3(e0.w, e1.xy); BL = vec3(e1.zw, e2.x); BR = e2.yzw;
    precise float dx = e3.x * uv.x + e3.y * uv.y + e3.z;
    precise float dy = e3.w * uv.x + e4.x * uv.y + e4.y;
    precise float dz = e4.z * uv.x + e4.w * uv.y + e5.x;
    D = vec3(dx, dy, dz);
    return true;
}

/* op_drain_fold_delta for a walked triangle whose isoline slot is skipped:
 * OP_DRAIN_SKIP unless its quadrant folds and its record carries the fold
 * data. */
float op_drain_fold_rec(usampler2D rec, float tag, vec2 uv, uvec4 t0, uvec4 t1,
                        float margin)
{
    vec3 D, TL, TR, BL, BR;
    if (!op_drain_folded(t0) || !op_drain_record_fold(rec, tag, uv, D, TL, TR, BL, BR))
        return OP_DRAIN_SKIP;
    return op_drain_fold_delta(D, TL, TR, BL, BR, t0, t1, margin);
}

/* op_drain_koff_pf for a walked triangle: its record by the vertex tag,
 * evaluated at texture coordinate uv, the fold data read by
 * op_drain_fold_rec. */
float op_drain_koff(float koff, float tag, vec2 uv, usampler2D rec)
{
    uvec4 t0, t1;
    vec2 p;
    if (koff <= 0.25 || !op_drain_record(rec, tag, uv, t0, t1, p)) return koff;
    float d = op_drain_delta(p, t0, t1, OP_DRAIN_MARGIN);
    if (d >= OP_DRAIN_SKIP) d = op_drain_fold_rec(rec, tag, uv, t0, t1, OP_DRAIN_MARGIN);
    return d >= OP_DRAIN_SKIP ? OP_DRAIN_DISCARD : koff + d;
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
 * steps of OP_PKEY_STEP. The band sits below every reachable real key -
 * a key is floored at one world unit, which stores at 2^-26, four times
 * the band's top - so the depth test that composes the image resolves a
 * pixel both classes cover to the PLAYER, and every reader that is not
 * the aura contest filters the band out through op_akey_contest() rather
 * than decoding it as an impossibly near key. The key feed carries a band
 * vertex's stored value itself as its z, and the key stage writes that
 * value as its fragment depth unchanged (terrain_key.vert, the band form):
 * a band value is never clamped or encoded as a view-Z, which would store
 * the one-world-unit floor's 2^-26 and read back as a real key 1. The
 * player's faces raster only where an aura could read them (the key pass
 * clips them to the aura footprint), so the actor key they displace is one
 * no other contest reaches.
 *
 * OP_PKEY_BASE (2^-36) and OP_PKEY_STEP (2^-40) are powers of two, so every
 * band value (16 + slot) * 2^-40 is exact in float and its slot decodes
 * exactly; the C mirror (native_render.c NR_PKEY_*) computes the same
 * values. */
#define OP_PKEY_BASE 1.4551915228366852e-11   /* 2^-36 */
#define OP_PKEY_STEP 9.094947017729282e-13    /* 2^-40 */
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
 * value, placed at z_ndc = z with w = 1; any other z is a view-Z, placed at
 * z_ndc = op_key_store(kv) with w = kv, a (z, w) pair the hardware clipper
 * interpolates without disturbing the window depth. Both lie inside the
 * clip volume's depth range, so no key face is depth-clipped; the fragment
 * stages write the stored value themselves (terrain_key.vert). A true
 * view-Z inside the band's range (0, 3.8e-9] lies at the eye. */
vec4 op_key_clip(vec2 cxy, float z, float kv)
{
    if (op_pkey_is(z)) return vec4(cxy, z, 1.0);
    return vec4(cxy * kv, op_key_store(kv) * kv, kv);
}

/* The forms of a key-feed face's stored value (terrain_key.vert vKeyForm). */
#define OP_KEY_FORM_FLAT 1.0   /* a flat key: op_key_store of the face's key */
#define OP_KEY_FORM_BAND 0.0   /* the player band: the band value itself */
#define OP_KEY_FORM_TRUE -1.0  /* the prop class: op_key_store of true view-Z */
#define OP_KEY_FORM_PIXEL -2.0 /* an actor face: op_key_store of its per-pixel key */

/* The fragment depth a walk key stage writes for form form: a flat key
 * stores op_key_store(kv) (kv the key, a drain sub-cell's or the tier-moved
 * one), the band stores kv (the band value), the prop class stores
 * op_key_store of the fragment's true view-Z, 1/iz with iz the owner's
 * screen-linear true 1/view-Z, and an actor face's per-pixel form stores
 * op_key_store(op_actor_key(1/iz, kv)), iz its screen-linear 1/(ordering
 * view-Z) and kv its key offset (rhi_vertex.akc). */
float op_key_stored_depth(float form, float kv, float iz)
{
    if (form > 0.5) return op_key_store(kv);
    if (form > -0.5) return kv;
    if (form > -1.5) return op_key_store(1.0 / max(iz, 1e-9));
    return op_key_store(max(1.0 / max(iz, 1e-9) + kv, 1.0));
}

/* The actor key image as every contest but the aura's sees it: a player
 * texel is not a key, so it never wins a pixel from an actor or a terrain
 * fragment. */
float op_akey_contest(float stored)
{
    return op_pkey_is(stored) ? OP_KEY_NONE : op_key_from_stored(stored);
}

/* An actor face's moby-against-moby key at a fragment of ordering view-Z
 * vz: vz plus the face's key offset akc (rhi_vertex.akc), the written depth
 * less 128 x m_DepthOffset there, floored at one world unit. The actor key
 * pass stores it per pixel into the per-pixel actor key image
 * (OP_KEY_FORM_PIXEL) and an actor fragment contests its own against it
 * (op_key_loses). */
float op_actor_key(float vz, float akc)
{
    return max(vz + akc, 1.0);
}

/* ---- the owner of an actor key ------------------------------------------
 *
 * The actor key pass writes, beside each texel's nearest key, the moby
 * identity of the face that wrote it (openpete_scene_tri.mid, the moby
 * record's PSX RAM word index + 1, 0 = none) into the image's colour
 * attachment, as three 8-bit channels (r = bits 0-7, g = 8-15, b = 16-23).
 * An actor fragment whose own moby owns the texel's key does not contest
 * it: order between two faces of one moby is left to true depth and the
 * same-moby rank (op_rank_loses). A fragment of any other moby, a fragment
 * that carries no identity, and a player-band texel keep the stored key.
 *
 * Residual: the image keeps only the nearest key, so where a moby's own
 * face owns the nearest actor key, another moby's key between the fragment
 * and that face no longer discards the fragment. This needs an own-face
 * pair whose keys invert their depths and a second moby inside that key
 * interval. */

/* The moby identity a texel of the actor key image's colour holds. */
float op_akey_id(vec4 c)
{
    return floor(c.r * 255.0 + 0.5) + floor(c.g * 255.0 + 0.5) * 256.0 +
           floor(c.b * 255.0 + 0.5) * 65536.0;
}

/* The stored actor key a fragment of moby own_id contests: the cleared
 * no-cover value 1.0 (decodes to OP_KEY_NONE) where own_id owns the stored
 * key k1, else k1. own_id <= 0.5 (no identity: terrain, unkeyed classes)
 * always reads k1. */
float op_akey_select(float k1, vec4 k1_owner, float own_id)
{
    if (own_id > 0.5 && !op_pkey_is(k1) && abs(op_akey_id(k1_owner) - own_id) < 0.5)
        return 1.0;
    return k1;
}

/* The moby identity of a scene fragment for op_akey_select. An unranked
 * actor face (rank -1, or -1.25 for an embedded prop) carries its moby
 * identity in the rank pair's y; every other face reads 0. */
float op_akey_owner(vec2 rank)
{
    return (rank.x < -0.5 && rank.x > -1.5) ? rank.y : 0.0;
}

/* The additive glow fan's order against the bodies it overlaps. The fan is
 * a flat billboard inserted at its own view-Z bucket and it writes no
 * depth, so it occludes nothing: the only question is which of the two the
 * table paints last. Both sides carry their ordering-table slot, so the
 * verdict is the table's own - the fan loses a pixel whose actor or player
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
    return op_key_slot_nearer(op_key_from_stored(akey_stored), own_slot + 1.0);
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
           op_key_slot_nearer(tkey, floor((own_koff - 0.5) / OP_KEY_SLOT) + 1.0);
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

/* Whether a rank channel value carries the embedded-prop sentinel
 * (OPENPETE_SCENE_RANK_PROP, -1.25): an unranked opaque face of a curated
 * embedded prop. The sentinel reads as an actor (< -0.5) and not as an
 * aura (< -1.5) at every other rank consumer. */
bool op_rank_is_prop(float rank)
{
    return abs(rank + 1.25) < 0.125;
}

/* ---- the terrain override bound ---------------------------------------
 *
 * A terrain fragment F that the terrain key K at its pixel sorts in front of
 * (the slot compare of op_key_loses) is discarded only when K's owner lies
 * near F's surface, or when K is the HP mesh over an LP face: the ordering
 * table's order between two surfaces that do not touch is not reproduced.
 * The owner's distance from F's plane along the pixel's ray is
 *     perp = |z_K - z_F| * |n_F . (x/z, y/z, 1)| = |z_K - z_F| * D_F / z_F,
 * z_K the owner's true view-Z at the pixel, z_F the fragment's, D_F the
 * distance from the eye to F's plane (n_F . P for any camera-space point P
 * of the plane, n_F unit). The verdict by the two faces' classes:
 *   K HP, F LP (an HP stand-in carrying the LP base slot term counts as LP):
 *     the override holds at any distance (the LP/HP partition, whose HP mesh
 *     lies at K wherever an LP face inverts against it);
 *   K's authored slots fewer than F's (a face biased in front of its
 *     neighbour): the override holds while perp < OP_KEY_BIAS_REACH (authored
 *     layering of touching surfaces, at most 68 world on the corpus);
 *   equal or more slots: the override holds while perp < OP_KEY_TIE_REACH
 *     (coplanar overlaps, under 0.5 world on the corpus).
 *
 * The terrain key pass writes the owner's attributes into the key image's
 * colour attachment beside the key (op_key_attr_store): r, g, b the owner's
 * true view-Z as 24 bits of z/(z+4096), a the owner's class byte
 * (op_key_class). The class byte 255, the colour every other key pass
 * writes and the clear value's alpha, means no attributes: the override
 * holds unbounded, the contest's rule before the bound existed. A face's
 * class byte is 128 for HP (0 for LP or a stand-in) plus its authored slot
 * count / 4 (every slot term is a multiple of 4; at most 78). */
#define OP_KEY_BIAS_REACH 128.0
#define OP_KEY_TIE_REACH  8.0
#define OP_KEY_CLASS_NONE 255.0

/* The class byte of a terrain face: hp = it keys as HP (a stand-in carrying
 * the LP base term does not), slots = its authored slot term
 * (tface_ot_slots). */
float op_key_class(bool hp, float slots)
{
    return (hp ? 128.0 : 0.0) + min(floor(slots * 0.25 + 0.5), 127.0);
}

/* The key image colour a terrain key-pass fragment writes: the owner's true
 * view-Z z and class byte cls. Exact in an 8-bit UNORM target. */
vec4 op_key_attr_store(float z, float cls)
{
    float e = floor(clamp(z / (z + 4096.0), 0.0, 1.0) * 16777215.0 + 0.5);
    float hi = floor(e / 65536.0);
    float mid = floor((e - hi * 65536.0) / 256.0);
    float lo = e - hi * 65536.0 - mid * 256.0;
    return vec4(lo, mid, hi, cls) / 255.0;
}

/* The owner's true view-Z of a key image colour texel. */
float op_key_attr_z(vec4 c)
{
    vec4 b = floor(c * 255.0 + 0.5);
    float e = (b.r + b.g * 256.0 + b.b * 65536.0) / 16777215.0;
    return 4096.0 * e / max(1.0 - e, 1e-7);
}

/* The owner's class byte of a key image colour texel (OP_KEY_CLASS_NONE: no
 * attributes). */
float op_key_attr_class(vec4 c)
{
    return floor(c.a * 255.0 + 0.5);
}

/* A terrain fragment's own override inputs: x its class byte
 * (OP_KEY_CLASS_NONE = none), y D_F, the distance from the eye to its plane.
 * The walk carries both in the rank channel of a terrain vertex
 * (openpete_scene_tri.kattr), a value in [0.25, 0.5) that every rank
 * reader takes as unranked: 0.25 + (class * 32768 + q) * 2^-25, D_F =
 * (q / 64)^2. Any other rank value carries none. */
vec2 op_key_fattr_walk(float rank)
{
    if (rank < 0.2499 || rank >= 0.5) return vec2(OP_KEY_CLASS_NONE, 0.0);
    float code = floor((rank - 0.25) * 33554432.0 + 0.5);
    float cls = floor(code / 32768.0);
    float q = code - cls * 32768.0;
    return vec2(cls, (q / 64.0) * (q / 64.0));
}

/* The class byte a walk key-pass vertex's rank channel carries (the same
 * encoding as op_key_fattr_walk), or OP_KEY_CLASS_NONE. */
float op_key_class_walk(float rank)
{
    return op_key_fattr_walk(rank).x;
}

/* Whether the terrain override holds: K's attribute texel tattr against the
 * fragment's view-Z own and its inputs fattr (op_key_fattr_walk). */
bool op_key_override_holds(vec4 tattr, float own, vec2 fattr)
{
    float ck = op_key_attr_class(tattr);
    float cf = fattr.x;
    if (ck >= OP_KEY_CLASS_NONE - 0.5 || cf >= OP_KEY_CLASS_NONE - 0.5) return true;
    bool hpK = ck >= 127.5, hpF = cf >= 127.5;
    if (hpK && !hpF) return true;
    float sK = ck - (hpK ? 128.0 : 0.0);
    float sF = cf - (hpF ? 128.0 : 0.0);
    float zF = max(own, 1.0);
    float perp = abs(op_key_attr_z(tattr) - zF) * fattr.y / zF;
    return perp < (sK < sF - 0.5 ? OP_KEY_BIAS_REACH : OP_KEY_TIE_REACH);
}

/* ---- in-bucket tier order -----------------------------------------------
 *
 * Within one ordering-table bucket the PS1 paints prims in insertion order.
 * Every terrain bucket insert appends at the bucket's tail: it reads the
 * tail word, stores the new prim there, and links the old tail's tag to the
 * new prim, or stores it as the head of an empty bucket (Spyro 2
 * environment.s 80028FA8-80028FD8; Spyro 3 80024BD0-80024BF8 on the HP
 * passes and 80027768-8002779C on the LP pass). Every HP prim of a frame is
 * inserted before the first LP prim: the world draw runs its HP passes in
 * sequence, each entered when the one before exhausts its list, and enters
 * the LP pass only after the last of them, returning from its end (Spyro 2
 * HP 80024584, 80025434, 80025CC0, 80026C74, 80027AC8, 80028504, LP
 * 80028B14, end 80029118; Spyro 3 HP 80022D48, 80023BF8, 800244AC,
 * 80025438, 8002628C, 80026CC8, LP 800272D8, end 800278DC). In a bucket
 * holding both tiers the LP face paints over the HP face. Spyro 1 does not
 * submit the scene through one table and takes none of this (slot_contest
 * off).
 *
 * The terrain key image keeps the least key at a pixel, so an HP fragment
 * would never see a later-painting LP face of its bucket whose key is
 * larger. On one ordering table every terrain key pass therefore stores a
 * key moved inside its own bucket (op_key_tier_key): an LP key into the
 * bucket's first half, an HP key into its second half, each scaled by one
 * half so the order within a tier is kept. No key leaves its bucket; the
 * contest reads the stored key back through op_key_tier_unkey (the owner's
 * class byte names the half) where it compares a key value rather than a
 * bucket, and sees the key within twice its float step; only the owner of a pixel whose
 * least bucket holds both tiers changes. An HP fragment whose key
 * owner is an LP face of the same bucket lying within OP_KEY_TIER_REACH world
 * of it along the ray (a coplanar overlap) loses the pixel
 * (op_key_tier_gap); farther apart, true depth decides as before.
 *
 * Specialisation constant 7 (`OP_KEY_TIER_ORDER`, OPENPETE_KEY_TIER_ORDER
 * for the process): 1 the order above (the default), 0 neither the stored
 * key move nor the rule (an in-binary A/B leg), 2 the order with a fragment
 * the rule would discard kept and marked (op_key_tier_hit,
 * op_key_tier_mark: a census of the rule's pixels). */
layout(constant_id = 7) const int OP_KEY_TIER_ORDER = 1;
#define OP_KEY_TIER_REACH 8.0

/* The depth gap of the last fragment op_key_loses kept under
 * OP_KEY_TIER_ORDER 2 where the tier rule would have discarded it; -1 none. */
float op_key_tier_hit = -1.0;

/* The key view-Z a terrain key pass stores for a face of key view-Z kv and
 * class byte cls (op_key_class; OP_KEY_CLASS_NONE: not a terrain key, stored
 * unchanged). On one ordering table (slot_contest) the key's offset into its
 * bucket is halved, and an HP key's is then raised by half a bucket, so
 * every LP key of a bucket stores below every HP key of it and the order
 * within a tier is kept. Halving doubles the key's float step in world units
 * (0.004 to 0.008 at a key of 33792). */
float op_key_tier_key(float kv, float cls, bool slot_contest)
{
    if (!slot_contest || OP_KEY_TIER_ORDER == 0 || cls >= OP_KEY_CLASS_NONE - 0.5) return kv;
    float base = floor(kv / OP_KEY_SLOT) * OP_KEY_SLOT;
    return base + (cls >= 127.5 ? 0.5 * OP_KEY_SLOT : 0.0) + 0.5 * (kv - base);
}

/* The key view-Z of a stored key tkey (op_key_from_stored of a key image
 * texel) whose owner's attribute texel is tattr: op_key_tier_key undone by
 * the owner's class byte, on one ordering table (slot_contest). A key with no
 * class (OP_KEY_CLASS_NONE) was stored unchanged and is returned unchanged,
 * as is OP_KEY_NONE. */
float op_key_tier_unkey(float tkey, vec4 tattr, bool slot_contest)
{
    float ck = op_key_attr_class(tattr);
    if (!slot_contest || OP_KEY_TIER_ORDER == 0 || ck >= OP_KEY_CLASS_NONE - 0.5 ||
        tkey >= OP_KEY_NONE * 0.5) return tkey;
    float base = floor(tkey / OP_KEY_SLOT) * OP_KEY_SLOT;
    float f = tkey - base - (ck >= 127.5 ? 0.5 * OP_KEY_SLOT : 0.0);
    return base + 2.0 * max(f, 0.0);
}

/* The in-bucket tier rule for a terrain fragment on one ordering table: its
 * key channel koff (absolute key + 0.5), its view-Z own and override inputs
 * fattr (op_key_fattr_walk), against the key tkey and attribute texel tattr
 * at its pixel. Returns |z_K - own| when the fragment is HP, the key owner
 * is LP, both keys lie in one bucket and the gap is under OP_KEY_TIER_REACH
 * (the fragment loses: the LP face is inserted later); else -1. */
float op_key_tier_gap(float tkey, vec4 tattr, float own, float koff, vec2 fattr)
{
    if (OP_KEY_TIER_ORDER == 0) return -1.0;
    float ck = op_key_attr_class(tattr);
    if (ck >= 127.5 || fattr.x < 127.5 || fattr.x >= OP_KEY_CLASS_NONE - 0.5) return -1.0;
    float slot = floor((koff - 0.5) / OP_KEY_SLOT);
    if (op_key_slot_nearer(tkey, slot) || !op_key_slot_nearer(tkey, slot + 1.0)) return -1.0;
    float gap = abs(op_key_attr_z(tattr) - own);
    return gap < OP_KEY_TIER_REACH ? gap : -1.0;
}

/* ---- blended-terrain contest census ----------------------------------
 *
 * Blended terrain neither emits a key (the key-pass pack drops blend != 0)
 * nor contests one (its koff is 0, terrain.vert and terrain.c), so the
 * terrain-against-terrain arm of op_key_loses is the rule that would remove a
 * blended terrain fragment for a key if it were keyed; a mode-1 mark is a
 * blended face that still reaches the arm.
 * Specialisation constant 8 (`OP_KEY_BLEND_CENSUS`, OPENPETE_KEY_BLEND_CENSUS
 * for the process): 0 off (the default); 1 a fragment the caller flags as
 * blended terrain (op_key_blend_frag) that the arm would discard is kept,
 * skipping every later arm as a blended face keyed 0 would, and its true-depth
 * verdict against the key owner is recorded in op_key_blend_hit for the
 * caller to paint (op_key_blend_mark); 2 every blended terrain fragment that
 * survives its contests is painted yellow, the coverage count a mode-1 zero
 * is read against. */
layout(constant_id = 8) const int OP_KEY_BLEND_CENSUS = 0;

/* Set by the caller before op_key_loses: the fragment is blended terrain. */
bool op_key_blend_frag = false;

/* The verdict of the last fragment the census kept: 0 none, 1 in front of
 * the key owner by true view-Z, 2 at or behind it, 3 the owner carries no
 * attributes (OP_KEY_CLASS_NONE), so its view-Z is unknown. */
int op_key_blend_hit = 0;

/* The census verdict of a fragment of view-Z own against the key owner's
 * attribute texel tattr. */
int op_key_blend_verdict(vec4 tattr, float own)
{
    if (op_key_attr_class(tattr) >= OP_KEY_CLASS_NONE - 0.5) return 3;
    return own < op_key_attr_z(tattr) ? 1 : 2;
}

/* The census colour of a verdict: red in front, green behind, blue owner
 * unknown. Opaque, so an alpha-blended draw stores it exactly; an additive
 * or subtractive draw moves only the verdict's channel. */
vec4 op_key_blend_mark(int v)
{
    return v == 1 ? vec4(1.0, 0.0, 0.0, 1.0)
         : v == 2 ? vec4(0.0, 1.0, 0.0, 1.0)
                  : vec4(0.0, 0.0, 1.0, 1.0);
}

/* The caller's output colour c under the census: the mode-1 verdict mark of
 * a kept fragment, yellow for every blended terrain fragment under mode 2,
 * else c. */
vec4 op_key_blend_paint(vec4 c)
{
    if (op_key_blend_hit != 0) return op_key_blend_mark(op_key_blend_hit);
    if (OP_KEY_BLEND_CENSUS == 2 && op_key_blend_frag) return vec4(1.0, 1.0, 0.0, 1.0);
    return c;
}

/* The census colour of a fragment op_key_tier_hit marks: yellow, blue =
 * the gap in 1/32 world. */
vec4 op_key_tier_mark(float gap)
{
    return vec4(1.0, 1.0, min(floor(gap * 32.0), 255.0) / 255.0, 1.0);
}

/* The key contest. koff is the face's key channel: positive = its absolute
 * OT key + 0.5 (OT-band terrain and moby faces); negative = near-band
 * terrain, contest-only, where the fragment's own view-Z is the limit.
 * actor = an unranked moby face; no actor face loses to a terrain key.
 * prop = an actor face of a curated embedded prop: terrain within the
 * embed bound in front of it yields to it, terrain beyond the bound in
 * front occludes it by true depth, terrain behind it loses by true depth.
 * The verdict reads prop through actor alone. truescope = the default
 * scoping: only an actor fragment contests an actor key, the per-pixel
 * one (pkey) at its own per-pixel key (akc, below); false restores the full
 * compound painter among the flat moby keys (akey).
 *
 * Moby against moby, under truescope: an actor fragment loses the pixel
 * when another moby's key in the per-pixel actor key image (pkey, each
 * opaque actor face rastered at its per-pixel key) is smaller than its own,
 * both evaluated per pixel (op_actor_key: ordering view-Z plus the face's
 * offset akc, -128 x m_DepthOffset). akey, the flat actor key image the
 * glow-fan aura and the key-order class read, is contested only with
 * truescope false. Two mobys with equal
 * offsets are ordered by their depth at the pixel; an authored offset pulls
 * a moby in front of a nearer one by 128 world per unit, as the ordering
 * table does, but decided per pixel instead of per face. Order within one
 * moby stays with true depth and the same-moby rank (op_rank_loses).
 *
 * Under truescope, terrain never yields to general moby keys: the authored
 * pulls (-128*dofs) float pickups and decor over whatever the OT sorts them
 * against, and a per-pixel window derived from them re-admits punch-through
 * (a chest through a wall lip, a gem through a near wall). Terrain over
 * moby is decided by true depth plus the -127 stored tie pull where the
 * terrain key is not in a nearer slot (op_actor_tie_depth), the prop
 * embed rule, the actor-front pass, and the moby fragment's own snap in
 * front of terrain it is sunk into (psx_moby_snap.glsl).
 *
 * Prop embed: a curated embedded prop (pedestal discs, statue bases; a
 * class list in the moby feeder) within 1024 world behind this surface
 * wins the pixel. This carries dofs-0 pedestals at mid range, where the
 * original floated them on the ground's own push.
 *
 * Terrain against terrain is bounded (op_key_override_holds): tattr is the
 * terrain key image's colour texel at the pixel (the key owner's view-Z
 * and class), fattr the fragment's own class byte and plane distance; a
 * caller with neither passes vec4(1.0) or vec2(OP_KEY_CLASS_NONE, 0.0) and
 * the override holds unbounded. On one ordering table an HP fragment also
 * loses to a coplanar LP face of its bucket (op_key_tier_gap). */
bool op_key_loses(float tkey, vec4 tattr, float akey, float pkey, float mkey, float own,
                  float koff, float akc, vec2 fattr, bool actor, bool prop,
                  bool truescope, bool slot_contest)
{
    if (koff > 0.0) {
        float band = (koff - 0.5) - 64.0;      /* half an OT slot nearer */
        if (!actor && slot_contest) {
            float gap = op_key_tier_gap(tkey, tattr, own, koff, fattr);
            if (gap >= 0.0) {
                if (OP_KEY_TIER_ORDER != 2) return true;
                op_key_tier_hit = gap;
            }
        }
        /* Terrain against terrain only. An actor face (an embedded prop
         * included: its rank reads as an actor's) is not compared against
         * the terrain key: the table paints a terrain face over a nearer
         * moby face whose key is the same or farther, so a moby resting on
         * a step loses its lower half to the step's lip. Terrain against
         * moby is decided by true depth with the -127 stored tie pull
         * where the terrain key is not in a nearer slot
         * (op_actor_tie_depth), a deliberate departure from the table's
         * order. On one ordering
         * table (slot_contest, OP_KEY_SLOT world per bucket) terrain
         * compares buckets, the table's own granularity: a difference
         * smaller than a bucket is no difference at all, and a terrain
         * fragment keeps a tie (it must never lose to its own key). */
        if (!actor && (slot_contest
                ? op_key_slot_nearer(tkey, floor((koff - 0.5) / OP_KEY_SLOT))
                : op_key_band_nearer(tkey, band)) &&
            op_key_override_holds(tattr, own, fattr)) {
            if (OP_KEY_BLEND_CENSUS == 0 || !op_key_blend_frag) return true;
            op_key_blend_hit = op_key_blend_verdict(tattr, own);
            return false;
        }
        if (truescope ? actor && pkey < op_actor_key(own, akc) : akey < band) return true;
        return truescope && !actor && mkey < own + 1024.0;
    }
    /* Near band: per-pixel own key, same strict rule, same embed rule. The
     * stored key is read back to its key view-Z (op_key_tier_unkey); the
     * bucket compares above take it as stored, since the tier move keeps
     * every key in its bucket. */
    return op_key_tier_unkey(tkey, tattr, slot_contest) < own - 64.0 ||
           (!truescope && akey < own - 64.0) ||
           mkey < own + 1024.0;
}

/* ---- the tie pull against terrain -------------------------------------
 *
 * Every moby vertex is stored OP_ACTOR_TIE_PULL world nearer than its true
 * view-Z (OPENPETE_MOBY_TIE_PULL_VZ in openpete_native_render.h), so a moby
 * face wins terrain the game puts in its ordering-table slot: the table
 * resolves a same-slot moby-against-terrain pair to the moby. Where the
 * terrain key at the pixel sits in a strictly nearer slot the table paints
 * the terrain over the moby, so the pull would only pass a moby that lies
 * behind a nearer surface. There an opaque moby fragment writes its true
 * view-Z instead (the pull added back) and true depth decides the pixel
 * against terrain; where the terrain key is in the same slot or a farther
 * one it keeps the pulled depth. Nearer is the terrain-against-terrain test
 * of op_key_loses: a smaller bucket on one ordering table (slot_contest),
 * else a key smaller by more than half a slot.
 *
 * Residual: the image holds the nearest terrain key at the pixel, which is
 * not always the key of the face that lies nearest in true depth. */
#define OP_ACTOR_TIE_PULL 127.0

/* The window depth an opaque moby fragment writes. zwin = the depth it
 * would write otherwise (gl_FragCoord.z, or the front-shell depth); rank =
 * its rank pair (a ranked face, an actor -1 or an embedded prop -1.25; any
 * other face, terrain and the glow fan included, keeps zwin); koff = its key
 * channel (absolute key + 0.5, <= 0.25 unkeyed: keeps zwin); tkey = the
 * decoded terrain key at the pixel. */
float op_actor_tie_depth(float zwin, vec2 rank, float koff, float tkey,
                         bool slot_contest, bool reversed)
{
    bool moby = rank.x > 0.5 || (rank.x < -0.5 && rank.x > -1.5);
    if (!moby || koff <= 0.25) return zwin;
    bool nearer = slot_contest ? op_key_slot_nearer(tkey, floor((koff - 0.5) / OP_KEY_SLOT))
                               : tkey < (koff - 0.5) - 64.0;
    if (!nearer) return zwin;
    float vz = op_vz_from_depth(zwin, reversed) + OP_ACTOR_TIE_PULL;
    return reversed ? 1.0 / vz : 1.0 - 1.0 / vz;
}

/* ---- key order against terrain --------------------------------------
 *
 * A moby class the game draws in front of terrain that lies in front of it
 * in true depth (Spyro 3's sub-area door effect against the door's plate)
 * marks its actor key texels in the owner colour's alpha
 * (openpete_scene_tri.key_order): 0x80 yields, 0x40 yields painted (a
 * diagnostic), 0xFF unmarked and the clear value. A terrain fragment at a
 * marked texel yields the pixel when the marked key sorts in front of its
 * own: on one ordering table (slot_contest) a nearer bucket, the table's
 * granularity, else by more than half a slot. This is the table's painter
 * order, applied to that class against terrain alone; true depth decides
 * every other pair. A terrain face keyed in front of the effect (a closed
 * door, a wall between it and the camera) keeps its pixels. */

/* The mark of an actor key owner texel: 0 none, 1 yield, 2 yield painted. */
int op_key_order_mark(vec4 owner)
{
    float a = floor(owner.a * 255.0 + 0.5);
    return abs(a - 128.0) < 0.5 ? 1 : abs(a - 64.0) < 0.5 ? 2 : 0;
}

/* The verdict for a terrain fragment: 0 keep, 1 yield (discard), 2 yield
 * painted. akey_stored = the raw actor key texel, owner = its owner colour,
 * tkoff = the fragment's key channel (absolute key + 0.5; <= 0.25 = not
 * keyed or near band, never yields). */
int op_key_order_yields(float akey_stored, vec4 owner, float tkoff, bool slot_contest)
{
    if (tkoff <= 0.25) return 0;
    int m = op_key_order_mark(owner);
    if (m == 0 || op_pkey_is(akey_stored)) return 0;
    float akey = op_key_from_stored(akey_stored);
    float tk = tkoff - 0.5;
    bool front = slot_contest ? op_key_slot_nearer(akey, floor(tk / OP_KEY_SLOT))
                              : akey < tk - 64.0;
    return front ? m : 0;
}

/* ---- terrain order against a moby class --------------------------------
 *
 * The reverse of the key order above: a moby class the game draws under
 * terrain that lies behind it in true depth (Spyro 2's swallowed fish
 * against the idol's closed jaw) carries the rank sentinel
 * OPENPETE_SCENE_RANK_TORDER (-0.875) on its opaque faces. Each moby's
 * prims are spliced into their buckets ahead of the terrain linked there,
 * so the table paints a terrain face keyed into the moby face's own slot
 * or a nearer one after it. Such a fragment loses its pixel to that
 * terrain key: on one ordering table (slot_contest) by bucket, else when
 * the terrain key is at or below its own. tkey = the decoded terrain key
 * at the pixel; koff = the fragment's key channel (absolute key + 0.5,
 * <= 0.25 unkeyed: never loses). */
bool op_rank_is_torder(float rank)
{
    return abs(rank + 0.875) < 0.0625;
}

bool op_terrain_order_loses(float tkey, float koff, float rank, bool slot_contest)
{
    if (!op_rank_is_torder(rank) || koff <= 0.25) return false;
    float k = koff - 0.5;
    return slot_contest ? op_key_slot_nearer(tkey, floor(k / OP_KEY_SLOT) + 1.0)
                        : tkey <= k;
}

#endif /* OPENPETE_PSX_KEY_CONTEST_GLSL */
