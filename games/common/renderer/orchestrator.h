/* orchestrator.h — the interface a game's sub-tick re-extract orchestrator
 * provides to the shared renderer translation units.
 *
 * A game that runs sub-tick re-extraction defines these; a game that does not
 * links games/common/renderer/orchestrator_none.c, whose definitions report
 * "no bracket active" and make each commit a no-op. A shared TU therefore
 * calls them unconditionally and falls back to canonical RAM on its own. */

#ifndef OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H
#define OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H

#include <stdint.h>

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

/** @brief Set the native clear colour from the DRAWENV the game will apply to the scene being presented.
 *
 * Called once per canonical present by render_paths_commit. The game flips
 * g_CurDB at the top of its draw pass, writes the clear into
 * g_CurDB->m_DrawEnv, builds the OT, then waits a VSync (during which the host
 * presents the fed scene) before PutDrawEnv and DrawOTag; a PutDrawEnv latch
 * therefore trails the presented scene by one VSync. Every gamestate writes
 * its clear into that struct before building the OT. This reads g_CurDB
 * (game_layout.db.cur_ptr) through the sub-tick view, returns without effect
 * when the layout names no g_CurDB or unless g_CurDB points at one of the two draw buffers, and passes the DRAWENV's
 * isbg (+0x18) and r0/g0/b0 (+0x19..+0x1B) to
 * openpete_native_set_clear_color. The PutDrawEnv latch stays in effect while
 * g_CurDB is not yet valid. Defined by the shared cyclorama feeder
 * (games/common/renderer/cyclorama.c) for every game. */
void cyclorama_native_env_clear_commit(void);

/* Canonical-render epoch. A feeder that snapshots transient render-pass
 * scratch stamps the epoch at capture time and replays the capture on the
 * sub-tick presents that follow the canonical present it was taken during.
 *   _stamp:  the value a producer hook stores at capture time.
 *   _fresh:  whether a stored stamp belongs to the last canonical present.
 * Per-producer freshness for the three canonical moby producers, which share
 * one render-path bit (a load or pause screen runs only the third). A game
 * with no sub-tick spine reports never-fresh, so every replay is skipped. */
uint32_t spyro_native_canon_epoch_stamp(void);
int      spyro_native_canon_epoch_fresh(uint32_t stamp);
int      spyro_native_canon_mobyreg_fresh(void);
int      spyro_native_canon_shaded_fresh(void);

/* Whether a canonical latch may be written at the producer hook running now.
 *
 * Runahead speculates 2N VBLs past the committed tick and rolls the guest
 * state back afterwards, and the producer hooks fire during those VBLs as
 * well. A capture taken then holds a speculative tick's render scratch,
 * addressed off that tick's primitive-buffer end, and stamps the epoch the
 * next canonical present latches; the replay would stage one tick's bytes
 * over another tick's address and decode them as geometry. The committed
 * tick's fire is the canonical capture, so every speculative fire is
 * skipped. */
extern int openpete_speculative;   /* openpete_fiber.h - runahead lookahead */
static inline int spyro_native_canon_capture_open(void) {
    return !openpete_speculative;
}

/* Canonical-scratch capture, called from a canonical render hook while the
 * transient render-pass scratch is still live.
 *   _range: a sub-range of the render scratch block (offset, length).
 *   _recs:  the whole cull-record region, captured before a later pass reuses
 *           the overlapping bytes. */
void spyro_native_canon_capture_range(void* cpu, unsigned off, unsigned len);
void spyro_native_canon_capture_recs(void* cpu);
/*   _sxy:   the block the player draw left every projected vertex's int16
 *           screen position in, which is also the walk's cull verdict. */
void spyro_native_canon_capture_sxy(void* cpu);

/* The camera a producer drew under. A feeder re-derives its geometry from the
 * camera, which the sub-tick bracket interpolates between tick-end snapshots;
 * a producer whose draw ran under matrices the tick did not end with must be
 * re-derived from its own camera.
 *   _capture_camera: at the producer hook, record the camera transform block.
 *   _prod_camera_enter/_leave: around that producer's feeder in the
 *   re-extract; enter installs the captured camera when it differs from the
 *   tick-end camera (returns 1), leave restores. */
enum { SPYRO_PROD_MOBYREG = 0, SPYRO_PROD_SPYRO = 1, SPYRO_PROD_SHADED = 2 };
void spyro_native_canon_capture_camera(void* cpu, int producer);
int  spyro_native_prod_camera_enter(int producer);
void spyro_native_prod_camera_leave(void);

/** @brief Registers the game's interp region table and per-object capture with the interp ring.
 *
 * Defined by the game's interp_regions TU, or as a no-op by
 * interp_regions_none.c for a game with no sub-tick presentation. main()
 * calls it once before openpete_run(), so openpete_interp_init() sees the
 * regions. An explicit call replaces a weak-symbol override, which the
 * PE/COFF linker mis-resolves. */
void game_interp_register(void);
/** @brief Registers the game's sub-tick re-extract body with the native renderer.
 *
 * The game's reextract TU passes its body to game_reextract_view_install;
 * orchestrator_none.c defines it as a no-op. main() calls it beside
 * game_interp_register, as an explicit call for the same reason. */
void game_reextract_init(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H */
