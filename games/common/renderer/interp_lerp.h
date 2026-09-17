/* games/common/renderer/interp_lerp.h — the sub-tick interpolation primitives
 * the re-extract paths share.
 *
 * lerp16/lerp32 round half away from zero, so a value held constant across the
 * pair reproduces itself at every alpha. The two field helpers read the prev
 * and curr ring snapshots at `off` and write the interpolated result to the
 * live staged copy at that same offset, so a caller states an offset once. */
#ifndef OPENPETE_GAME_INTERP_LERP_H
#define OPENPETE_GAME_INTERP_LERP_H

#include <stdint.h>

static inline int16_t lerp16(int16_t a, int16_t b, double t) {
    double v = (double)a + ((double)b - (double)a) * t;
    return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}
static inline int32_t lerp32(int32_t a, int32_t b, double t) {
    double v = (double)a + ((double)b - (double)a) * t;
    return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/* A 3x3 int16 rotation matrix (9 contiguous shorts; the 10th short is pad and
 * is left as curr). Endpoint matrices one tick apart differ by a tiny rotation,
 * so a componentwise lerp with no renormalise is sub-pixel. */
static inline void lerp_matrix(uint8_t* live, const uint8_t* prev,
                               const uint8_t* curr, uint32_t off, double t) {
    const int16_t* pp = (const int16_t*)(prev + off);
    const int16_t* pc = (const int16_t*)(curr + off);
    int16_t*       pl = (int16_t*)(live + off);
    for (int i = 0; i < 9; ++i) pl[i] = lerp16(pp[i], pc[i], t);
}
/* An int32 x/y/z world position (3 contiguous words). */
static inline void lerp_vec3(uint8_t* live, const uint8_t* prev,
                             const uint8_t* curr, uint32_t off, double t) {
    const int32_t* pp = (const int32_t*)(prev + off);
    const int32_t* pc = (const int32_t*)(curr + off);
    int32_t*       pl = (int32_t*)(live + off);
    for (int i = 0; i < 3; ++i) pl[i] = lerp32(pp[i], pc[i], t);
}

#endif /* OPENPETE_GAME_INTERP_LERP_H */
