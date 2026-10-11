// core/include/openpete_state.h - save-state public API.
//
// A state file carries the CPUState register file, 2 MiB main RAM, 1 KiB
// scratchpad, VRAM, the SPU voice mirror and sample RAM, the CD cursors,
// the per-game extra blob, the mod arena, the enabled-modset tag, and the
// VBL counters. DRAWENV/DISPENV are not carried. The layout is documented
// at the top of core/record/savestate.c.

#ifndef OPENPETE_STATE_H
#define OPENPETE_STATE_H

#include "openpete_psx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_STATE_NUM_SLOTS 10

/** @brief Write the running state to slot file slot<N>.state in the running game's states directory.
 *
 * Resolves the path and delegates to openpete_state_save_file.
 * @param slot Slot number, below OPENPETE_STATE_NUM_SLOTS.
 * @param cpu Guest CPU state whose register prefix is saved.
 * @param frame openpete_frame value stored in the header.
 * @return 0 on success; -1 on an out-of-range slot, a path failure or a
 *         write failure, with the reason logged. */
int openpete_state_save(unsigned slot, const CPUState* cpu, unsigned frame);

/** @brief Load slot file slot<N>.state into the running state.
 *
 * Resolves the path and delegates to openpete_state_load_file, which
 * refuses a file from another game or with a different identity, section
 * sizes or modset.
 * @param slot Slot number, below OPENPETE_STATE_NUM_SLOTS.
 * @param cpu Guest CPU state whose register prefix is overwritten.
 * @param out_frame Receives the saved openpete_frame, so the caller can
 *        re-anchor it; may be NULL.
 * @return 0 on success; -1 on refusal or failure, with the running state
 *         untouched and the reason logged. */
int openpete_state_load(unsigned slot, CPUState* cpu, unsigned* out_frame);

/** @brief Write the running state to a file.
 *
 * Writes the header (magic, version, content identity, the running game's
 * id, openpete_frame,
 * section sizes, the guest-visible VBL counter) and then every section.
 * The host mirrors (VRAM, SPU, game extra, mod arena) are copied to heap
 * buffers before the disk writes. Checks the mod arena's immutable rows
 * once per save. Used by the slot saves, the mod process handoff, and
 * --state-save-on-frame.
 * @param path Destination file, overwritten.
 * @param cpu Guest CPU state whose register prefix is saved.
 * @param frame openpete_frame value stored in the header.
 * @return 0 on success; -1 on an open, snapshot or write failure, with
 *         the reason logged. */
int openpete_state_save_file(const char* path, const CPUState* cpu,
                             unsigned frame);
/** @brief Load a state file into the running state.
 *
 * Refuses the load when the run follows the reference schedule, when the
 * header names another game, or names none and the running module carries
 * no ROM hash to prove the game through the identity, or when the magic,
 * version, content identity, a section size, the mod arena ledger or the
 * enabled-modset tag differs; every section is read before
 * any is applied, so a refusal leaves the running state untouched. On
 * commit it restores the register prefix, RAM (re-seeding the kernel's
 * fixed low-RAM words), scratchpad, VRAM, SPU, CD cursors and the VBL
 * counter, runs the game's post-load resync and extra-blob load, applies
 * the arena, and broadcasts the guest discontinuity last.
 * @param path Source file.
 * @param cpu Guest CPU state whose register prefix is overwritten.
 * @param out_frame Receives the saved openpete_frame; may be NULL.
 * @return 0 on success; -1 on refusal or failure, with the reason
 *         logged. */
int openpete_state_load_file(const char* path, CPUState* cpu,
                             unsigned* out_frame);

/* ── In-memory snapshot (rewind keyframes / runahead speculation) ──────
 * Same-process only: no header, no identity gating, no file I/O - pure
 * memcpy of the volatile state (CPU regs + RAM + scratch + SPU
 * deterministic mirror + CD cursors + frame/vbl counters). A process
 * runs one game, installed once before its entry, so a snapshot is always
 * the running game's and carries no game check.
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
 * load(quiet=1): speculative restore - deterministic mirrors only; no
 * discontinuity broadcast, no game/mod resync, no OpenAL perturbation.
 * The caller is re-running the SAME timeline (runahead rollback, stress).
 * load(quiet=0): user-visible restore - full resync semantics (interp
 * flush via the discontinuity broadcast, overlay re-sync, AL stops). */
typedef struct openpete_state_mem openpete_state_mem_t;

/** @brief Allocate an empty in-memory snapshot.
 *
 * Sizes the buffers for guest RAM, the SPU state, the CD transport, the
 * game-extra blob and VRAM; the mod arena copy is allocated by the first
 * save. The snapshot is not valid until a save succeeds.
 * @return The snapshot, or NULL when calloc or any buffer allocation fails
 *         (nothing is leaked then). */
openpete_state_mem_t* openpete_state_mem_new(void);

/** @brief Free an in-memory snapshot and its buffers.
 * @param m Snapshot from openpete_state_mem_new; NULL is ignored. */
void openpete_state_mem_free(openpete_state_mem_t* m);

/** @brief Capture the running state into an in-memory snapshot.
 *
 * Copies the register prefix, RAM, scratchpad, the SPU state, the CD
 * cursors and queue, the game-extra blob, VRAM, the used prefix of the mod
 * arena and the guest-visible VBL counter. The arena copy buffer only
 * grows. A failed SPU, VRAM or arena-growth step returns -1 partway: the
 * buffers already copied stay overwritten and the snapshot's valid flag
 * is left as it was, so a snapshot that was valid stays valid over a
 * mixed capture.
 * @param m Snapshot to overwrite; NULL returns -1.
 * @param cpu Guest CPU state whose register prefix is copied.
 * @param frame openpete_frame value recorded with the snapshot.
 * @return 0 on success; -1 on a NULL snapshot or a failed step. */
int  openpete_state_mem_save(openpete_state_mem_t* m, const CPUState* cpu,
                             unsigned frame);

/** @brief Restore the running state from an in-memory snapshot.
 *
 * Refuses, restoring nothing, when the snapshot is NULL or was never
 * saved, or when the run follows the reference schedule. Otherwise
 * restores the register prefix, RAM, scratchpad, SPU state, CD cursors
 * and queue, the game-extra blob, VRAM (by row delta), the mod arena and
 * the VBL counter, in that order. A failed SPU restore returns -1 after
 * the registers, RAM and scratchpad were already replaced. With @p quiet
 * clear it then runs the game's post-load resync and broadcasts the guest
 * discontinuity.
 * @param m Snapshot to restore from.
 * @param cpu Guest CPU state whose register prefix is overwritten.
 * @param out_frame Receives the snapshot's frame; may be NULL.
 * @param quiet Nonzero for a speculative rollback: no resync, no
 *        discontinuity broadcast, and the SPU's ring-out tails keep
 *        playing.
 * @return 0 on success; -1 on refusal or a failed SPU restore. */
int  openpete_state_mem_load(const openpete_state_mem_t* m, CPUState* cpu,
                             unsigned* out_frame, int quiet);

/** @brief Resolve the running game's states directory, creating it when missing.
 *
 * The directory is "states/<game id>", "states" lying beside the shared
 * memory-card "cards" directory: under the executable in a portable
 * install, else under the per-user state base
 * (openpete_paths_game_state_dir). No trailing separator.
 * @param out Receives the path.
 * @param out_size Capacity of @p out.
 * @return 0 on success, -1 when no base resolves or the path does not
 *         fit. */
int openpete_state_resolve_dir(char* out, unsigned long out_size);

/** @brief Report whether a slot's file exists as a regular file.
 *
 * A stat only; the magic and identity are not checked.
 * @param slot Slot number; an out-of-range slot reports 0.
 * @return 1 when the file exists, else 0. */
int openpete_state_slot_present(unsigned slot);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_STATE_H */
