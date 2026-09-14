/* spyro_state_regions.c — register Spyro 1's gameplay-readable
 * region table with openpete_core's --gameplay-hash-log path.
 *
 * The actual region list is generated from
 * manifest/gameplay_state_regions.toml by tools/gen_state_regions.py
 * into <build>/games/spyro-1/gen/gameplay_state_regions.gen.h. This
 * TU includes that header and registers the table explicitly from
 * spyro_state_regions_register() (called by game main — the former
 * weak-symbol override mis-resolved under the MinGW PE linker and
 * NULL-deref'd the Windows --psxtrace-verify path).
 *
 * See notes/plans/openpete-determinism-enforcement.md Phase 1 for
 * the architectural rationale: core/ stays game-agnostic (no Spyro
 * addresses live there), each per-game TU owns its manifest and its
 * registration TU.
 */

#include "openpete_psx.h"
#include "gameplay_state_regions.gen.h"

void spyro_state_regions_register(void) {
    openpete_state_regions_register(openpete_state_regions_table,
                                    OPENPETE_STATE_REGION_COUNT,
                                    OPENPETE_STATE_REGIONS_TABLE_HASH);
}
