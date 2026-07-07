// games/spyro-1/recomp_attach.h — attach the recomp module before boot.
//
// Two implementations, selected by OPENPETE_RECOMP_LINKAGE:
//   recomp_attach_static.c — registers the linked-in descriptor.
//   recomp_attach_shared.c — dlopen/LoadLibrary + descriptor lookup +
//                            forwarder resolution (distribution D1).
#ifndef SPYRO1_RECOMP_ATTACH_H
#define SPYRO1_RECOMP_ATTACH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Attach + register the recomp module. `module_path_override` is the
 * --recomp-module value or NULL for the default (shared linkage: the
 * platform library name next to the exe). `disc_path` feeds the
 * first-boot module build when no module exists yet (shared linkage
 * only; ignored by static builds). Returns 0 on success. */
int spyro1_recomp_attach(const char* module_path_override,
                         const char* disc_path,
                         int allow_build);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_RECOMP_ATTACH_H */
