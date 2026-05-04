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

/* --- PSX bus constants --------------------------------------------------- *
 * Plain PSX hardware values, named so core/ doesn't carry bare hex
 * literals that the purity grep would mistake for game-specific
 * addresses. KSEG0 = cached unmapped, KSEG1 = uncached unmapped, KUSEG
 * = user — all alias the same 2MB RAM in this emulator. */
#define OPENPETE_PSX_KSEG0_BASE     0x80000000u
#define OPENPETE_PSX_KSEG1_BASE     0xA0000000u
#define OPENPETE_PSX_SCRATCH_BASE   0x9F800000u
#define OPENPETE_PSX_RAM_SIZE       (2u * 1024u * 1024u)
#define OPENPETE_PSX_SCRATCH_SIZE   1024u
#define OPENPETE_PSX_KSEG0_END      (OPENPETE_PSX_KSEG0_BASE + OPENPETE_PSX_RAM_SIZE)
#define OPENPETE_PSX_KSEG1_END      (OPENPETE_PSX_KSEG1_BASE + OPENPETE_PSX_RAM_SIZE)

/* --- Memory map ---------------------------------------------------------- */

/* Install read_word/write_word/lwl/... callbacks on cpu, zero g_ram + scratch. */
void openpete_psx_runtime_init(CPUState* cpu);

/* 2MB main RAM + 1KB scratchpad accessors. */
uint8_t* openpete_psx_get_ram(void);
uint8_t* openpete_psx_get_scratch(void);

/* Gameplay-state regions for the determinism harness. The runtime's
 * --gameplay-hash-log path hashes the union of these regions per
 * yield. Per-game TUs (games/<game>/...) define the actual table; if
 * a game doesn't supply one, openpete_state_region_count == 0 and the
 * hash log is just the FNV1a-64 offset basis (effectively a no-op
 * sentinel). See manifest/gameplay_state_regions.toml +
 * tools/gen_state_regions.py for the canonical generation path, and
 * notes/plans/openpete-determinism-enforcement.md Phase 1. */
/* DIRECT regions live at a fixed PSX vaddr.
 * INDIRECT_ARRAY regions live behind a pointer at `start`: the runtime
 * reads a 4-byte PSX vaddr from RAM[start] and hashes `length` bytes
 * starting there. Used for moby pools whose backing storage moves with
 * level overlays. NULL/unmapped pointers are skipped (the slot just
 * isn't allocated this frame). */
typedef enum {
    OPENPETE_REGION_DIRECT         = 0,
    OPENPETE_REGION_INDIRECT_ARRAY = 1,
} OpenpeteStateRegionKind;

typedef struct {
    uint32_t    kind;        /* OpenpeteStateRegionKind */
    uint32_t    start;       /* PSX virtual address (KSEG0); for INDIRECT, the addr of the base pointer */
    uint32_t    length;      /* bytes to hash (for INDIRECT: element_size * max_elements) */
    const char* name;        /* for diagnostics */
} OpenpeteStateRegion;
extern const OpenpeteStateRegion* openpete_state_regions;
extern size_t                     openpete_state_region_count;

/* Map a PSX virtual address to a host pointer. Returns NULL if `addr` is
 * unmapped, OR if `addr == 0` (treated as PSX NULL). PSYQ thunks use this
 * to translate pointer arguments (held in cpu->aN as PSX vaddrs) to host
 * pointers PsyCross can dereference. */
void* openpete_psx_to_host_ptr(uint32_t addr);

/* Inverse: maps a host pointer back to its PSX KSEG0 vaddr. Returns 0
 * for NULL or any pointer outside g_ram/scratch (treated as PSX NULL).
 * Used by the source rewriter when native code stores a host pointer
 * into a PSX-aliased slot. */
uint32_t openpete_host_to_psx(const void* host_ptr);

/* Copy `size` bytes into g_ram at virtual address `addr`. */
void openpete_psx_load(uint32_t addr, const uint8_t* data, uint32_t size);

/* --- Dispatch ------------------------------------------------------------ */

/* Defined in generated <stem>_dispatch.c — calls the recompiled function
 * registered for `addr`. Returns 1 if dispatched, 0 otherwise. */
int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);

/* Call a PSX function by vaddr from native C/C++ code. Used by the source
 * rewriter for `(*FN_PTR_GLOBAL)(args...)` call sites in matching-decomp,
 * where FN_PTR_GLOBAL stores a PSX vaddr. Routes through call_by_address
 * — tries the native dispatch table first, falls back to compiled, then
 * to MIPS interpretation for overlay code. Saves/restores the caller's
 * cpu scratch slots. Pointer args must already be converted to PSX
 * vaddrs (use openpete_host_to_psx); int args pass through as uint32_t.
 * Up to 4 register args (a0..a3); 5+ args not yet supported. Returns
 * cpu->v0 — caller casts to int / pointer / etc. as appropriate. */
uint32_t openpete_psx_call_fnptr(uint32_t vaddr,
                                 uint32_t a0, uint32_t a1,
                                 uint32_t a2, uint32_t a3);

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
