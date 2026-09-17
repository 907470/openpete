/* renderer/player.h — the shared player-avatar feeder's entries.
 *
 * The walk lives in games/common/renderer/player.c and reads every engine
 * address from game_layout.player; the per-game stream formats, the skeleton
 * transform and the game's own draw come through player_game
 * (renderer/player_game.h). A game reaches the feeder by listing player_module
 * in its feature list; a game whose layout names no draw hook registers
 * nothing and the walk never runs.
 *
 * The record store below replaces what used to be a singleton over the player
 * hooks. Every canonical draw-hook call appends a record — the state bytes the
 * extractor reads, the part-transform block, the pre-blend colour table, the
 * projected-SXY block and the polybuf range — to the epoch's list. The
 * canonical-epoch commit closes the list into a three-deep store (curr, prev,
 * prev2) with one clock per store, pairs each record with a record of the
 * previous list (index first, position-verified within the layout's pair
 * bound; a second pass by nearest position), and appends a pairing-only record
 * when no hook observed the ring's tick-end avatar. The sub-tick re-extract
 * runs the feeder once per record, staging that record's bytes and brackets.
 * With one call per tick the k=0 iteration is the old singleton path exactly.
 *
 * The auxiliary feeders (drop shadow, flame) append their own records bound to
 * the model record of the same epoch whose position equals the live one at the
 * hook, so a game that draws several avatars in one tick keeps each avatar's
 * shadow and flame with its own model record. */
#ifndef OPENPETE_COMMON_RENDERER_PLAYER_H
#define OPENPETE_COMMON_RENDERER_PLAYER_H

#include <stdint.h>
#include "openpete_time.h"   /* op_time_t */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

enum { PLAYER_REC_MODEL = 0, PLAYER_REC_SHADOW = 1, PLAYER_REC_FLAME = 2,
       PLAYER_REC_FEEDERS = 3 };

#define PLAYER_REC_MAX        16      /* per feeder per epoch */
#define PLAYER_REC_STATE_MAX  0x2E8u  /* the largest state span a record copies */
#define PLAYER_REC_PART_MAX   0x100u  /* the part-transform block */
#define PLAYER_REC_COL_MAX    0x300u  /* the pre-blend colour table */
#define PLAYER_REC_SXY_MAX    0x400u  /* the projected-SXY block */
#define PLAYER_REC_AUX_MAX    0xD0u   /* the largest auxiliary feeder's state */

typedef struct {
    uint8_t  spyro[PLAYER_REC_STATE_MAX];  /* the hook's state block: the staged base */
    uint8_t  src[PLAYER_REC_STATE_MAX];    /* the lerp source (position, rotation, clocks): the same
                                            * bytes, except under --runahead where the led list's
                                            * record goes here */
    uint8_t  seg[PLAYER_REC_PART_MAX];
    uint8_t  fk[PLAYER_REC_COL_MAX];
    uint8_t  sxy[PLAYER_REC_SXY_MAX];
    uint32_t rp_start, rp_end;   /* polybuf range of the call (0,0 = pairing-only) */
    uint8_t  drawn;              /* 1 = a hook call; 0 = pairing-only (never rendered) */
    uint8_t  is_player;          /* 1 = the call the gameplay drives; 0 = a duplicate copy */
    int8_t   pair;               /* index in the previous list, -1 unpaired */
    int8_t   pair2;              /* index in the list before that (via pair), -1 */
} player_model_rec_t;

typedef struct {
    uint8_t  sh[PLAYER_REC_AUX_MAX];   /* the shadow state at the hook */
    uint8_t  src[PLAYER_REC_AUX_MAX];  /* the anchor lerp source */
    int8_t   model;                    /* index of the model record it renders under */
    uint8_t  drawn;                    /* 1 = a hook call; 0 = pairing-only */
} player_shadow_rec_t;

typedef struct {
    uint8_t  fl[PLAYER_REC_AUX_MAX];   /* the flame state at the hook */
    uint8_t  src[PLAYER_REC_AUX_MAX];
    int8_t   model;
    uint8_t  drawn;
} player_flame_rec_t;

/* ---- orchestrator inputs (set per record, read by the re-extract) ------ */

/* The sub-tick alpha and whether keyframe-blend interpolation is active
 * (camera bracketed). */
void player_native_set_anim_interp(double alpha, int on);
/* The part-block pair the two-pass shape lerp blends: prev = the paired
 * previous record's block (NULL when unpaired), curr = this record's. */
void player_native_set_anim_pair(const uint8_t* prev_seg, const uint8_t* curr_seg);
/* The record index, which is the PLAYER tag slot of the triangles it emits. */
void player_native_set_player_slot(int k);
/* The front-layer flatten cap is shared by the calls of one tick or present:
 * a new key opens a new group. */
void player_native_set_flat_key(uint32_t key);
/* Per-part float byte-angles the orchestrator interpolated from the prev/curr
 * euler snapshots, so the composer renders rotation below the 8-bit euler grid.
 * `active[p]` gates each part; `ang` is a row-major parts x 3 of byte-angles
 * [part][Z, X, Y]. active = NULL resets (off), so the canonical inline render
 * stays byte-exact. */
void player_native_set_rot_interp(const int* active, const double* ang);

/* The mirrored-copy latch: set from player_game.call_is_mirror at each hook
 * call and cleared for the sub-tick replay, which reproduces the avatar the
 * gameplay drives. A provider's transform reads it to apply the draw's own
 * reflection. */
void player_native_set_mirror(int on);
int  player_mirror_pass(void);

/* The interpolated angles for part p, read by a provider's euler composer:
 * returns 0 when the canonical byte-exact table lookup is the one to use. */
int player_rot_interp(int part, double out[3]);

/* Sub-tick re-extract: reproduce the avatar from the current gamestate and the
 * interpolated camera the orchestrator wrote. Extract-only. */
void player_reextract(struct CPUState* cpu);
/* The same, for one record of the store: `mirror` is that record's own answer
 * to player_game.call_is_mirror, taken from the bytes the caller staged rather
 * than from live RAM, which the driver has already restored. */
void player_reextract_record(struct CPUState* cpu, int mirror);

/* ---- record store, canonical side (game fiber) ------------------------ */

/* The draw hook's own append. Returns the record's index (the tag slot) or -1
 * when refused (speculative fire, cap). Called by the shared hook; an
 * auxiliary feeder calls its own append below. */
int  player_records_model_append(struct CPUState* cpu, uint32_t rp_start, uint32_t rp_end);
int  player_records_shadow_append(struct CPUState* cpu);
int  player_records_flame_append(struct CPUState* cpu);
/* Once per canonical present, after the interp ring captured the tick. */
void player_records_epoch_commit(void);

/* ---- record store, present side (host loop) --------------------------- */

int  player_records_present_begin(int ring_paired, int ring_cut, double ring_alpha,
                                  double* out_alpha);
const player_model_rec_t* player_records_curr(int k);
const player_model_rec_t* player_records_prev(int k);
const player_model_rec_t* player_records_prev2(int k);
int  player_records_lerps(int k);
/* Whether the model record an auxiliary feeder just bound to was drawn by a
 * hook call this tick, as opposed to created for pairing alone. An auxiliary
 * feeder whose geometry belongs to the avatar takes this verdict so it feeds
 * exactly what the sub-tick replay will reproduce. */
int  player_records_model_drawn(int k);
int  player_records_shadow_count(void);
const player_shadow_rec_t* player_records_shadow_curr(int j);
const player_shadow_rec_t* player_records_shadow_prev(int j);
void player_records_shadow_note(int j, const int32_t* staged_anchor);
int  player_records_flame_count(void);
const player_flame_rec_t* player_records_flame_curr(int j);
const player_flame_rec_t* player_records_flame_prev(int j);
void player_records_flame_note(int j, const int32_t* staged_origin);
void player_records_present_note(int k, const int32_t* staged_pos);
void player_records_present_end(void);
/* The store's newest clock stamp (0 = none). */
op_time_t player_records_curr_ns(void);
/* 1 while OPENPETE_PLAYER_RECDBG is set (latched on first query). */
int  player_records_dbg(void);

/* ---- introspection, for a game's own numerical harness ----------------- */

/* Decode and project the whole skeleton from the live pose. Returns the
 * vertex count, which indexes player_proj_vert below. */
int  player_project_all(struct CPUState* cpu);
/* Build the face-stream walk over the current projection. Returns triangles. */
int  player_build_walk(struct CPUState* cpu);
/* One projected vertex: screen position, view-Z, whether the projection
 * saturated, and which part it came from. */
void player_proj_vert(int i, float* sx, float* sy, float* vz, int* sat, int* part);
/* One walk triangle's three stream vertex indices, per-vertex colour and UV. */
void player_walk_tri(int k, int idx[3], uint32_t rgb[3], int u[3], int v[3]);
/* The transform the provider derived for part p (NULL when absent). */
const struct player_xform* player_part_xform(int p);
/* The part-transform block's guest base this tick, and the block length. */
uint32_t player_part_block_base(struct CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_PLAYER_H */
