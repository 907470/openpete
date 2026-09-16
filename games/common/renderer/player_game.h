/* renderer/player_game.h — the per-game half of the shared player feeder.
 *
 * The walk in renderer/player.c owns everything the games share: the hook and
 * its polybuf bracket, the per-call record store and its epoch pairing, the
 * skeleton iteration, the two-keyframe vertex blend, the projection, the
 * face-stream emit, the OT/depth policy and the sub-tick shape and rotation
 * interpolation. Every engine address it reads is a game_layout.player field.
 *
 * What it cannot share is the bit layout of the vertex stream, the shape of a
 * part's transform, the seam that joins one part to its parent, and the way a
 * game selects a model for the avatar, so those are function slots here. The
 * vertex, face and record slots take the moby feeder's own types
 * (renderer/moby_game.h): a game whose player uses its moby encodings points
 * the slot straight at its moby provider's function instead of transcribing it
 * a second time.
 *
 * A NULL function slot or a zero address makes the walk skip the path that
 * would read it; a game with no player feeder links
 * games/common/renderer/player_game_none.c. */
#ifndef OPENPETE_COMMON_RENDERER_PLAYER_GAME_H
#define OPENPETE_COMMON_RENDERER_PLAYER_GAME_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "renderer/moby_game.h"   /* moby_vstream_t, moby_wface_t, moby_face_stats_t */
#include "renderer/native_gte.h"  /* ngte_view */

#ifdef __cplusplus
extern "C" {
#endif

/* The most skeleton parts any player walk iterates. */
#define PLAYER_PARTS_MAX 4

/* One part's model->view transform: the 1.0 = 4096 rotation and the view-space
 * origin in full world units. `present` is 0 for a part the pose does not
 * carry, and the walk emits nothing for it. */
typedef struct player_xform {
    int    M[3][3];
    double TR[3];
    int    present;
} player_xform_t;

/* The face and colour streams one part's topology comes from. A game whose
 * whole skeleton shares one stitched mesh answers the same pair for every part
 * and the walk decodes it once. */
typedef struct player_faces {
    uint32_t faces;    /* the face block: a leading byte count, records after it */
    uint32_t colours;  /* the packed-RGB colour block the face indices reach into */
} player_faces_t;

typedef struct player_game {
    /* ---- the guest draw ---------------------------------------------- */
    /* Run the game's own player draw. The walk brackets the polybuf cursor
     * around this call and reads back what it left; it never replaces it. */
    void (*run_draw)(CPUState* cpu);

    /* ---- skeleton ----------------------------------------------------- */
    /* How many parts the pose's record block holds. 0 makes the walk read
     * game_layout.player.part_count instead, so a game with a fixed skeleton
     * needs no slot. */
    int (*part_count)(CPUState* cpu);

    /* Part `part`'s model->view transform, derived from gamestate rather than
     * read back from the record the draw built. `parent` is part 0's transform
     * (NULL for part 0 itself). Returns 0 when the part is absent this pose. */
    int (*part_xform)(CPUState* cpu, const ngte_view* v, int part,
                      const player_xform_t* parent, player_xform_t* out);

    /* The seam joint: part `part`'s view-space origin, the offset the parent's
     * keyframe stream carries for it, rotated and translated by the parent's
     * own transform. A game whose parts each carry their own absolute origin
     * leaves this NULL and answers the origin in part_xform. */
    void (*seam_joint)(CPUState* cpu, const player_xform_t* parent,
                       int part, int ir0, int dual, double out[3]);

    /* ---- streams ------------------------------------------------------ */
    /* Advance one vertex of an open stream; `first` opens it. The walk opens
     * the stream itself from the part record's cursor triple
     * (game_layout.player.part_stream_*), so only the step is per-game. Where
     * the player and the mobys share a codec this is the moby provider's own
     * vstream_next. */
    void (*vstream_next)(CPUState* cpu, moby_vstream_t* s, int first);

    /* How many vertices part `part` carries this pose. `flags` is the part
     * record's flags word and `cursor` a walk-owned running value a game whose
     * counts are cumulative across parts advances. */
    int (*part_vcount)(uint32_t flags, int* cursor);

    /* The part's face and colour streams. */
    int (*part_faces)(CPUState* cpu, int part, player_faces_t* out);

    /* Decode a face block, exactly as the moby slot of the same name. */
    int (*decode_faces)(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                        moby_wface_t* out, int cap, moby_face_stats_t* st);

    /* ---- policy ------------------------------------------------------- */
    /* 1 = the draw's strip-face test reads the face record's winding flag and
     * admits a flagged face at either winding. 0 = it reads no such flag, and
     * every untextured strip face is back-face culled on its NCLIP sign alone,
     * a zero-area face included. */
    int face_winding_flag;

    /* 1 = the draw admits a face whose record carries the `untested` bit
     * without the NCLIP test, emitting its full topology; only the faces
     * without that bit are back-face culled. 0 = every face takes the test. */
    int face_flag_skips_nclip;

    /* The draw's per-face depth admission, run after the NCLIP tree and before
     * the emit. `depth_sum` is the sum of the emitted topology's saturated
     * projected depths in the draw's own summing order (four terms: the quad's
     * four vertices, or a triangle's three with the middle one counted twice),
     * `root` is part 0's transform, whose view-space origin the threshold is
     * measured against, and `f` carries the face's own ordering bias. Return 0
     * for a face the draw drops. NULL = the game admits every face the NCLIP
     * tree kept. */
    int (*face_depth_keep)(CPUState* cpu, const player_xform_t* root,
                           const moby_wface_t* f, double depth_sum);

    /* The saturated sort-depth value that means "draw in front of everything"
     * rather than "pull toward the camera by this many ordering slots". The
     * walk flattens the whole model onto the near plane for exactly that
     * value; 0 = the game has no such sentinel and the walk only ever applies
     * the one-slot tie pull. */
    int sort_depth_front;

    /* ---- identity ----------------------------------------------------- */
    /* Which of several calls in one tick is the avatar the gameplay drives.
     * A game whose draw also renders a mirrored or duplicate copy answers 0
     * for the copies, and the walk keeps their records for pairing but feeds
     * nothing. NULL = every call is the avatar. */
    int (*call_is_player)(CPUState* cpu);

    /* ---- carve-outs ---------------------------------------------------- */
    /* A game-side numerical harness, run inside the hook after the guest draw
     * with the call's polybuf range. NULL where the game has none. */
    void (*verify)(CPUState* cpu, uint32_t startp, uint32_t endp);
    /* 1 while that harness wants the hook live even with no native scene. */
    int  (*verify_enabled)(void);
} player_game_t;

extern const player_game_t player_game;

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_PLAYER_GAME_H */
