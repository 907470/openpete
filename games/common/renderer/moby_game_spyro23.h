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
/** @brief Advances the vertex stream by one vertex, with the literal form's Y decode selectable.
 *
 * Consumes one static vertex word and one item of the form the previous item
 * named, updating VX/VY/VZ and the next form. The two draws decode the literal
 * form's Y field differently: the moby draw reads bits 2-11 as a 12-bit value
 * (mobydraw.s:1076-1078) and the player draw reads bits 1-11 as an 11-bit one
 * (spyrodraw.s:498-499), so the player provider passes `y_lit_halved` 1.
 * @param cpu The guest CPU state, read only.
 * @param s The stream state s23_vstream_init opened; advanced in place.
 * @param first Unused: the keyframe word already named the first item's form.
 * @param y_lit_halved 1 for the player draw's literal Y decode, 0 for the moby draw's. */
void s23_vstream_next_lit(CPUState* cpu, moby_vstream_t* s, int first, int y_lit_halved);
int  s23_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                     moby_wface_t* out, int cap, moby_face_stats_t* st);
/** @brief Decodes a face stream into walk faces, with the record stride and the textured rule selectable.
 *
 * Word 0 holds four 8-bit vertex indices, and the face is a triangle when the
 * last two match (mobydraw.s:1495-1506). Word 1 holds four 7-bit colour indices
 * at bits 24-30, 17-23, 10-16 and 3-9 (mobydraw.s:1554,1561-1566); on a
 * triangle the fourth field is flags instead: bit 3 accepts either winding
 * (mobydraw.s:1707,1731-1734), bits 4 and 5 select the special-material
 * dispatch (mobydraw.s:1710), and bits 0-2 are a signed per-face ordering bias
 * on every face. The three texture words are {u0|v0<<8|clut<<16},
 * {u1|v1<<8|tpage<<16} and {u2|v2<<8|u3<<16|v3<<24} (mobydraw.s:1655-1662), the
 * second word's sign the semi-transparency enable. A quad decodes as a "fresh"
 * face and a triangle as a "strip" face with bit 3 in `bit0`.
 *
 * The moby draw sizes a record by word 1's sign, two words untextured and five
 * textured (mobydraw.s:1536); the player draw advances a fixed 0x14 on both
 * branches (spyrodraw.s:936-937, 960-961), so its stream is walked with
 * `fixed_stride` 0x14. `all_textured` ignores the record sign when deciding
 * whether to read the three texture words: the player draw emits POLY_GT3 and
 * POLY_GT4 for every face it keeps and never a untextured code, so the sign
 * there is the back-face exemption (`untested`), not a texture flag.
 * OPENPETE_MOBY_FACEBITS=1 prints each stream's triangle counts per flag bit.
 * @param cpu The guest CPU state, read only.
 * @param fb The faceblock; the records start at fb + 4.
 * @param bytecount The stream's length in bytes after the leading word.
 * @param out Receives up to `cap` faces.
 * @param cap The capacity of `out`.
 * @param st Receives the raw record count and the alignment verdict: 0 when the
 *        last record ends exactly at the stream's end, 2 otherwise.
 * @param fixed_stride The record stride in bytes, or 0 to size each record by word 1's sign.
 * @param all_textured 1 to read the texture words of every record.
 * @return The number of faces decoded, at most `cap`. */
int  s23_decode_faces_stride(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                            moby_wface_t* out, int cap, moby_face_stats_t* st,
                            uint32_t fixed_stride, int all_textured);

#endif /* OPENPETE_COMMON_RENDERER_MOBY_GAME_SPYRO23_H */
