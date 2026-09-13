// games/common/feature_list_none.c — the empty feature registry.
//
// A game with no feature modules runs the recompiled code and the PSYQ thunk
// surface only; core still links these two symbols unconditionally.

#include "feature_registry.h"

#include <stddef.h>

const feature_module_t* const openpete_features[] = { NULL };
const size_t openpete_feature_count = 0u;
