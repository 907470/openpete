/* games/common/state_regions_none.h — the region table of a game whose
 * manifest directory carries no gameplay_state_regions.toml. Same symbols
 * as the generated gameplay_state_regions.gen.h, at count 0; the
 * registration TU passes the table as a pointer, so a NULL table and a
 * generated array both satisfy it. No oracle golden can verify such a
 * game: --gameplay-hash-log and --psxtrace-verify refuse at zero regions. */
#pragma once
#include "openpete_psx.h"

#define OPENPETE_STATE_REGION_COUNT 0
#define OPENPETE_STATE_REGIONS_TABLE_HASH 0ull
static const OpenpeteStateRegion* const openpete_state_regions_table = NULL;
