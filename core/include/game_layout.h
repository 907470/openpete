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

enum { TERRAIN_FACES_SPYRO1 = 0, TERRAIN_FACES_SPYRO3 = 1 };
enum { CYC_FACES_SPYRO1 = 0, CYC_FACES_SPYRO3 = 1 };
/* Where the moby-shadow draw finds the base of the drawn-moby list, and which
 * screen-extent rejection it applies before emitting a fan. */
enum { SHADOW_LIST_ADDR = 0, SHADOW_LIST_GTE_DATA = 1 };
enum { SHADOW_CULL_SPYRO1 = 0, SHADOW_CULL_SPYRO3 = 1 };

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
    } env;

    /* The environment animation lists (Spyro 1 `EnvironmentAnimations`).
     * Each is {count, pointer to a descriptor-pointer array}; the field is
     * the address of the count and the array pointer follows at +4. */
    struct {
        uint32_t base;
        uint32_t scroll;            /* scrolling-texture descriptors */
        uint32_t lp_poly;           /* LP geometry animations */
        uint32_t hp_poly;           /* HP geometry animations */
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
    } shadow;

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
        uint32_t rec_moby_off;
        int      blend_shift;
        uint32_t blend_mask;

        /* A game that carves the draw scratch out of the polybuf every tick
         * has no fixed record address: with `rec_base` 0 the queue starts at
         * `*polybuf.end - rec_base_polybuf_off`, recomputed per pass, and the
         * block that holds it moves with the draw buffer the tick allocated. */
        uint32_t rec_base_polybuf_off;
        /* 1 = the record's translation is already the one the draw loads into
         * the GTE: the cull applied the per-model scale and the per-moby size
         * rescale (and dropped the records the rescale pushed past the far
         * limit), so the walk applies neither. `tr_depth_shift` is then how far
         * left a view-Z in that translation's units shifts to reach full world
         * units. 0 = the draw rescales per record, the walk's model-scale path.
         */
        int      rec_tr_final;
        int      tr_depth_shift;
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

    /* Hook sites: the guest entry addresses the feeders dispatch on.
     * `visibility` is the function that fills the visibility verdict when
     * the game computes it outside the draw; 0 when the draw computes it. */
    struct {
        uint32_t world_draw;
        uint32_t sky_draw;
        uint32_t visibility;
    } hooks;
} game_layout_t;

extern const game_layout_t game_layout;

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_LAYOUT_H */
