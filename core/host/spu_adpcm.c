/* spu_adpcm - see spu_adpcm.h. */

#include "spu_adpcm.h"

#include <stdlib.h>

uint32_t spu_adpcm_decode(const uint8_t* body, uint32_t len, int16_t** out) {
    static const int k_f0[5] = {0, 60, 115, 98, 122};
    static const int k_f1[5] = {0, 0, -52, -55, -60};
    uint32_t max_frames = (len / 16u) * 28u;
    int16_t* pcm = (int16_t*)malloc(max_frames * 2u);
    if (!pcm) return 0;
    int s1 = 0, s2 = 0;
    uint32_t n = 0;
    for (uint32_t off = 0; off + 16u <= len; off += 16u) {
        int shift = body[off] & 0xF;
        int pred  = (body[off] >> 4) & 0xF;
        if (body[off + 1] & 0x1) break; /* loop end */
        if (pred > 4) pred = 0;
        for (int i = 0; i < 14; i++) {
            uint8_t b = body[off + 2u + (uint32_t)i];
            for (int hi = 0; hi < 2; hi++) {
                int nib = hi ? (b >> 4) : (b & 0xF);
                if (nib >= 8) nib -= 16;
                int s = ((nib << 12) >> shift) +
                        ((s1 * k_f0[pred] + s2 * k_f1[pred] + 32) >> 6);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                s2 = s1;
                s1 = s;
                pcm[n++] = (int16_t)s;
            }
        }
    }
    *out = pcm;
    return n;
}
