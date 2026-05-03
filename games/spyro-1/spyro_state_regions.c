/* spyro_state_regions.c — register Spyro 1's gameplay-readable
 * region table with openpete_core's --gameplay-hash-log path.
 *
 * The actual region list is generated from
 * manifest/gameplay_state_regions.toml by tools/gen_state_regions.py
 * into <build>/games/spyro-1/gen/gameplay_state_regions.gen.h. This
 * TU includes that header and exports the symbols core/runtime/run.c
 * declares as weak fallbacks (NULL / 0).
 *
 * See notes/plans/openpete-determinism-enforcement.md Phase 1 for
 * the architectural rationale: core/ stays game-agnostic (no Spyro
 * addresses live there), each per-game TU owns its manifest and its
 * registration TU.
 */

#include "openpete_psx.h"
#include "gameplay_state_regions.gen.h"

const OpenpeteStateRegion* openpete_state_regions = openpete_state_regions_table;
size_t                     openpete_state_region_count = OPENPETE_STATE_REGION_COUNT;
