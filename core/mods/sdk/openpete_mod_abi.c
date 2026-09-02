/* The ABI descriptor every code mod exports (see openpete_mod_api.h,
 * "ABI descriptor"). The engine appends this file to each mod's compile
 * line, so a mod's sources never have to mention it; a hand-built object
 * compiles it in the same way. Compiled against the SAME header as the
 * mod's own sources, which is what makes the recorded version and size
 * the mod's rather than the engine's. */
#include "openpete_mod_api.h"

OPENPETE_MOD_ABI_DEFINE();
