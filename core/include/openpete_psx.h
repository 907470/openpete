// core/include/openpete_psx.h — slim PSX runtime public API.
//
// Phase 1b: replaces psxrecomp's bundled (Tomba-shaped) runner with a
// minimal layer. We keep psxrecomp's CPUState definition (so generated
// code stays compatible) but own boot/dispatch/memory/crash here.

#ifndef OPENPETE_PSX_H
#define OPENPETE_PSX_H

#include <stddef.h>
#include <stdint.h>

/* CPUState lives in psxrecomp's psx_runtime.h — vendored, untouched. */
#include "psx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Boot ---------------------------------------------------------------- */

/* Parse PSX-EXE header in `bytes`, copy .text/.data into g_ram, zero
 * memfill (BSS), and seed cpu->sp/gp/pc per the header.
 * Returns 0 on success, non-zero with a logged error on failure. */
int openpete_psx_boot(CPUState* cpu, const uint8_t* bytes, size_t len);

/* --- Memory map ---------------------------------------------------------- */

/* Install read_word/write_word/lwl/... callbacks on cpu, zero g_ram + scratch. */
void openpete_psx_runtime_init(CPUState* cpu);

/* 2MB main RAM + 1KB scratchpad accessors. */
uint8_t* openpete_psx_get_ram(void);
uint8_t* openpete_psx_get_scratch(void);

/* Copy `size` bytes into g_ram at virtual address `addr`. */
void openpete_psx_load(uint32_t addr, const uint8_t* data, uint32_t size);

/* --- Dispatch ------------------------------------------------------------ */

/* Defined in generated <stem>_dispatch.c — calls the recompiled function
 * registered for `addr`. Returns 1 if dispatched, 0 otherwise. */
int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);

/* --- Crash handler ------------------------------------------------------- */

void openpete_psx_install_crash_handler(void);

/* --- EXE loader (BIN/CUE → bytes) ---------------------------------------- */

/* Open a .cue/.bin disc image, find game_get_exe_filename() in the root,
 * and return its bytes. Caller frees with free(). On failure returns NULL
 * and logs the reason. */
uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                         const char* exe_filename,
                                         size_t* out_size);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PSX_H */
