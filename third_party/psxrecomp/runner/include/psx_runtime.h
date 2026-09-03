#ifndef PSX_RUNTIME_H
#define PSX_RUNTIME_H

/**
 * @file psx_runtime.h
 * @brief Runtime interface between the recompiled game code and the engine.
 *
 * Mods use ::CPUState, the register file an override receives.
 *
 * Only `<stdint.h>` is included: the generated code uses fixed-width
 * integers alone, so the recompiled module builds against the compiler's
 * freestanding headers on a machine with no libc development package.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MIPS R3000A register file, GTE registers and memory callbacks.
 *
 * Overrides receive one of these: arguments in `a0..a3`, return value in
 * `v0`. The callbacks read and write guest memory at a guest vaddr.
 */
typedef struct CPUState {
    uint32_t zero, at, v0, v1, a0, a1, a2, a3;
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
    uint32_t t8, t9, k0, k1, gp, sp, fp, ra;
    uint32_t pc, hi, lo;
    uint32_t cop0[32];
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];

    uint32_t (*read_word)(uint32_t addr);
    void     (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void     (*write_half)(uint32_t addr, uint16_t value);
    uint8_t  (*read_byte)(uint32_t addr);
    void     (*write_byte)(uint32_t addr, uint8_t value);
    uint32_t (*lwl)(uint32_t addr, uint32_t rt_value);
    uint32_t (*lwr)(uint32_t addr, uint32_t rt_value);
    void     (*swl)(uint32_t addr, uint32_t rt_value);
    void     (*swr)(uint32_t addr, uint32_t rt_value);
} CPUState;

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_H */
