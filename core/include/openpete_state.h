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
#include "openpete_park_site.h"

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
 * @return 0 on success; 1 when the fiber is not parked at the file's park
 *         site (nothing restored; retry at a later final yield); -1 on
 *         refusal or failure, with the running state untouched and the
 *         reason logged. */
int openpete_state_load(unsigned slot, CPUState* cpu, unsigned* out_frame);

/** @brief Write the running state to a file.
 *
 * Writes the header (magic, version, content identity, the running game's
 * id, openpete_frame,
 * section sizes, the guest-visible VBL counter, the park site when the
 * fiber is parked at a final yield) and then every section.
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
 * any is applied, so a refusal leaves the running state untouched. A
 * file whose park site is not the one the fiber is parked at (a final
 * yield of a psyq_VSync call with the same ra and sp) is not applied and
 * returns 1; a file with no park site (v4) applies at any final yield,
 * with a warning. On
 * commit it restores the register prefix, RAM (re-seeding the kernel's
 * fixed low-RAM words), scratchpad, VRAM, SPU, CD cursors and the VBL
 * counter, runs the game's post-load resync and extra-blob load, applies
 * the arena, and broadcasts the guest discontinuity last.
 * @param path Source file.
 * @param cpu Guest CPU state whose register prefix is overwritten.
 * @param out_frame Receives the saved openpete_frame; may be NULL.
 * @return 0 on success; 1 when the fiber is not parked at the file's park
 *         site (nothing restored); -1 on refusal or failure, with the
 *         reason logged. */
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
 * arena, the guest-visible VBL counter and the park site (none when the
 * fiber is not parked at a final yield). The arena copy buffer only
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
 * saved, or when the run follows the reference schedule; restores nothing
 * and returns 1 when the fiber is not parked at a final yield at the
 * snapshot's park site. Otherwise
 * restores the register prefix, RAM, scratchpad, SPU state, CD cursors
 * and queue, the game-extra blob, VRAM (by row delta), the mod arena and
 * the VBL counter, in that order. A failed SPU restore returns -2 after
 * the registers, RAM and scratchpad were already replaced. With @p quiet
 * clear it then runs the game's post-load resync and broadcasts the guest
 * discontinuity.
 * @param m Snapshot to restore from.
 * @param cpu Guest CPU state whose register prefix is overwritten.
 * @param out_frame Receives the snapshot's frame; may be NULL.
 * @param quiet Nonzero for a speculative rollback: no resync, no
 *        discontinuity broadcast, and the SPU's ring-out tails keep
 *        playing.
 * @return 0 on success; 1 when the fiber is not parked at the snapshot's
 *         site; -1 on refusal, restoring nothing; -2 on a failed SPU
 *         restore (a partial restore). */
int  openpete_state_mem_load(const openpete_state_mem_t* m, CPUState* cpu,
                             unsigned* out_frame, int quiet);

/** @brief Exclude the mod arena from an in-memory snapshot: save no longer copies it and load no longer restores it.
 *
 * For an owner that stores and restores the arena's used ranges itself
 * (the rewind keyframe ring's shared page tables); that owner restores
 * them after a successful openpete_state_mem_load. Frees any arena copy
 * the snapshot holds. Irreversible for the snapshot.
 * @param m Snapshot from openpete_state_mem_new; NULL is ignored. */
void openpete_state_mem_arena_external(openpete_state_mem_t* m);

/** @brief Return the park site an in-memory snapshot was saved at.
 * @param m A snapshot (valid or not).
 * @return The site; ra 0 when the save was not at a final yield. */
openpete_park_site_t openpete_state_mem_site(const openpete_state_mem_t* m);

/** @brief Seed the running game's states directory from the shared one, once per process and once per game.
 *
 * Slot files (slot<N>.state) in the shared "states" directory that belong
 * to the running game are copied into "states/<game id>/": a file whose
 * header names the running game, and a file naming no game when the
 * running module carries a ROM hash and the file's identity is the
 * module's (the one case the loader accepts it). A file naming another
 * game, naming no game under a module without a ROM hash, or not readable
 * as a state is left where it is and logged with the reason. A slot the
 * game's directory already holds is never overwritten. Each copy is
 * written to a temporary and renamed into place. The shared files are
 * only read. When every copy succeeds and every file was attributed, a
 * marker file ".seeded" is written in the game's directory, and a later
 * call finds it and copies nothing. A failed copy, or a file naming no game
 * under a module without a ROM hash, leaves no marker, so a later run
 * retries the slots still missing; the slots copied are not copied again,
 * since a slot the game's directory holds is skipped. With no slot file in
 * the shared directory nothing is created.
 * @return The number of files copied, 0 when nothing was due, -1 when a
 *         directory does not resolve or a copy or the marker write failed. */
int openpete_state_seed_game_dir(void);

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

/** @brief Report the active slot the quick-save and quick-load keys use.
 *
 * @return The slot, below OPENPETE_STATE_NUM_SLOTS. */
unsigned openpete_state_active_slot(void);

/** @brief Report whether the active slot holds a state file, as the host loop last checked.
 *
 * Checked on each slot change and set after a save to the active slot.
 * @return 1 when the slot's file exists, else 0. */
int openpete_state_active_slot_full(void);

/** @brief Make a slot the active one and announce it on the host loop's next iteration.
 *
 * Callable from any thread; an out-of-range slot is ignored.
 * @param slot Slot number, below OPENPETE_STATE_NUM_SLOTS. */
void openpete_state_set_active_slot(unsigned slot);

/** @brief Queue a save to, or a load from, the active slot, as the quick-save and quick-load keys do.
 *
 * Callable from any thread; the host loop queues the request on its next
 * iteration outside the settings menu.
 * @param load Nonzero for a load, zero for a save. */
void openpete_state_request(int load);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_STATE_H */
