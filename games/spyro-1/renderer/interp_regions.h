#ifndef SPYRO_RENDERER_INTERP_REGIONS_H
#define SPYRO_RENDERER_INTERP_REGIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Returns the interp-objs key that identifies a moby across ticks.
 *
 * The per-tick HUD moby pool (bump-allocated down from D_800756FC) is
 * double-buffered with the polybuf halves, so the same logical moby
 * alternates between two addresses, and it is rebuilt in draw order every
 * tick, so an entry's absolute position in it is no identity either. A
 * pool moby is keyed by the first identity the game keeps for it: the
 * CreditsLetter record index plus a reuse generation during the credits
 * roll; emitter-call provenance for a text glyph (renderer/text_moby_id.h);
 * the g_LevelTransGems source slot for a level-exit tally gem (disabled by
 * OPENPETE_GEM_FLYOUT_KEY=0); else its class and its ordinal among pool
 * entries of the same class, which is stable across the halves and blind to
 * other classes' traffic. A moby outside the pool keys by its address. The
 * capture walk and the shaded-path consumers both call it, so their keys
 * agree.
 * @param m The moby's guest address.
 * @return The key. */
uint32_t game_interp_moby_key(uint32_t m);

/** @brief Tabulate the HUD pool's same-class ordinals for the key lookups that follow.
 *
 * Called at the start of a pass that keys many pool entries against one pool
 * state (the tick-end capture, a re-extract); game_interp_moby_key then reads
 * an entry's ordinal from the table instead of rescanning the entries
 * allocated before it. Reads guest RAM only. */
void game_interp_hud_batch_begin(void);
/** @brief Close the batch game_interp_hud_batch_begin opened; later lookups scan. */
void game_interp_hud_batch_end(void);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_RENDERER_INTERP_REGIONS_H */
