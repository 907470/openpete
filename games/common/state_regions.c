/* games/common/state_regions.c — register this game's gameplay-readable
 * region table with the core's hash and .psxtrace paths.
 *
 * The table is generated from games/<game>/manifest/gameplay_state_regions.toml
 * by tools/gen_state_regions.py into <build>/games/<game>/gen/
 * gameplay_state_regions.gen.h (cmake/openpete_game.cmake); a game without
 * the manifest compiles this TU against state_regions_none.h (count 0).
 * Registration is an explicit call from game main before openpete_run(),
 * not a weak-symbol override: MinGW's PE linker mis-resolves weak overrides
 * in this configuration (count resolved strong, pointer weak-NULL). core/
 * stays game-agnostic: no game address lives there.
 */

#include "openpete_psx.h"
#ifdef OPENPETE_STATE_REGIONS_NONE
#include "state_regions_none.h"
#else
#include "gameplay_state_regions.gen.h"
#endif

void game_state_regions_register(void) {
    openpete_state_regions_register(openpete_state_regions_table,
                                    OPENPETE_STATE_REGION_COUNT,
                                    OPENPETE_STATE_REGIONS_TABLE_HASH);
}
