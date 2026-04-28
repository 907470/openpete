/* games/spyro-1/decomp/native_thunks.c — outbound thunks for the
 * Phase 3 main() slot.
 *
 * Native `void main(void)` (compiled directly from spyro-1/src/main.c
 * via this CMake target) calls Initialize/GamestateUpdate/GamestateDraw
 * with C-style signatures. These callees are still recompiled MIPS,
 * exposed under `func_<addr>(CPUState*)`. The thunk bridges the gap by
 * stashing the active CPUState* on entry to native main and feeding it
 * into the recompiled body on each outbound call.
 *
 * One-fiber model: the dispatcher is single-threaded inside the libco
 * game fiber, so a plain static is sufficient. If a second native entry
 * point is added later, lift this to a TLS-per-fiber slot. */

#include "openpete_psx.h"

extern void func_800127C0(CPUState* cpu);  /* Initialize       */
extern void func_8003385C(CPUState* cpu);  /* GamestateUpdate  */
extern void func_8001ED5C(CPUState* cpu);  /* GamestateDraw    */
extern void psyq___main(CPUState* cpu);

static CPUState* s_active_cpu = (CPUState*)0;

void spyro_native_set_cpu(CPUState* cpu) { s_active_cpu = cpu; }
CPUState* spyro_native_get_cpu(void)     { return s_active_cpu; }

/* Outbound thunks — symbol names match the prototypes spyro-1/include/*.h
 * declares for the C-side, so spyro-1/src/main.c compiles unchanged. */
void Initialize(void)      { func_800127C0(s_active_cpu); }
void GamestateUpdate(void) { func_8003385C(s_active_cpu); }
void GamestateDraw(void)   { func_8001ED5C(s_active_cpu); }

/* PSYQ libgcc startup. spyro-1/src/main.c doesn't call __main directly —
 * it's emitted by GCC as the first instruction of `void main(void)`.
 * GCC 10 (host) only emits __main when targeting a freestanding ABI;
 * the hosted Linux ABI we're compiling under skips it. So the byte-level
 * RAM-diff invariant against pcsx-redux (write of 1 to 0x800730BC, see
 * psyq_thunks.cpp::psyq___main) needs an explicit kick from our hook
 * before native main runs. */
void spyro_native_run_psyq_main(void) { psyq___main(s_active_cpu); }
