/* spyro_records.h — per-call records of the player render hooks.
 *
 * notes/plans/openpete-native-multi-spyro-subtick.md. The three player
 * feeders (model func_80023AC4, drop shadow func_80059A48, flame
 * func_80058D64) fire once per game call; a mod that draws extra dragons
 * calls them once per player with the player's state swapped into g_Spyro.
 * The sub-tick spine is a singleton on those calls today; this TU is the
 * record store that replaces the singleton (P1+).
 *
 * P0 (this revision): the OPENPETE_SPYRO_RECDBG instrument only. Every hook
 * reports each canonical call here; the canonical-epoch commit reports the
 * epoch. The instrument counts calls per feeder per epoch, epochs per VBL,
 * refused (speculative) observations, whether the DIRECT ring rolled and
 * whether the observed record set changed since the previous epoch, the
 * hook-vs-ring byte comparison per region, the per-tick displacement of each
 * index-matched call (the pairing bound B), and the closest two calls of an
 * epoch (the formation spacing). No render behaviour depends on it.
 */

#ifndef SPYRO_RECORDS_H
#define SPYRO_RECORDS_H

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

enum { SPYRO_REC_MODEL = 0, SPYRO_REC_SHADOW = 1, SPYRO_REC_FLAME = 2, SPYRO_REC_FEEDERS = 3 };

/* Called by each player hook after the game renderer ran and emitted prims
 * (endp > startp), on the fiber, while g_Spyro still holds the state the
 * renderer read. Speculative (runahead lookahead) fires are refused and
 * counted, never observed. */
void spyro_records_hook_observe(int feeder, struct CPUState* cpu);

/* Called once per canonical present from spyro_native_canon_epoch_commit,
 * after the DIRECT ring captured the tick (run.c: openpete_interp_capture
 * precedes the commit callback in the present block). Closes the epoch's
 * observation set. */
void spyro_records_epoch_commit(void);

/* 1 while OPENPETE_SPYRO_RECDBG is set (latched on first query). */
int spyro_records_dbg(void);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_RECORDS_H */
