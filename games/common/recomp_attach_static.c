// games/common/recomp_attach_static.c - static-linkage module attach.
//
// The module objects are linked into this exe (the <tag>_recomp_module
// OBJECT library), so attaching is registering the built-in descriptor. The
// shared-linkage build compiles recomp_attach_shared.c instead; both are
// selected by OPENPETE_RECOMP_LINKAGE in cmake/openpete_game.cmake.
#include "recomp_attach.h"
#include "openpete_recomp_module.h"

#include <stddef.h>

extern const openpete_recomp_module openpete_game_recomp_module; /* module_main.c */

/* Only a build with a first-boot pipeline loads a module from the disc
 * library; a static build carries the module in the exe. */
int openpete_game_library_module_path(const struct openpete_game_build* b, const char* entry_dir,
                                      char* out, size_t cap) {
    (void)b; (void)entry_dir; (void)out; (void)cap;
    return -1;
}

void openpete_recomp_announce_build(const char* why) { (void)why; }

int openpete_game_recomp_attach(const char* module_path_override,
                                const char* disc_path,
                                int allow_build) {
    (void)module_path_override; /* meaningful only for shared linkage */
    (void)disc_path;
    (void)allow_build;
    return openpete_recomp_register(&openpete_game_recomp_module);
}
