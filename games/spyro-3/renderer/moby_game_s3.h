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
int  s3_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                     moby_wface_t* out, int cap, moby_face_stats_t* st);
/* The same decode with the record stride named. The moby draw sizes a record
 * by word 1's sign, two words untextured and five textured (mobydraw.s:1536);
 * the player draw advances a fixed 0x14 on both branches (spyrodraw.s:936-937,
 * 960-961), so its stream is walked with `fixed_stride` 0x14. */
int  s3_decode_faces_stride(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                            moby_wface_t* out, int cap, moby_face_stats_t* st,
                            uint32_t fixed_stride);

#endif /* OPENPETE_SPYRO3_RENDERER_MOBY_GAME_H */
