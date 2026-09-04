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
 * The recompiled game is C that reads and writes these fields where the
 * original instructions read and wrote registers; there is no
 * interpreter and no other copy of the state. One instance exists for
 * the session, owned by the sim thread. An override receives a pointer
 * to it at the entry of the function it overrides and the pointer is
 * valid until the override returns: keep no copy of it, and touch it
 * from no other thread.
 *
 * Calling convention (MIPS o32, as the game was compiled): arguments in
 * `a0..a3`, a fifth and later argument in the caller's frame at
 * `sp + 16`, `sp + 20`, ... at entry, read through `read_word`. The
 * return value is in `v0`, with `v1` carrying the high word of a 64-bit
 * return. `ra` is the return address, written by the caller before the
 * call, and `sp` at entry is the caller's stack pointer: the overridden
 * function has not yet built its frame.
 *
 * Preservation: the recompiled function saves and restores exactly the
 * registers its original code did, so at return `s0..s7`, `sp`, `fp` and
 * `gp` hold their entry values and every other register is clobbered.
 * An override that runs around `api->base(cpu)` sees this too: read the
 * arguments before base(), the result after it, and expect `a0..a3`,
 * `t0..t9`, `v0`, `v1` and `ra` to have changed across it. The
 * natural-signature wrappers save and restore `a0..a3`, `v0`, `v1` and
 * `ra` around the calls they make and nothing else. An override that
 * calls `api->call` itself and then returns into the original must
 * restore the argument registers it overwrote.
 *
 * `pc` is written by the recompiled code only in a build regenerated
 * with per-instruction PC tracking (`OPENPETE_REGEN_PC_TRACKING=1`) and
 * at indirect jumps; in a stock build it is stale and not a way to find
 * where the game is. `cop0` is carried for completeness: the game
 * executes no coprocessor-0 instruction. The GTE arrays are live: every
 * recompiled COP2 instruction executes against them.
 *
 * The memory callbacks translate a guest vaddr in kuseg, kseg0 or kseg1
 * to the 2 MB RAM, the 1 KB scratchpad or a registered region (the mod
 * arena included) and access it at the requested width; an unmapped
 * read returns 0 and an unmapped write is dropped. Addresses are
 * naturally aligned, as the game's are. `api->guest` resolves the same
 * map to a host pointer, so the two are interchangeable for bulk work.
 *
 * Savestates, rewind and runahead carry the register file, from `zero`
 * through `gte_ctrl`, with guest RAM; the callback pointers are bound
 * once at boot and are not timeline state.
 */
typedef struct CPUState {
    uint32_t zero; /**< Hard-wired 0. */
    uint32_t at;   /**< Assembler temporary. */
    uint32_t v0;   /**< Return value; the low word of a 64-bit return. */
    uint32_t v1;   /**< High word of a 64-bit return; otherwise scratch. */
    uint32_t a0;   /**< First argument. */
    uint32_t a1;   /**< Second argument. */
    uint32_t a2;   /**< Third argument. */
    uint32_t a3;   /**< Fourth argument; later ones are on the stack at `sp + 16` and up. */
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7; /**< Caller-saved temporaries. */
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7; /**< Callee-saved; a function returns them as it found them. */
    uint32_t t8, t9;  /**< Caller-saved temporaries. */
    uint32_t k0, k1;  /**< Kernel reserved; unused by the game. */
    uint32_t gp;      /**< Global pointer: base of the game's small-data area, constant across the session. */
    uint32_t sp;      /**< Stack pointer; the caller's frame at override entry. */
    uint32_t fp;      /**< Frame pointer (`s8`); callee-saved. */
    uint32_t ra;      /**< Return address of the current call. */
    uint32_t pc;      /**< Program counter; maintained only under PC-tracking regeneration. */
    uint32_t hi;      /**< Multiply/divide result, high word or remainder. */
    uint32_t lo;      /**< Multiply/divide result, low word or quotient. */
    uint32_t cop0[32];     /**< System coprocessor registers; the game never touches them. */
    uint32_t gte_data[32]; /**< GTE data registers (VXY0..LZCR), live. */
    uint32_t gte_ctrl[32]; /**< GTE control registers (rotation matrix, translation, screen offsets, flags), live. */

    uint32_t (*read_word)(uint32_t addr);                  /**< Aligned 32-bit load. */
    void     (*write_word)(uint32_t addr, uint32_t value); /**< Aligned 32-bit store. */
    uint16_t (*read_half)(uint32_t addr);                  /**< Aligned 16-bit load. */
    void     (*write_half)(uint32_t addr, uint16_t value); /**< Aligned 16-bit store. */
    uint8_t  (*read_byte)(uint32_t addr);                  /**< 8-bit load. */
    void     (*write_byte)(uint32_t addr, uint8_t value);  /**< 8-bit store. */
    uint32_t (*lwl)(uint32_t addr, uint32_t rt_value);     /**< Unaligned load, left part, merged into the `rt_value` word. */
    uint32_t (*lwr)(uint32_t addr, uint32_t rt_value);     /**< Unaligned load, right part, merged into the `rt_value` word. */
    void     (*swl)(uint32_t addr, uint32_t rt_value);     /**< Unaligned store, left part. */
    void     (*swr)(uint32_t addr, uint32_t rt_value);     /**< Unaligned store, right part. */
} CPUState;

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_H */
