// game_layout.h - the engine-layout descriptor: where a game keeps the RAM
// the presentation feeders read.
//
// Each game defines one game_layout_t (games/<game>/renderer/layout.c) from
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
#include "openpete_thread_roles.h"

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
/* What a canonical present with no game render since the last one shows when
 * the game waits at a known VSync (game_layout.db.repeat_site). Without a row
 * the present reads the current buffer's DRAWENV: isbg 1 presents an empty
 * frame cleared to its colour, isbg 0 holds the presented frame.
 * REPEAT_SITE_HOLD: the screen keeps the presented frame (the game draws
 * nothing between this wait and its next OT, or copies the screen and
 * zeroes isbg on both buffers after the wait).
 * REPEAT_SITE_CLEAR_BLACK: the game ClearImage's both buffers black after the
 * wait, and its next draw clears black.
 * REPEAT_SITE_CLEAR_DB: the game ClearImage's both buffers after the wait,
 * and its next draw clears to the current buffer's DRAWENV colour with isbg
 * set again (the copy loop before the wait zeroed isbg, not the colour). */
enum {
    REPEAT_SITE_NONE        = 0,
    REPEAT_SITE_HOLD        = 1,
    REPEAT_SITE_CLEAR_BLACK = 2,
    REPEAT_SITE_CLEAR_DB    = 3,
};
#define GAME_REPEAT_SITE_MAX 8

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
 * stale and only KIND_VERT sectors need the prev/cur vertex banks; KIND_COLOUR
 * sectors take the prev/cur colour-word copies the sub-tick colour lerp reads. */
enum { ENV_ANIM_KIND_VERT = 0, ENV_ANIM_KIND_COLOUR = 1 };

#define ENV_ANIM_SUBLISTS 3

/* The closed form a sub-list's walker computes, which the native mirror
 * reproduces for a sector the game's walker skips. Spyro 2 func_80023778,
 * func_80023994, func_80023A9C; Spyro 3 func_80021FF4, func_80022158,
 * func_80022260 (environment.s), called in that order each update:
 * - COLOUR: per HP colour word k, i = ((d >> 24) + T * {1,2,2,3}[(d >> 24) & 3])
 *   & 0xFF, IR0 = (c >> 3) + (c >> 4) + 0x480 with c the s16 at
 *   wave_table + 0x80 + 2i, then DPCS of d & 0xFFFFFF toward the far colour
 *   word at count + 4.
 * - WAVE1: per HP vertex word k, i = (d + T + T/2) & 0xFF, low 10 bits =
 *   (d >> 16) + ((c * 320) >> 16) with c the s16 at wave_table + 0x80 + 2i,
 *   bits 10-31 kept.
 * - WAVE2: per word pair (d0, d1), i = (d0 + T + T/2) & 0xFF, vertex
 *   (d0 >> 16) & 0xFF = d1 + (s[i+0x20] >> 10) + ((s[i] >> 8) << 10) +
 *   ((s[i+0x40] >> 8) << 21), s the s16 table at wave_table.
 * An entry's data starts at the count word + (entry >> 16) * 4 and runs
 * (entry >> 8) & 0xFF words or pairs; T is the update's timer word. */
enum { ENV_ANIM_MODEL_NONE = 0, ENV_ANIM_MODEL_COLOUR = 1,
       ENV_ANIM_MODEL_WAVE1 = 2, ENV_ANIM_MODEL_WAVE2 = 3 };

/* Particle classes that link at a depth bucket: the routine dispatches
 * classes 0-4 to their own bodies and links class 5 and up at a fixed front
 * entry, which stamps nothing. */
#define GAME_PART_DEPTH_CLASSES 5

/* One ENV_ANIM_ARENA sub-list. `entry_off` = 0 marks an unused row. */
typedef struct env_anim_sublist {
    uint16_t hdr_off;    /* byte offset into the block that locates the list */
    uint8_t  indirect;   /* 1: the block word at hdr_off holds the list's own
                          * byte offset from the block base; 0: the count word
                          * is at hdr_off itself */
    uint8_t  entry_off;  /* bytes from the count word to the first entry */
    uint8_t  skip_top;   /* 1: the walker skips an entry with bit 31 set */
    uint8_t  kind;       /* ENV_ANIM_KIND_* */
    uint8_t  model;      /* ENV_ANIM_MODEL_*: the walker's closed form */
} env_anim_sublist_t;

/* One moby placement marker: a moby of class `cls` whose state byte goes
 * from `state_prev` to `state_curr` between two ticks was placed by the
 * game's code, not moved, on that tick. `state_prev` 0xFF matches any
 * previous state. */
typedef struct game_moby_place {
    uint16_t cls;
    uint8_t  state_prev;
    uint8_t  state_curr;
} game_moby_place_t;

/* One director placement marker: a game record drives one moby through a
 * pointer word captured as the interp ring row `moby_row`; when its state
 * word, captured as the ring row `state_row`, goes from `state_prev` to
 * `state_curr` between two snapshots, the record's code placed the moby the
 * later snapshot's pointer names on that tick. */
typedef struct game_moby_director {
    const char* state_row;
    const char* moby_row;
    uint32_t    state_prev;
    uint32_t    state_curr;
    /* Byte offset of the pointer word within `moby_row`. With `state_row`
     * NULL the row is a pointer edge: the moby the word names in the later
     * snapshot and not in the earlier one was placed on that tick. */
    uint32_t    moby_off;
} game_moby_director_t;

/* One props-word placement marker: in the overlays whose name starts with
 * `overlay` (NULL: any), a moby of class `cls` whose props word at
 * `word_off` goes from 0 to nonzero on the tick its record steps was placed
 * by the game's code on that tick. `base_off` nonzero also requires the
 * record's position component `base_axis` on the earlier tick to equal the
 * props word at `base_off` (the placement starts from that stored value). */
typedef struct game_moby_props {
    uint16_t    cls;
    uint16_t    word_off;
    uint16_t    base_off;
    uint16_t    base_axis;
    const char* overlay;
} game_moby_props_t;

/* One attachment marker: in the overlays whose name starts with `overlay`
 * (NULL: any), a moby of class `cls` copies the position of its parent, the
 * moby-array record whose index is the props word at `parent_off`; its step
 * is a jump on the tick its parent's step is one. */
typedef struct game_moby_attach {
    uint16_t    cls;
    uint16_t    parent_off;
    const char* overlay;
} game_moby_attach_t;

/* game_moby_regime_t.snaps: which snapshots of a ring pair the row tests. */
#define GAME_REGIME_EITHER 0u   /* either snapshot of the pair */
#define GAME_REGIME_BEFORE 1u   /* the snapshot before the pair's earlier one */

/* One regime marker: the 4-byte interp ring row `row` holding a value in
 * [lo, hi] (unsigned) on the snapshots `snaps` names marks every moby step
 * of that pair as a placement (a load or reload writes the records). */
typedef struct game_moby_regime {
    const char* row;
    uint32_t    lo;
    uint32_t    hi;
    uint32_t    snaps;
} game_moby_regime_t;

typedef struct game_layout {
    /* The game's display environment, in VRAM pixels, as its boot code hands
     * it to SetDefDispEnv/SetDefDrawEnv. `disp_w` by `disp_h` is the region
     * the GPU scans out; `clip_h` is the DRAWENV clip height, the rows the
     * game actually draws into. The (disp_h - clip_h) rows are the overscan
     * bands the game leaves black, which the present blit crops, so what
     * reaches the screen is disp_w by clip_h. These are a claim about the
     * game read from its source; the runtime values arrive from the guest's
     * own PutDispEnv/PutDrawEnv and win wherever they disagree. */
    struct {
        int disp_w, disp_h;
        int clip_h;
    } display;

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
         * `lp_poly` and `hp_poly` are the geometry registry and `lp_col`
         * and `hp_col` the vertex-colour registry the animation update
         * walks in every game; the iterator reads them only under
         * ENV_ANIM_LISTS. `swap` and `scroll` carry the same shape. A
         * colour descriptor names its sector in the u16 at +4. */
        uint32_t swap;              /* texture-swap descriptors: the destination
                                     * tile is the word at desc+4 */
        uint32_t scroll;            /* scrolling-texture descriptors */
        uint32_t lp_poly;           /* LP geometry animations */
        uint32_t hp_poly;           /* HP geometry animations */
        uint32_t lp_col;            /* LP vertex-colour animations */
        uint32_t hp_col;            /* HP vertex-colour animations */
        uint32_t poly_sec_u8;       /* 1: a geometry descriptor names its sector in
                                     * the byte at +4; 0: in the u16 at +4 */

        /* Deferred keyframe vertex animations, each {count, descriptor-
         * pointer array}. The animation update posts a descriptor's list
         * index into its sector's pending byte (+0x18 LP, +0x1A HP) when the
         * keyframe advances; the world draw packs the posted pose into the
         * sector's vertex bank and writes 0xFF there only while it bins the
         * sector in that tier, so a bank the draw does not list holds an
         * older pose. 0 when the game packs every animated bank each tick. */
        uint32_t kf_lp_poly;        /* LP deferred keyframe vertex animations */
        uint32_t kf_hp_poly;        /* HP deferred keyframe vertex animations */
        /* Deferred keyframe colour animations, the same scheme with pending
         * bytes +0x19 LP and +0x1B HP; 0 when the game has none. */
        uint32_t kf_lp_col;         /* LP deferred keyframe colour animations */
        uint32_t kf_hp_col;         /* HP deferred keyframe colour animations */
        /* 1: byte 3 of a vertex keyframe word is the tier-hide verdict the
         * pack writes into the sector's no-draw flag; 0: the pack writes no
         * flag. A vertex pose lands at bank index byte +5 on a game whose
         * descriptor names its sector in a byte (poly_sec_u8), else at 0. */
        uint32_t kf_hide;

        /* ENV_ANIM_ARENA. `block_ptr` is the address of the word holding the
         * block base. `live_table` is a byte table indexed by an entry's low
         * byte (the sector index); bit 0 set = that animation runs, and the
         * game's walkers skip the entry when it is clear. */
        uint32_t block_ptr;
        uint32_t live_table;
        env_anim_sublist_t sub[ENV_ANIM_SUBLISTS];

        /* The sub-list models' inputs (ENV_ANIM_MODEL_*): the s16 sine table
         * the walkers index (cosine at +0x80) and the timer word they read.
         * 0 = no model; every sector then follows guest RAM alone. */
        uint32_t wave_table;
        uint32_t timer;
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
     * inside a buffer. `repeat_site` names the VSync waits whose canonical
     * present (one with no game render since the last) the DRAWENV the
     * present reads does not describe: `yield_ra` is the guest return
     * address of the VSync call the game waits in (openpete_yield_site.ra),
     * `mode` a REPEAT_SITE_* value; a row with yield_ra 0 ends the table. */
    struct {
        uint32_t cur_ptr;
        uint32_t base;
        uint32_t stride;
        uint32_t drawenv;
        struct { uint32_t yield_ra; int mode; } repeat_site[GAME_REPEAT_SITE_MAX];
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
    /* The simulation regime word the tick dispatcher branches on (4 bytes);
     * the interp ring's g_GameState row. 0 = not located. */
    uint32_t game_state;

    /* The terrain decode switches. The 7-word sector header and the 11/11/10
     * vertex packing are common; the games differ in the header's radius and
     * flag bits, in the vertex Z offset scale, and in the tables the walk may
     * consult. `radius_mask` and `flags_mask` split header word 1's low half;
     * `flat_flag` is the bit (0 when the game has no flat rule) that marks a
     * sector whose Z offsets are stored at `vz_flat_shr` bits less; every
     * other sector's Z offset is `vz_shl` bits more than the 10 packed bits.
     * `uv_ori_table` is the HQ tile UV-orientation offset table (0 =
     * identity) and `uv_ori_mask` the mask applied to the index the emit
     * derives as `tileWord1 >> 25`, bounding it to the table; `defer_q` are
     * the two near-cell defer queues the draw fills (0 = none). */
    struct {
        uint32_t radius_mask;
        uint32_t flags_mask;
        uint32_t flat_flag;
        int      vz_shl;
        int      vz_flat_shr;
        uint32_t uv_ori_table;
        uint32_t uv_ori_mask;
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
        /* The 4x4 near-cell path's tile source, 0 when the port does not draw
         * that path. A face with a corner SZ3 under `near_corner` and word 3
         * `near_flag_tier2` clear is drained as a 5x5 vertex grid, 16 cells in
         * row-major order, cell i textured from the 16x16 tile at
         * `hq_cells4` + i*8 of the face's HQ record. Spyro 1: 0x28, after the
         * base entry and the four 2x2 quadrants (the near drain's tier-4
         * quad pass, table D_8006CF58). */
        uint32_t hq_cells4;
        /* The same drain's triangle pass. A tier-4 triangle is split into 16
         * sub-triangles over 15 nodes; `hq_tri4_nodes` is its 16-word
         * sub-triangle table (node ids A, B, C in bytes 3, 2, 1, cell
         * selector in bits 2-5, UV orientation in bits 0-1), read for the
         * sub-triangles' ordering keys wherever it is located. The textures
         * are 0 when the port does not draw the pass: `hq_tri4_cells` the
         * byte table the selector plus the face's UV rotation indexes (cell *
         * 8, the rotated 4x4 grid), `hq_tri_uv` the per-orientation UV offset
         * table (4 blocks of 128 bytes) and `hq_tri_uv_mask` the mask on the
         * tile's word1 >> 25 index into a block. Spyro 1: D_8006D0F8,
         * D_8006D388, D_8006D3C8, 0x78 (the near drain's tier-4 triangle
         * pass). The node tables are the same words in all three games. */
        uint32_t hq_tri4_nodes;
        uint32_t hq_tri4_cells;
        uint32_t hq_tri_uv;
        uint32_t hq_tri_uv_mask;
        /* The +0x2000 queue's triangle pass. A tier-2 triangle is split into
         * 4 sub-triangles over 6 nodes (the corners and the edge midpoints);
         * `hq_tri2_nodes` is its 4-word sub-triangle table (node ids 0-5 in
         * bytes 3, 2, 1, tile selector in bits 2-3, UV orientation in bits
         * 0-1), read for the keys as `hq_tri4_nodes` is. `hq_tri2_tiles`, 0
         * when the port does not draw the pass, is the byte table the
         * selector plus the face's UV rotation indexes (quadrant tile * 8
         * from `hq_sub0`). UVs come from `hq_tri_uv` as for tier 4. Spyro 1:
         * D_8006D0E8, D_8006D378 (the near drain's tier-2 triangle pass). */
        uint32_t hq_tri2_nodes;
        uint32_t hq_tri2_tiles;
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
        /* The LP walk's per-vertex near outcode. A sector the binning lists
         * with view-Z minus radius at or below `lp_near_band` carries the
         * near-band entry bit; the LP vertex transform of such a sector tags
         * each vertex whose SZ3 is below `lp_near_sz` minus `lp_near_sz_step`
         * times bits 24-25 of header word +0x10, and the face walk skips a
         * face whose corners all carry the tag. 0 in `lp_near_sz` on a game
         * whose LP walk has no such outcode (the authored admission rules
         * there instead). */
        int      lp_near_band;
        int      lp_near_sz, lp_near_sz_step;
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
        /* The LP far cull's operand. 0 = the face's depth slot (4-corner
         * SZ sum >> 5) is dropped at or beyond env.cull_dist >> 7. 1 = its
         * whole LP bucket index (the depth slot plus lp_base_slots plus the
         * biased slots) is dropped at or beyond (env.cull_dist >> 7) - 1. */
        int      lp_farcull_keyed;
        /* 1 = the near/HQ-classified faces key through that same ordinary
         * insert, so each publishes a positive ordering key like every
         * other terrain face. 0 = the near/HQ queue carries no ordering
         * key, and those faces contest per-pixel against their own view-Z
         * without publishing one. */
        int      nearband_ot_keys;
        /* 1 = the draw submits mobys and terrain through one ordering
         * table, so both classes' keys share one 128-world bucket scale:
         * the terrain-versus-terrain key contest compares slot indices
         * rather than world distances, an equal slot keeping the
         * fragment, and a moby face takes its world bucket as its key.
         * An actor face is not compared against the terrain key; true
         * depth with the stored tie pull decides it against terrain. */
        int      slot_ot_contest;
        /* The WAD entry of a level whose terrain another scene reuses: a
         * scene sector whose LP faces all occur, in world space, in one
         * sector of this level's terrain (sub-file 1 of the entry) borrows
         * that sector's HP faces and the LP faces it lacks and takes that
         * sector's header for its tier verdicts, in every LOD mode; the
         * borrowed faces sample this level's VRAM image (sub-file 0) from
         * an auxiliary bank. A scene whose every sector matches also
         * imports the level's sectors it lacks. 0 = no such level. */
        int      hp_source_wad_entry;
        /* Nonzero: a borrowed HP face admitted only because it lies on the
         * scene sector's own uncov LP faces (a stand-in) adds lp_base_slots
         * to its key, the term the LP face it replaces carried. Off, the
         * stand-in keys as HP and a far stand-in face seen near edge-on can
         * key below a nearer scene HP wall and paint over it. On, it also
         * loses to any HP-keyed face within lp_base_slots behind it: the
         * whole-mesh borrows of neighbouring sectors with no HP of their
         * own, and own LP faces at a nearer key, so a scene whose stand-ins
         * adjoin such faces opens holes in the borrowed surface. */
        int      standin_lp_key;
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
        /* The ordering-table slot of a moby shadow triangle is its key >> 7
         * less (signed byte at moby + `moby_ot_bias`) - `moby_ot_bias_sub`.
         * `moby_ot_bias` = 0: the slot is not mirrored and the triangles
         * carry no key. */
        uint32_t moby_ot_bias;
        int      moby_ot_bias_sub;
        /* The moby cull appends a list record for a moby whose `moby_ground`
         * word has its sign bit set and whose view-space Z is below 0x1200,
         * once its lateral test has passed; the record's size word is the
         * byte at `frame_radius_off` inside the moby's current keyframe
         * record (AnimationHeader + moby.anim_frames + frame *
         * moby.anim_frame_stride). 0 = the list is not extended past the
         * cull's own records. */
        uint32_t frame_radius_off;

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
     * record and sets once the record passes its screen and depth window; a
     * record whose class appends right there emitted exactly when the byte is
     * set. `fixed_layer_class` is the class index at and above which the
     * routine links its primitive at a fixed front ordering-table entry
     * instead of at the record's own depth, and whose records pack their
     * position differently; those primitives draw over the world.
     *
     * `stamp_skip[c]` is class c's test between the stamp and the append:
     * with `on` set, the routine forms the ordering key (SZ3 >> 5) - size,
     * where SZ3 is the record point's projected depth and size the byte at
     * `size_off` (0: no size term), and appends nothing when the key is below
     * `key_min`. A stamped record that fails it emitted no primitive. */
    struct {
        uint32_t hook;
        uint32_t array_ptr;
        uint32_t rec_stride;
        uint32_t sel_off;
        uint32_t pos_off;
        uint32_t drawn_off;
        uint32_t fixed_layer_class;
        struct game_part_stamp_skip {
            uint8_t on;
            uint8_t size_off;
            int8_t  key_min;
        } stamp_skip[GAME_PART_DEPTH_CLASSES];
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
        /* The cutscene path sampler's actor count variable: while an overlay
         * whose name starts with one of the space-separated prefixes in
         * cutpath_overlays runs, the first [cutpath_count] array records
         * ride the sampler's track, and it moves an actor across a track
         * segment of cutpath_snap world units or more in one step instead of
         * interpolating it. 0 where the game's sampler has no such rule. */
        uint32_t    cutpath_count;
        uint32_t    cutpath_snap;
        const char* cutpath_overlays;
        /* A moby step longer than this many world units in one tick is a
         * jump, for a moby no game marker decides; 0 = no distance bound. */
        uint32_t    jump_bound;
        /* The placement markers (game_moby_place_t), `place_n` rows; NULL
         * where the game names none. */
        const game_moby_place_t* place;
        uint32_t    place_n;
        /* The director placement markers (game_moby_director_t),
         * `director_n` rows; NULL where the game names none. */
        const game_moby_director_t* director;
        uint32_t    director_n;
        /* The props-word markers (game_moby_props_t), `props_mark_n` rows,
         * the attachment markers (game_moby_attach_t), `attach_n` rows, and
         * the regime markers (game_moby_regime_t), `regime_n` rows; NULL
         * where the game names none. */
        const game_moby_props_t*  props_mark;
        uint32_t                  props_mark_n;
        const game_moby_attach_t* attach;
        uint32_t                  attach_n;
        const game_moby_regime_t* regime;
        uint32_t                  regime_n;

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

        /* The world ordering table, read only by the paint-rank calibration
         * instrument (moby_rank_cal.c): the address of the pointer to the
         * bucket array, and the bucket count. 0: the instrument reports no
         * table. */
        uint32_t cal_world_ot;
        uint32_t cal_ot_buckets;

        /* Curated embedded props: static scenery the game authored sunk into
         * the ground, which the native key contest renders whole (see
         * moby_class_is_prop in games/common/renderer/moby.c). The first
         * `prop_n` rows of `props` each name a class and a scene. A row with
         * scene -1 matches the class anywhere. Any other row matches while
         * the word at `prop_state_addr` equals `prop_state_scene` and the
         * word at `prop_scene_addr` equals the row's scene: a game whose
         * cutscenes number their actors by slot names the cutscene there,
         * since one class number is a different model in each cutscene. */
        struct { uint16_t cls; int32_t scene; } props[4];
        int      prop_n;
        uint32_t prop_scene_addr;
        uint32_t prop_state_addr;
        uint32_t prop_state_scene;

        /* The door-effect class: the stars and swirl the game draws in front
         * of a sub-area door, whose black plate is terrain the effect lies
         * behind in true depth. The native key contest draws this class by
         * the game's ordering-table key against terrain, so it paints over
         * the plate where its key sorts in front, as the ordering table does
         * (moby_class_draws_by_key in games/common/renderer/moby.c). 0: no
         * class draws by key. */
        uint16_t key_order_cls;
        /* The game.state values (bit n = state n, game_layout.game_state)
         * whose regime draws an overlay's own models: the class halfword
         * indexes the model table, which a cutscene or the title screen
         * fills with its own set, so key_order_cls names the door effect
         * only outside these states. 0: every state. */
        uint32_t key_order_off_states;

        /* Hook sites: the cull pass and one entry per draw pass. A zero
         * address registers no hook, so a game reaches only the passes it
         * names. `draw_simple` is the second model kind's own pass. */
        uint32_t hook_cull;
        uint32_t hook_draw_regular;
        uint32_t hook_draw_specular;
        uint32_t hook_draw_shaded;
        uint32_t hook_draw_simple;
        /* The word holding the one moby a menu state draws over the finished
         * frame: the game points `array_head` at it, terminates the array
         * after it, and runs the cull and regular draw again into an ordering
         * table linked after every other primitive. A regular pass run while
         * the array head equals this word draws HUD3D. 0 where the game has
         * no such pass. */
        uint32_t overlay_moby;
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
     * `colour_filter_off` is the state's tint word: bytes 0-2 a far colour,
     * byte 3 a weight; a non-zero weight makes the draw cue every entry of
     * the model colour block toward the far colour, which the walk computes
     * itself from the word. `sxy_base`/`sxy_len` is where the draw leaves every projected vertex's
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

    /* The glow ring: one screen-facing fan per live record of a pool the
     * world draw's glow routine walks. `hook` = 0 on a game with no located
     * routine, and the producer registers no dispatch. `ra_guard` is the
     * return address of the world draw's own call, 0 when the image holds
     * only one call site; a non-zero value refuses every other caller, which
     * is how a second site over a frozen frame is kept out.
     *
     * The pool is `slots` records of `stride` bytes: world XYZ at `pos_off`
     * and a packed word at `packed_off` holding the shape index in byte 0 and
     * a signed ordering-table bias in byte 3. A record is dropped when its
     * camera-relative offset leaves the +/-`box` cube, when its rotated depth
     * falls under `near_z` or reaches `far_z`, or when it leaves the view
     * pyramid 4|X| - 3Z < `pyr_x` or 3|Y| - Z < `pyr_y`. `front_bias` is the
     * bias value whose records the routine links at the fixed front entry
     * instead of their own bucket; negative on a game whose routine has no
     * such branch and clamps to bucket 0 instead. `owner_offset` is the
     * authored depth offset of the moby a glow sits on, in ordering-table
     * slots: the record names no owner, so the producer cannot read it, and
     * the fan's native depth needs it to sort against that moby's faces as
     * the table does. */
    struct {
        uint32_t hook;
        uint32_t ra_guard;
        uint32_t pool;
        uint32_t slots;
        uint32_t stride;
        uint32_t pos_off;
        uint32_t packed_off;
        int32_t  box;
        int32_t  near_z, far_z;
        int32_t  pyr_x, pyr_y;
        int      front_bias;
        int      owner_offset;
    } glow;

    /* The gem glint: one screen-facing four-point star, or a plain cross
     * beyond `near_vz`, per live record of a second pool. `hook` = 0 on a
     * game with no located routine.
     *
     * The pool is `slots` records of `stride` bytes: world XYZ at `pos_off`
     * and a packed word at `packed_off` holding the remaining lifetime in
     * byte 0. The routine halves the camera-relative offset before rotating,
     * so `box`, `near_z`, `far_z`, `near_vz` and the pyramid bounds `pyr_x`
     * and `pyr_y` are all in half-world units. */
    struct {
        uint32_t hook;
        uint32_t pool;
        uint32_t slots;
        uint32_t stride;
        uint32_t pos_off;
        uint32_t packed_off;
        int32_t  box;
        int32_t  near_z, far_z;
        int32_t  pyr_x, pyr_y;
        int32_t  near_vz;
    } glint;

    /* Zoe's zap: one ragged textured ribbon per live record of a third pool,
     * spawned by the level overlays' Zoe update. `hook` = 0 on a game with no
     * located routine.
     *
     * The pool is `slots` records of `stride` bytes: the head world XYZ at
     * `head_off`, the tail twelve bytes on and a packed word at `packed_off`
     * holding the segment count in byte 0. A record is dropped when the
     * head's camera-relative offset leaves the +/-`box` cube, and the whole
     * record again when its middle point's SZ3 reaches `far_sz`. The routine
     * projects its widened point chain into the scratchpad at `spad`, two
     * 8-byte entries per chain point. */
    struct {
        uint32_t hook;
        uint32_t pool;
        uint32_t slots;
        uint32_t stride;
        uint32_t head_off;
        uint32_t packed_off;
        int32_t  box;
        int32_t  far_sz;
        uint32_t spad;
    } zap;

    /* The two tracer producers: a point-ring ribbon over an array of records
     * and an indexed mesh over a descriptor the caller passes in a0. Either
     * `hook` is 0 on a game with no located routine.
     *
     * The ribbon array is `slots` records of `stride` bytes: a live pointer
     * at +0x00, a circular buffer of `ring_n` packed unsigned XYZ halves at
     * `points_off` (6 bytes per point) and a packed word at `packed_off`
     * holding the head index in byte 0 and the point count in byte 1. A
     * record is dropped whole when any of its points leaves the +/-`box`
     * cube, when every point shares a screen outcode bit, or when its first
     * point's SZ3 reaches `far_sz`.
     *
     * Both routines project into the scratchpad table at `spad`, 8 bytes per
     * vertex: the packed screen XY word and the MAC3-plus-outcode word. */
    struct {
        uint32_t ribbon_hook;
        uint32_t mesh_hook;
        uint32_t ring;
        uint32_t slots;
        uint32_t stride;
        uint32_t points_off;
        uint32_t packed_off;
        int      ring_n;
        int32_t  box;
        int32_t  far_sz;
        uint32_t spad;
    } tracers;

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
     * Form GAME_FONT_WAD_MODEL: every glyph is one geometry block of a
     * model the producer reads from the disc at first use, never from guest
     * RAM. The model is sub-file `wad_subfile` of entry `wad_entry` of
     * WAD.WAD: the WAD's first two sectors hold one (offset, size) word pair
     * per entry, and the entry's first sector one (offset, size) pair per
     * sub-file, offsets relative to the entry. The model's first word is the
     * negated block count, +0x10 the offset of a shared face pool and +0x14
     * one block offset per glyph index, every offset relative to the model.
     * A block holds the vertex count in header byte 1, one-word vertices at
     * +0x10 (y in bits 0-11, x in 11-21, z in 21-31) and, in the low half of
     * +0x08, the offset of its byte-counted face list inside the pool; a
     * face is 8 bytes flat-lit (four 7-bit indices in word 0 bits 21/14/7/0,
     * the normal in word 1 bits 15/7/23 as x, y, z) or 20 bytes textured
     * (word 1 negative). Model -Z faces the reader.
     *
     * A character maps to `digit_base` + (c - '0'), `letter_base` +
     * (c - 'A'), or the index `punct` pairs it with (the list ends at a
     * zero character), drawn `dy` model units below where the model puts
     * it. A font without a ':' entry draws the colon as two periods, each
     * scaled by `colon_scale`, moved by `colon_recenter` and then apart by
     * +-`colon_spread` (model units).
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
        int      wad_entry, wad_subfile;
        int      digit_base;
        int      letter_base;
        struct { char ch; int index; int dy; } punct[8];
        float    colon_recenter, colon_spread, colon_scale;
        float    model_h;
        float    light_add[3];
        float    hi_threshold, hi_gain;
        float    fixed_light[3];
        unsigned fixed_tint;
    } font;

    /* The credits letter roll (renderer/credits_key.h): `gamestate` is the
     * game-state word, which holds `gamestate_value` while the roll runs;
     * `stage` (0 = none) is a word the roll runs while it reads below
     * `stage_below`, and `draw_gate` (0 = none) a halfword the letters are
     * drawn while it is not -1. `recs` is the word holding the
     * CreditsLetter array's address, `count` the record count (a word when
     * `count_width` is 4, a signed halfword when 2), each record
     * `rec_stride` bytes with its state byte at `state_off`, at most
     * `rec_max` records. `drawn_states` is the bitmask of the states the
     * draw gives a moby (bit s for state s), 0 when it draws every record;
     * `flyin_state` is the state a new letter enters. Slot j of the letter
     * mobys is `slot_stride` bytes, at base + j * stride when `slot_dir` is
     * +1 and at base - (j + 1) * stride when it is -1, base the word at
     * `slot_base`. `letter_class` (0 = any) is the class the draw gives
     * every letter. A game with no roll leaves `slot_base` 0. */
    struct {
        uint32_t gamestate;
        uint32_t gamestate_value;
        uint32_t stage;
        int32_t  stage_below;
        uint32_t draw_gate;
        uint32_t recs;
        uint32_t count;
        uint32_t count_width;
        uint32_t rec_stride;
        uint32_t state_off;
        uint32_t rec_max;
        uint32_t drawn_states;
        uint32_t flyin_state;
        uint32_t slot_base;
        int      slot_dir;
        uint32_t slot_stride;
        uint32_t letter_class;
    } credits;

    /* The loading screen's treasure tally (renderer/credits_key.h): while
     * the builder state word `state` holds `state_value`, the overlay's draw
     * rewrites the moby array from the word at `slot_base` every tick,
     * `slot_stride` bytes a slot: at most `prefix_max` slots of another class
     * first, then one `gem_class` moby per tally record whose byte at
     * `active_off` is nonzero, in record order, while (unsigned)(word at
     * `timer` - `timer_lo`) < `timer_span`, then the HUD gems, `gem_class`
     * mobys told apart by the word at +0x0C (`hud_x`, one value per role)
     * with `hud_z` at +0x14. The records are `rec_count` entries of
     * `rec_stride` bytes at the address the word at `recs` holds. A game
     * with no tally leaves `state` 0. */
    struct {
        uint32_t state;
        uint32_t state_value;
        uint32_t timer;
        uint32_t timer_lo;
        uint32_t timer_span;
        uint32_t recs;
        uint32_t rec_stride;
        uint32_t active_off;
        uint32_t rec_count;
        uint32_t slot_base;
        uint32_t slot_stride;
        uint32_t prefix_max;
        uint32_t gem_class;
        int32_t  hud_x[2];
        int32_t  hud_z;
    } tally;
} game_layout_t;

#define GAME_FONT_NONE            0
#define GAME_FONT_CLASS_PER_GLYPH 1
#define GAME_FONT_WAD_MODEL       2

/* The running game's layout: openpete_game_runtime_register copies the
 * registered descriptor's layout here before any feeder runs, and
 * openpete_run seals it; read-only afterwards. */
OP_SEAL_DECLARE(game_layout);
extern game_layout_t game_layout OP_SEALED(game_layout); /* thread-roles: written before openpete_run seals it, read-only after */

/** @brief Return the default window width in window pixels: the 4:3 width of twice the DISPENV height.
 *
 * The DISPENV line is 512 VRAM pixels but not square, since the PSX
 * presents its `disp_h` rows at 4:3, so the width is disp_h * 2 * 4 / 3
 * and is independent of the clip height and of disp_w: 640 for every game
 * with disp_h 240. With game_default_window_h, Spyro 1 (clip 224 of 240
 * rows) gets 640x448 and Spyro 2 and Spyro 3 (clip 216 of 240) get
 * 640x432. The aspect correction between the full display and the drawn
 * clip is game_display_crop_factor(). */
static inline int game_default_window_w(void)
{
    return (game_layout.display.disp_h * 2 * 4) / 3;
}

/** @brief Return the default window height in window pixels: two window rows per drawn VRAM row (2 * clip_h).
 *
 * The visible clip then maps 1:1 at integer scale with no bars. */
static inline int game_default_window_h(void)
{
    return game_layout.display.clip_h * 2;
}

/** @brief Return disp_h / clip_h, the ratio of scanned-out display rows to drawn rows.
 *
 * A window sized for a requested display aspect is this much wider than
 * the aspect alone implies, because the cropped source covers only clip_h
 * of the disp_h rows that aspect describes. Returns 1.0 when clip_h is
 * not positive. */
static inline float game_display_crop_factor(void)
{
    return (game_layout.display.clip_h > 0)
        ? (float)game_layout.display.disp_h / (float)game_layout.display.clip_h
        : 1.0f;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_LAYOUT_H */
