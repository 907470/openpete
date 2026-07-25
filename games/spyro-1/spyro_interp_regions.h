#ifndef SPYRO_INTERP_REGIONS_H
#define SPYRO_INTERP_REGIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register Spyro's render-side interpolation region table with the
 * openpete_interp ring. Call once from main() before openpete_run(). */
void spyro_interp_register(void);

/* Stable interp-objs key for a moby address. The per-frame HUD moby pool
 * (g_HudMobys, bump-down allocated) is DOUBLE-BUFFERED with the polybuf
 * halves — the same logical moby alternates between two addresses every
 * tick, so an address key is evicted every other tick and never pairs.
 * HUD-pool mobys key as 0x40000000 + (addr - pool base) (slot-relative,
 * stable across halves); everything else keys by address. Text glyphs
 * take precedence over both: they key by emitter-call provenance
 * (features/text_moby_id.h), because a pool slot shifts whenever any
 * earlier allocation appears or disappears. Used by BOTH the capture
 * walk and the shaded-path consumers — they must agree. */
uint32_t spyro_interp_moby_key(uint32_t m);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_INTERP_REGIONS_H */
