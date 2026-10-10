// games/common/recomp_attach_shared.c - shared-linkage module attach.
//
// The ROM-derived code lives in <tag>-recomp.{so,dll}, built from the same
// sources as the static OBJECT library. This TU is the host half of the
// boundary:
//
//   1. dlopen/LoadLibrary the module (default: the platform library name
//      next to the exe; --recomp-module or a library entry overrides).
//   2. Resolve and register the `openpete_game_recomp_module` descriptor
//      (openpete_recomp_register refuses an ABI-version mismatch).
//   3. Resolve the host->module forwarders.
//
// Forwarders: host code calls a few recompiled functions and the overlay
// dispatcher by name. In the static build those names resolve at link; here
// each is a forwarder through a pointer filled at attach. The overlay
// dispatcher's four entry points are common to every game
// (overlay_dispatch.c); the recompiled functions are the game's
// OPENPETE_GAME_MODULE_FUNCS list (module_imports.h in the game's
// directory). The list is self-enforcing: host code naming a func_* the
// list lacks links in the static build and fails this build's exe link with
// an undefined symbol.
//
// OPENPETE_GAME_MODULE_NAME ("<tag>-recomp") comes from the game's CMake
// target; OPENPETE_RECOMP_FIRST_BOOT is 1 only in a build whose game has a
// first-boot pipeline and whose preset ships one (the distribution
// presets).
#include "recomp_attach.h"
#include "module_imports.h"
#include "openpete_recomp_module.h"
#include "spyro_overlay_table.h"
#include "openpete_log.h"
#include "openpete_paths.h"
#include "openpete_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OPENPETE_RECOMP_FIRST_BOOT
#define OPENPETE_RECOMP_FIRST_BOOT 0
#endif

#ifdef _WIN32
#include <windows.h>
#define MODULE_DEFAULT_NAME OPENPETE_GAME_MODULE_NAME ".dll"
#define MODULE_SEP '\\'
/** @brief Load the recomp module at path; an absolute path resolves the module's own imports from its directory.
 *
 * For an absolute path, LOAD_WITH_ALTERED_SEARCH_PATH makes the loader
 * search the module's directory for its imports instead of the working
 * directory. The narrow path converts through the active code page, as
 * fopen reads it.
 * @param p The module path.
 * @return The module handle, or NULL. */
static void* mod_open(const char* p) {
    wchar_t w[4096];
    if (!MultiByteToWideChar(CP_ACP, 0, p, -1, w, (int)(sizeof w / sizeof w[0]))) return NULL;
    /* The altered search order is defined for absolute paths only. */
    int absolute = (w[0] && w[1] == L':') || (w[0] == L'\\' && w[1] == L'\\');
    return (void*)LoadLibraryExW(w, NULL, absolute ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
}
/** @brief Resolve an exported symbol of the module.
 *
 * @param h The module handle.
 * @param n The symbol name.
 * @return The symbol's address, or NULL. */
static void* mod_sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
/** @brief Unload the module.
 *
 * @param h The module handle. */
static void mod_close(void* h) { FreeLibrary((HMODULE)h); }
#else
#include <dlfcn.h>
#define MODULE_DEFAULT_NAME OPENPETE_GAME_MODULE_NAME ".so"
#define MODULE_SEP '/'
/** @brief Load the recomp module at path with every symbol bound now and none made global.
 *
 * @param p The module path.
 * @return The module handle, or NULL. */
static void* mod_open(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
/** @brief Resolve a symbol of the module.
 *
 * @param h The module handle.
 * @param n The symbol name.
 * @return The symbol's address, or NULL. */
static void* mod_sym(void* h, const char* n) { return dlsym(h, n); }
/** @brief Unload the module.
 *
 * @param h The module handle. */
static void mod_close(void* h) { dlclose(h); }
#endif

typedef struct CPUState CPUState;

#define DECL_PTR(n) static void (*p_##n)(CPUState*);
OPENPETE_GAME_MODULE_FUNCS(DECL_PTR)
#undef DECL_PTR

static void (*p_set_overlay)(uint32_t);
static uint32_t (*p_get_overlay_magic)(void);
static const spyro_overlay_entry_t* (*p_overlay_find)(uint32_t);
static void (*p_overlay_set_resync)(void (*)(void));

#define DEF_FWD(n) \
    void n(CPUState* cpu) { p_##n(cpu); }
OPENPETE_GAME_MODULE_FUNCS(DEF_FWD)
#undef DEF_FWD

void spyro_set_active_overlay_by_magic(uint32_t magic) { p_set_overlay(magic); }
uint32_t spyro_get_active_overlay_magic(void) { return p_get_overlay_magic(); }
const spyro_overlay_entry_t* spyro_overlay_find(uint32_t magic) {
    return p_overlay_find(magic);
}
void spyro_overlay_set_resync(void (*fn)(void)) { p_overlay_set_resync(fn); }

static void* g_module;
static int s_missing;

/** @brief Resolve a symbol the host needs from the module, counting and logging a missing one.
 *
 * @param name The symbol name.
 * @return The symbol's address, or NULL. */
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
    char dir[4000]; /* leaves room in a 4096-byte buf for the separator and name */
    if (openpete_exe_dir(dir, sizeof dir) == 0)
        snprintf(buf, n, "%s%c%s", dir, MODULE_SEP, MODULE_DEFAULT_NAME);
    else
        snprintf(buf, n, "%s", MODULE_DEFAULT_NAME);
}

/** @brief Whether a file can be opened for reading.
 *
 * @param p The path.
 * @return 1 when it opens, else 0. */
static int file_readable(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/** @brief Load the module at path and bind the descriptor and every forwarder.
 *
 * The module is closed again on failure: a second dlopen of a path already
 * mapped hands back the same image, so a rebuild would appear not to have
 * taken.
 * @param path The module path.
 * @param why Receives the reason on failure.
 * @return The descriptor, or NULL. */
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
        OPENPETE_GAME_MODULE_FUNCS(LOAD_PTR)
#undef LOAD_PTR
        p_set_overlay = (void (*)(uint32_t))need("spyro_set_active_overlay_by_magic");
        p_get_overlay_magic = (uint32_t (*)(void))need("spyro_get_active_overlay_magic");
        p_overlay_find = (const spyro_overlay_entry_t* (*)(uint32_t))need("spyro_overlay_find");
        p_overlay_set_resync = (void (*)(void (*)(void)))need("spyro_overlay_set_resync");
        if (s_missing) *why = "is missing symbols the host needs";
    }

    if (*why) { mod_close(g_module); g_module = NULL; return NULL; }
    return desc;
}

#if OPENPETE_RECOMP_FIRST_BOOT
/* First-boot build as a launcher task: the extract -> recomp -> compile ->
 * link pipeline must not run on the window's thread. */
struct build_task { const char* disc; const char* out; };
/** @brief Run the game's first-boot module build for a launcher task.
 *
 * @param p The build_task.
 * @return The build's status. */
static int build_task_fn(void* p) {
    struct build_task* t = (struct build_task*)p;
    return openpete_game_recomp_build_module(t->disc, t->out);
}

/** @brief Rebuild a missing or unusable module from the disc and bind it.
 *
 * @param path The module path to write.
 * @param disc_path The disc.
 * @param why The reason the first bind failed; receives the new reason.
 * @return The descriptor, or NULL. */
static const openpete_recomp_module* rebuild_module(const char* path,
                                                    const char* disc_path,
                                                    const char** why) {
    /* Either first boot, or an update whose emission inputs changed: both
     * recompile from the disc. The message does not say "first boot",
     * since an update that changes the codegen stamp lands here too. */
    if (!strcmp(*why, "is not there"))
        fprintf(stderr, "openpete: building the recomp module from your "
                        "disc - this happens on first boot and after an "
                        "update that changes the generated code\n");
    else
        fprintf(stderr, "openpete: the recomp module %s - rebuilding it "
                        "from your disc\n", *why);
    struct build_task bt = { disc_path, path };
    *why = NULL;
    if (openpete_launcher_run_task(build_task_fn, &bt) != 0) {
        *why = "could not be rebuilt";
        return NULL;
    }
    return bind_module(path, why);
}
#else
/* No first-boot pipeline: the module comes from the build tree alone, and
 * no library entry carries one. */
int openpete_game_library_module_path(const char* entry_dir, char* out, size_t cap) {
    (void)entry_dir; (void)out; (void)cap;
    return -1;
}
#endif

int openpete_game_recomp_attach(const char* module_path_override,
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

#if OPENPETE_RECOMP_FIRST_BOOT
    if (!desc && allow_build && disc_path)
        desc = rebuild_module(path, disc_path, &why);
#else
    (void)disc_path;
    (void)allow_build;
#endif

    if (!desc) {
        fprintf(stderr,
                "openpete: recomp module '%s' %s\n"
                "  (shared-linkage build: the module is built separately as "
                OPENPETE_GAME_MODULE_NAME "; %s)\n",
                path, why ? why : "could not be loaded",
                OPENPETE_RECOMP_FIRST_BOOT
                    ? "pass --recomp-module PATH to override"
                    : "this build never compiles one from the disc - build "
                      "its CMake target or pass --recomp-module PATH");
        return -1;
    }

    if (openpete_recomp_register(desc) != 0) return -1;
    OPL_INFO_F("recomp", "shared recomp module attached: %s", path);
    return 0;
}
