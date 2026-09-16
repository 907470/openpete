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
