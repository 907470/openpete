// games/spyro-1/recomp_attach_static.c — static-linkage module attach.
//
// The module objects are linked into this exe (spyro1_recomp_module
// OBJECT library), so attaching is just registering the built-in
// descriptor. The shared-linkage build compiles recomp_attach_shared.c
// instead. Selected by OPENPETE_RECOMP_LINKAGE in CMakeLists.txt.
#include "recomp_attach.h"
#include "openpete_recomp_module.h"

#include <stddef.h>

extern const openpete_recomp_module spyro1_recomp_module; /* module_main.c */

/* Library module-dir keying is a shared-linkage concept; static builds
 * carry the module in the exe. */
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
    return openpete_recomp_register(&spyro1_recomp_module);
}
