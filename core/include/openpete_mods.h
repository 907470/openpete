// openpete_mods.h — ENGINE-facing mod framework API (loader + chains).
//
// The mod-facing surface lives in openpete_mod_api.h; this header is what
// the runtime and per-game code call. See
// notes/plans/openpete-modding-framework.md (P1 vertical slice).
//
// Interposition rides the recompiler's universal function-entry
// chokepoint: every recompiled function's prologue calls
// psx_override_dispatch(cpu, addr) (code_generator.cpp), which fans into
// psx_override_dispatch_game(), which calls openpete_mods_dispatch()
// below. Direct jal calls, indirect calls, and dispatched calls all pass
// through the callee's own prologue, so a per-address chain here
// intercepts every call to a chosen function with zero generated-C edits.
// Cost when no mods are loaded: one global-flag branch
// (openpete_mods_active == 0).

#ifndef OPENPETE_MODS_H
#define OPENPETE_MODS_H

#include <stddef.h>
#include <stdint.h>

#include "openpete.h" /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct CPUState CPUState;

/* Name→vaddr symbol table (generated from manifest/symbols.toml by
 * tools/gen_mod_sdk.py into sdk_symbols_table.c, game-side). The game's
 * main registers it before openpete_run(). Entries sorted by name. */
typedef struct {
    const char* name;
    uint32_t    addr;
} openpete_sdk_symbol_t;

void openpete_mods_set_symbol_table(const openpete_sdk_symbol_t* table,
                                    size_t count);

/* Discover, build, load, and initialize mods. Called once at boot after
 * options parsing (core/runtime/run.c). Resolution order for the mods
 * directory: opts->mods_dir → env OPENPETE_MODS_DIR → <exe>/mods.
 * A missing/empty directory is not an error: zero mods loaded,
 * openpete_mods_active stays 0, and the sim path is byte-identical to a
 * modless build (inertness gate #1/#2). Returns the number of mods
 * loaded (>= 0), or -1 on a hard setup error (which disables mods and
 * continues; mods never brick a launch). */
int openpete_mods_init(const openpete_options_t* opts);

/* Per-function-entry chain dispatch. Returns 1 = an override chain ran
 * (skip the recompiled body), 0 = no chain for addr (run the body).
 * Called from psx_override_dispatch_game behind the
 * openpete_mods_active fast gate. */
int openpete_mods_dispatch(CPUState* cpu, uint32_t addr);

/* 1 while at least one override chain is registered. Hot-path gate. */
extern int openpete_mods_active;

/* Crash attribution: non-NULL = an override chain frame is live on this
 * thread; returns the innermost mod id (addr_out = the overridden
 * vaddr). Consumed by core/runtime/crash.c so a crash report names the
 * offending mod (PoC rung 6). */
const char* openpete_mods_crash_attribution(uint32_t* addr_out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MODS_H */
