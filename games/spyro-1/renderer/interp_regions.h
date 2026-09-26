#ifndef SPYRO_RENDERER_INTERP_REGIONS_H
#define SPYRO_RENDERER_INTERP_REGIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Stable interp-objs key for a moby address. The per-tick HUD moby pool
 * (g_HudMobys, bump-down allocated) is DOUBLE-BUFFERED with the polybuf
 * halves — the same logical moby alternates between two addresses every
 * tick, so an address key is evicted every other tick and never pairs.
 * Nothing about an entry's POSITION in that pool is an identity either: it
 * is rebuilt in draw order every tick, so anything emitted earlier that
 * appears or disappears renames every entry behind it. So HUD-pool mobys
 * key by whatever the game itself keeps for them, most specific first —
 * emitter-call provenance for text glyphs (renderer/text_moby_id.h), the
 * g_LevelTransGems source slot for level-exit tally gems, the CreditsLetter
 * record index (plus a reuse generation) for the credits roll, else the
 * ordinal among pool entries of the SAME class (immune to unrelated traffic, stable
 * across the halves). Everything outside the pool keys by address. Used by
 * BOTH the capture walk and the shaded-path consumers — they must agree. */
uint32_t game_interp_moby_key(uint32_t m);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_RENDERER_INTERP_REGIONS_H */
