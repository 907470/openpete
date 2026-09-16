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

typedef struct game_layout {
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

    /* The cyclorama header (Spyro 1 `Cyclorama`). */
    struct {
        uint32_t base;
        uint32_t sector_count;
        uint32_t sectors;           /* sector pointer array */
        uint32_t occ_group_count;
        uint32_t occ_groups;
        uint32_t bg_colour;         /* 0x00BBGGRR */
    } cyc;

    /* The GTE depth cue the world and sky draws run: IR0 (0x1000 = 1.0)
     * and the far colour FC (0x00BBGGRR). */
    struct {
        uint32_t factor;
        uint32_t far_colour;
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
