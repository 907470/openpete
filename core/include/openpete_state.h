// core/include/openpete_state.h — save-state public API.
//
// First-pass coverage: CPUState register file + 2 MiB main RAM + 1 KiB
// scratchpad + frame counter. Full coverage (VRAM, DRAWENV/DISPENV, SPU
// voice state) lands in a follow-up that depends on the PsyCross
// state-export API. See notes/plans/openpete-savestate-frame-step-rewind.md.

#ifndef OPENPETE_STATE_H
#define OPENPETE_STATE_H

#include "openpete_psx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_STATE_NUM_SLOTS 10

/* Persist g_game's CPUState register portion + g_ram + g_scratch + the
 * current openpete_frame to slot N. Returns 0 on success, non-zero on
 * I/O or path-resolution failure (errors are logged). */
int openpete_state_save(unsigned slot, const CPUState* cpu, unsigned frame);

/* Inverse of openpete_state_save. Refuses to load if the slot's
 * build_hash doesn't match this binary's. On success, *out_frame
 * receives the saved frame counter so the caller can re-anchor
 * openpete_frame. Returns 0 on success, non-zero otherwise. */
int openpete_state_load(unsigned slot, CPUState* cpu, unsigned* out_frame);

/* Path-based variants (the process-handoff checkpoint file; slots
 * delegate to these). Same format + build-hash gating as slots. */
int openpete_state_save_file(const char* path, const CPUState* cpu,
                             unsigned frame);
int openpete_state_load_file(const char* path, CPUState* cpu,
                             unsigned* out_frame);

/* Resolve the per-user state directory (created as needed). */
int openpete_state_resolve_dir(char* out, unsigned long out_size);

/* Whether slot N has a readable file on disk. Cheap stat-only check;
 * does not validate magic or build_hash. */
int openpete_state_slot_present(unsigned slot);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_STATE_H */
