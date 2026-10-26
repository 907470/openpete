/* spu_adpcm - host-side decode of SPU-ADPCM sample bodies read from a disc
 * file (a demo's sound bank, a VAB body), for audio played through the
 * host voice path rather than keyed on the emulated SPU. */
#ifndef OPENPETE_SPU_ADPCM_H
#define OPENPETE_SPU_ADPCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Decode SPU-ADPCM lines to mono s16, stopping before the first line whose loop-end flag is set.
 *
 * Each 16-byte line carries a shift and filter nibble, a flag byte and 28
 * four-bit samples; a line whose flag bit 0 is set ends the decode before
 * it is decoded, and a filter index above 4 is treated as 0. Samples clamp
 * to the s16 range. Integer arithmetic throughout, so the output is
 * identical on every host.
 * @param body The ADPCM lines.
 * @param len The byte length of body; a trailing partial line is ignored.
 * @param out Receives the malloc'd sample buffer, which the caller frees;
 *        untouched when the allocation fails.
 * @return The sample count, or 0 when the allocation fails. */
uint32_t spu_adpcm_decode(const uint8_t* body, uint32_t len, int16_t** out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SPU_ADPCM_H */
