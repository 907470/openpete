/* core/config/build_id.c — the configure-time build identity, as accessors.
 *
 * The macros are PRIVATE compile definitions of openpete_core (core/
 * CMakeLists.txt), so only a core translation unit can read them. The game
 * targets need the same strings, and these two functions are the whole
 * interface they get. */

#include "openpete_build_id.h"

#ifndef OPENPETE_BUILD_VERSION
#define OPENPETE_BUILD_VERSION "unknown"
#endif
#ifndef OPENPETE_BUILD_TAGGED
#define OPENPETE_BUILD_TAGGED 0
#endif
#ifndef OPENPETE_BUILD_SHARED_RECOMP
#define OPENPETE_BUILD_SHARED_RECOMP 0
#endif

const char *openpete_build_version(void) { return OPENPETE_BUILD_VERSION; }

int openpete_build_tagged(void) { return OPENPETE_BUILD_TAGGED ? 1 : 0; }

int openpete_build_shared_recomp(void) { return OPENPETE_BUILD_SHARED_RECOMP ? 1 : 0; }
