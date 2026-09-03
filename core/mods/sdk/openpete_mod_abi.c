/* Defines the ABI descriptor every code mod exports (openpete_mod_api.h,
 * "ABI descriptor"). The engine appends this file to each mod's compile
 * line; a hand-built object compiles it in the same way. It is compiled
 * against the same header as the mod's sources, so the recorded version
 * and size are the mod's. */
#include "openpete_mod_api.h"

OPENPETE_MOD_ABI_DEFINE();
