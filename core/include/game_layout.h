// game_layout.h — the engine-layout descriptor: where a game keeps the RAM
// the presentation feeders read.
//
// Each game defines one `game_layout` (games/<game>/renderer/layout.c) from
// its symbol aliases and, where the game has a generated typed header, from
// `offsetof` against that header, whose `_Static_assert`s pin every member to
// the probed MIPS layout. A feeder reads a field of this struct, never a
// per-game macro, so one feeder body serves every game whose layout fills the
// fields it reads.
//
// Every address is a PSX vaddr. A field the game has no counterpart for is 0,
// and a feeder that needs it refuses to run rather than read address 0.
#ifndef OPENPETE_GAME_LAYOUT_H
#define OPENPETE_GAME_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A descriptor slot the game fills with a function reading guest RAM. */
struct CPUState;

enum { TERRAIN_FACES_SPYRO1 = 0, TERRAIN_FACES_SPYRO3 = 1 };
enum { CYC_FACES_SPYRO1 = 0, CYC_FACES_SPYRO3 = 1 };
/* The bounding-sphere rejection the world draw runs before it bins a sector
 * into the LP/HP lists. TERRAIN_SECTOR_CULL_NONE: the draw bins every sector
 * the pre-pass offered. TERRAIN_SECTOR_CULL_SPHERE: the sphere-frustum test at
 * environment.s:800224E0-80022548. */
enum { TERRAIN_SECTOR_CULL_NONE = 0, TERRAIN_SECTOR_CULL_SPHERE = 1 };
/* Where the moby-shadow draw finds the base of the drawn-moby list, and which
 * screen-extent rejection it applies before emitting a fan. */
enum { SHADOW_LIST_ADDR = 0, SHADOW_LIST_GTE_DATA = 1 };
enum { SHADOW_CULL_SPYRO1 = 0, SHADOW_CULL_SPYRO3 = 1 };

/* How a game stores its environment animations.
 * ENV_ANIM_LISTS: three independent {count, descriptor-pointer array} pairs
 * (Spyro 1 `EnvironmentAnimations`), each descriptor a separate record whose
 * u16 at +4 names the sector it animates.
 * ENV_ANIM_ARENA: one block of inline words (Spyro 3, reached through the
 * word at `block_ptr`), holding up to ENV_ANIM_SUBLISTS sub-lists. Each
 * sub-list is a count word followed by one-word entries; an entry's low byte
 * is the sector index and bits 16-31 a word offset into the block at which
 * that sector's keyframe data sits. There are no per-animation descriptor
 * records, so nothing in the block has a stable address to key a snapshot on.
 */
enum { ENV_ANIM_NONE = 0, ENV_ANIM_LISTS = 1, ENV_ANIM_ARENA = 2 };

/* What a sub-list rewrites in the sector it names. The terrain decode cache
 * re-reads colours live, so only KIND_VERT makes a cached sector's geometry
 * stale and only KIND_VERT sectors need the prev/cur vertex banks. */
enum { ENV_ANIM_KIND_VERT = 0, ENV_ANIM_KIND_COLOUR = 1 };

#define ENV_ANIM_SUBLISTS 3

/* One ENV_ANIM_ARENA sub-list. `entry_off` = 0 marks an unused row. */
typedef struct env_anim_sublist {
    uint16_t hdr_off;    /* byte offset into the block that locates the list */
    uint8_t  indirect;   /* 1: the block word at hdr_off holds the list's own
                          * byte offset from the block base; 0: the count word
                          * is at hdr_off itself */
    uint8_t  entry_off;  /* bytes from the count word to the first entry */
    uint8_t  skip_top;   /* 1: the walker skips an entry with bit 31 set */
    uint8_t  kind;       /* ENV_ANIM_KIND_* */
} env_anim_sublist_t;

typedef struct game_layout {
    /* The in-RAM sin/cos table: 256 entries per circle of s16 1.12 fixed
     * point, cos at +0x80 bytes into the same table, so a byte angle indexes
     * sin at (angle << 1) and cos at 0x80 + (angle << 1). 0 = unlocated, and
     * every helper that reads the table returns sin 0 / cos 4096 instead. */
    uint32_t sin_table;

    /* The environment header (Spyro 1 `Environment`). */
    struct {
        uint32_t base;
        uint32_t sector_ptr;        /* sector list pointer */
        uint32_t sector_count;
        uint32_t occ_groups;        /* occlusion group list pointer */
        uint32_t occ_group_count;
        uint32_t lq_tex;            /* LQ texture table pointer */
        uint32_t hq_tex;            /* HQ texture table pointer */
        uint32_t tex_count;
        uint32_t lod_dist;          /* LP/HP LOD boundary */
        uint32_t cull_dist;         /* LP far draw distance */

        /* 1: the game's per-sector visibility verdict carries no aspect,
         * frustum or view-direction term, so it is already the candidate set
         * at any aspect and the widescreen widening is skipped. 0: the verdict
         * is computed inside the draw's own frustum cull, and widescreen falls
         * back to the occlusion-group sublist. */
        uint32_t vis_aspect_independent;
    } env;

    /* The environment animations. `form` picks which registry the shared
     * geometry/colour iterator walks; ENV_ANIM_NONE means none is located and
     * the iterator yields nothing. `scroll` is read on its own by the UV-scroll
     * mirror, whose per-descriptor accumulator record exists only in the LISTS
     * shape, so a game can carry a scroll list under either form. */
    struct {
        uint32_t form;              /* ENV_ANIM_* */
        uint32_t base;

        /* Each is {count, pointer to a descriptor-pointer array}; the field
         * is the address of the count and the array pointer follows at +4.
         * `lp_poly` and `hp_poly` are the ENV_ANIM_LISTS geometry registry;
         * `scroll` carries the same shape under either form. */
        uint32_t scroll;            /* scrolling-texture descriptors */
        uint32_t lp_poly;           /* LP geometry animations */
        uint32_t hp_poly;           /* HP geometry animations */

        /* ENV_ANIM_ARENA. `block_ptr` is the address of the word holding the
         * block base. `live_table` is a byte table indexed by an entry's low
         * byte (the sector index); bit 0 set = that animation runs, and the
         * game's walkers skip the entry when it is clear. */
        uint32_t block_ptr;
        uint32_t live_table;
        env_anim_sublist_t sub[ENV_ANIM_SUBLISTS];
    } env_anim;

    /* The world visibility verdict: one byte per sector, 0xFF = drawn. */
    struct {
        uint32_t base;
    } visibility;

    /* The cyclorama header (Spyro 1 `Cyclorama`) and the sky decode switches.
     * Both games share the sector header through +0x0F (cull point, radius,
     * centre), the vertex word packing and the colour table after the
     * vertices; they differ in where the vertex array starts, how the vertex
     * and colour counts are encoded, and the face encoding (CYC_FACES_*:
     * Spyro 1 lists two words per triangle, Spyro 3 walks triangle strips
     * from two parallel lists). `visibility` is a per-sector byte array the
     * game's own visibility pass fills before the sky draw (nonzero = drawn),
     * 0 when the draw selects sectors through the occlusion group list its
     * first argument names. `hook_args` = 1 when the sky draw takes (group,
     * cull matrix, draw matrix) as its arguments; 0 when it reads the
     * camera's projection matrix for both. `fade_in_draw` = 1 when the sky
     * draw itself depth-cues the colours toward `fog.far_colour` by
     * `fog.factor` whenever the factor is nonzero. */
    struct {
        uint32_t base;
        uint32_t sector_count;
        uint32_t sectors;           /* sector pointer array */
        uint32_t occ_group_count;
        uint32_t occ_groups;
        uint32_t bg_colour;         /* 0x00BBGGRR */
        uint32_t vertex_base;       /* byte offset of the vertex array in a sector */
        int      face_layout;       /* CYC_FACES_* */
        uint32_t visibility;
        int      hook_args;
        int      fade_in_draw;
    } cyc;

    /* The GTE depth cue the world and sky draws run: IR0 (0x1000 = 1.0)
     * and the far colour FC, stored as one packed 0x00BBGGRR word
     * (`far_colour_words` 0) or as three words R, G, B at 16 times the
     * 8-bit scale, the form the GTE's RFC/GFC/BFC registers take
     * (`far_colour_words` 1). */
    struct {
        uint32_t factor;
        uint32_t far_colour;
        int      far_colour_words;
    } fog;

    /* The draw-buffer pair (Spyro 1 `DB`): the pointer to the buffer under
     * construction, the pair's base and stride, and the DRAWENV offset
     * inside a buffer. */
    struct {
        uint32_t cur_ptr;
        uint32_t base;
        uint32_t stride;
        uint32_t drawenv;
    } db;

    /* The per-frame primitive allocator: the cursor word and the end word. */
    struct {
        uint32_t cursor;
        uint32_t end;
    } polybuf;

    /* The camera (Spyro 1 `Camera`). `[base, xform_end)` is the draw-time
     * transform the interp ring snapshots: the projection (sky) matrix, the
     * view (world) matrix and the position. */
    struct {
        uint32_t base;
        uint32_t proj_matrix;       /* SHORTMATRIX, the sky draw's rotation */
        uint32_t view_matrix;       /* SHORTMATRIX, the world draw's rotation */
        uint32_t position;          /* int32 x, y, z */
        uint32_t rotation;          /* int16 x, y, z */
        uint32_t xform_end;
    } cam;

    uint32_t level_id;

    /* The terrain decode switches. The 7-word sector header and the 11/11/10
     * vertex packing are common; the games differ in the header's radius and
     * flag bits, in the vertex Z offset scale, and in the tables the walk may
     * consult. `radius_mask` and `flags_mask` split header word 1's low half;
     * `flat_flag` is the bit (0 when the game has no flat rule) that marks a
     * sector whose Z offsets are stored at `vz_flat_shr` bits less; every
     * other sector's Z offset is `vz_shl` bits more than the 10 packed bits.
     * `uv_ori_table` is the HQ tile UV-orientation offset table (0 =
     * identity); `defer_q` are the two near-cell defer queues the draw fills
     * (0 = none). */
    struct {
        uint32_t radius_mask;
        uint32_t flags_mask;
        uint32_t flat_flag;
        int      vz_shl;
        int      vz_flat_shr;
        uint32_t uv_ori_table;
        uint32_t defer_q[2];
        /* The face record layout (TERRAIN_FACES_*): the games pack the HP
         * texture index, flip, OT bias and triangle UV rotation, and the LP
         * indices and colours, in different words and bit positions. */
        int      face_layout;
        /* Bytes per texture-table entry: the LQ far tile at +0 and the LQ near
         * tile at +8 in both games; Spyro 3 keeps its four HQ sub-tiles inline
         * after them (48 bytes) where Spyro 1 has a separate HQ table (16). */
        uint32_t tex_stride;
        /* The near/HQ 2x2 subdivide's tile source. The record holding a face's
         * four HQ sub-tiles is `hq_tex` + (HQ index) * `hq_stride`, and sub-quad
         * q (quadrant order TL, TR, BL, BR) reads the 8-byte tile half at
         * `hq_sub0` + q*8 inside it. Spyro 1: a separate 168-byte record whose
         * entry 0 is the detail-path base, so the quadrants are entries 1..4
         * (offset 8). Spyro 3: the sub-tiles sit inline in the same 48-byte
         * texture-table entry as the LQ halves, at +0x10
         * (environment.s .L80024C08 bumps the entry pointer by 8 in the loop's
         * delay slot, then reads +0x8/+0xC of it; the quadrant order is the
         * vertex-base table D_80065C28 over the 3x3 grid). */
        uint32_t hq_stride;
        uint32_t hq_sub0;
        /* Semi-transparency: Spyro 1 flags it per face; Spyro 3 in bit 15 of
         * the tile's TPAGE halfword. */
        int      abe_in_tpage;
        /* The tile's second word holds (u1, v1): the top-right corner in
         * Spyro 1, the bottom-right corner in Spyro 3 (its emitter subtracts
         * 0x1F00 to make the top-right). */
        int      tile_word1_br;
        /* The LP/HP LOD distance when the environment header has no field for
         * it (env.lod_dist == 0): Spyro 3's draw bins with constants. */
        uint32_t lod_dist_default;
        /* The HP walk's CLUT-fog ladder row and near-tile select. 0 = the
         * row is (key - fogRef + 4096) >> 8 with key the four-corner SZ sum
         * >> 7 << 5, and the near tile half is taken at key - fogRef + 4096
         * <= 0. 1 = per corner ir0 = fogRef - SZ clamped to [0, 0xFFF], the
         * row is 15 - (max ir0 >> 8), and the near half is taken at row 0:
         * the nearest corner alone selects, and the row is the ceiling of
         * (SZ - (fogRef - 0xF00)) / 256 instead of the floor. */
        int      ladder_nearest_corner;
        /* The world draw's own per-sector list binning. `no_lp_flag`,
         * `no_hp_flag` and `force_lp_flag` are header flag bits: no LP list, no
         * HP list, and LP drawn even where the sector has HP. `sector_cull`
         * names the bounding-sphere rejection run before the binning.
         *   `hp_far` 0 means the draw bins by flags alone and the tier is
         * distance-independent. Otherwise, with vz the bounding centre's view-Z
         * through the binning matrix and r the header radius, both in world/16
         * units: the LP list takes the sector while
         * vz + r > lp_near - (flags & lp_near_shift_flag ? lp_near_shift : 0),
         * and the HP list while vz - r < hp_far, tightened to hp_far_tight when
         * any `tight_flags` bit is set. */
        uint32_t no_lp_flag, no_hp_flag, force_lp_flag;
        uint32_t lp_near_shift_flag;
        int      lp_near_shift;
        int      hp_far, lp_near, hp_far_tight;
        uint32_t tight_flags;
        int      sector_cull;
        /* The HP defer classifier: which faces the draw routes to its near/HQ
         * queues instead of the main OT, so they carry no OT key and no
         * authored OT bias. Both games sum the four corner SZ3s and compare the
         * sum against `near_band` and each corner against `near_corner`.
         * `near_flag_excl` is a face word-3 bit that excludes the face from the
         * queues outright and `near_flag_tier2` one that refuses the
         * no-near-corner half of the band; a game whose classifier reads no
         * face flags leaves both 0 and defers on depth alone. */
        int      near_band;
        int      near_corner;
        uint32_t near_flag_excl;
        uint32_t near_flag_tier2;
        /* The authored ordering-table terms the ordinary insert adds to a
         * face's bucket index, in 128-world slots: `lp_base_slots` the
         * constant base the LP insert adds before the authored bias, and
         * `lp_bias_slots`/`hp_bias_slots` the multipliers on the face's
         * bias word. Ported onto the face's key as world units. */
        int      lp_base_slots;
        int      lp_bias_slots, hp_bias_slots;
        /* 1 = the near/HQ-classified faces key through that same ordinary
         * insert, so each publishes a positive ordering key like every
         * other terrain face. 0 = the near/HQ queue carries no ordering
         * key, and those faces contest per-pixel against their own view-Z
         * without publishing one. */
        int      nearband_ot_keys;
        /* 1 = the moby-versus-terrain key contest compares 128-world slot
         * indices rather than world distances, and resolves an equal slot
         * to the terrain: the draw submits both classes through one
         * ordering table with the moby pass first, so a terrain face in
         * the same bucket is appended after the moby face and paints over
         * it. An actor face in a farther-or-equal slot than the terrain
         * key owning the pixel loses the pixel. */
        int      slot_ot_contest;
    } terrain;

    /* The moby drop-shadow draw. Both games run the same walk: a list of
     * 8-byte {Moby*, size} records the moby draw appended, a three-word
     * descriptor whose +0x00 and +0x04 are the shadow sprite's two UV words
     * (clut in the high half of the first, tpage in the high half of the
     * second) and whose +0x08 is the list's end pointer, and per moby a
     * ground anchor projected from the moby position and a packed ground
     * word, four corners of a diamond tilted to the ground normal, and four
     * textured semi-transparent FT3s fanned from the anchor.
     *
     * `list_source` says where the list starts: SHADOW_LIST_ADDR reads
     * `list_base`; SHADOW_LIST_GTE_DATA reads GTE data register `list_reg`
     * (the moby draw leaves the base there as a cross-function live value)
     * and adds `list_offset`. The register is captured before the draw runs,
     * since the draw itself overwrites the GTE data registers.
     *
     * `moby_pos` is the Vector3D the anchor's horizontal components come
     * from (x at +0, y at +4); `moby_ground` is the word the gameplay update
     * packs the ground height (bits 0..15, quarter scale), the two 6-bit
     * ground-normal angles (bits 16..21 and 22..27) and the shadow-enabled
     * bit into. `size_shl` shifts the record's size word before it becomes
     * the diamond's half-extents. `uv1_bottom_right` = 1 when +0x04 holds
     * the sprite's bottom-right corner and the emitter subtracts 0x1F00 to
     * make the top-right. `matrix_fold_a` = 1 reproduces the first rotation
     * block's handwritten form that drops the R12 term and adds the second
     * angle index times 8 into R11 instead. `ground_plane` is the address of
     * the signed world height of the player's own ground plane, which the
     * depth-aware decal shader rejects walls against (0 = no such plane). */
    struct {
        uint32_t desc;
        uint32_t hook;
        int      list_source;       /* SHADOW_LIST_* */
        uint32_t list_base;
        int      list_reg;          /* GTE data register index */
        uint32_t list_offset;
        uint32_t moby_pos;
        uint32_t moby_ground;
        int      size_shl;
        int      uv1_bottom_right;
        int      matrix_fold_a;
        int      cull_layout;       /* SHADOW_CULL_* */
        uint32_t ground_plane;

        /* The player's own ground shadow, where the game draws it as a leg of
         * this feeder. `hook` = 0 on a game whose player shadow is a separate
         * renderer with its own feeder (Spyro 1's silhouette fan).
         *
         * The leg emits one gouraud triangle per ring point, fanned from a
         * ground anchor and flat in the shadow's own plane. The silhouette
         * radii are the eight bytes at the head of the avatar's current
         * animation frame, interpolated towards a second frame's eight by the
         * weight `blend_table` maps the avatar's blend byte to, then doubled
         * to sixteen by inserting each neighbouring pair's midpoint; radius k
         * scales the packed s16 XZ direction pair at `ring_dirs` + k*4.
         *
         * `state` is the shadow state block: +0x00 a flag that scales every
         * radius by three quarters, +0x04 the world position the anchor
         * projects from, +0x10 a packed triple of byte angles (Y in bits
         * 16-23, X in 8-15, Z in 0-7) rotating the camera matrix into the
         * shadow's plane, whose top byte suppresses the shadow entirely, and
         * +0x14 the bias subtracted from each triangle's ordering key.
         *
         * `anim_table` is the animation pointer table the avatar's animation
         * word indexes: its low byte and bits 8-15 select the two animations,
         * bits 16-23 and 24-31 the frame within each. `avatar_anim` and
         * `avatar_blend` are the offsets of that word and of the blend byte in
         * the player state block at game_layout.player.base. */
        struct {
            uint32_t hook;
            uint32_t state;
            uint32_t anim_table;
            uint32_t blend_table;
            uint32_t ring_dirs;
            uint32_t avatar_anim;
            uint32_t avatar_blend;
        } player;
    } shadow;

    /* The particle renderer. One routine walks a pool of fixed-stride records
     * and appends one primitive per drawn particle to the polybuf, linking
     * each into the 3D ordering table itself, so the primitives never reach
     * the 2D chokepoint even though their classes (textured quads, gouraud
     * lines) are ones the 2D feeder would take. `hook` = 0 on a game whose
     * particles are fed by their own re-derivation instead.
     *
     * `array_ptr` is the word holding the pool base; a record is `rec_stride`
     * bytes. `sel_off` is a signed halfword: bits 8-11 select the class the
     * routine dispatches on, a negative value whose arithmetic shift by 8 is
     * -1 ends the pool, and any other negative value is an inactive slot the
     * routine steps over. `pos_off` is the first of two words packing the
     * record's camera-relative position in quarter world units, X in the low
     * half and Y in the high half of the first word and Z in the low half of
     * the second. `drawn_off` is the byte the routine clears when it starts a
     * record and sets when it has appended that record's primitive, so after
     * the draw the set bytes name, in pool order, exactly the records that
     * emitted. `fixed_layer_class` is the class index at and above which the
     * routine links its primitive at a fixed front ordering-table entry
     * instead of at the record's own depth, and whose records pack their
     * position differently; those primitives draw over the world. */
    struct {
        uint32_t hook;
        uint32_t array_ptr;
        uint32_t rec_stride;
        uint32_t sel_off;
        uint32_t pos_off;
        uint32_t drawn_off;
        uint32_t fixed_layer_class;
    } particles;

    /* The moby (3D actor) draw. The games share the walk: a cull pass builds
     * one render record per drawn moby in the render scratch, and one or more
     * draw passes iterate those records, decode each model's vertex and face
     * streams, project through the record's matrix and translation and emit.
     * The fields below are where that data lives; the stream encodings and
     * the record shape are the moby_game decoders
     * (games/common/renderer/moby_game.h).
     *
     * `array_head` is the address of the pointer to the first Moby, or of the
     * array itself when the game keeps one at a fixed address (`array_direct`
     * says which). The array ends at the first element whose `state_off` byte
     * equals `state_term`; an element whose state byte has bit 7 set is
     * dormant. `blend_shift` recovers the keyframe blend weight from the
     * record's flags word as (w0 >> blend_shift) & blend_mask, with 4096 as
     * 1.0.
     *
     * `model_table` is indexed by the moby's class halfword; an entry points
     * at a Model, whose animation-pointer array is at `model_anims_off` and
     * whose shared data pointer at `model_data_off`. Inside one
     * AnimationHeader, `anim_ptr_block` is the first of the six geometry
     * pointers (vertices, HP faces, HP colours, LP faces, LP colours, far
     * colours, four bytes apart), `anim_vcount_hi`/`_lo` the HP and LP vertex
     * counts, `anim_radius` the bounding radius byte and `anim_frames` the
     * start of the `anim_frame_stride`-byte keyframe records. */
    struct {
        uint32_t array_head;
        int      array_direct;
        uint32_t array_stride;
        uint32_t state_off;
        uint32_t state_term;
        uint32_t pos_off;
        uint32_t angle_off;
        uint32_t class_off;
        uint32_t anim_state_off;
        uint32_t colour_off;        /* per-moby colour word; 0 where none */
        uint32_t size_off;          /* per-moby screen-scale byte; 0 where none */
        uint32_t drawn_off;         /* the drawn verdict the cull writes back */

        uint32_t model_table;
        uint32_t model_anims_off;
        uint32_t model_data_off;

        uint32_t anim_ptr_block;
        uint32_t anim_scale_off;    /* per-model shrink factor (2 = identity) */
        uint32_t anim_shift_off;    /* per-model short-delta shift */
        uint32_t anim_ccount_off;   /* colour count, low byte */
        uint32_t anim_vcount_hi;
        uint32_t anim_vcount_lo;
        uint32_t anim_radius;
        uint32_t anim_frames;
        uint32_t anim_frame_stride;

        uint32_t rec_base;
        uint32_t rec_stride;
        uint32_t rec_header_off;
        uint32_t rec_frame_a_off;
        uint32_t rec_frame_b_off;
        uint32_t rec_tr_off;        /* three words, x/y/z */
        uint32_t rec_matrix_off;    /* 3x3 int16 */
        uint32_t rec_tier_off;      /* the LOD verdict's word; high half */
        uint32_t rec_material_off;  /* per-moby material word; 0 where none */
        /* The address of the one texture word a special-material treatment
         * substitutes for the face's own: low half the texture page, high half
         * the CLUT, in the GPU's own packing. 0 where the game has no such
         * treatment. */
        uint32_t special_tiledef;
        /* The three lookup tables the substituting treatment's coordinate
         * generation reads: a 16-bit square-root table indexed by the
         * normalized squared length, an 8-bit arctangent table indexed by the
         * minor/major ratio, and an 8-bit table mapping the elevation to the
         * disc radius. 0 where the game has no such treatment. */
        uint32_t env_sqrt_table;
        uint32_t env_atan_table;
        uint32_t env_radius_table;
        /* The global word whose top bits the coordinate generation adds to
         * every azimuth, the per-moby half of that bias coming from the
         * record. 0 where the game has no such treatment. */
        uint32_t env_bias_word;
        uint32_t rec_moby_off;
        int      blend_shift;
        uint32_t blend_mask;

        /* A game that carves the draw scratch out of the polybuf every tick
         * has no fixed record address: with `rec_base` 0 the queue starts at
         * `*polybuf.end - rec_base_polybuf_off`, recomputed per pass, and the
         * block that holds it moves with the draw buffer the tick allocated. */
        uint32_t rec_base_polybuf_off;
        /* 1 = the record's translation is already the one the draw loads into
         * the GTE: the cull applied the per-model scale (the byte at
         * `anim_scale_off`, which still names the translation's unit) and the
         * per-moby size rescale, and dropped the records the rescale pushed
         * past the far limit, so the walk applies neither. 0 = the draw
         * rescales per record, the walk's model-scale path. */
        int      rec_tr_final;
        /* 1 = the cull copies each keyframe's leading stream word into the
         * record rather than a pointer into the model's frame array, so a pose
         * exists exactly when its AnimationHeader pointer does. */
        int      rec_frame_is_word;
        /* The second keyframe's own AnimationHeader*, for a game whose two
         * blended keyframes may come from different animations; 0 where both
         * share the record's first header. */
        uint32_t rec_header_b_off;

        /* The ordering table the same-moby paint rank is read from: the
         * address of the pointer to the bucket array, and the bucket count.
         * 0 retires the rank source. */
        uint32_t world_ot;
        uint32_t ot_buckets;

        /* Hook sites: the cull pass and one entry per draw pass. A zero
         * address registers no hook, so a game reaches only the passes it
         * names. `draw_simple` is the second model kind's own pass. */
        uint32_t hook_cull;
        uint32_t hook_draw_regular;
        uint32_t hook_draw_specular;
        uint32_t hook_draw_shaded;
        uint32_t hook_draw_simple;
    } moby;

    /* The player avatar's own draw. The avatar is not a moby: a dedicated
     * routine renders it from a dedicated state block, as a skeleton of parts
     * whose per-part transform records the draw builds in the tick's render
     * scratch. The walk is games/common/renderer/player.c and the encodings
     * are the player_game slots (games/common/renderer/player_game.h).
     *
     * `base` is the state block; `rec_len` is how much of it one per-call
     * record copies, which is also the span the interp ring captures.
     * `pos_off` is the world position; `rot_off[p]` part p's packed byte
     * euler and `anim_off[p]` its animation selector, both 0 for a part the
     * game does not carry.
     *
     * `part_base`/`part_off`/`part_stride`/`part_count` locate the transform
     * records. With `part_base` 0 the block starts at `*polybuf.end -
     * part_base_polybuf_off`, recomputed per pass, because the game carves
     * its render scratch out of the polybuf every tick. `part_block_len` is
     * how many bytes of the block a record copies.
     *
     * Inside one part record, `part_flags_off` is the flags word, whose
     * `blend_mask` bits are the keyframe blend weight in 1.12 and whose
     * `part_matrix_sign` bit (0 = none) marks a part that carries its own
     * matrix rather than inheriting the previous part's. `part_matrix_off` is
     * the packed 3x3, `part_tr_off` the three translation words, and
     * `part_stream_a_off`/`_b_off` the two keyframes' cursor triples: the
     * per-vertex static/correction stream, the byte stream and the halfword
     * stream, four bytes apart. `part_stream_flag_bit[2]` say which flags bit
     * opens each keyframe's stream form. `part_seam_off` (0 = none) is where
     * a host-composed block keeps the root part's two seam words, from the
     * block base: past the terminating flags word and inside
     * `part_block_len`, bytes the guest's own block never defines.
     *
     * `colour_scratch_off`/`_len` is the pre-blend colour table the draw
     * builds in the same scratch when `colour_filter_off` selects it;
     * `sxy_base`/`sxy_len` is where the draw leaves every projected vertex's
     * int16 screen position, which the walk reads as the game's own cull
     * verdict and the record store snapshots.
     *
     * `flame_*` and `shadow_*` are the auxiliary player feeders whose state
     * the record store brackets alongside the model record, 0 where the game
     * has none. `pair_bound` is how far one record's position may move in a
     * tick and still pair with the previous tick's, in world units. */
    struct {
        uint32_t base;
        uint32_t rec_len;
        uint32_t pos_off;
        uint32_t rot_off[4];
        uint32_t anim_off[4];
        uint32_t sort_depth_off;
        uint32_t colour_filter_off;
        uint32_t emit_mode_off;
        uint32_t hook_draw;

        uint32_t part_base;
        uint32_t part_base_polybuf_off;
        uint32_t part_off;
        uint32_t part_stride;
        int      part_count;
        uint32_t part_block_len;

        uint32_t part_flags_off;
        uint32_t part_matrix_off;
        uint32_t part_tr_off;
        uint32_t part_stream_a_off;
        uint32_t part_stream_b_off;
        uint32_t part_seam_off;
        int      part_stream_flag_bit[2];
        uint32_t part_matrix_sign;
        uint32_t blend_mask;

        uint32_t colour_scratch_off, colour_scratch_len;
        uint32_t sxy_base, sxy_len;

        uint32_t flame_base, flame_len;
        uint32_t shadow_base, shadow_len;

        double   pair_bound;
    } player;

    /* The player's fire-breath ribbon. One handwritten GTE routine walks
     * eight strands of a static cross-section spine table and appends a
     * textured additive POLY_GT4 strip per strand, plus four untextured
     * gouraud triangles capping the strand's first cell. `hook` = 0 on a
     * game with no located emitter, and the producer registers no dispatch.
     *
     * The strand state lives in the block at game_layout.player.flame_base:
     * the ribbon origin at +0x00, the strip's two texture words at +0x10 and
     * +0x14, the per-strand keyframe counter at +0x20 and the strand length
     * at +0x28, the active flag at +0x98, and the attach SHORTMATRIX at
     * `attach_off`. `super_off` is the word selecting the wider second form,
     * 0 on a game with no such form, which also removes the halved cap
     * flicker vector and the second texture-word pair 8 bytes on that the
     * wide form reads.
     *
     * Strand s reads its spine at `geom` + `geom_stride_lo` * s for s < 4 and
     * at `geom` + `geom_hi_base` + `geom_stride_hi` * (s - 4) above that, and
     * its connectivity word at `conn` + 4 * (counter - 1), plus `conn_hi_off`
     * for s >= 4. `cap_colours` is the first of two four-word cap colour
     * sets; `cap_colour_off` returns the byte offset of the set this tick, 0
     * or 0x10, and NULL means always the first set. `cap_amp` and `strip_amp`
     * scale the sine-table flicker vector of the cap ring and of each strip
     * cross-section, `*_super` the same under a set `super_off`. `uv1_bias`
     * is added to the second texture word before the per-step ramp. */
    struct {
        uint32_t hook;
        uint32_t attach_off;
        uint32_t super_off;
        uint32_t geom;
        uint32_t geom_stride_lo;
        uint32_t geom_stride_hi;
        uint32_t geom_hi_base;
        uint32_t conn;
        uint32_t conn_hi_off;
        uint32_t cap_colours;
        int      cap_amp, cap_amp_super;
        int      strip_amp, strip_amp_super;
        int32_t  uv1_bias;
        uint32_t (*cap_colour_off)(struct CPUState* cpu);
    } flame;

    /* Hook sites: the guest entry addresses the feeders dispatch on.
     * `visibility` is the function that fills the visibility verdict when
     * the game computes it outside the draw; 0 when the draw computes it. */
    struct {
        uint32_t world_draw;
        uint32_t sky_draw;
        uint32_t visibility;
    } hooks;

    /* The game's extruded 3D glyph models, the font the host text producer
     * (games/common/renderer/text_moby.c) draws the settings menu, the OSD
     * toasts and the performance readout with. `form` 0 means the game has
     * none and the producer publishes no glyphs.
     *
     * Form GAME_FONT_CLASS_PER_GLYPH: every glyph is its own moby class; the
     * glyph index is the class, `model_table[index]` points at a model whose
     * first word is negative, byte 0 the vertex count, byte 1 the face
     * count, +0x04 a stream of 3-byte packed vertices and +0x0C a stream of
     * 8-byte faces (four 7-bit vertex indices in word 0 bits 23/16/9/2, the
     * face normal in word 1's three high bytes as z, x, y). Model +Z faces
     * the reader.
     *
     * Form GAME_FONT_ONE_CLASS: one class (`font_class`) carries every glyph
     * as one geometry block per index; the model-table entry is a KUSEG
     * address of a model whose first word is the block count and whose
     * block pointers start at `block_table_off`. A block holds the vertex
     * count in header byte 1, one-word vertices at +0x10 (y in bits 0-11, x
     * in 11-21, z in 21-31), the offset of the byte-counted face list in
     * the low half of +0x08, and 8-byte flat-lit faces (four 7-bit indices
     * in word 0 bits 21/14/7/0, the normal in word 1 bits 15/7/23 as x, y,
     * z). Model -Z faces the reader. The model is resident only in the
     * levels that draw it, so the producer keeps the decoded glyphs in a
     * host cache file.
     *
     * A character maps to `digit_base` + (c - '0'), `letter_base` +
     * (c - 'A'), or the index `punct` pairs it with (the list ends at a
     * zero character), drawn `dy` model units below where the model puts
     * it. `synth_period` builds the period the font lacks as
     * an extruded square as deep as the glyph of `synth_depth_char`. A colon
     * is two periods, each scaled by `colon_scale`, moved by
     * `colon_recenter` and then apart by +-`colon_spread` (model units).
     *
     * `model_h` is the model-unit height a caller's cap height maps to.
     * A face's colour is the caller's base colour plus, per channel,
     * `light_add` * t + max(0, t - `hi_threshold`) * `hi_gain`, t the unit
     * N.L. `fixed_light` is the unit light of the performance readout (x
     * right, y down, z toward the reader) and `fixed_tint` its 0xRRGGBB
     * base colour. */
    struct {
        int      form;
        uint32_t model_table;
        int      font_class;
        uint32_t block_table_off;
        int      digit_base;
        int      letter_base;
        struct { char ch; int index; int dy; } punct[8];
        int      synth_period;
        char     synth_depth_char;
        float    colon_recenter, colon_spread, colon_scale;
        float    model_h;
        float    light_add[3];
        float    hi_threshold, hi_gain;
        float    fixed_light[3];
        unsigned fixed_tint;
    } font;
} game_layout_t;

#define GAME_FONT_NONE            0
#define GAME_FONT_CLASS_PER_GLYPH 1
#define GAME_FONT_ONE_CLASS       2

extern const game_layout_t game_layout;

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_LAYOUT_H */
