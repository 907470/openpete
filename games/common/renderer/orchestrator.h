/* orchestrator.h — the interface a game's sub-tick re-extract orchestrator
 * provides to the shared renderer translation units.
 *
 * A game that runs sub-tick re-extraction defines these; a game that does not
 * links games/common/renderer/orchestrator_none.c, whose definitions report
 * "no bracket active" and make each commit a no-op. A shared TU therefore
 * calls them unconditionally and falls back to canonical RAM on its own. */

#ifndef OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H
#define OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* The sub-tick interpolated position for `key` (a camera, player or moby
 * vaddr) at double precision, un-rounded: feeders use it for their
 * camera/object-relative deltas instead of re-reading the floored RAM, so the
 * projection tracks the smooth sub-tick path. Returns 1 and fills out[3] when
 * a bracket is active for `key` this present, 0 otherwise. */
int openpete_native_interp_pos(unsigned int key, double out[3]);

/* Latch the in-flight canonical-render epoch as fresh and start the next;
 * called once per canonical present. Producer-freshness queries compare a
 * stored stamp against the latched epoch. */
void spyro_native_canon_epoch_commit(void);

/* Present-time draw-env clear sync, called from the same anchor: re-read the
 * clear colour the environment's DrawOTag will use for the scene being
 * presented from PSX RAM, instead of a PutDrawEnv latch that trails the OT
 * build by one VSync. Defined by the shared cyclorama feeder
 * (games/common/renderer/cyclorama.c) for every game. */
void cyclorama_native_env_clear_commit(void);

/* Registration, called by main() before the run loop: the game's interp
 * region table into the interp ring (the game's interp_regions TU), and the
 * orchestrator's re-extract entry with the native renderer. Explicit calls
 * rather than weak symbols: the PE/COFF linker mis-resolves weak overrides. */
void game_interp_register(void);
void game_reextract_init(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H */
