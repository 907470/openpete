// core/runtime/runahead.h - runahead's snapshot, rollback and
// prediction-check helpers and counters, shared by the host loop (run.c).
#pragma once

#include <stdbool.h>

#include "openpete_park_site.h"
#include "openpete_state.h"
#include "psxrt/psx_runtime.h"

#include <stdint.h>

/** @brief Read OPENPETE_RA_PREDICT_CHECK once: the runahead prediction-exactness check level.
 *
 * The check tests that with unchanged input the led state is the future
 * committed state bit for bit. A thunk that reads host state behaving
 * differently under speculation than in the real future (the wall clock, a
 * host static outside the snapshot) breaks that while every
 * state-roundtrip determinism gate stays green. After each speculation the
 * host loop hashes the led guest RAM, scratchpad and mod arena, keyed by
 * the led tick; when the committed timeline reaches that tick the hashes
 * must match. An input edge invalidates the pending predictions (the branch
 * legitimately changed), and so does a guest discontinuity (rewind and
 * state-load replays run under live input, not the predicted branch).
 * @return 0 off, 1 hash check, 2 hash check plus a full copy of the led RAM
 *         so a mismatch dumps the differing ranges. */
int ra_pc_on(void);

/** @brief Print the prediction check's totals on stderr when OPENPETE_RA_PREDICT_CHECK armed it.
 * @return true when the check is armed and the line was printed. */
bool runahead_predict_report(void);

/** @brief Take runahead's per-tick snapshot, timing the save for the snapshot cost the Mods panel shows.
 *
 * @param snap The runahead snapshot.
 * @param cpu The guest register file.
 * @param frame The committed frame the snapshot records.
 * @return openpete_state_mem_save's result, 0 on success. */
int ra_snapshot_save(openpete_state_mem_t* snap, const CPUState* cpu,
                            unsigned frame);

/** @brief Roll back to runahead's snapshot quietly, recording that tick's save plus restore time.
 *
 * @param snap The runahead snapshot.
 * @param cpu The guest register file, restored.
 * @param frame Receives the snapshot's frame.
 * @return openpete_state_mem_load's result, 0 on success. */
int ra_snapshot_rollback(const openpete_state_mem_t* snap, CPUState* cpu,
                                unsigned* frame);

/** @brief Settle the card writes of a speculation after its rollback attempt.
 *
 * A rollback that restored the state (0), or restored part of it before
 * failing (-2), discards the led ticks' card writes and save records; one
 * refused before restoring anything (-1, or 1 off the park site) leaves the
 * led state in place, so its card writes are adopted.
 * @param rc openpete_state_mem_load's result. */
void ra_card_settle(int rc);

/** @brief Drop every pending runahead prediction, counting each one dropped. */
void ra_pc_invalidate_all(void);

/** @brief Hash guest RAM, the scratchpad and the mod arena's three used ranges with FNV-1a-64.
 *
 * @return The hash the runahead prediction check compares. */
uint64_t ra_pc_hash(void);

/** @brief Record the led prediction in a free pending slot while RAM is the speculation head, keyed by the led tick.
 *
 * Nothing when the check is off or every slot is pending. At level 2 the
 * slot also keeps a copy of the led RAM and scratchpad for the mismatch
 * dump. */
void ra_pc_record_led(void);

/** @brief Carry a runahead snapshot owed to a later yield, counting the request once when it first waits.
 * @param owe Nonzero when this yield owes the snapshot (due, not at a final yield).
 * @param owed The value carried from the previous yield.
 * @return The value to carry to the next yield. */
int ra_note_owed(int owe, int owed);

/** @brief Continue a runahead speculation until the game fiber parks at a final yield at the snapshot's park site, so the rollback loads only there.
 *
 * The rollback restores a snapshot taken at a final yield and the fiber then
 * resumes wherever it is parked; below any other yield, or below a final
 * yield of another psyq_VSync call site, its host call stack is not the one
 * the snapshot continues from. The speculation loop ends at a final yield
 * when it breaks on the tick count, but that yield can be another site
 * than the snapshot's (a game with two frame-wait sites), and its yield
 * bound, its no-advance break (the no-tick yield after a stubbed dispatch)
 * and its break on leaving gameplay can leave the fiber at an intermediate
 * yield or at another site. Speculation then continues with the same held
 * input, the rollback counted as deferred (and as a park-site deferral when
 * the fiber was at a final yield), until the site or the bound. A
 * speculation that has left gameplay waits only to its first final yield:
 * a transition that waits at the snapshot's psyq_VSync site rolls back
 * there, and one that waits at another site has moved its frame wait, so
 * the speculation is adopted at once rather than after further held-input
 * VBLs. A site that does not recur within the bound leaves the speculation
 * adopted: no rollback can be applied at another site.
 * @param site The snapshot's park site.
 * @param cap Further yields allowed.
 * @param left In: nonzero when the speculation already left gameplay. Out:
 *        also set when the continuation leaves it.
 * @return 0 when parked at the site (the rollback may load), 1 when the
 *         speculation is adopted. */
int ra_speculate_to_final(openpete_park_site_t site, int cap, int* left);

/** @brief Log the end of a speculation that left gameplay or was adopted, count it, and commit an adopted one's card writes.
 * @param left Nonzero when the speculation left gameplay.
 * @param adopted Nonzero when it became the timeline (its park site did not
 *        recur within the bound): its card writes and save records are
 *        committed; zero when it was rolled back. */
void ra_end_log(int left, int adopted);

#define RA_PC_PENDING 8
/* Pending predictions, keyed by the led tick (ra_pc_record_led). */
extern struct ra_pc_slot { uint32_t target; uint64_t hash; int valid; uint8_t* led; } s_ra_pc[RA_PC_PENDING];
extern unsigned s_ra_pc_checked, s_ra_pc_mismatches, s_ra_pc_invalidated;
extern unsigned long long s_ra_deferred[2];
extern unsigned long long s_ra_adopted;
extern unsigned long long s_ra_left[2];
