// games/spyro-3/module_imports.h - the recompiled functions Spyro 3's host
// code calls by name.
//
// Under shared linkage each name is a forwarder in
// the game's generated build record (games/common/game_build.c.in),
// bound from the loaded module at
// attach; under static linkage the names resolve at link and this list is
// unused. Every entry is `void f(CPUState*)`. A host reference to a func_*
// missing here fails the shared build's exe link.
#ifndef SPYRO3_MODULE_IMPORTS_H
#define SPYRO3_MODULE_IMPORTS_H

#define OPENPETE_GAME_MODULE_FUNCS(X) \
    X(func_80022378)

#endif /* SPYRO3_MODULE_IMPORTS_H */
