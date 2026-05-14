/* spyro_interp_regions.c — Spyro 1's render-side interpolation region
 * table. Captured per-tick into the openpete_interp ring so render
 * frames at >29.97 Hz can lerp transforms and re-emit DrawOTag.
 *
 * Registration model:
 *   This TU defines spyro_interp_register() which forwards the static
 *   table to openpete_interp_register_regions(). main.cpp calls it
 *   once at startup before openpete_run(). We avoid the weak-symbol
 *   override pattern used by spyro_state_regions.c because MinGW's
 *   PE/COFF linker has been observed to mis-resolve it, leaving the
 *   weak NULL in place at runtime and crashing the recomp boot path.
 *   Explicit registration works identically across toolchains.
 *
 * Scope (2026-05-14 architecture revision — see
 * notes/plans/openpete-renderer-foundation.md and
 * notes/plans/openpete-fps-unlock.md):
 *   - g_Camera ONLY. Captured for use by the renderer-foundation
 *     Phase 2g stretch (depth-aware async reprojection), which wants
 *     a single authoritative camera per tick rather than per-prim
 *     composite matrices.
 *   - g_Spyro / mobies / animation are NOT snapshotted here. Their
 *     lerp story lives in renderer-foundation Phases 0+1+2: the
 *     matcher reads per-vertex world-space + composite matrices
 *     directly from psxrecomp's gte_execute, pairs prims across
 *     frames by (texpage+clut+bbox+velocity), and decompose-lerp-
 *     recomposes the composite transform. That path doesn't read
 *     g_Spyro / g_LevelMobys from PSX RAM at all, so snapshotting
 *     them here is dead weight.
 *   - Particles are deferred indefinitely (no host-readable list;
 *     handled by the matcher if they're identifiable, otherwise they
 *     pop at 29.97 Hz).
 *
 * Address verification: 2026-05-14 nm of games/spyro-1/spyro-1/build/psx.elf:
 *   80076dd0 B g_Camera                   (next = 0x80076ee0 → 0x110 bytes)
 *
 * Lengths match manifest/gameplay_state_regions.toml so the same
 * boundaries the determinism gate trusts are the boundaries we
 * snapshot.
 */

#include "openpete_interp.h"
#include "spyro_interp_regions.h"

static const openpete_interp_region_t s_spyro_interp_regions[] = {
    { "g_Camera", 0x80076DD0, 0x110 },
};

void spyro_interp_register(void) {
    openpete_interp_register_regions(
        s_spyro_interp_regions,
        sizeof(s_spyro_interp_regions) / sizeof(s_spyro_interp_regions[0]));
}
