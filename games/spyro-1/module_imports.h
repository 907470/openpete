// games/spyro-1/module_imports.h - the recompiled functions Spyro 1's host
// code calls by name.
//
// Native feature code calls these leaves directly (sub-tick re-extraction
// replays render functions). Under shared linkage each name is a forwarder
// defined by the game's generated build record (games/common/game_build.c.in),
// bound from the loaded module at attach; under static linkage the names resolve at link and this list is
// unused. Every entry is `void f(CPUState*)`. A host reference to a func_*
// missing here fails the shared build's exe link.
#ifndef SPYRO1_MODULE_IMPORTS_H
#define SPYRO1_MODULE_IMPORTS_H

#define OPENPETE_GAME_MODULE_FUNCS(X) \
    X(func_800168DC) X(func_800189F0) X(func_800190D4) X(func_8001C694) \
    X(func_8001ED5C) X(func_8001F798) X(func_80020F34) X(func_80022A2C) \
    X(func_80023AC4) X(func_800258F0) X(func_80034358) X(func_8003DFA4) \
    X(func_8004EBA8) X(func_8004F000) X(func_8004F4BC) X(func_8004FEA0) \
    X(func_80050240) X(func_80050BE0) X(func_800573C8) X(func_800580F4) \
    X(func_800584C4) X(func_80058864) X(func_80058D64) X(func_80059A48) \
    X(func_80059F8C)

#endif /* SPYRO1_MODULE_IMPORTS_H */
