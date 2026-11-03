/* core/config/build_id.c - the configure-time build identity, as accessors.
 *
 * The macros are PRIVATE compile definitions of openpete_core (core/
 * CMakeLists.txt), so only a core translation unit can read them. The game
 * targets need the same strings, and these accessors are the whole
 * interface they get. */

#include "openpete_build_id.h"

#ifndef OPENPETE_BUILD_VERSION
#define OPENPETE_BUILD_VERSION "unknown"
#endif
#ifndef OPENPETE_BUILD_COMMIT
#define OPENPETE_BUILD_COMMIT "unknown"
#endif
#ifndef OPENPETE_BUILD_TAGGED
#define OPENPETE_BUILD_TAGGED 0
#endif
#ifndef OPENPETE_BUILD_DISTRIBUTED
#define OPENPETE_BUILD_DISTRIBUTED 0
#endif

const char *openpete_build_version(void) { return OPENPETE_BUILD_VERSION; }

const char *openpete_build_commit(void) { return OPENPETE_BUILD_COMMIT; }

int openpete_build_tagged(void) { return OPENPETE_BUILD_TAGGED ? 1 : 0; }

int openpete_build_distributed(void) { return OPENPETE_BUILD_DISTRIBUTED ? 1 : 0; }
