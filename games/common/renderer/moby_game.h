/* renderer/moby_game.h — the per-game half of the shared moby feeder.
 *
 * The walk in renderer/moby.c owns everything the games share: the cull-record
 * iteration, the model->view projection and its near fixup, the OT/depth
 * policy, the keyframe blend, the faceblock-patch apply/restore, the tri/quad
 * fan-out, the face-decode cache shell, the emit and the sub-tick shape and
 * rotation interpolation. Every engine address it reads is a game_layout.moby
 * field.
 *
 * What it cannot share is the bit layout of the two streams the models are
 * encoded in and the shape of one cull record, so those three decoders are
 * function slots here, plus a pair for a second model kind a game may carry
 * (a static geometry list drawn by its own pass, canonically and on a sub-tick
 * replay) and one for the object identity the sub-tick interp ring keys on. The remaining slots are per-game
 * data with no engine address behind it — the HUD/cutscene classifications and
 * the one-shot instance table — and the addresses of scratch windows and
 * material assets a game keeps outside the descriptor. A NULL function slot or
 * a zero address makes the walk skip the path that would read it; a game with
 * no moby feeder links games/common/renderer/moby_game_none.c. */
#ifndef OPENPETE_COMMON_RENDERER_MOBY_GAME_H
#define OPENPETE_COMMON_RENDERER_MOBY_GAME_H

#include <stdint.h>
#include <string.h>
#include "psx_runtime_internal.h"   /* CPUState: the decoders read guest memory */

#ifdef __cplusplus
extern "C" {
#endif

/* One model's vertex stream, mid-decode. The walk owns the loop and the
 * per-vertex projection; the game owns what one step of the stream means.
 *
 * VX/VY/VZ is the current model-space vertex. `s5` is the per-model shift the
 * header carries. `base`/`base_va`/`len` are a bounded host alias of the
 * stream's mapping, so a stream inside the aliased window costs no guest
 * access per vertex; `flag` and the four scratch words are the decoder's own
 * state (which form the next item takes, cursors into a second stream, a
 * per-model static vertex base). */
typedef struct moby_vstream {
    uint32_t       litp, shortp;
    const uint8_t* base;
    uint32_t       base_va, len;
    int            VX, VY, VZ, flag, s5;
    uint32_t       scratch_u[4];
    int            scratch_i[4];
} moby_vstream_t;

/* Bounded reads through a vstream's host alias: inside [base_va, base_va+len)
 * the load goes straight to host memory, anything else takes the guest path. */
static inline uint32_t moby_vs_read_word(CPUState* cpu, const moby_vstream_t* s, uint32_t va) {
    if (s->base && va >= s->base_va && va - s->base_va + 4u <= s->len) {
        uint32_t w; memcpy(&w, s->base + (va - s->base_va), 4); return w;
    }
    return cpu->read_word(va);
}
static inline uint32_t moby_vs_read_half(CPUState* cpu, const moby_vstream_t* s, uint32_t va) {
    if (s->base && va >= s->base_va && va - s->base_va + 2u <= s->len) {
        uint16_t h; memcpy(&h, s->base + (va - s->base_va), 2); return h;
    }
    return cpu->read_half(va);
}

/* One decoded face, the walk's unit of topology and material. i0..i3 are
 * stream vertex indices, c0..c3 byte offsets into the model's colour block;
 * `fresh` marks a face that carries its own three (or four) indices against a
 * strip face that advances one, `bit0` the winding flag the NCLIP test reads,
 * `hl` the per-face special-material flag, `blendbit` the GPU
 * semi-transparency enable, `abr` its blend mode and `otbias` the signed
 * per-face ordering-table bias a draw subtracts from the face's depth key.
 * `untested` is the face record's sign bit on its own: a draw that emits every
 * face textured still carries that bit, and reads it as the face's exemption
 * from the back-face test rather than as a texture flag. tpx/tpy/clx/cly/bpp and
 * u[]/v[] are the face's texture page, CLUT and texel corners.
 *
 * n1/n2/n3 is the signed face normal a special-material face carries in the
 * same word the colour indices occupy, for the games whose special treatment
 * reinterprets that word; 0 on every other face. */
typedef struct moby_wface {
    uint16_t i0, i1, i2, i3;
    uint16_t c0, c1, c2, c3;
    uint8_t  fresh, bit0, textured, hl;
    uint8_t  untested;
    uint8_t  blendbit, abr, bpp;
    int8_t   otbias;
    int8_t   n1, n2, n3;
    int16_t  tpx, tpy, clx, cly;
    uint8_t  u[4], v[4];
} moby_wface_t;

/* A texture page and CLUT one face draws from instead of its own, in the same
 * decoded form as the face's fields. `set` 0 leaves the face's texture alone;
 * a treatment that sets it also names the colour depth and blend mode of the
 * substitute page. */
typedef struct moby_face_tex {
    int16_t tpx, tpy, clx, cly;
    uint8_t bpp, abr, set;
    /* Texture coordinates the treatment generates for the face's four corners,
     * in place of the ones the face record carries. `uv_set` 0 keeps the
     * face's own. */
    uint8_t u[4], v[4], uv_set;
} moby_face_tex_t;

/* What a face-stream decode reports about the stream as a whole: the total
 * records it walked, how many it skipped without emitting, and whether the
 * cursor ended on the bytecount (0), on the generous end bound (1), or
 * somewhere else, which means the per-record advance desynced (2). */
typedef struct moby_face_stats {
    int raw, sprites, skips;
    int align;
} moby_face_stats_t;

/* The fields of one cull record the walk consumes. `tier` is the game's own
 * per-record LOD verdict (negative = the low-poly band); `material` is the
 * per-moby material word a game may carry there, 0 where it has none. */
typedef struct moby_record {
    uint32_t header;        /* AnimationHeader* */
    uint32_t header_b;      /* keyframe B's own AnimationHeader* (== header where
                             * the game blends within one animation) */
    uint32_t frame_a;       /* keyframe A */
    uint32_t frame_b;       /* keyframe B (0 = no blend) */
    uint32_t w0;            /* the record's flags word */
    uint32_t moby;          /* the Moby* the record was built from */
    uint32_t material;      /* per-moby material/colour word */
    int      TR_raw[3];     /* view-space translation, as stored */
    int      M[3][3];       /* model->view rotation, 1.0 = 4096 */
    int      tier;          /* LOD/colour-fade verdict */
} moby_record_t;

typedef struct moby_game {
    /* ---- stream decoders --------------------------------------------- */
    /* Open one keyframe's vertex stream. `header` is the AnimationHeader, so
     * a decoder whose stream is relative to a per-model static vertex array
     * can find it; `s5` is the per-model shift the walk already read. */
    void (*vstream_init)(CPUState* cpu, moby_vstream_t* s,
                         uint32_t header, uint32_t frame, int s5);
    /* Advance to the next vertex; `first` opens the stream (the first item is
     * always a literal). Leaves the vertex in s->VX/VY/VZ. */
    void (*vstream_next)(CPUState* cpu, moby_vstream_t* s, int first);
    /* Decode a whole faceblock into out[0..cap) and fill `st`. `bytecount` is
     * the block's leading length word, already read and range-checked.
     * Returns the emittable face count. */
    int  (*decode_faces)(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                         moby_wface_t* out, int cap, moby_face_stats_t* st);
    /* Read one cull record. Returns 0 when the record is not a live animated
     * record (the walk then stops or skips it). */
    int  (*read_record)(CPUState* cpu, uint32_t rec, moby_record_t* out);

    /* The faceblock patch list this record's keyframe carries, or 0 when it
     * carries none. Entries are ((count << 24) | byte offset into the model's
     * high-poly faceblock) followed by `count` replacement words, ending with
     * the entry whose header word is not positive; the walk applies them in
     * place around the decode and restores the originals inside the same hook
     * call. NULL where the game has no patch mechanism. */
    uint32_t (*frame_patch_list)(CPUState* cpu, uint32_t rec, uint32_t frame_a);

    /* The record's keyframe-blend byte (record word 0 bits 8-15, the numerator
     * the draw scales by 16) for a given value of the moby's animation-state
     * byte at anim_state_off + 4. A game whose cull stores that byte straight
     * through leaves this NULL; a game whose cull maps it through a table
     * answers with the mapped byte, so a pose rebuilt from a gamestate
     * snapshot blends exactly as the cull's own record does. */
    uint32_t (*anim_blend_byte)(CPUState* cpu, uint32_t anim_state_byte4);

    /* Which of the two model kinds a model-table entry is. The walk decodes
     * an animated model itself; a game that flags the second kind by the
     * entry's own sign, by a count field or any other rule answers here. */
    int  (*model_is_animated)(CPUState* cpu, uint32_t model);

    /* ---- second geometry kind ---------------------------------------- */
    /* A game whose moby set includes a second model kind drawn by its own
     * pass (a static geometry list) feeds it here, on that pass's own draw
     * hook (game_layout.moby.hook_draw_simple). The pass owns the guest
     * dispatch and the decode, and hands the walk view-space triangles
     * through moby_simple_record / moby_simple_tri; it returns the triangle
     * count. `simple_replay` re-runs the decode alone for a sub-tick present,
     * from the queue the canonical call captured, so the second kind survives
     * between ticks. NULL where the game has no such kind, and the walk feeds
     * the animated records alone. */
    int  (*simple_pass)(CPUState* cpu);
    int  (*simple_replay)(CPUState* cpu);

    /* ---- per-face special material -----------------------------------
     * A face the stream marks with a special-material flag (moby_wface_t.hl)
     * whose treatment the game selects from the record's material word, not
     * from the face. The hook rewrites the face's four vertex colours in place,
     * may clear `textured`, and may fill `tex` to draw the face from a
     * substitute texture page and CLUT; it returns 0 to leave the face alone,
     * or 1 + the treatment index it applied, which the walk counts. `material`
     * is the record's material word, `M` its model-to-view matrix, `moby` the
     * Moby the record was built from, `rec` the cull record's own address (0
     * for a record the walk built itself, which carries no guest record), and
     * `ev` the four corners' model-to-view rotated positions, which a
     * coordinate-generating treatment reads. NULL
     * where the game's special faces are handled by the walk's own branch. */
    int  (*special_face)(CPUState* cpu, uint32_t material, const int M[3][3],
                         uint32_t moby, uint32_t rec, const struct moby_wface* F,
                         uint32_t c[4], const float ev[4][3], int* textured,
                         moby_face_tex_t* tex);

    /* 1 = the walk may rebuild cull records from gamestate for the mobys the
     * canonical cull never queued (the widescreen and extended-LOD reveal).
     * The rebuild writes the record fields in this game's own encoding, so a
     * game whose encoding it does not implement leaves it 0 and feeds only
     * what the cull queued. */
    int  host_reveal;

    /* The canonical cull's own distance-box threshold for one moby, with
     * `drawn_bits` standing in for the moby's drawn-verdict byte so the caller
     * can substitute the reveal's own hysteresis for it. 0 or negative = the
     * box is degenerate and admits nothing, the dormant-slot sentinel.
     * Together with `reveal_record` this is what a game supplies instead of
     * `host_reveal` when its record encoding is its own: the shared candidate
     * walk moby_reveal_candidates() applies the cull's admission and the view
     * window, and the game appends the record. NULL leaves the walk feeding
     * exactly what the cull queued. */
    int  (*cull_box_threshold)(CPUState* cpu, uint32_t moby, unsigned drawn_bits);
    /* Append one record for a moby the cull's distance box rejected, built
     * from that moby's gamestate in the game's own encoding, to whatever queue
     * the game's pass decodes. Returns 1 when a record was appended, 0 when
     * the moby carries no record of that pass's kind or the queue is full.
     * Reads gamestate; writes no guest memory, so the cull's own verdict byte
     * and the canonical queue are untouched. */
    int  (*reveal_record)(CPUState* cpu, uint32_t moby);

    /* ---- identity ----------------------------------------------------- */
    /* The key the sub-tick interp ring stores a moby's prev/curr snapshots
     * under. Slot-stable across pool churn where the game reuses records. */
    uint32_t (*interp_key)(uint32_t moby);

    /* ---- per-game classifications (NULL = the path is skipped) -------- */
    /* A moby drawn by a cutscene renderer that owns the draw for the frames
     * it runs: its triangles go to the front pass, and the host-side reveal
     * adds nothing while that renderer owns the draw. */
    int  (*cutscene_actor)(CPUState* cpu, uint32_t moby);
    int  (*cutscene_owns_draw)(CPUState* cpu);
    /* A moby whose position the cutscene path drives, so a clip switch on it
     * must snap rather than crossfade. */
    int  (*cutpath_actor)(uint32_t moby);
    /* An instance whose first update does a one-shot job and retires it: the
     * walk emits nothing for it and the reveal never admits it. */
    int  (*run_once_hidden)(CPUState* cpu, uint32_t moby, unsigned cls);
    /* The game is in a state that draws the HUD under the player model. */
    int  (*hud_under_player)(CPUState* cpu);

    /* ---- addresses outside the descriptor (0 = the path is skipped) --- */
    /* The render scratch block the draw passes build their transient data in,
     * and the windows inside it the feeder reads back: the per-vertex
     * depth-cued colours the draw wrote, the second model kind's Moby* list
     * and its per-vertex lit colours. `spec_window_off`/`_len` is the
     * sub-range of the block a second material pass rebuilds, which the
     * sub-tick re-extract snapshots and replays. */
    uint32_t scratch;
    uint32_t colour_scratch;
    uint32_t shaded_list;
    uint32_t shaded_colour;
    uint32_t spec_window_off, spec_window_len;
    /* The far-geometry suppression flag the low-poly colour band consults. */
    uint32_t skip_lowpoly;
    /* The player's world position block, read by the reveal-sizing census. */
    uint32_t player_pos;
    /* The specular material block: the env-reflection matrix at +0x18, the
     * light direction at +0x0C, and the static Tiledef of the reflection
     * texture. 0 on a game with no specular material, and the walk's
     * per-face special-material branch never arms. */
    uint32_t spec_light;
    uint32_t metal_tiledef;
    /* Offsets into one Moby that only this game's materials and culls read:
     * the spawn-parameter pointer, the LOD/fade byte, the specular colour
     * word, the render radius byte, the update-distance byte and the
     * substate byte. */
    uint32_t props_off, renderer_off, spec_colour_off,
             render_radius_off, update_dist_off, substate_off;
    /* The camera's fine heading word, which the env reflection folds into its
     * azimuth so the reflection stays world-anchored under a pan. */
    uint32_t cam_heading;
    /* The second model kind's base-colour tables, flat and metal. */
    uint32_t shade_coltab, metal_coltab;
} moby_game_t;

extern const moby_game_t moby_game;

/* Read one word of a cull record. Where the game's cull is hooked the walk
 * holds a copy of the record list taken at that hook, and this resolves
 * against the copy: the draw pass that follows the cull reuses the scratch the
 * list sits in, so a record read back from memory afterwards may hold that
 * draw's output instead of the cull's fields. Addresses outside the copy, and
 * every game whose cull is not hooked, read guest memory. A record reader uses
 * this in place of cpu->read_word, and moby_rec_matrix in place of
 * ngte_read_matrix, for every field it takes out of a record. */
uint32_t moby_rec_word(CPUState* cpu, uint32_t addr);
void     moby_rec_matrix(CPUState* cpu, uint32_t addr, int M[3][3]);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_MOBY_GAME_H */
