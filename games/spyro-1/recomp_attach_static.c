// games/spyro-1/recomp_attach_static.c - static-linkage module attach.
//
// The module objects are linked into this exe (the <tag>_recomp_module
// OBJECT library), so attaching is just registering the built-in
// descriptor. The shared-linkage build compiles recomp_attach_shared.c
// instead. Selected by OPENPETE_RECOMP_LINKAGE in CMakeLists.txt.
#include "recomp_attach.h"
#include "openpete_recomp_module.h"

#include <stddef.h>

extern const openpete_recomp_module openpete_game_recomp_module; /* module_main.c */

/** @brief Reports that a static-linkage build has no codegen stamp.
 *
 * The stamp keys a disc library's module directory, which only a
 * shared-linkage build loads a module from; a static build carries the
 * module in the exe. The shared build's definition is in
 * dist/recomp_build.cpp.
 * @param out Unused.
 * @param cap Unused.
 * @return -1. */
int spyro1_recomp_codegen_stamp(char* out, size_t cap) {
    (void)out; (void)cap;
    return -1;
}

int spyro1_recomp_attach(const char* module_path_override,
                         const char* disc_path,
                         int allow_build) {
    (void)module_path_override; /* meaningful only for shared linkage */
    (void)disc_path;
    (void)allow_build;
    return openpete_recomp_register(&openpete_game_recomp_module);
}
