/* games/spyro-3/renderer/moby_game.h — the two Spyro 3 moby decoders the
 * player feeder also uses.
 *
 * The avatar's geometry is encoded exactly as an animated moby's: the same
 * three-form vertex stream against the same 32-bit adjustment table
 * (spyrodraw.s:413-470 against mobydraw.s:1011-1086) and the same 0x14-byte
 * face record with four 8-bit vertex indices and four 7-bit colour indices
 * (spyrodraw.s:892-941 against mobydraw.s:1495-1566). The player provider
 * points its slots at these rather than carrying a second copy. */
#ifndef OPENPETE_SPYRO3_RENDERER_MOBY_GAME_H
#define OPENPETE_SPYRO3_RENDERER_MOBY_GAME_H

#include "renderer/moby_game.h"

void s3_vstream_next(CPUState* cpu, moby_vstream_t* s, int first);
/* The same stream step with the literal form's Y field named. The two draws
 * decode it differently: the moby draw reads bits 2-11 as a 12-bit value
 * (mobydraw.s:1076-1078) and the player draw reads bits 1-11 as an 11-bit one
 * (spyrodraw.s:498-499), so the player provider passes `y_lit_halved` 1. */
void s3_vstream_next_lit(CPUState* cpu, moby_vstream_t* s, int first, int y_lit_halved);
int  s3_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                     moby_wface_t* out, int cap, moby_face_stats_t* st);
/* The same decode with the record stride named. The moby draw sizes a record
 * by word 1's sign, two words untextured and five textured (mobydraw.s:1536);
 * the player draw advances a fixed 0x14 on both branches (spyrodraw.s:936-937,
 * 960-961), so its stream is walked with `fixed_stride` 0x14. `all_textured`
 * ignores the record sign when deciding whether to read the three texture
 * words: the player draw emits POLY_GT3 and POLY_GT4 for every face it keeps
 * and never a untextured code, so the sign there is the back-face exemption
 * (`untested`), not a texture flag. */
int  s3_decode_faces_stride(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                            moby_wface_t* out, int cap, moby_face_stats_t* st,
                            uint32_t fixed_stride, int all_textured);

#endif /* OPENPETE_SPYRO3_RENDERER_MOBY_GAME_H */
