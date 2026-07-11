// games/spyro-1/recomp_attach_shared.c — shared-linkage module attach
// (distribution D1; plan openpete-distribution-local-recomp).
//
// The ROM-derived code lives in spyro1-recomp.{so,dll}, built from the
// same sources as the static OBJECT library. This TU is the host half
// of the boundary:
//
//   1. dlopen/LoadLibrary the module (default: platform library name
//      next to the exe; --recomp-module overrides).
//   2. Resolve + register the `spyro1_recomp_module` descriptor
//      (openpete_recomp_register refuses an ABI-version mismatch).
//   3. Resolve the host->module forwarders below.
//
// FORWARDERS: native feature code calls a small set of recompiled
// leaves directly (sub-tick re-extraction replays render functions —
// see the plan's D0-spike results). In the static build those names
// resolve at link; here each is a forwarder through a pointer filled
// at attach. The list is SELF-ENFORCING: a feature referencing a new
// func_* compiles in static mode but fails THIS build's exe link with
// an undefined symbol — add the symbol here (it is exported by the
// module automatically; nothing else to update).
#include "recomp_attach.h"
#include "openpete_recomp_module.h"
#include "openpete_log.h"
#include "splash.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define MODULE_DEFAULT_NAME "spyro1-recomp.dll"
static void* mod_open(const char* p) { return (void*)LoadLibraryA(p); }
static void* mod_sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
#else
#include <dlfcn.h>
#include <unistd.h>
#include <limits.h>
#define MODULE_DEFAULT_NAME "spyro1-recomp.so"
static void* mod_open(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* mod_sym(void* h, const char* n) { return dlsym(h, n); }
#endif

typedef struct CPUState CPUState;

/* Host->module forwarder surface (uniform `void f(CPUState*)`). */
#define SPYRO1_MODULE_FUNCS(X) \
    X(func_800168DC) X(func_800189F0) X(func_800190D4) X(func_8001C694) \
    X(func_8001ED5C) X(func_8001F798) X(func_80020F34) X(func_80022A2C) \
    X(func_80023AC4) X(func_800258F0) X(func_8004EBA8) X(func_8004F000) \
    X(func_8004F4BC) X(func_8004FEA0) X(func_80050240) X(func_80050BE0) \
    X(func_800573C8) X(func_800580F4) X(func_800584C4) X(func_80058864) \
    X(func_80058D64) X(func_80059A48) X(func_80059F8C)

#define DECL_PTR(n) static void (*p_##n)(CPUState*);
SPYRO1_MODULE_FUNCS(DECL_PTR)
#undef DECL_PTR

static void (*p_set_overlay)(uint32_t);

#define DEF_FWD(n) \
    void n(CPUState* cpu) { p_##n(cpu); }
SPYRO1_MODULE_FUNCS(DEF_FWD)
#undef DEF_FWD

void spyro_set_active_overlay_by_magic(uint32_t magic) { p_set_overlay(magic); }

static void* g_module;
static int s_missing;

static void* need(const char* name) {
    void* s = mod_sym(g_module, name);
    if (!s) {
        OPL_ERROR_F("recomp", "module is missing symbol %s", name);
        s_missing++;
    }
    return s;
}

/* Default: MODULE_DEFAULT_NAME next to the executable. LoadLibraryA
 * already searches the application directory; ELF dlopen does not, so
 * build the path from /proc/self/exe. */
static void default_module_path(char* buf, size_t n) {
#ifdef _WIN32
    snprintf(buf, n, "%s", MODULE_DEFAULT_NAME);
#else
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        char* slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(buf, n, "%s/%s", exe, MODULE_DEFAULT_NAME);
            return;
        }
    }
    snprintf(buf, n, "%s", MODULE_DEFAULT_NAME);
#endif
}

extern int spyro1_recomp_build_module(const char* disc_path,
                                      const char* out_module_path);

/* First-boot build as a splash worker task: the extract → recomp →
 * compile → link pipeline must not run on the UI thread (a window drag
 * would freeze it — see spyro1_splash_run_task). */
struct build_task { const char* disc; const char* out; };
static int build_task_fn(void* p) {
    struct build_task* t = (struct build_task*)p;
    return spyro1_recomp_build_module(t->disc, t->out);
}

int spyro1_recomp_attach(const char* module_path_override,
                         const char* disc_path,
                         int allow_build) {
    char pathbuf[4096];
    const char* path = module_path_override;
    if (!path) {
        default_module_path(pathbuf, sizeof pathbuf);
        path = pathbuf;
    }
    g_module = mod_open(path);
    if (!g_module && allow_build && disc_path) {
        /* First boot: no module next to the exe — build one from the
         * disc (extract -> recomp -> compile -> link; D2.4). */
        fprintf(stderr,
                "openpete: no recomp module at %s — building it from the "
                "disc (first boot)\n", path);
        struct build_task bt = { disc_path, path };
        if (spyro1_splash_run_task(build_task_fn, &bt) == 0)
            g_module = mod_open(path);
    }
    if (!g_module) {
        fprintf(stderr,
                "openpete: failed to load recomp module '%s'\n"
                "  (shared-linkage build: the module is built separately as "
                "spyro1-recomp; pass --recomp-module PATH to override)\n",
                path);
        return -1;
    }
    const openpete_recomp_module* desc =
        (const openpete_recomp_module*)mod_sym(g_module, "spyro1_recomp_module");
    if (!desc) {
        fprintf(stderr,
                "openpete: '%s' has no spyro1_recomp_module descriptor — "
                "not a recomp module?\n", path);
        return -1;
    }

    s_missing = 0;
#define LOAD_PTR(n) p_##n = (void (*)(CPUState*))need(#n);
    SPYRO1_MODULE_FUNCS(LOAD_PTR)
#undef LOAD_PTR
    p_set_overlay = (void (*)(uint32_t))need("spyro_set_active_overlay_by_magic");
    if (s_missing) {
        fprintf(stderr,
                "openpete: recomp module '%s' is missing %d symbols "
                "(stale module? rebuild it)\n", path, s_missing);
        return -1;
    }

    if (openpete_recomp_register(desc) != 0) return -1;
    OPL_INFO_F("recomp", "shared recomp module attached: %s", path);
    return 0;
}
