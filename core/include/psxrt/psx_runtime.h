#ifndef PSX_RUNTIME_H
#define PSX_RUNTIME_H

/**
 * @file psx_runtime.h
 * @brief The guest CPU state shared by the recompiled game, the engine and mods.
 *
 * The header defines ::CPUState and nothing else. It includes only
 * `<stdint.h>`, so the first-boot module build and a mod compile it with a
 * freestanding compiler and no C library headers.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The guest's R3000A general registers, coprocessor register banks and memory access callbacks.
 *
 * Each recompiled function is C that takes a pointer to this struct and
 * reads and writes its fields at the points where the original machine
 * code read and wrote the corresponding register. The process has one
 * instance, created and owned by the sim thread. An override is passed
 * that instance when the function it replaces is entered; the pointer may
 * be used until the override returns, must not be stored, and must not be
 * used from another thread.
 *
 * Arguments follow the MIPS o32 convention the game was compiled with:
 * the first four in `a0`, `a1`, `a2`, `a3`; the fifth and later ones as
 * words in the caller's stack frame at `sp + 16`, `sp + 20`, and so on,
 * read with `read_word`. A function returns its result in `v0`, and the
 * upper 32 bits of a 64-bit result in `v1`. On entry `ra` holds the
 * address the call returns to and `sp` still holds the caller's stack
 * pointer, because the callee's prologue has not run.
 *
 * A recompiled function saves and restores the same registers its
 * original machine code did: `s0` to `s7`, `sp`, `fp` and `gp` have their
 * entry values when it returns, and any other register may have changed.
 * An override that calls `api->base(cpu)` therefore reads its arguments
 * before that call and the result after it, and does not rely on `a0` to
 * `a3`, `t0` to `t9`, `v0`, `v1` or `ra` keeping their values across it.
 * The natural-signature wrappers preserve `a0` to `a3`, `v0`, `v1` and
 * `ra` across each call they make, and no other register. An override
 * that uses `api->call` and then falls through to the original function
 * puts back any argument register it changed.
 *
 * The callbacks map a guest virtual address in kuseg, kseg0 or kseg1 onto
 * the 2 MB of main RAM, the 1 KB scratchpad, or a registered region such
 * as the mod arena, and load or store at the stated width. A load from an
 * unmapped address returns 0; a store to one is discarded. The game's
 * accesses are naturally aligned. `api->guest` resolves an address
 * through the same map to a host pointer, for copying blocks.
 *
 * A savestate, a rewind snapshot and a runahead snapshot store every
 * field from `zero` to the end of `gte_ctrl` together with guest RAM. The
 * callback pointers are installed at boot, are not part of the guest's
 * state, and are not saved.
 */
typedef struct CPUState {
    uint32_t zero; /**< Register 0; always reads 0, and the recompiled code never writes it. */
    uint32_t at;   /**< Register 1, `at`; the game's code uses it as a scratch register for pseudo-instructions. */
    uint32_t v0;   /**< Register 2; holds a function's return value, or its low 32 bits for a 64-bit result. */
    uint32_t v1;   /**< Register 3; holds the upper 32 bits of a 64-bit result, otherwise scratch. */
    uint32_t a0;   /**< Register 4; the first argument of a call. */
    uint32_t a1;   /**< Register 5; the second argument of a call. */
    uint32_t a2;   /**< Register 6; the third argument of a call. */
    uint32_t a3;   /**< Register 7; the fourth argument of a call, with any further argument on the stack from `sp + 16`. */
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7; /**< Registers 8 to 15; temporaries a callee may change. */
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7; /**< Registers 16 to 23; a callee that writes one restores it before returning. */
    uint32_t t8, t9;  /**< Registers 24 and 25; temporaries a callee may change. */
    uint32_t k0, k1;  /**< Registers 26 and 27; reserved for an exception handler, which the recompiled game does not run. */
    uint32_t gp;      /**< Register 28; the global pointer through which the game's code addresses its small-data section; boot writes it from the executable header and the game's own code writes it. */
    uint32_t sp;      /**< Register 29; the stack pointer, equal to the caller's stack pointer when an override is entered. */
    uint32_t fp;      /**< Register 30 (`s8`); the frame pointer, restored by a callee that writes it. */
    uint32_t ra;      /**< Register 31; the return address, written by the calling instruction. */
    uint32_t pc;      /**< Program counter; boot writes the entry address, and the recompiled code writes it only in a build regenerated with `OPENPETE_REGEN_PC_TRACKING=1`. */
    uint32_t hi;      /**< Written by a multiply (upper 32 bits of the product) or a divide (the remainder). */
    uint32_t lo;      /**< Written by a multiply (lower 32 bits of the product) or a divide (the quotient). */
    uint32_t cop0[32];     /**< Coprocessor 0 registers; the recompiled `mtc0` and `mfc0` of the game's library code write and read them, and the engine does not act on their values. */
    uint32_t gte_data[32]; /**< GTE data registers 0 to 31 (`VXY0` to `LZCR`); written by the recompiled `mtc2` and `lwc2` and by every GTE command. */
    uint32_t gte_ctrl[32]; /**< GTE control registers 0 to 31 (rotation and light matrices, translation and colour vectors, screen offset and projection distance, `FLAG`); written by the recompiled `ctc2` and by every GTE command. */

    uint32_t (*read_word)(uint32_t addr);                  /**< Loads the 32-bit word at a 4-byte-aligned guest address. */
    void     (*write_word)(uint32_t addr, uint32_t value); /**< Stores a 32-bit word at a 4-byte-aligned guest address. */
    uint16_t (*read_half)(uint32_t addr);                  /**< Loads the 16-bit halfword at a 2-byte-aligned guest address. */
    void     (*write_half)(uint32_t addr, uint16_t value); /**< Stores a 16-bit halfword at a 2-byte-aligned guest address. */
    uint8_t  (*read_byte)(uint32_t addr);                  /**< Loads the byte at a guest address. */
    void     (*write_byte)(uint32_t addr, uint8_t value);  /**< Stores a byte at a guest address. */
    uint32_t (*lwl)(uint32_t addr, uint32_t rt_value);     /**< The `lwl` instruction: returns `rt_value` with its upper bytes replaced by the bytes from `addr` down to the start of its word. */
    uint32_t (*lwr)(uint32_t addr, uint32_t rt_value);     /**< The `lwr` instruction: returns `rt_value` with its lower bytes replaced by the bytes from `addr` up to the end of its word. */
    void     (*swl)(uint32_t addr, uint32_t rt_value);     /**< The `swl` instruction: stores the upper bytes of `rt_value` from `addr` down to the start of its word. */
    void     (*swr)(uint32_t addr, uint32_t rt_value);     /**< The `swr` instruction: stores the lower bytes of `rt_value` from `addr` up to the end of its word. */
} CPUState;

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_H */
