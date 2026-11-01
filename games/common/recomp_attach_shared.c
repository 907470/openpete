// games/common/recomp_attach_shared.c - shared-linkage module attach.
//
// The ROM-derived code lives in <tag>-recomp.{so,dll}, built from the same
// sources as the static OBJECT library. This TU is the host half of the
// boundary, compiled once for every game the executable links; the running
// game's build record (game_build.h) names its module and forwarders:
//
//   1. dlopen/LoadLibrary the module (default: the platform library name
//      next to the exe, else a first-boot game's stamped module under the
//      "modules" data directory; --recomp-module or a library entry
//      overrides).
//   2. Resolve and register the `openpete_game_recomp_module` descriptor
//      (openpete_recomp_register refuses an ABI-version mismatch).
//   3. Resolve the host->module forwarders.
//
// Forwarders: host code calls a few recompiled functions and the overlay
// dispatcher by name. In the static build those names resolve at link; here
// each is a forwarder through a pointer filled at attach. The overlay
// dispatcher's four entry points are common to every game
// (overlay_dispatch.c) and defined here; the recompiled functions are the
// game's module_imports.h list, whose forwarders the game's generated build
// record defines and binds (bind_module_funcs). The list is self-enforcing:
// host code naming a func_* the list lacks fails this build's exe link with
// an undefined symbol.
//
// A first-boot rebuild runs only for a game whose build record names the
// pipeline (the distribution presets).
#include "recomp_attach.h"
#include "game_build.h"
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
#define MODULE_SUFFIX ".dll"
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
#define MODULE_SUFFIX ".so"
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

static void (*p_set_overlay)(uint32_t);
static uint32_t (*p_get_overlay_magic)(void);
static const spyro_overlay_entry_t* (*p_overlay_find)(uint32_t);
static void (*p_overlay_set_resync)(void (*)(void));

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

/** @brief Write the default module path, a game's module name and the platform suffix in the executable's directory, into buf.
 *
 * Absolute on every platform, so the existence check, the loader and a
 * first-boot build all name the same file whatever the working directory.
 * Falls back to the bare name only when the executable's directory cannot
 * be read.
 * @param b The game's build record.
 * @param buf Receives the path.
 * @param n Capacity of buf. */
static void default_module_path(const openpete_game_build_t* b, char* buf, size_t n) {
    const char* name = b->module_name;
    char dir[3800]; /* leaves room in a 4096-byte buf for the separator and name */
    if (openpete_exe_dir(dir, sizeof dir) == 0)
        snprintf(buf, n, "%s%c%s%s", dir, MODULE_SEP, name, MODULE_SUFFIX);
    else
        snprintf(buf, n, "%s%s", name, MODULE_SUFFIX);
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

/** @brief Write the module path a run with no library entry and no --recomp-module loads, into buf.
 *
 * The module beside the executable when it is there (a build tree stages
 * one). Otherwise, for a game with the first-boot pipeline, the stamped
 * path the pipeline names for the directory <modules data dir>/<game id>:
 * openpete_paths_data_dir resolves the "modules" leaf as it resolves the
 * library, beside the executable in a writable portable install and in the
 * per-user data directory otherwise, so a read-only install (an AppImage
 * mount, Program Files) never receives the build, and the codegen stamp in
 * the path rebuilds the module after an update that changes the generated
 * code. A game without the pipeline, or a data directory that does not
 * resolve, keeps the path beside the executable.
 * @param b The game's build record.
 * @param buf Receives the path.
 * @param n Capacity of buf. */
static void disc_module_path(const openpete_game_build_t* b, char* buf, size_t n) {
    default_module_path(b, buf, n);
    if (file_readable(buf) || !b->build_module || !b->library_module_path) return;
    char dir[3800];
    char game_dir[3900];
    if (openpete_paths_data_dir("modules", dir, sizeof dir, 1) != 0) return;
    openpete_paths_join(game_dir, sizeof game_dir, dir, b->id);
    char stamped[4096];
    if (b->library_module_path(b, game_dir, stamped, sizeof stamped) == 0)
        snprintf(buf, n, "%s", stamped);
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
        const openpete_game_build_t* b = openpete_game_build();
        if (b->bind_module_funcs) s_missing += b->bind_module_funcs(need);
        p_set_overlay = (void (*)(uint32_t))need("spyro_set_active_overlay_by_magic");
        p_get_overlay_magic = (uint32_t (*)(void))need("spyro_get_active_overlay_magic");
        p_overlay_find = (const spyro_overlay_entry_t* (*)(uint32_t))need("spyro_overlay_find");
        p_overlay_set_resync = (void (*)(void (*)(void)))need("spyro_overlay_set_resync");
        if (s_missing) *why = "is missing symbols the host needs";
    }

    if (*why) { mod_close(g_module); g_module = NULL; return NULL; }
    return desc;
}

/* First-boot build as a launcher task: the extract -> recomp -> compile ->
 * link pipeline must not run on the window's thread. */
struct build_task { const char* disc; const char* out; };
/** @brief Run the game's first-boot module build for a launcher task.
 *
 * @param p The build_task.
 * @return The build's status. */
static int build_task_fn(void* p) {
    struct build_task* t = (struct build_task*)p;
    return openpete_game_build()->build_module(t->disc, t->out);
}

void openpete_recomp_announce_build(const char* why) {
    /* Either first boot, or an update whose emission inputs changed: both
     * recompile from the disc. The message does not say "first boot",
     * since an update that changes the codegen stamp lands here too. */
    if (!why || !strcmp(why, "is not there"))
        fprintf(stderr, "openpete: building the recomp module from your "
                        "disc - this happens on first boot and after an "
                        "update that changes the generated code\n");
    else
        fprintf(stderr, "openpete: the recomp module %s - rebuilding it "
                        "from your disc\n", why);
}

/** @brief Rebuild a missing or unusable module from the disc and bind it.
 *
 * @param path The module path to write.
 * @param disc_path The disc.
 * @param why The reason the first bind failed; receives the new reason.
 * A failed rebuild writes the failed-build marker of the library entry the
 * launcher booted (openpete_launcher_mark_build_failed); a stopped one (a
 * quit, -2) writes none.
 * @return The descriptor, or NULL. */
static const openpete_recomp_module* rebuild_module(const char* path,
                                                    const char* disc_path,
                                                    const char** why) {
    openpete_recomp_announce_build(*why);
    struct build_task bt = { disc_path, path };
    *why = NULL;
    const int rc = openpete_launcher_run_task_exclusive(build_task_fn, &bt);
    if (rc != 0) {
        /* A library entry's module (the launcher's pick) records the
         * failure, so the next launch shows it instead of rebuilding. */
        if (rc != -2) openpete_launcher_mark_build_failed();
        *why = rc == -2 ? "was not rebuilt (stopped)" : "could not be rebuilt";
        return NULL;
    }
    return bind_module(path, why);
}
int openpete_game_library_module_path(const openpete_game_build_t* b, const char* entry_dir,
                                      char* out, size_t cap) {
    /* Without a first-boot pipeline the module comes from the build tree
     * alone, and no library entry carries one. */
    if (!b || !b->library_module_path) return -1;
    return b->library_module_path(b, entry_dir, out, cap);
}

int openpete_game_recomp_attach(const char* module_path_override,
                                const char* disc_path,
                                int allow_build) {
    char pathbuf[4096];
    const char* path = module_path_override;
    if (!path) {
        disc_module_path(openpete_game_build(), pathbuf, sizeof pathbuf);
        path = pathbuf;
    }

    const char* why = NULL;
    const openpete_recomp_module* desc = NULL;
    if (file_readable(path)) desc = bind_module(path, &why);
    else why = "is not there";

    const openpete_game_build_t* b = openpete_game_build();
    const int first_boot = b->build_module != NULL;
    if (!desc && first_boot && allow_build && disc_path)
        desc = rebuild_module(path, disc_path, &why);

    if (!desc) {
        fprintf(stderr,
                "openpete: recomp module '%s' %s\n"
                "  (shared-linkage build: the module is built separately as "
                "%s; %s)\n",
                path, why ? why : "could not be loaded", b->module_name,
                first_boot
                    ? "pass --recomp-module PATH to override"
                    : "this build never compiles one from the disc - build "
                      "its CMake target or pass --recomp-module PATH");
        return -1;
    }

    if (openpete_recomp_register(desc) != 0) return -1;
    OPL_INFO_F("recomp", "shared recomp module attached: %s", path);
    return 0;
}
