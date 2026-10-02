// games/spyro-1/recomp_attach.h — attach the recomp module before boot.
//
// Two implementations, selected by OPENPETE_RECOMP_LINKAGE:
//   recomp_attach_static.c — registers the linked-in descriptor.
//   recomp_attach_shared.c — dlopen/LoadLibrary + descriptor lookup +
//                            forwarder resolution.
#ifndef SPYRO1_RECOMP_ATTACH_H
#define SPYRO1_RECOMP_ATTACH_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Attaches the recomp module and registers it as the active module.
 *
 * Static linkage registers the descriptor linked into the exe and ignores
 * every argument. Shared linkage loads the module library, binds its
 * descriptor and forwarders, and, when the library is absent or unusable
 * and a build is allowed, rebuilds it from the disc behind the splash
 * before binding again.
 * @param module_path_override The --recomp-module path, or NULL for the
 *     default (shared linkage: the platform library name next to the exe).
 * @param disc_path The disc a missing or unusable module is rebuilt from
 *     (shared linkage only).
 * @param allow_build Nonzero to permit that rebuild.
 * @return 0 on success, -1 when no usable module could be registered. */
int spyro1_recomp_attach(const char* module_path_override,
                         const char* disc_path,
                         int allow_build);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_RECOMP_ATTACH_H */
