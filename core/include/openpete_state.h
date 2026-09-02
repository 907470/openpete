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

/* ── In-memory snapshot (rewind keyframes / runahead speculation) ──────
 * Same-process only: no header, no identity gating, no file I/O — pure
 * memcpy of the volatile state (CPU regs + RAM + scratch + SPU
 * deterministic mirror + CD cursors + frame/vbl counters).
 *
 * Carried: CPU regs, RAM, scratch, the SPU deterministic mirror, CD
 * cursors + queue, the game-extra blob, VRAM (row-delta restore) and the
 * mod arena's used prefix (re-anchored on restore: bytes allocated after
 * the snapshot keep their contents, the cursor never rewinds).
 *
 * Deliberately EXCLUDED (callers must gate accordingly):
 *   - SPU sample RAM: mutates only inside load windows; users run at
 *     gameplay steady-state (runahead is hard-gated OFF during loads).
 *   - the arena LEDGER and its verdicts (same process, same layout).
 *
 * load(quiet=1): speculative restore — deterministic mirrors only; no
 * discontinuity broadcast, no game/mod resync, no OpenAL perturbation.
 * The caller is re-running the SAME timeline (runahead rollback, stress).
 * load(quiet=0): user-visible restore — full resync semantics (interp
 * flush via the discontinuity broadcast, overlay re-sync, AL stops). */
typedef struct openpete_state_mem openpete_state_mem_t;
openpete_state_mem_t* openpete_state_mem_new(void);
void openpete_state_mem_free(openpete_state_mem_t* m);
int  openpete_state_mem_save(openpete_state_mem_t* m, const CPUState* cpu,
                             unsigned frame);
int  openpete_state_mem_load(const openpete_state_mem_t* m, CPUState* cpu,
                             unsigned* out_frame, int quiet);

/* Resolve the per-user state directory (created as needed). */
int openpete_state_resolve_dir(char* out, unsigned long out_size);

/* Whether slot N has a readable file on disk. Cheap stat-only check;
 * does not validate magic or build_hash. */
int openpete_state_slot_present(unsigned slot);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_STATE_H */
