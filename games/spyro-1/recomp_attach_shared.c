// games/spyro-1/recomp_attach_shared.c - shared-linkage module attach.
//
// The ROM-derived code lives in spyro1-recomp.{so,dll}, built from the
// same sources as the static OBJECT library. This TU is the host half
// of the boundary:
//
//   1. dlopen/LoadLibrary the module (default: platform library name
//      next to the exe; --recomp-module overrides).
//   2. Resolve + register the `openpete_game_recomp_module` descriptor
//      (openpete_recomp_register refuses an ABI-version mismatch).
//   3. Resolve the host->module forwarders below.
//
// FORWARDERS: native feature code calls a small set of recompiled
// leaves directly (sub-tick re-extraction replays render functions).
// In the static build those names
// resolve at link; here each is a forwarder through a pointer filled
// at attach. The list is SELF-ENFORCING: a feature referencing a new
// func_* compiles in static mode but fails THIS build's exe link with
// an undefined symbol - add the symbol here (it is exported by the
// module automatically; nothing else to update).
#include "recomp_attach.h"
#include "openpete_recomp_module.h"
#include "spyro_overlay_table.h"
#include "openpete_log.h"
#include "openpete_paths.h"
#include "openpete_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define MODULE_DEFAULT_NAME "spyro1-recomp.dll"
#define MODULE_SEP '\\'
/** @brief Load the recomp module at path; an absolute path resolves the module's own imports from its directory.
 *
 * For an absolute path, LOAD_WITH_ALTERED_SEARCH_PATH makes the loader
 * search the module's directory for its imports instead of the working
 * directory. The narrow path converts through the active code page, as
 * fopen reads it. */
static void* mod_open(const char* p) {
    wchar_t w[4096];
    if (!MultiByteToWideChar(CP_ACP, 0, p, -1, w, (int)(sizeof w / sizeof w[0]))) return NULL;
    /* The altered search order is defined for absolute paths only. */
    int absolute = (w[0] && w[1] == L':') || (w[0] == L'\\' && w[1] == L'\\');
    return (void*)LoadLibraryExW(w, NULL, absolute ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
}
static void* mod_sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
static void mod_close(void* h) { FreeLibrary((HMODULE)h); }
#else
#include <dlfcn.h>
#include <unistd.h>
#include <limits.h>
#define MODULE_DEFAULT_NAME "spyro1-recomp.so"
#define MODULE_SEP '/'
static void* mod_open(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* mod_sym(void* h, const char* n) { return dlsym(h, n); }
static void mod_close(void* h) { dlclose(h); }
#endif

typedef struct CPUState CPUState;

/* Host->module forwarder surface (uniform `void f(CPUState*)`). */
#define SPYRO1_MODULE_FUNCS(X) \
    X(func_800168DC) X(func_800189F0) X(func_800190D4) X(func_8001C694) \
    X(func_8001ED5C) X(func_8001F798) X(func_80020F34) X(func_80022A2C) \
    X(func_80023AC4) X(func_800258F0) X(func_80034358) X(func_8003DFA4) \
    X(func_8004EBA8) X(func_8004F000) X(func_8004F4BC) X(func_8004FEA0) \
    X(func_80050240) X(func_80050BE0) X(func_800573C8) X(func_800580F4) \
    X(func_800584C4) X(func_80058864) X(func_80058D64) X(func_80059A48) \
    X(func_80059F8C)

#define DECL_PTR(n) static void (*p_##n)(CPUState*);
SPYRO1_MODULE_FUNCS(DECL_PTR)
#undef DECL_PTR

static void (*p_set_overlay)(uint32_t);
static uint32_t (*p_get_overlay_magic)(void);
static const spyro_overlay_entry_t* (*p_overlay_find)(uint32_t);

#define DEF_FWD(n) \
    void n(CPUState* cpu) { p_##n(cpu); }
SPYRO1_MODULE_FUNCS(DEF_FWD)
#undef DEF_FWD

void spyro_set_active_overlay_by_magic(uint32_t magic) { p_set_overlay(magic); }
uint32_t spyro_get_active_overlay_magic(void) { return p_get_overlay_magic(); }
const spyro_overlay_entry_t* spyro_overlay_find(uint32_t magic) {
    return p_overlay_find(magic);
}

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

/** @brief Write the default module path, MODULE_DEFAULT_NAME in the executable's directory, into buf.
 *
 * Absolute on every platform, so the existence check, the loader and a
 * first-boot build all name the same file whatever the working directory.
 * Falls back to the bare name only when the executable's directory cannot
 * be read.
 * @param buf Receives the path.
 * @param n Capacity of buf. */
static void default_module_path(char* buf, size_t n) {
    char dir[4096];
    if (openpete_exe_dir(dir, sizeof dir) == 0)
        snprintf(buf, n, "%s%c%s", dir, MODULE_SEP, MODULE_DEFAULT_NAME);
    else
        snprintf(buf, n, "%s", MODULE_DEFAULT_NAME);
}

static int file_readable(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

extern int spyro1_recomp_build_module(const char* disc_path,
                                      const char* out_module_path);

/* First-boot build as a launcher task: the extract -> recomp -> compile ->
 * link pipeline must not run on the window's thread (a window drag would
 * freeze it - see openpete_launcher_run_task). */
struct build_task { const char* disc; const char* out; };
static int build_task_fn(void* p) {
    struct build_task* t = (struct build_task*)p;
    return spyro1_recomp_build_module(t->disc, t->out);
}

/* Load the module at `path` and bind everything the host needs from it.
 * Returns the descriptor, or NULL with the handle closed again and
 * `*why` naming what was wrong - a caller holding a disc can rebuild and
 * retry rather than give up. The module must be CLOSED on failure: a
 * second dlopen of a path already mapped hands back the same stale
 * image, so a rebuild would appear not to have taken. */
static const openpete_recomp_module* bind_module(const char* path,
                                                 const char** why) {
    g_module = mod_open(path);
    if (!g_module) { *why = "cannot be loaded"; return NULL; }

    const openpete_recomp_module* desc =
        (const openpete_recomp_module*)mod_sym(g_module, "openpete_game_recomp_module");
    if (!desc) *why = "has no openpete_game_recomp_module descriptor";
    else if (desc->abi_version != OPENPETE_RECOMP_ABI_VERSION)
        *why = "was built against a different module ABI";

    if (!*why) {
        /* Bind the forwarders only once the descriptor vouches for the
         * ABI; a mismatched module's symbols are not to be trusted. */
        s_missing = 0;
#define LOAD_PTR(n) p_##n = (void (*)(CPUState*))need(#n);
        SPYRO1_MODULE_FUNCS(LOAD_PTR)
#undef LOAD_PTR
        p_set_overlay = (void (*)(uint32_t))need("spyro_set_active_overlay_by_magic");
        p_get_overlay_magic = (uint32_t (*)(void))need("spyro_get_active_overlay_magic");
        p_overlay_find = (const spyro_overlay_entry_t* (*)(uint32_t))need("spyro_overlay_find");
        if (s_missing) *why = "is missing symbols the host needs";
    }

    if (*why) { mod_close(g_module); g_module = NULL; return NULL; }
    return desc;
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

    const char* why = NULL;
    const openpete_recomp_module* desc = NULL;
    if (file_readable(path)) desc = bind_module(path, &why);
    else why = "is not there";

    if (!desc && allow_build && disc_path) {
        /* Either first boot, or an upgrade left a module this build
         * cannot use. Both are the same job - recompile from the disc
         * (extract -> recomp -> compile -> link) - and both are
         * recoverable without the user doing anything, so say what is
         * happening rather than failing at them.
         *
         * A version whose emission inputs changed normally lands on a
         * fresh codegen stamp and takes the "is not there" path; this
         * also catches whatever the stamp cannot see, so an incompatible
         * module is never a dead end. */
        if (!strcmp(why, "is not there"))
            /* Deliberately does NOT say "first boot". Since the codegen
             * stamp covers the emission inputs, an update that changes
             * them lands on a fresh stamp and arrives here too - with a
             * library the user has been playing for weeks. */
            fprintf(stderr, "openpete: building the recomp module from your "
                            "disc - this happens on first boot and after an "
                            "update that changes the generated code\n");
        else
            fprintf(stderr, "openpete: the recomp module %s - rebuilding it "
                            "from your disc\n", why);
        struct build_task bt = { disc_path, path };
        why = NULL;
        if (openpete_launcher_run_task(build_task_fn, &bt) == 0)
            desc = bind_module(path, &why);
        else
            why = "could not be rebuilt";
    }

    if (!desc) {
        fprintf(stderr,
                "openpete: recomp module '%s' %s\n"
                "  (shared-linkage build: the module is built separately as "
                "spyro1-recomp; pass --recomp-module PATH to override)\n",
                path, why ? why : "could not be loaded");
        return -1;
    }

    if (openpete_recomp_register(desc) != 0) return -1;
    OPL_INFO_F("recomp", "shared recomp module attached: %s", path);
    return 0;
}
