#pragma once
/* guest_call.h - a host-initiated call into a guest function from a dispatch
 * hook's entry, shared by the features that ride a game's own routine
 * (Spyro 2's cutscene skip and title back-out, Spyro 3's transition queue).
 */

#include "openpete_psxtrace.h"     /* openpete_psxtrace_gate_host_enter */
#include "psx_runtime_internal.h"  /* CPUState */

#include <stdint.h>

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime.h */

/** @brief Call a guest function with its first n argument registers set, restoring ra.
 *
 * Sets a0..a(n-1) from @p a0, @p a1 and @p a2 and leaves the other argument
 * registers as they are, runs the function through call_by_address inside
 * the psxtrace host-call gate, then restores ra. The caller runs at a hook
 * entry where ra is the only register live across the nested call.
 * @param cpu The guest CPU state.
 * @param fn The guest function's address.
 * @param n Number of argument registers to set, 0 to 3.
 * @param a0 The first argument.
 * @param a1 The second argument.
 * @param a2 The third argument. */
static inline void game_guest_call(CPUState* cpu, uint32_t fn, int n, uint32_t a0,
                                   uint32_t a1, uint32_t a2) {
    const uint32_t ra = cpu->ra;
    if (n > 0) cpu->a0 = a0;
    if (n > 1) cpu->a1 = a1;
    if (n > 2) cpu->a2 = a2;
    openpete_psxtrace_gate_host_enter();
    call_by_address(cpu, fn);
    openpete_psxtrace_gate_host_leave();
    cpu->ra = ra;
}
