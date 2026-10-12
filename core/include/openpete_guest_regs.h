/* core/include/openpete_guest_regs.h - the register set an interrupt
 * delivery saves around the guest code it runs.
 *
 * A delivery that lands inside a guest loop (or a handler the host runs from
 * inside a call) must leave the interrupted code's registers as it found
 * them, as the console's exception handler does: every general-purpose
 * register, `pc`, `hi` and `lo`, the CPUState words from `zero` through `lo`.
 * The GTE banks are not saved; no guest handler touches them. */
#ifndef OPENPETE_GUEST_REGS_H
#define OPENPETE_GUEST_REGS_H

#include "psx_runtime.h"

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_GUEST_REGS_BYTES (offsetof(CPUState, lo) + sizeof(uint32_t))

/** @brief The saved words of CPUState from `zero` through `lo`. */
typedef struct openpete_guest_regs {
    uint32_t w[OPENPETE_GUEST_REGS_BYTES / sizeof(uint32_t)]; /**< `zero`..`lo` in CPUState order. */
} openpete_guest_regs_t;

/** @brief Copy the general-purpose registers, `pc`, `hi` and `lo` of cpu into s.
 * @param cpu Guest CPU state.
 * @param s Receives the registers. */
static inline void openpete_guest_regs_save(const CPUState* cpu, openpete_guest_regs_t* s) {
    memcpy(s->w, &cpu->zero, OPENPETE_GUEST_REGS_BYTES);
}

/** @brief Write the registers saved in s back into cpu.
 * @param cpu Guest CPU state.
 * @param s Registers saved by openpete_guest_regs_save. */
static inline void openpete_guest_regs_restore(CPUState* cpu, const openpete_guest_regs_t* s) {
    memcpy(&cpu->zero, s->w, OPENPETE_GUEST_REGS_BYTES);
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GUEST_REGS_H */
