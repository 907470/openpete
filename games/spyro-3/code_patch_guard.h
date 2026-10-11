// games/spyro-3/code_patch_guard.h - the entry guard over the EXE functions
// a level overlay's anti-piracy check stubs out in guest RAM.
//
// A failed main scan in level 22 makes func_level_22_8008E278 store
// `jr ra; nop` (0x03E00008, 0x00000000) over the first two words of
// func_8005663C (the pause update); levels 31, 41 and 43 carry the same
// store over func_8005663C or func_800512E4. On hardware every later call of
// the stubbed function returns at once. The recompiled body does not read
// its own code, so the guard reads the two entry words from guest RAM at
// each entry of the two functions: the stub ends the call with every
// register as the caller left it, the EXE's words run the body, and any
// other pair is an unmodelled code store, logged through the anti-piracy
// FAIL path. The verdict lives in guest RAM, so savestates, rewind and
// level loads carry it.

#ifndef SPYRO3_CODE_PATCH_GUARD_H
#define SPYRO3_CODE_PATCH_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUState CPUState;

/** @brief Register the exit report of the guard's counters.
 *
 * Call once from the boot hook, so a run that never enters a guarded
 * function still prints its zero counts. */
void spyro3_code_patch_guard_boot(void);

/** @brief Decide at a function entry whether a stub stored over a guarded function's entry ends the call.
 *
 * Returns 0 at once for every address but func_8005663C and func_800512E4.
 * For those, the first two words in guest RAM decide: `jr ra; nop` returns
 * 1 (the body must not run); the EXE's own words return 0; any other pair
 * logs `antipiracy: FAIL patch:` once per distinct pair and returns 0. Guard
 * returns and body entries are counted per function on non-speculative ticks;
 * spyro3_code_patch_guard_boot prints them at exit.
 * @param cpu Guest CPU state at the entry.
 * @param addr The entered function's guest vaddr.
 * @return 1 when the recompiled body must return at once, else 0. */
int spyro3_code_patch_guard(CPUState* cpu, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
