/* text_moby_id.h — provenance identity for Spyro's HUD "text moby" glyphs.
 *
 * Every glyph of Spyro's 3D text (HUD counters, pause/inventory menus,
 * dialogue, cutscene captions) is a throwaway Moby bump-allocated out of
 * the per-tick HUD pool by the two shared emitters
 *   func_80017FE4(pText, pPosition, pSpaceWidth, pShadeIndex)
 *   func_800181AC(pText, pPosition, pTextSpacing, pSpaceWidth, pShadeIndex)
 * and the whole pool is discarded at the end of the tick. Nothing about a
 * glyph's storage survives: the same logical letter lands at a different
 * address every tick, so the sub-tick interpolator has to reconstruct which
 * of this tick's glyphs is "the same letter" as one from the previous tick.
 *
 * Pool position cannot answer that. A glyph's slot ordinal shifts by one
 * whenever ANY earlier pool allocation appears or disappears — and the
 * inventory page's blinking left/right page arrows do exactly that, twice a
 * second, renaming all ~100 glyphs behind them. The renamed glyphs then pair
 * with their neighbour's previous position and slide back and forth by one
 * character cell.
 *
 * The stable identity is the emitter call itself: a glyph is "the Nth
 * character of the Mth call made from callsite C". That tuple is a pure
 * function of which layout code ran, so it is blind to unrelated pool
 * traffic. This module records it by hooking the two emitters at entry
 * (read-only: it reads the caller's return address, the string and the pool
 * pointer, then lets the real function run) and computing the addresses the
 * emitter is about to bump-allocate.
 *
 * Consumed by game_interp_moby_key(); see interp_regions.c.
 */
#ifndef SPYRO_TEXT_MOBY_ID_H
#define SPYRO_TEXT_MOBY_ID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Look up the provenance key for a HUD-pool moby address. Returns 1 and
 * writes *out_key when `moby` is a glyph emitted this tick whose recorded
 * m_Class still matches what the emitter would have written (the guard that
 * keeps a stale record from claiming a recycled address); 0 otherwise. */
int text_moby_id_lookup(uint32_t moby, uint32_t* out_key);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_TEXT_MOBY_ID_H */
