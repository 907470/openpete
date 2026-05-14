/* spyro_interp_regions.c — Spyro 1's render-side interpolation region
 * table. Captured per-tick into the openpete_interp ring so render
 * frames at >29.97 Hz can lerp transforms and re-emit DrawOTag.
 *
 * Scope decisions (Phase 2 scaffolding):
 *   - g_Spyro covers Spyro's Moby header (pos / rotation matrix /
 *     anim state) and the Spyro-specific tail. Lerping starts here.
 *   - g_Camera covers projection / view / position / rotation. Camera
 *     interpolation is the single biggest perceived smoothness win.
 *   - Moby pools (g_LevelMobys / g_DynMobys / g_Sparx) are deferred
 *     to task #59 — they need indirect-array support in the interp
 *     module, plus a per-element transform-field schema.
 *   - Particles are deferred indefinitely (no host-readable list).
 *
 * Address verification: 2026-05-14 nm of games/spyro-1/spyro-1/build/psx.elf:
 *   80076dd0 B g_Camera                   (next = 0x80076ee0 → 0x110 bytes)
 *   80078a58 B g_Spyro                    (next = 0x80078d00 → 0x2A8 bytes;
 *                                           manifest uses 0x2A4 — Spyro struct
 *                                           size; bytes 0x2A4..0x2A7 belong
 *                                           to g_FairyCutscene tail padding)
 *
 * Lengths match manifest/gameplay_state_regions.toml so the same
 * boundaries that the determinism gate trusts are the boundaries we
 * snapshot.
 */

#include "openpete_interp.h"

static const openpete_interp_region_t s_spyro_interp_regions[] = {
    { "g_Spyro",  0x80078A58, 0x2A4 },
    { "g_Camera", 0x80076DD0, 0x110 },
};

const openpete_interp_region_t* openpete_interp_regions =
    s_spyro_interp_regions;
size_t openpete_interp_region_count =
    sizeof(s_spyro_interp_regions) / sizeof(s_spyro_interp_regions[0]);
