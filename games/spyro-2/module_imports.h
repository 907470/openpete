// games/spyro-2/module_imports.h - the recompiled functions Spyro 2's host
// code calls by name: none. Its host code reaches the module through the
// descriptor and the overlay dispatcher alone.
//
// Under shared linkage each listed name is a forwarder in
// games/common/recomp_attach_shared.c, bound from the loaded module at
// attach; every entry is `void f(CPUState*)`. A host reference to a func_*
// missing here fails the shared build's exe link.
#ifndef SPYRO2_MODULE_IMPORTS_H
#define SPYRO2_MODULE_IMPORTS_H

#define OPENPETE_GAME_MODULE_FUNCS(X)

#endif /* SPYRO2_MODULE_IMPORTS_H */
