/* games/common/renderer/moby_game_spyro23.c's interface: the moby stream
 * decoders, the cull-record reader and the second model kind, shared by the
 * two games whose moby draw is the same three handwritten routines at
 * different addresses.
 *
 * The avatar's geometry is encoded exactly as an animated moby's: the same
 * three-form vertex stream against the same 32-bit adjustment table
 * (spyrodraw.s:413-470 against mobydraw.s:1011-1086) and the same 0x14-byte
 * face record with four 8-bit vertex indices and four 7-bit colour indices
 * (spyrodraw.s:892-941 against mobydraw.s:1495-1566). A player provider
 * points its slots at these rather than carrying a second copy. */
#ifndef OPENPETE_COMMON_RENDERER_MOBY_GAME_SPYRO23_H
#define OPENPETE_COMMON_RENDERER_MOBY_GAME_SPYRO23_H

#include "renderer/moby_game.h"

/* What the two games do not share. Every engine address the decoders read
 * otherwise comes from game_layout; these four are tables the draw reaches
 * through a lui/addiu pair of its own, and the fifth field is the one
 * behavioural difference between the two draws. Each game's
 * renderer/moby_game.c defines the instance.
 *
 *   vertex_adjust_table  the 32-bit table the one-byte vertex form indexes
 *   anim_blend_table     the table the cull maps Moby+0x40 through into the
 *                        record's keyframe blend byte
 *   simple_skip_lowpoly  the far-geometry suppression word the second kind's
 *                        colour band consults
 *   simple_flat_coltab   the second kind's base-colour table, four words per
 *                        Moby+0x54 entry
 *   spec_light           the light vector the specular material projects the
 *                        rotated face normal onto
 *
 * mode0_ir0_from_colour_sum picks which of the two forms the special
 * material's mode 0 takes. 1: the draw zeroes the source colour and sets IR0
 * per vertex to the sum of the colour word's three bytes shifted left 3, so
 * every channel interpolates from 0 toward the far colour by that sum.
 * 0: the draw loads the colour word itself as the source and leaves IR0 at
 * the per-moby value the material setup stored, which is the same treatment
 * mode 1 gives its colour half. */
typedef struct moby_game_s23_consts {
    uint32_t vertex_adjust_table;
    uint32_t anim_blend_table;
    uint32_t simple_skip_lowpoly;
    uint32_t simple_flat_coltab;
    int      spec_light[3];
    int      mode0_ir0_from_colour_sum;
} moby_game_s23_consts_t;

extern const moby_game_s23_consts_t moby_game_s23;

void s23_vstream_next(CPUState* cpu, moby_vstream_t* s, int first);
/* The same stream step with the literal form's Y field named. The two draws
 * decode it differently: the moby draw reads bits 2-11 as a 12-bit value
 * (mobydraw.s:1076-1078) and the player draw reads bits 1-11 as an 11-bit one
 * (spyrodraw.s:498-499), so the player provider passes `y_lit_halved` 1. */
void s23_vstream_next_lit(CPUState* cpu, moby_vstream_t* s, int first, int y_lit_halved);
int  s23_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                     moby_wface_t* out, int cap, moby_face_stats_t* st);
/* The same decode with the record stride named. The moby draw sizes a record
 * by word 1's sign, two words untextured and five textured (mobydraw.s:1536);
 * the player draw advances a fixed 0x14 on both branches (spyrodraw.s:936-937,
 * 960-961), so its stream is walked with `fixed_stride` 0x14. `all_textured`
 * ignores the record sign when deciding whether to read the three texture
 * words: the player draw emits POLY_GT3 and POLY_GT4 for every face it keeps
 * and never a untextured code, so the sign there is the back-face exemption
 * (`untested`), not a texture flag. */
int  s23_decode_faces_stride(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                            moby_wface_t* out, int cap, moby_face_stats_t* st,
                            uint32_t fixed_stride, int all_textured);

#endif /* OPENPETE_COMMON_RENDERER_MOBY_GAME_SPYRO23_H */
