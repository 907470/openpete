/* mod_shader.c - the mod GLSL compile service.
 *
 * Mods ship GLSL source; the ENGINE compiles it to SPIR-V at load through
 * mod_glsl.c (in-process glslang, the dialect of the engine's own
 * shaders). Compiles are content-hash cached under <mod>/.build/shaders/
 * with the same stamp mechanics as the C compile-on-boot cache
 * (mod_loader.c mod_compute_key): stamp removed on failure, object kept.
 * A prebuilt "<path>.spv" beside a MISSING source loads instead
 * (binary-mod parity) but only with a "<path>.spv.ihash" sidecar
 * matching the current interface hash - a stale prebuilt refuses
 * cleanly instead of misrendering.
 *
 * A broken shader is a log line + an unavailable effect, never a
 * bricked boot. Registration is entry/tick context (file IO - refused
 * in present hooks).
 *
 * The postfx pass registry and the GPU half of the material-selector tier
 * (shader-handle validation, the locked mutators, the GPU descriptions of
 * a resolved material) are implemented below the shader service in this
 * file; the material table and its resolution rules are in
 * mod_material_resolve.c.
 */

#include "openpete_mods.h"
#include "openpete_atomic_file.h"
#include "openpete_log.h"
#include "mod_vec.h"
#include "openpete_native_render.h"   /* set_tag_split arming (material selectors) */
#include "../host/sha256.h"
#include "mod_glsl.h"
#include "mod_spv_cdata.h"
#include "mod_spv_reflect.h"
#include "mod_mat_link.h"
#include "mod_material_resolve.h"

#ifndef OPENPETE_NO_GLSLANG
#include <glslang/build_info.h>
#endif

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/* Registry lock. The tables below (shaders, postfx passes, pipelines,
 * materials) are mutated from mod hooks on the thread that runs the guest
 * and read by the compose, which may run on a present thread. Mutators take
 * the write side; a compose holds the read side for its whole duration, so
 * a tick boundary that retunes a material waits for the present in flight
 * rather than shading half of it under the old key. Uncontended in the
 * common case: the tables change at tick boundaries and mod load/unload. */
static SDL_RWLock* s_reg_lock;
/** @brief Return the registry lock, creating it on first use.
 *
 * The creation is not synchronized: the first call must not race another
 * thread's first call.
 *
 * @return The lock, or NULL when SDL cannot create one. */
static SDL_RWLock* reg_lock(void) {
    if (!s_reg_lock) s_reg_lock = SDL_CreateRWLock();
    return s_reg_lock;
}
/* A mutator called from INSIDE the compose (a mod UI section retuning a
 * selector, a postfx uniform set from the dev overlay) runs on the thread
 * that holds the read side; taking the write side there would deadlock, and
 * the mutation is by the reader itself, which is the single-threaded case
 * this lock exists to preserve. Only one compose runs at a time, so one
 * reader thread id suffices. */
static _Atomic(SDL_ThreadID) s_reg_reader_tid;   /* read by writers on other threads */
/** @brief Take the registry write lock unless the caller is the compose reader thread.
 *
 * The thread recorded by openpete_mods_registry_read_lock() already holds
 * the read side, so it proceeds unlocked instead of deadlocking.
 *
 * @return 1 when the write lock was taken, 0 when it was not. */
static int reg_wr_begin(void) {
    const SDL_ThreadID reader = atomic_load_explicit(&s_reg_reader_tid, memory_order_relaxed);
    if (reader && reader == SDL_GetCurrentThreadID()) return 0;
    SDL_LockRWLockForWriting(reg_lock());
    return 1;
}
/** @brief Release the registry write lock when reg_wr_begin() took it.
 *
 * @param locked The value reg_wr_begin() returned. */
static void reg_wr_end(int locked) { if (locked) SDL_UnlockRWLock(s_reg_lock); }
#define REG_WR()     const int reg_l_ = reg_wr_begin()
#define REG_WR_END() reg_wr_end(reg_l_)
void openpete_mods_registry_read_lock(void) {
    SDL_LockRWLockForReading(reg_lock());
    atomic_store_explicit(&s_reg_reader_tid, SDL_GetCurrentThreadID(), memory_order_relaxed);
}
void openpete_mods_registry_read_unlock(void) {
    atomic_store_explicit(&s_reg_reader_tid, (SDL_ThreadID)0, memory_order_relaxed);
    SDL_UnlockRWLock(s_reg_lock);
}
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#define shader_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define shader_mkdir(p) mkdir((p), 0755)
#endif

/* Shaders the table's first block holds; it grows by whole blocks. */
#define MOD_SHADER_BLOCK    256
#define MOD_SHADER_PATH_MAX OPENPETE_MOD_GLSL_PATH_MAX

typedef struct {
    const char* owner;       /* mod id (loader-owned string) */
    char        path[MOD_SHADER_PATH_MAX]; /* as registered, mod-relative */
    char        mod_dir[MOD_SHADER_PATH_MAX * 2]; /* the owner's directory, for a later source read */
    int         stage;       /* GLSLANG_STAGE_VERTEX / _FRAGMENT */
    uint32_t*   spv;         /* malloc'd SPIR-V words */
    size_t      spv_words;
    /* The same source compiled for the retained terrain pipelines
     * (OP_PROFILE_TERRAIN). NULL for anything that is not a material
     * fragment, and for one whose terrain profile failed to compile -
     * terrain draws then stay stock rather than the mod failing. */
    uint32_t*   spv_ter;
    size_t      spv_ter_words;
    /* The same source compiled with OP_PROFILE_PORTAL, the walked scene
     * half that discards what a portal preview covers. NULL like spv_ter. */
    uint32_t*   spv_portal;
    size_t      spv_portal_words;
    /* OPENPETE_MATERIAL_CONTRACT_* of the stock profile, read from its
     * SPIR-V at registration (shader_spv_contract). */
    int         contract;
    /* The constant tables moved out of each fragment profile (stock,
     * terrain, portal; mod_spv_cdata.h) and the renderer's cache slot for
     * each one's device copy. */
    openpete_mod_spv_cdata_t cdata[3];
    void*       cdata_gpu[3];
    /* The composition member of each fragment profile (mod_mat_link.h),
     * prepared at registration for a material fragment; link_n is the
     * profile count prepared, 0 for anything else. */
    openpete_mod_link_member_t link[OPENPETE_MOD_LINK_PROFILES];
    int         link_n;
    int         live;        /* 0 = swept slot (stale handle) */
} mod_shader_t;

_Static_assert(OPENPETE_MOD_LINK_CONTRACT_SCENE == OPENPETE_MATERIAL_CONTRACT_SCENE &&
               OPENPETE_MOD_LINK_CONTRACT_DECAL == OPENPETE_MATERIAL_CONTRACT_DECAL,
               "mod_mat_link.h contracts mirror openpete_mods.h");

/* Chunked: a handle is an index into this table and the renderer reads
 * an entry's SPIR-V under the registry read lock; entries never move, so a
 * registration made by the compose thread itself (which skips the write
 * lock) cannot invalidate a pointer the compose holds. */
static mod_cvec s_shaders;   /* mod_shader_t */

/** @brief Return the number of slots in the shader table, live or swept. */
static int shader_count(void) { return (int)mod_cvec_count(&s_shaders); }
/** @brief Return the shader table slot at an index; the caller bounds the index by shader_count(). */
static mod_shader_t* shader_at(int i) {
    return (mod_shader_t*)mod_cvec_at(&s_shaders, (size_t)i);
}

/** @brief Return the shading contract a SPIR-V module was compiled against.
 *
 * Scans the module's OpName instructions (opcode 5) for the decal
 * profile's push-block name `OP_DECAL_PC`, which only
 * openpete_psx_decal.glsl declares. A module that names it is
 * OPENPETE_MATERIAL_CONTRACT_DECAL; every other module, including one
 * whose debug names are stripped, is OPENPETE_MATERIAL_CONTRACT_SCENE.
 * The walk stops at a zero word count or one that runs past the module.
 * @param w The SPIR-V words, starting with the magic number.
 * @param n The word count.
 * @return The OPENPETE_MATERIAL_CONTRACT_* value. */
static int shader_spv_contract(const uint32_t* w, size_t n) {
    static const char k_decal_block[] = "OP_DECAL_PC";
    return openpete_mod_spv_has_name(w, n, k_decal_block, sizeof k_decal_block)
        ? OPENPETE_MATERIAL_CONTRACT_DECAL : OPENPETE_MATERIAL_CONTRACT_SCENE;
}

/** @brief Move the large run-time-indexed constant tables of a material fragment's profiles into storage-block data.
 *
 * Applies to a fragment module that names the contract function
 * `op_material(` (both material contracts; a module with stripped names
 * keeps its tables). Each profile's SPIR-V is rewritten in place by
 * openpete_mod_spv_cdata_extract and its table block kept in sh->cdata;
 * the cache files stay the glslang output, so the rewrite runs on every
 * registration, prebuilt modules included. A profile the rewrite rejects
 * as malformed keeps its module. OPENPETE_MOD_SPV_CDATA=0 skips the
 * rewrite (an A/B instrument for the moved and unmoved module).
 * @param sh The shader, its profiles loaded.
 * @param id The mod id, for log lines. */
static void shader_move_tables(mod_shader_t* sh, const char* id) {
    static const char k_material_fn[] = "op_material(";
    const char* off = getenv("OPENPETE_MOD_SPV_CDATA");
    if ((off && strcmp(off, "0") == 0) || sh->stage != OPENPETE_MOD_GLSL_STAGE_FRAG ||
        !openpete_mod_spv_has_name(sh->spv, sh->spv_words, k_material_fn, sizeof k_material_fn - 1))
        return;
    uint32_t** spv[3] = { &sh->spv, &sh->spv_ter, &sh->spv_portal };
    size_t* words[3] = { &sh->spv_words, &sh->spv_ter_words, &sh->spv_portal_words };
    for (int p = 0; p < 3; p++) {
        uint32_t* out = NULL;
        size_t out_n = 0;
        if (!*spv[p] ||
            openpete_mod_spv_cdata_extract(*spv[p], *words[p], &out, &out_n, &sh->cdata[p]) <= 0)
            continue;
        free(*spv[p]);
        *spv[p] = out;
        *words[p] = out_n;
        OPL_INFO_F("mods", "%s: shader %s%s: %d constant table(s), %zu B, moved to "
                   "the material table block", id, sh->path,
                   p == 1 ? ".terrain" : p == 2 ? ".portal" : "",
                   sh->cdata[p].tables, sh->cdata[p].len);
    }
}

/* ------------------------------------------------------------------ */
/* small file helpers                                                  */

/** @brief List the files in a directory whose extension is in a `;`-separated list, sorted by strcmp.
 *
 * The extension is the name from its last `.`; a name with no `.` or of
 * 128 bytes or more is skipped, and the scan stops at `cap` entries. The
 * strcmp order makes the build and watch hashes that consume the list
 * independent of readdir order.
 * @param dir Directory to scan; a missing one lists nothing.
 * @param exts Extensions with their dots, such as `.vert;.frag`.
 * @param names Receives the names.
 * @param cap Capacity of @p names.
 * @return The number of names written. */
static int list_dir_sorted(const char* dir, const char* exts,
                           char names[][128], int cap) {
    DIR* d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL && n < cap) {
        const char* name = de->d_name;
        const char* dot = strrchr(name, '.');
        if (!dot) continue;
        char pat[16];
        snprintf(pat, sizeof pat, "%s;", dot);
        char hay[256];
        snprintf(hay, sizeof hay, "%s;", exts);
        if (!strstr(hay, pat)) continue;
        if (strlen(name) >= 128) continue;
        snprintf(names[n++], 128, "%s", name);
    }
    closedir(d);
    for (int i = 1; i < n; i++)          /* insertion sort, tiny n */
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char tmp[128];
            memcpy(tmp, names[j - 1], 128);
            memcpy(names[j - 1], names[j], 128);
            memcpy(names[j], tmp, 128);
        }
    return n;
}

/** @brief Flatten a mod-relative shader path into a cache file name.
 *
 * Replaces each `/` and `\\` with `_`, so `shaders/crt.frag` becomes
 * `shaders_crt.frag`, and truncates to `cap - 1` bytes. */
static void flatten_path(const char* path, char* out, size_t cap) {
    size_t i = 0;
    for (; path[i] && i + 1 < cap; i++)
        out[i] = (path[i] == '/' || path[i] == '\\') ? '_' : path[i];
    out[i] = '\0';
}

#define MOD_SHADER_STAGE_VERT OPENPETE_MOD_GLSL_STAGE_VERT
#define MOD_SHADER_STAGE_FRAG OPENPETE_MOD_GLSL_STAGE_FRAG

/* ------------------------------------------------------------------ */
/* build key                                                           */

/** @brief Feed a NUL-terminated string, without its terminator, to a SHA-256 context. */
static void key_update_str(openpete_sha256_ctx* hc, const char* s) {
    openpete_sha256_update(hc, s, strlen(s));
}

/** @brief Feed the contents of a file to a SHA-256 context.
 *
 * An empty file feeds nothing and succeeds.
 *
 * @return 0, or -1 when the file cannot be read. */
static int key_update_file(openpete_sha256_ctx* hc, const char* path) {
    size_t len = 0;
    char* data = openpete_mod_glsl_read_file(path, &len);
    if (!data) return -1;
    openpete_sha256_update(hc, data, len);
    free(data);
    return 0;
}

/* Every staged file a mod's shader can compile against: the tier contracts
 * and everything they include. All of it keys the build, because a change to
 * any of it changes the SPIR-V a source produces - a cached .spv that
 * outlives an edit to one of these is a shader lying about its interface. */
static const char* const k_iface_files[] = {
    "openpete_shader_api.glsl",     /* postfx tier                        */
    "openpete_psx_material.glsl",   /* material tier: both engine halves  */
    "openpete_psx_decal.glsl",      /* decal draw path                    */
    "psx_key_contest.glsl",         /* og painter verdicts (both halves)  */
    "psx_clut.glsl",                /* texel decode, fogged-CLUT row mix  */
    "psx_texel.glsl",               /* integer texel of a texel coordinate */
    "terrain_shade.glsl",           /* retained-terrain engine half       */
    "portal_cover.glsl",            /* portal plane-cover test            */
    "portal_cover_scene.glsl",      /* the scene half's portal profile    */
};

/** @brief Feed every staged shader interface file under the SDK directory to a SHA-256 context.
 *
 * The files are the k_iface_files entries under `<sdk>/shaders/`, in table
 * order; a missing file contributes nothing, so the hash is stable without
 * the SDK staged. */
static void key_update_interface(openpete_sha256_ctx* hc) {
    for (size_t i = 0; i < sizeof k_iface_files / sizeof *k_iface_files; i++) {
        char p[MOD_SHADER_PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/shaders/%s", openpete_mods_sdk_dir(),
                 k_iface_files[i]);
        (void)key_update_file(hc, p);   /* absent = stable no-op */
    }
}

/** @brief Hash the shader interface: the mod API version and the staged interface files.
 *
 * A prebuilt `.spv` loads only when its `.spv.ihash` sidecar holds this
 * hash. The glslang version is excluded, since a prebuilt module does not
 * depend on the engine's compiler.
 * @param out_hex Receives the 64-character hex digest and a NUL. */
static void interface_hash(char out_hex[65]) {
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    const unsigned api_ver = OPENPETE_MOD_API_VERSION;
    openpete_sha256_update(&hc, &api_ver, sizeof api_ver);
    key_update_interface(&hc);
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, out_hex);
}

/** @brief Compute a shader source's build key.
 *
 * SHA-256 over the mod API version, the glslang version, the staged
 * interface files, the source, and then every `.vert`, `.frag` and `.glsl`
 * file in the source's directory in strcmp order. The sibling files make
 * an edit to a mod-local include invalidate its includers; every other
 * shader in that directory recompiles with them.
 * @return 0, or -1 when the source cannot be read. */
static int shader_compute_key(const char* mod_dir, const char* rel_path,
                              char key_out[65]) {
    char src_path[MOD_SHADER_PATH_MAX * 2];
    snprintf(src_path, sizeof src_path, "%s/%s", mod_dir, rel_path);

    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    const unsigned api_ver = OPENPETE_MOD_API_VERSION;
    openpete_sha256_update(&hc, &api_ver, sizeof api_ver);
    char glslang_ver[64];
#ifndef OPENPETE_NO_GLSLANG
    snprintf(glslang_ver, sizeof glslang_ver, "glslang %d.%d.%d",
             GLSLANG_VERSION_MAJOR, GLSLANG_VERSION_MINOR,
             GLSLANG_VERSION_PATCH);
#else
    snprintf(glslang_ver, sizeof glslang_ver, "glslang none");
#endif
    key_update_str(&hc, glslang_ver);
    key_update_interface(&hc);
    if (key_update_file(&hc, src_path) != 0) return -1;
    /* Siblings: the mod's whole shader dir keys every shader in it, so
     * a mod-local include edit invalidates its includers. */
    {
        char dir[MOD_SHADER_PATH_MAX * 2];
        snprintf(dir, sizeof dir, "%s/%s", mod_dir, rel_path);
        char* slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        char names[64][128];
        int n = list_dir_sorted(dir, ".vert;.frag;.glsl", names, 64);
        for (int i = 0; i < n; i++) {
            char p[MOD_SHADER_PATH_MAX * 2 + 128];
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            (void)key_update_file(&hc, p);
        }
    }
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key_out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* compile                                                             */


/** @brief Produce one profile of one shader source, from the cache when its build key matches, else by compiling it.
 *
 * The cache files are `<build_dir>/<flat><suffix>.spv` and its `.hash`
 * stamp. A stamp equal to `key` loads the cached SPIR-V, provided it is a
 * nonempty whole number of words; otherwise the source compiles, and
 * success writes the `.spv` and then the stamp. A failed compile removes
 * only the stamp and leaves any older `.spv` file in place, unused until a
 * compile rewrites both.
 * @param suffix Variant name in the cache files: "" for the stock profile,
 *        `.terrain` for the retained-terrain engine half.
 * @param preamble Text glslang injects after `#version`, or NULL.
 * @param out_words Receives the malloc'd SPIR-V words.
 * @return The word count, 0 on failure. */
static size_t compile_profile(const char* src, int stage,
                              const openpete_mod_glsl_include_t* ctx, const char* key,
                              const char* build_dir, const char* flat,
                              const char* suffix, const char* preamble,
                              const char* log_path, const char* id,
                              const char* path, uint32_t** out_words) {
    char spv_path[MOD_SHADER_PATH_MAX * 3], stamp_path[MOD_SHADER_PATH_MAX * 3];
    snprintf(spv_path, sizeof spv_path, "%s/%s%s.spv", build_dir, flat, suffix);
    snprintf(stamp_path, sizeof stamp_path, "%s/%s%s.hash", build_dir, flat, suffix);

    char stamped[65] = "";
    FILE* f = fopen(stamp_path, "rb");
    if (f) {
        size_t got = fread(stamped, 1, 64, f);
        stamped[got] = '\0';
        if (got && stamped[got - 1] == '\n') stamped[got - 1] = '\0';
        fclose(f);
    }
    if (strcmp(stamped, key) == 0) {
        size_t len = 0;
        char* cached = openpete_mod_glsl_read_file(spv_path, &len);
        if (cached && len >= 4 && (len & 3u) == 0) {
            *out_words = (uint32_t*)cached;
            return len / 4;
        }
        free(cached);
    }
    size_t n = openpete_mod_glsl_compile(src, stage, ctx, preamble, log_path,
                                         out_words);
    if (n == 0) {
        remove(stamp_path);          /* stamp only; a cached .spv survives */
        return 0;
    }
    /* The stamp marks the .spv current, so it is written only after the
     * .spv landed whole; a failed .spv write leaves no stamp and the next
     * boot compiles again. */
    char stamp_line[80];
    int stamp_len = snprintf(stamp_line, sizeof stamp_line, "%s\n", key);
    if (op_atomic_write_file(spv_path, *out_words, n * 4) != 0 ||
        stamp_len <= 0 || (size_t)stamp_len >= sizeof stamp_line ||
        op_atomic_write_file(stamp_path, stamp_line, (size_t)stamp_len) != 0)
        remove(stamp_path);
    OPL_INFO_F("mods", "%s: shader %s%s compiled (%zu words)", id, path,
               suffix, n);
    return n;
}

/** @brief Compile the extra profiles of a material fragment: the retained-terrain half and the portal profile.
 *
 * The retained terrain pipelines take flat per-face varyings instead of a
 * per-draw push block (OP_PROFILE_TERRAIN); terrain that yields to a portal
 * preview draws through the portal profile (OP_PROFILE_PORTAL). The source
 * is the same source: only the profile define differs. Anything that is not
 * a material fragment (postfx, decal, mesh vertex stages) has one profile
 * and gets neither. A failed profile is logged and left NULL; its draws stay
 * on the engine's own stages.
 * @param src The shader source.
 * @param stage Its stage.
 * @param ctx Include resolution.
 * @param key The build key.
 * @param build_dir The cache directory.
 * @param flat The flattened path naming the cache files.
 * @param log_path The compile log path.
 * @param id The mod id, for log lines.
 * @param path The shader's mod-relative path, for log lines.
 * @param sh Receives spv_ter and spv_portal with their word counts. */
static void compile_material_profiles(const char* src, int stage,
                                      const openpete_mod_glsl_include_t* ctx,
                                      const char* key, const char* build_dir,
                                      const char* flat, const char* log_path,
                                      const char* id, const char* path,
                                      mod_shader_t* sh) {
    if (!openpete_mod_glsl_has_terrain_profile(src, stage)) return;
    sh->spv_ter_words = compile_profile(src, stage, ctx, key, build_dir, flat, ".terrain",
                                        OPENPETE_MOD_GLSL_TERRAIN_PREAMBLE, log_path, id,
                                        path, &sh->spv_ter);
    if (sh->spv_ter_words == 0)
        OPL_ERROR_F("mods", "%s: shader %s: terrain profile FAILED (see %s) - "
                    "terrain draws stay stock", id, path, log_path);
    sh->spv_portal_words = compile_profile(src, stage, ctx, key, build_dir, flat, ".portal",
                                           OPENPETE_MOD_GLSL_PORTAL_PREAMBLE, log_path, id,
                                           path, &sh->spv_portal);
    if (sh->spv_portal_words == 0)
        OPL_ERROR_F("mods", "%s: shader %s: portal profile FAILED (see %s) - "
                    "terrain behind portal previews draws stock", id, path, log_path);
}

/* ------------------------------------------------------------------ */
/* composition members                                                 */

/* The link context of the staged SDK (mod_mat_link.h); created by the first
 * material registration, under the registry write lock or on the compose
 * thread that holds the read side, which never run at once. */
static openpete_mod_link_ctx_t* s_link_ctx;

/** @brief Return the link context, created on first use; NULL on allocation failure. */
static openpete_mod_link_ctx_t* link_ctx(void) {
    if (!s_link_ctx) s_link_ctx = openpete_mod_link_ctx_new(openpete_mods_sdk_dir());
    return s_link_ctx;
}

/** @brief Return whether registration decides composition members at once (OPENPETE_MATERIAL_LINK_EAGER=1), a census instrument; read on each call. */
static int link_eager(void) {
    const char* e = getenv("OPENPETE_MATERIAL_LINK_EAGER");
    return e && *e && *e != '0';
}

/* What a one-member compile needs to go through the shader's compile cache. */
typedef struct {
    const openpete_mod_glsl_include_t* inc;
    const char* key;          /* the shader's build key                  */
    const char* build_dir;
    const char* flat;
    const char* suffix;       /* the profile's cache-file suffix         */
    const char* id;
    const char* path;
} link_cache_t;

/** @brief Compile a one-member module through the shader's compile cache (an openpete_mod_link_compile_fn).
 *
 * The cache files are `<flat><suffix>.spv` and `.hash`; the stamp is
 * SHA-256 over the shader's build key, the preamble and the module text,
 * so an edit to the source, the interface files, the compiler or the
 * generated text recompiles.
 * @param user The link_cache_t.
 * @param text The module text.
 * @param preamble The profile preamble, or NULL.
 * @param log_path The failure log path.
 * @param out Receives the words.
 * @return The word count, 0 on failure. */
static size_t link_compile_cached(void* user, const char* text, const char* preamble,
                                  const char* log_path, uint32_t** out) {
    const link_cache_t* c = (const link_cache_t*)user;
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    openpete_sha256_update(&hc, c->key, strlen(c->key) + 1);
    if (preamble) openpete_sha256_update(&hc, preamble, strlen(preamble));
    openpete_sha256_update(&hc, "", 1);
    key_update_str(&hc, text);
    uint8_t digest[32];
    char key[65];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key);
    return compile_profile(text, MOD_SHADER_STAGE_FRAG, c->inc, key, c->build_dir, c->flat,
                           c->suffix, preamble, log_path, c->id, c->path, out);
}

/** @brief Return whether a module is a material fragment: it names the contract function `op_material(`. */
static int shader_is_material(int stage, const uint32_t* w, size_t n) {
    static const char k_material_fn[] = "op_material(";
    return stage == MOD_SHADER_STAGE_FRAG &&
           openpete_mod_spv_has_name(w, n, k_material_fn, sizeof k_material_fn - 1);
}

/** @brief Refuse the modules of a material fragment pinned at api 13 or later whose op_material reaches a discard.
 *
 * The stock module's refusal refuses the shader; a refused terrain or
 * portal profile is freed and its draws stay on the engine's own stages,
 * as for a profile that fails to compile.
 * @param pin The owner's api pin.
 * @param words The stock module.
 * @param n Its word count.
 * @param prof The extra profiles; a refused one is freed and cleared.
 * @return 0, or -1 when the stock module is refused. */
static int shader_refuse_discard(int pin, const char* id, const char* path, const uint32_t* words,
                                 size_t n, mod_shader_t* prof) {
    if (openpete_mod_link_discard_refused(pin, words, n)) {
        OPL_ERROR_F("mods", "%s: shader %s: op_material reaches a discard, which the material "
                    "contract forbids (api %d) - refused", id, path, pin);
        return -1;
    }
    uint32_t** spv[2] = { &prof->spv_ter, &prof->spv_portal };
    size_t* words_n[2] = { &prof->spv_ter_words, &prof->spv_portal_words };
    for (int p = 0; p < 2; p++) {
        if (!*spv[p] || !openpete_mod_link_discard_refused(pin, *spv[p], *words_n[p])) continue;
        OPL_ERROR_F("mods", "%s: shader %s: %s profile's op_material reaches a discard - "
                    "refused, its draws stay stock", id, path, p ? "portal" : "terrain");
        free(*spv[p]);
        *spv[p] = NULL;
        *words_n[p] = 0;
    }
    return 0;
}

/** @brief Log a material fragment's composition verdict per profile. */
static void shader_link_log(const char* id, const char* path, const mod_shader_t* prof) {
    static const char* const k_prof[OPENPETE_MOD_LINK_PROFILES] = { "stock", "terrain", "portal" };
    for (int p = 0; p < prof->link_n; p++) {
        const openpete_mod_link_member_t* m = &prof->link[p];
        OPL_INFO_F("mods", "%s: shader %s [%s]: composition %s%s%s%s", id, path, k_prof[p],
                   openpete_mod_link_verdict_name(m->verdict), m->identity ? ", identity" : "",
                   m->reason[0] ? " - " : "", m->reason);
    }
}

/** @brief Inspect the composition member of each profile of a material fragment at registration (mod_mat_link.h).
 *
 * Reads SPIR-V only: identity, discard and the block layout, verdict
 * pending until openpete_mods_shader_link_resolve runs the source checks
 * and the one-member compile. A prebuilt (no source) is unlinkable on every
 * profile; a profile with no module (its compile failed or it was refused)
 * is unlinkable.
 * @param has_src Nonzero when the shader was compiled from source.
 * @param words The stock module, before the table move.
 * @param n Its word count.
 * @param prof The extra profiles; receives link and link_n. */
static void shader_link_members(int has_src, const uint32_t* words, size_t n, mod_shader_t* prof) {
    const int contract = shader_spv_contract(words, n);
    const uint32_t* spv[OPENPETE_MOD_LINK_PROFILES] = { words, prof->spv_ter, prof->spv_portal };
    const size_t wn[OPENPETE_MOD_LINK_PROFILES] = { n, prof->spv_ter_words, prof->spv_portal_words };
    prof->link_n = contract == OPENPETE_MATERIAL_CONTRACT_SCENE ? OPENPETE_MOD_LINK_PROFILES : 1;
    for (int p = 0; p < prof->link_n; p++) {
        openpete_mod_link_member_t* m = &prof->link[p];
        if (!spv[p]) {
            memset(m, 0, sizeof *m);
            m->verdict = OPENPETE_MOD_LINK_COMPILE;
            m->contract = contract;
            m->profile = p;
            m->block_fields = -1;
            snprintf(m->reason, sizeof m->reason, "no module in this profile");
        } else if (has_src) {
            openpete_mod_link_inspect(contract, p, spv[p], wn[p], m);
        } else {
            openpete_mod_link_prebuilt(contract, p, spv[p], wn[p], m);
        }
    }
}

/** @brief Apply the discard refusal to a material fragment's modules and inspect its composition members.
 *
 * @param pin The owner's api pin.
 * @param has_src Nonzero when the shader was compiled from source.
 * @param id The mod id, for log lines.
 * @param path The shader's mod-relative path, for log lines.
 * @param words The stock module.
 * @param n Its word count.
 * @param prof The extra profiles; receives link and link_n.
 * @return 0, or -1 when the stock module is refused. */
static int shader_material_members(int pin, int has_src, const char* id, const char* path,
                                   const uint32_t* words, size_t n, mod_shader_t* prof) {
    if (shader_refuse_discard(pin, id, path, words, n, prof) != 0) return -1;
    shader_link_members(has_src, words, n, prof);
    return 0;
}

/** @brief Decide the pending composition members of one shader: preprocess, source checks and the one-member compile through its compile cache.
 *
 * Reads the source again from `<mod>/<path>` and recomputes its build key;
 * a source that no longer reads, or whose key cannot be computed, leaves
 * the members pending. Logs each decided profile's verdict.
 * @param sh The shader.
 * @return The number of profiles decided. */
static int shader_link_decide(mod_shader_t* sh) {
    static const char* const k_suffix[OPENPETE_MOD_LINK_PROFILES] = { ".link", ".terrain.link",
                                                                      ".portal.link" };
    openpete_mod_link_ctx_t* ctx = link_ctx();
    char src_path[MOD_SHADER_PATH_MAX * 3], key[65], build_dir[MOD_SHADER_PATH_MAX * 3];
    char flat[MOD_SHADER_PATH_MAX], log_path[MOD_SHADER_PATH_MAX * 4];
    snprintf(src_path, sizeof src_path, "%s/%s", sh->mod_dir, sh->path);
    snprintf(build_dir, sizeof build_dir, "%s/.build/shaders", sh->mod_dir);
    flatten_path(sh->path, flat, sizeof flat);
    snprintf(log_path, sizeof log_path, "%s/%s.link.log", build_dir, flat);
    char* src = ctx ? openpete_mod_glsl_read_file(src_path, NULL) : NULL;
    if (!src || shader_compute_key(sh->mod_dir, sh->path, key) != 0) { free(src); return 0; }
    openpete_mod_glsl_include_t inc;
    openpete_mod_glsl_include_init(&inc, src_path, openpete_mods_sdk_dir());
    int decided = 0;
    for (int p = 0; p < sh->link_n; p++) {
        if (sh->link[p].verdict != OPENPETE_MOD_LINK_PENDING) continue;
        link_cache_t c = { &inc, key, build_dir, flat, k_suffix[p], sh->owner, sh->path };
        openpete_mod_link_prepare(ctx, src, src_path, link_compile_cached, &c, log_path, &sh->link[p]);
        decided++;
    }
    free(src);
    if (decided) shader_link_log(sh->owner, sh->path, sh);
    return decided;
}

/** @brief Load an author-shipped prebuilt module: `<path>.spv` beside a missing source, accepted only with a `<path>.spv.ihash` sidecar equal to the current interface hash.
 *
 * @param id The mod id, for log lines.
 * @param path The shader's mod-relative path, for log lines.
 * @param src_path The missing source's full path.
 * @param words Receives the malloc'd words.
 * @param n_words Receives their count.
 * @return 0, or -1 (logged) for a missing or stale sidecar or a missing or malformed module. */
static int shader_load_prebuilt(const char* id, const char* path, const char* src_path,
                                uint32_t** words, size_t* n_words) {
    char pre_path[MOD_SHADER_PATH_MAX * 2 + 8];
    char side_path[MOD_SHADER_PATH_MAX * 2 + 16];
    snprintf(pre_path, sizeof pre_path, "%s.spv", src_path);
    snprintf(side_path, sizeof side_path, "%s.spv.ihash", src_path);
    size_t side_len = 0;
    char* side = openpete_mod_glsl_read_file(side_path, &side_len);
    if (!side) {
        OPL_ERROR_F("mods", "%s: shader %s: no source and no %s.spv.ihash "
                    "sidecar - refusing the prebuilt (unverifiable "
                    "interface)", id, path, path);
        return -1;
    }
    while (side_len && (side[side_len - 1] == '\n' ||
                        side[side_len - 1] == '\r'))
        side[--side_len] = '\0';
    char want[65];
    interface_hash(want);
    const int stale = strcmp(side, want) != 0;
    free(side);
    if (stale) {
        OPL_ERROR_F("mods", "%s: shader %s: prebuilt .spv was built "
                    "against a DIFFERENT shader interface - refused "
                    "(re-issue with the current SDK)", id, path);
        return -1;
    }
    size_t len = 0;
    char* pre = openpete_mod_glsl_read_file(pre_path, &len);
    if (!pre || len < 4 || (len & 3u) != 0) {
        OPL_ERROR_F("mods", "%s: shader %s: missing/malformed %s.spv",
                    id, path, path);
        free(pre);
        return -1;
    }
    *words = (uint32_t*)pre;
    *n_words = len / 4;
    OPL_INFO_F("mods", "%s: shader %s: prebuilt .spv accepted (%zu words)",
               id, path, *n_words);
    return 0;
}

/** @brief Free a registration's modules before they reach the table: the stock words, the extra profiles and the composition members. */
static void shader_modules_free(uint32_t* words, mod_shader_t* prof) {
    free(words);
    free(prof->spv_ter);
    free(prof->spv_portal);
    for (int p = 0; p < prof->link_n; p++) openpete_mod_link_member_free(&prof->link[p]);
}

/* ------------------------------------------------------------------ */
/* the service                                                         */

/** @brief Body of openpete_mods_shader_register, run under the registry write lock. */
static int openpete_mods_shader_register_locked(openpete_mod_t* self, const char* path) {
    const char* id = openpete_mods_id_of(self);
    const char* mod_dir = openpete_mods_dir_of(self);
    if (!id || !mod_dir || !path || !*path) return -1;
    if (openpete_mods_in_present) {
        OPL_ERROR_F("mods", "%s: shader_register refused inside a present "
                    "hook (file IO; register at entry)", id);
        return -1;
    }
    if (strstr(path, "..")) return -1;    /* stay inside the mod dir */
    if (strlen(path) >= MOD_SHADER_PATH_MAX) return -1;
    int stage = openpete_mod_glsl_stage_from_path(path);
    if (stage < 0) {
        OPL_ERROR_F("mods", "%s: shader %s: unknown stage (want .vert/.frag)",
                    id, path);
        return -1;
    }
    /* cache paths */
    char build_dir[MOD_SHADER_PATH_MAX * 2];
    snprintf(build_dir, sizeof build_dir, "%s/.build", mod_dir);
    shader_mkdir(build_dir);
    snprintf(build_dir, sizeof build_dir, "%s/.build/shaders", mod_dir);
    shader_mkdir(build_dir);
    char flat[MOD_SHADER_PATH_MAX];
    flatten_path(path, flat, sizeof flat);
    char log_path[MOD_SHADER_PATH_MAX * 3];
    snprintf(log_path, sizeof log_path, "%s/%s.log", build_dir, flat);

    char src_path[MOD_SHADER_PATH_MAX * 2];
    snprintf(src_path, sizeof src_path, "%s/%s", mod_dir, path);

    uint32_t* words = NULL;
    size_t n_words = 0;
    mod_shader_t prof;                 /* the extra material profiles */
    memset(&prof, 0, sizeof prof);

    size_t src_len = 0;
    char* src = openpete_mod_glsl_read_file(src_path, &src_len);
    char key[65] = "";
    openpete_mod_glsl_include_t ctx;
    if (src) {
        if (shader_compute_key(mod_dir, path, key) != 0) { free(src); return -1; }
        openpete_mod_glsl_include_init(&ctx, src_path, openpete_mods_sdk_dir());

        n_words = compile_profile(src, stage, &ctx, key, build_dir, flat,
                                  "", NULL, log_path, id, path, &words);
        if (n_words == 0) {
            OPL_ERROR_F("mods", "%s: shader %s: compile FAILED (see %s) "
                        "- effect unavailable", id, path, log_path);
            free(src);
            return -1;
        }
        compile_material_profiles(src, stage, &ctx, key, build_dir, flat, log_path,
                                  id, path, &prof);
    } else if (shader_load_prebuilt(id, path, src_path, &words, &n_words) != 0) {
        return -1;
    }
    const int refused = shader_is_material(stage, words, n_words) &&
        shader_material_members(openpete_mods_api_pin_of(self), src != NULL, id, path, words,
                                n_words, &prof) != 0;
    free(src);
    if (refused) { shader_modules_free(words, &prof); return -1; }

    if (!s_shaders.elem_size)
        mod_cvec_init(&s_shaders, sizeof(mod_shader_t), MOD_SHADER_BLOCK,
                      "shaders", MOD_SHADER_BLOCK);
    mod_shader_t* sh = (mod_shader_t*)mod_cvec_push(&s_shaders);
    if (!sh) {
        OPL_ERROR_F("mods", "%s: shader table allocation failed", id);
        shader_modules_free(words, &prof);
        return -1;
    }
    sh->owner = id;
    snprintf(sh->path, sizeof sh->path, "%s", path);
    snprintf(sh->mod_dir, sizeof sh->mod_dir, "%s", mod_dir);
    sh->stage = stage;
    sh->spv = words;
    sh->spv_words = n_words;
    sh->spv_ter = prof.spv_ter;
    sh->spv_ter_words = prof.spv_ter_words;
    sh->spv_portal = prof.spv_portal;
    sh->spv_portal_words = prof.spv_portal_words;
    sh->contract = shader_spv_contract(words, n_words);
    memcpy(sh->link, prof.link, sizeof sh->link);
    sh->link_n = prof.link_n;
    memset(sh->cdata, 0, sizeof sh->cdata);
    memset(sh->cdata_gpu, 0, sizeof sh->cdata_gpu);
    shader_move_tables(sh, id);
    sh->live = 1;
    if (link_eager()) shader_link_decide(sh);
    return shader_count() - 1;
}
int openpete_mods_shader_register(openpete_mod_t* self, const char* path) { REG_WR(); int r_ = openpete_mods_shader_register_locked(self, path); REG_WR_END(); return r_; }


const uint32_t* openpete_mods_shader_spv(int handle, size_t* words,
                                         int* stage) {
    const mod_shader_t* sh;
    if (handle < 0 || handle >= shader_count()) return NULL;
    sh = shader_at(handle);
    if (!sh->live) return NULL;
    if (words) *words = sh->spv_words;
    if (stage) *stage = sh->stage;
    return sh->spv;
}

/* The retained-terrain profile of a material fragment; NULL when it has none. */
const uint32_t* openpete_mods_shader_spv_terrain(int handle, size_t* words) {
    const mod_shader_t* sh;
    if (handle < 0 || handle >= shader_count()) return NULL;
    sh = shader_at(handle);
    if (!sh->live) return NULL;
    if (words) *words = sh->spv_ter_words;
    return sh->spv_ter;
}

/* The portal profile of a material fragment; NULL when it has none. */
const uint32_t* openpete_mods_shader_spv_portal(int handle, size_t* words) {
    const mod_shader_t* sh;
    if (handle < 0 || handle >= shader_count()) return NULL;
    sh = shader_at(handle);
    if (!sh->live) return NULL;
    if (words) *words = sh->spv_portal_words;
    return sh->spv_portal;
}

const struct openpete_mod_link_member* openpete_mods_shader_link(int handle, int profile) {
    if (handle < 0 || handle >= shader_count()) return NULL;
    const mod_shader_t* sh = shader_at(handle);
    if (!sh->live || profile < 0 || profile >= sh->link_n) return NULL;
    return &sh->link[profile];
}

/** @brief Copy a fragment shader's composition verdicts onto every material it shades: bit p of the link mask for each linkable profile p, of the identity mask for each identity profile.
 *
 * @param handle The fragment shader handle.
 * @param sh The shader. */
static void shader_link_to_materials(int handle, const mod_shader_t* sh) {
    unsigned link = 0, ident = 0;
    for (int p = 0; p < sh->link_n; p++) {
        if (sh->link[p].verdict == OPENPETE_MOD_LINK_OK) link |= 1u << p;
        if (sh->link[p].identity) ident |= 1u << p;
    }
    mod_mat_set_link_of_frag(handle, link, ident);
}

int openpete_mods_shader_link_resolve(int handle) {
    if (handle < 0 || handle >= shader_count()) return 0;
    mod_shader_t* sh = shader_at(handle);
    if (!sh->live) return 0;
    const int decided = shader_link_decide(sh);
    if (decided) shader_link_to_materials(handle, sh);
    return decided;
}

/** @brief Describe one fragment profile's moved constant tables for the renderer.
 *
 * @param handle Shader handle; out of range, swept or negative gives none.
 * @param profile 0 stock, 1 terrain, 2 portal.
 * @param out Receives the table block bytes, their length (0 = none) and
 *        the renderer's cache slot for the device copy. */
static void shader_cdata(int handle, int profile, openpete_mod_cdata_t* out) {
    memset(out, 0, sizeof *out);
    if (handle < 0 || handle >= shader_count()) return;
    mod_shader_t* sh = shader_at(handle);
    if (!sh->live || !sh->cdata[profile].len) return;
    out->bytes = sh->cdata[profile].bytes;
    out->len = sh->cdata[profile].len;
    out->gpu = &sh->cdata_gpu[profile];
}

/** @brief Body of openpete_mods_shader_remove_owner, run under the registry write lock.
 *
 * Frees the SPIR-V profiles, moved tables and composition members of every
 * live shader the mod owns and marks the slot dead; the slot is not reused,
 * so a stale handle reads as swept. */
static void openpete_mods_shader_remove_owner_locked(const char* mod_id) {
    for (int i = 0; i < shader_count(); i++) {
        mod_shader_t* sh = shader_at(i);
        if (!sh->live || !sh->owner) continue;
        if (strcmp(sh->owner, mod_id) != 0) continue;
        free(sh->spv);
        sh->spv = NULL;
        sh->spv_words = 0;
        free(sh->spv_ter);
        sh->spv_ter = NULL;
        sh->spv_ter_words = 0;
        free(sh->spv_portal);
        sh->spv_portal = NULL;
        sh->spv_portal_words = 0;
        for (int p = 0; p < 3; p++) {
            free(sh->cdata[p].bytes);
            memset(&sh->cdata[p], 0, sizeof sh->cdata[p]);
        }
        for (int p = 0; p < sh->link_n; p++) openpete_mod_link_member_free(&sh->link[p]);
        sh->link_n = 0;
        sh->live = 0;   /* slot stays dead: stale handles error */
    }
}
void openpete_mods_shader_remove_owner(const char* mod_id) { REG_WR(); openpete_mods_shader_remove_owner_locked(mod_id); REG_WR_END(); }


/* Watch key of <mod>/shaders/ for the loader's hot-reload poll. */
int openpete_mods_shader_watch_key(const char* mod_dir, char key_out[65]) {
    char dir[MOD_SHADER_PATH_MAX * 2];
    snprintf(dir, sizeof dir, "%s/shaders", mod_dir);
    char names[64][128];
    int n = list_dir_sorted(dir, ".vert;.frag;.glsl;.spv;.ihash", names, 64);
    if (n <= 0) { snprintf(key_out, 65, "-"); return 0; }
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    for (int i = 0; i < n; i++) {
        char p[MOD_SHADER_PATH_MAX * 2 + 128];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        key_update_str(&hc, names[i]);
        (void)key_update_file(&hc, p);
    }
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key_out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* STUB_ONCE logs one warning per use site that a mod API entry is not
 * available in this engine build. No entry point in this file uses it. */

#define STUB_ONCE(what)                                                     \
    do {                                                                    \
        static int s_warned = 0;                                            \
        if (!s_warned) {                                                    \
            s_warned = 1;                                                   \
            OPL_WARN_F("mods", "%s: " what " is not available in this "     \
                       "engine build yet", openpete_mods_id_of(self));      \
        }                                                                   \
    } while (0)

/* ------------------------------------------------------------------ */
/* postfx: registry + the renderer-facing collect. The GPU
 * pipeline object is executor-owned (native_render.c) but cached in
 * the slot via pass.pipeline_slot; a sweep moves it to the retirement
 * list, drained by the renderer under its own frame discipline. */

/* Passes the postfx table's first block holds, and retired pipelines the
 * drain list allocates up front; both grow past these. */
#define MOD_POSTFX_BLOCK 64

typedef struct {
    const char* owner;
    int  shader;                 /* frag handle (s_shaders) */
    int  point;                  /* OP_POSTFX_* */
    int  order;                  /* tie-break within a mod */
    int  load_index;             /* enabled-list position of the owner */
    int  enabled;
    int  live;
    void* pipeline;              /* executor-cached rhi_pipeline* */
    unsigned generation;         /* slot_generation_next at register and at each retirement */
    unsigned char uniforms[OPENPETE_POSTFX_UNIFORMS_MAX];
    uint32_t uniforms_len;
} mod_postfx_t;

/* Slot generations: one registry-wide counter, so a value names one
 * (registration, retirement) epoch of one slot. A compile job records the
 * generation its slot had at enqueue; a slot whose generation moved since
 * was retired or swept, and the job's result is stale. Written under the
 * registry write lock. */
static unsigned s_slot_generation;

/** @brief Return a fresh slot generation.
 *
 * @return The next value of the registry-wide counter, never 0. */
static unsigned slot_generation_next(void) {
    if (++s_slot_generation == 0) s_slot_generation = 1;
    return s_slot_generation;
}

/* Chunked: the renderer holds &fx->pipeline as its executor-cached
 * pipeline slot across presents. */
static mod_cvec s_fx;               /* mod_postfx_t */
static mod_fvec s_retired_pipelines; /* void*, drained by the renderer */

/** @brief Append a cached pipeline to the retirement list the renderer drains.
 *
 * NULL, OPENPETE_POSTFX_PIPE_FAILED and OPENPETE_POSTFX_PIPE_PENDING (a
 * compile in flight, which the stale generation discards) are ignored. The
 * list grows, so a
 * sweep that retires every variant of a material at once keeps them all;
 * an allocation failure logs an error and leaks that pipeline. The
 * renderer takes the list with openpete_mods_postfx_take_retired() and
 * destroys each pipeline. */
static void retire_pipeline(void* pipe) {
    void** slot;
    if (!pipe || pipe == OPENPETE_POSTFX_PIPE_FAILED || pipe == OPENPETE_POSTFX_PIPE_PENDING)
        return;
    if (!s_retired_pipelines.elem_size)
        mod_fvec_init(&s_retired_pipelines, sizeof(void*),
                      MOD_POSTFX_BLOCK, "postfx_retired", MOD_POSTFX_BLOCK);
    slot = (void**)mod_fvec_push(&s_retired_pipelines);
    if (!slot) {
        OPL_ERROR_F("mods", "postfx: retirement list allocation failed - "
                    "a cached pipeline is leaked");
        return;
    }
    *slot = pipe;
}

/** @brief Return the number of slots in the postfx table, live or swept. */
static int fx_count(void) { return (int)mod_cvec_count(&s_fx); }
/** @brief Return the postfx table slot at an index; the caller bounds the index by fx_count(). */
static mod_postfx_t* fx_at(int i) {
    return (mod_postfx_t*)mod_cvec_at(&s_fx, (size_t)i);
}

/** @brief Body of openpete_mods_postfx_register, run under the registry write lock.
 *
 * Appends a disabled pass that records the owner, the shader handle, the
 * insertion point, the order and the owner's load index. */
static int openpete_mods_postfx_register_locked(openpete_mod_t* self, int frag_shader,
                                  int point, int order) {
    const char* id = openpete_mods_id_of(self);
    if (!id) return -1;
    if (openpete_mods_in_present) {
        OPL_ERROR_F("mods", "%s: postfx_register refused inside a present "
                    "hook (register at entry)", id);
        return -1;
    }
    if (point != 0 /* COMPOSITE */ && point != 1 /* SCENE */) {
        OPL_WARN_F("mods", "%s: postfx point %d not available "
                   "(COMPOSITE / SCENE; PRESENT is future)", id, point);
        return -1;
    }
    size_t words = 0; int stage = -1;
    if (!openpete_mods_shader_spv(frag_shader, &words, &stage) ||
        stage != MOD_SHADER_STAGE_FRAG) {
        OPL_ERROR_F("mods", "%s: postfx_register: bad fragment shader "
                    "handle %d", id, frag_shader);
        return -1;
    }
    if (!s_fx.elem_size)
        mod_cvec_init(&s_fx, sizeof(mod_postfx_t), MOD_POSTFX_BLOCK,
                      "postfx", MOD_POSTFX_BLOCK);
    mod_postfx_t* fx = (mod_postfx_t*)mod_cvec_push(&s_fx);
    if (!fx) {
        OPL_ERROR_F("mods", "%s: postfx table allocation failed", id);
        return -1;
    }
    fx->owner = id;
    fx->shader = frag_shader;
    fx->point = point;
    fx->order = order;
    fx->load_index = openpete_mods_load_index_of(self);
    fx->enabled = 0;             /* explicit postfx_enable arms it */
    fx->live = 1;
    fx->generation = slot_generation_next();
    return fx_count() - 1;
}
int openpete_mods_postfx_register(openpete_mod_t* self, int frag_shader,
                                  int point, int order) { REG_WR(); int r_ = openpete_mods_postfx_register_locked(self, frag_shader, point, order); REG_WR_END(); return r_; }


/** @brief Body of openpete_mods_postfx_enable, run under the registry write lock.
 *
 * Refuses a pass another mod owns. */
static int openpete_mods_postfx_enable_locked(openpete_mod_t* self, int fx, int enabled) {
    const char* id = openpete_mods_id_of(self);
    if (!id || fx < 0 || fx >= fx_count() || !fx_at(fx)->live) return -1;
    if (strcmp(fx_at(fx)->owner, id) != 0) return -1;   /* not yours */
    fx_at(fx)->enabled = enabled ? 1 : 0;
    return 0;
}
int openpete_mods_postfx_enable(openpete_mod_t* self, int fx, int enabled) { REG_WR(); int r_ = openpete_mods_postfx_enable_locked(self, fx, enabled); REG_WR_END(); return r_; }


/** @brief Body of openpete_mods_postfx_set_uniforms, run under the registry write lock.
 *
 * Copies at most OPENPETE_POSTFX_UNIFORMS_MAX bytes; a longer block is
 * truncated and still succeeds. */
static int openpete_mods_postfx_set_uniforms_locked(openpete_mod_t* self, int fx,
                                      const void* data, uint32_t size) {
    const char* id = openpete_mods_id_of(self);
    if (!id || fx < 0 || fx >= fx_count() || !fx_at(fx)->live || !data)
        return -1;
    if (strcmp(fx_at(fx)->owner, id) != 0) return -1;
    if (size > OPENPETE_POSTFX_UNIFORMS_MAX) size = OPENPETE_POSTFX_UNIFORMS_MAX;
    memcpy(fx_at(fx)->uniforms, data, size); /* copy-on-call, any context */
    fx_at(fx)->uniforms_len = size;
    return 0;
}
int openpete_mods_postfx_set_uniforms(openpete_mod_t* self, int fx,
                                      const void* data, uint32_t size) { REG_WR(); int r_ = openpete_mods_postfx_set_uniforms_locked(self, fx, data, size); REG_WR_END(); return r_; }


/** @brief Warn once per run per insertion point that the enabled postfx passes exceed the renderer's cap.
 *
 * The renderer's pass ceiling is its own bound, so a set that exceeds it
 * keeps the highest-priority passes; the line names the number of dropped
 * passes and is not repeated per present. Points outside the table are
 * ignored.
 * @param point Postfx insertion point (OP_POSTFX_*).
 * @param have Enabled passes at the point.
 * @param cap Passes the renderer runs. */
static void postfx_truncation_notice(int point, int have, int cap) {
    static unsigned char s_said[OP_POSTFX_PRESENT + 1];
    if (point < 0 || point >= (int)(sizeof s_said / sizeof s_said[0])) return;
    if (s_said[point]) return;
    s_said[point] = 1;
    OPL_WARN_F("mods", "postfx point %d: %d enabled passes, the renderer "
               "runs %d - the %d lowest-priority pass(es) are dropped",
               point, have, cap, have - cap);
}

int openpete_mods_postfx_collect(int point, openpete_postfx_pass_t* out,
                                 int cap) {
    /* The whole enabled set is collected and sorted into run order, lowest
     * priority first, before the cap is applied; mod_postfx_rank drops the
     * lowest-priority passes, so the passes the renderer runs are the
     * highest-priority ones. Static, cleared per call: this runs per
     * present and the storage is worth keeping. */
    static mod_fvec s_idx;   /* mod_postfx_rank_t, one per enabled pass */
    int n, have;
    if (!s_idx.elem_size)
        mod_fvec_init(&s_idx, sizeof(mod_postfx_rank_t), MOD_POSTFX_BLOCK, NULL, 0);
    mod_fvec_clear(&s_idx);
    for (int i = 0; i < fx_count(); i++) {
        const mod_postfx_t* f = fx_at(i);
        mod_postfx_rank_t* slot;
        if (!f->live || !f->enabled || f->point != point) continue;
        if (!openpete_mods_is_enabled(f->owner)) continue;  /* live toggle */
        if (mod_owner_held(f->owner)) continue;             /* activates next present */
        slot = (mod_postfx_rank_t*)mod_fvec_push(&s_idx);
        if (!slot) break;
        slot->index = i;
        slot->load_index = f->load_index;
        slot->order = f->order;
    }
    have = (int)mod_fvec_count(&s_idx);
    mod_postfx_rank_t* rank = (mod_postfx_rank_t*)mod_fvec_data(&s_idx);
    n = mod_postfx_rank(rank, have, cap);
    if (have > cap) postfx_truncation_notice(point, have, cap);
    for (int i = 0; i < n; i++) {
        const int idx = rank[i].index;
        mod_postfx_t* fx = fx_at(idx);
        out[i].frag_spv = openpete_mods_shader_spv(fx->shader,
                                                   &out[i].frag_words, NULL);
        out[i].index = idx;
        out[i].point = fx->point;
        out[i].generation = fx->generation;
        out[i].uniforms = fx->uniforms;
        out[i].uniforms_len = fx->uniforms_len;
        out[i].pipeline_slot = &fx->pipeline;
        out[i].owner = fx->owner;
    }
    return n;
}

int openpete_mods_postfx_enum_count(void) { return fx_count(); }

int openpete_mods_postfx_enum_at(int i, openpete_postfx_pass_t* out) {
    if (!out || i < 0 || i >= fx_count()) return -1;
    mod_postfx_t* fx = fx_at(i);
    if (!fx->live) return -1;
    memset(out, 0, sizeof *out);
    out->frag_spv = openpete_mods_shader_spv(fx->shader, &out->frag_words, NULL);
    if (!out->frag_spv) return -1;
    out->pipeline_slot = &fx->pipeline;
    out->owner = fx->owner;
    out->generation = fx->generation;
    out->index = i;
    out->point = fx->point;
    return 0;
}

/* Drain executor-cached pipelines orphaned by a sweep (hot reload).
 * The renderer destroys them under its own synchronization. */
int openpete_mods_postfx_take_retired(void** out, int cap) {
    int have = (int)mod_fvec_count(&s_retired_pipelines);
    int n = have < cap ? have : cap;
    void** base = (void**)mod_fvec_data(&s_retired_pipelines);
    if (n <= 0) return 0;
    memcpy(out, base, (size_t)n * sizeof(void*));
    memmove(base, base + n, (size_t)(have - n) * sizeof(void*));
    mod_fvec_truncate(&s_retired_pipelines, (size_t)(have - n));
    return n;
}

/** @brief Move the cached pipeline of every live postfx pass, enabled or not, to the retirement list.
 *
 * Used at shutdown, under the registry write lock; the registrations stay
 * live and rebuild their pipelines on next use. */
static void openpete_mods_postfx_retire_all_locked(void) {
    for (int i = 0; i < fx_count(); i++) {
        mod_postfx_t* f = fx_at(i);
        if (!f->live) continue;
        retire_pipeline(f->pipeline);
        f->pipeline = NULL;
        f->generation = slot_generation_next();
    }
}
void openpete_mods_postfx_retire_all(void) { REG_WR(); openpete_mods_postfx_retire_all_locked(); REG_WR_END(); }


/** @brief Body of openpete_mods_postfx_remove_owner, run under the registry write lock.
 *
 * Retires the cached pipeline of each live pass the mod owns and marks the
 * slot dead. */
static void openpete_mods_postfx_remove_owner_locked(const char* mod_id) {
    for (int i = 0; i < fx_count(); i++) {
        mod_postfx_t* f = fx_at(i);
        if (!f->live || strcmp(f->owner, mod_id) != 0) continue;
        retire_pipeline(f->pipeline);
        f->pipeline = NULL;
        f->live = 0;
        f->generation = slot_generation_next();
    }
}
void openpete_mods_postfx_remove_owner(const char* mod_id) { REG_WR(); openpete_mods_postfx_remove_owner_locked(mod_id); REG_WR_END(); }


/* ------------------------------------------------------------------ */
/* material selectors (declarative + per-instance): the GPU half.
 *
 * The material table, its predicates, winner scans, refine calls and slot
 * interning are in mod_material_resolve.c. This half validates shader
 * handles, takes the registry lock around each mutator, and fills the
 * renderer's GPU descriptions (SPIR-V, pipeline slots, constant tables). */

int openpete_mods_material_terrain_bucket_gpu(
        int bucket, int pipe_idx, openpete_mod_material_terrain_t* out) {
    if (!out || bucket <= 0 || bucket > mod_mat_count()) return -1;
    if (pipe_idx < 0 || pipe_idx >= OPENPETE_MATERIAL_TER_PIPES) return -1;
    mod_material_t* m = mod_mat_at(bucket - 1);
    if (!m->live || !m->user_enabled || !openpete_mods_is_enabled(m->owner) ||
        mod_owner_held(m->owner))
        return -1;
    if (m->frag < 0) return -1;
    if (m->per_instance) {
        /* The outcome the sim thread computed for the composed scene; no
         * callback runs on the compose. A pass lists a refine only when a
         * class resolved to it, so a missing outcome is a pass from
         * another present's tables: those faces are already claimed out
         * of the stock pass, so the caller draws them with the STOCK
         * pipeline. */
        int slot = mod_mat_terrain_bucket_slot(bucket - 1);
        if (slot == OPENPETE_MATERIAL_TERRAIN_STOCK || slot <= 0) return -1;
        out->uniform_slot = slot;
    } else {
        out->uniform_slot = mod_mat_view_static_slot(bucket - 1);
    }
    out->frag_spv = openpete_mods_shader_spv_terrain(m->frag, &out->frag_words);
    if (!out->frag_spv || out->frag_words == 0) return -1;
    out->pipeline_slot = &m->pipeline_ter[pipe_idx];
    shader_cdata(m->frag, 1, &out->frag_cdata);
    out->owner = m->owner;
    out->generation = m->generation;
    out->index = bucket - 1;
    mod_mat_census_bucket_pass(bucket - 1);
    return 0;
}

/** @brief Body of openpete_mods_material_register, run under the registry write lock.
 *
 * Validates the selector, the shader handles and, for a refine material,
 * where the refine can take effect (mod_mat_refine_valid), then appends the material
 * through mod_mat_append, which registers it enabled, bumps the terrain
 * epoch when it could bucket terrain, and re-arms the tag split. */
static int openpete_mods_material_register_locked(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance) {
    const char* id = openpete_mods_id_of(self);
    if (!id || !sel) return -1;
    if (openpete_mods_in_present) {
        OPL_ERROR_F("mods", "%s: material_register refused inside a present "
                    "hook - register at entry, and retune a live selector "
                    "with material_set_selector", id);
        return -1;
    }
    if (!mod_mat_sel_valid(id, sel)) return -1;
    if (frag_shader >= 0) {
        size_t w = 0; int st = -1;
        if (!openpete_mods_shader_spv(frag_shader, &w, &st) ||
            st != MOD_SHADER_STAGE_FRAG) {
            OPL_ERROR_F("mods", "%s: material: bad frag handle %d", id,
                        frag_shader);
            return -1;
        }
    }
    if (vert_shader >= 0) {
        size_t w = 0; int st = -1;
        if (!openpete_mods_shader_spv(vert_shader, &w, &st) ||
            st != MOD_SHADER_STAGE_VERT) {
            OPL_ERROR_F("mods", "%s: material: bad vert handle %d", id,
                        vert_shader);
            return -1;
        }
    }
    const int contract = frag_shader >= 0 ? shader_at(frag_shader)->contract
                                          : OPENPETE_MATERIAL_CONTRACT_SCENE;
    if (per_instance && !mod_mat_refine_valid(id, sel, contract)) return -1;
    /* A generation is drawn only for a material the table accepts. */
    const int fits = (vert_shader >= 0 || frag_shader >= 0) && mod_mat_count() < MOD_MATERIAL_MAX;
    const int idx = mod_mat_append(id, sel, vert_shader, frag_shader, contract, per_instance,
                                   openpete_mods_load_index_of(self),
                                   openpete_mods_api_pin_of(self),
                                   fits ? slot_generation_next() : 0u);
    if (idx >= 0 && frag_shader >= 0) shader_link_to_materials(frag_shader, shader_at(frag_shader));
    return idx;
}
int openpete_mods_material_register(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance) { REG_WR(); int r_ = openpete_mods_material_register_locked(self, sel, vert_shader, frag_shader, per_instance); REG_WR_END(); return r_; }

int openpete_mods_material_enable(openpete_mod_t* self, int material, int on) { (void)self; REG_WR(); int r_ = mod_mat_enable(material, on); REG_WR_END(); return r_; }

int openpete_mods_material_set_params(openpete_mod_t* self, int material,
                                      const void* block, uint32_t len) { (void)self; REG_WR(); int r_ = mod_mat_set_params(material, block, len); REG_WR_END(); return r_; }

int openpete_mods_material_set_selector(
        openpete_mod_t* self, int material,
        const openpete_mod_material_selector_t* sel) { REG_WR(); int r_ = mod_mat_set_selector(openpete_mods_id_of(self), material, sel); REG_WR_END(); return r_; }

void openpete_mods_material_apply_pending(void) {
    if (!mod_mat_pending()) return;     /* nothing parked: never take the lock */
    REG_WR(); mod_mat_apply_pending(); REG_WR_END();
}

void openpete_mods_material_overlap_tick(void) {
    if (!mod_mat_overlap_due()) return;   /* nothing moved: never take the lock */
    { REG_WR(); mod_mat_overlap_snapshot(); REG_WR_END(); }
    mod_mat_overlap_compute();
    { REG_WR(); mod_mat_overlap_publish(); REG_WR_END(); }
}

int openpete_mods_material_overlap_count(void) {
    const int total = mod_mat_overlap_total();
    return total < MOD_MAT_OVERLAP_MAX ? total : MOD_MAT_OVERLAP_MAX;
}

int openpete_mods_material_overlap_total(void) { return mod_mat_overlap_total(); }

int openpete_mods_material_overlap_at(int i, openpete_mods_material_overlap_info_t* out) {
    const mod_mat_overlap_t* p = mod_mat_overlap_at(i);
    if (!p || !out) return -1;
    out->hi = p->hi;
    out->lo = p->lo;
    out->hi_owner = mod_mat_at(p->hi)->owner;
    out->lo_owner = mod_mat_at(p->lo)->owner;
    out->hi_refine = mod_mat_at(p->hi)->per_instance != NULL;
    out->lo_refine = mod_mat_at(p->lo)->per_instance != NULL;
    out->verdict = mod_mat_shadow_name(p->verdict);
    out->chans = p->chans;
    out->accept_all = p->accept_all;
    return 0;
}

/** @brief Fill a draw's GPU material description from a material index and a uniform slot.
 *
 * Copies the vertex and fragment SPIR-V (NULL with 0 words for a stock
 * stage), the pipeline variant slots, the stencil-mark pipeline slot and
 * the owner. The uniform slot is `uniform_slot` when positive, else the
 * material's static slot in the current view (0 = no params).
 * @return `best`. */
static int material_fill_gpu(int best, int uniform_slot,
                             openpete_mod_material_gpu_t* out) {
    mod_material_t* m = mod_mat_at(best);
    out->vert_spv = m->vert >= 0
        ? openpete_mods_shader_spv(m->vert, &out->vert_words, NULL) : NULL;
    out->frag_spv = m->frag >= 0
        ? openpete_mods_shader_spv(m->frag, &out->frag_words, NULL) : NULL;
    if (m->vert < 0) out->vert_words = 0;
    if (m->frag < 0) out->frag_words = 0;
    for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++)
        out->pipeline_slot[b] = &m->pipeline[b];
    out->pipeline_mark_slot = &m->pipeline_mark;
    out->frag_portal_spv = m->frag >= 0
        ? openpete_mods_shader_spv_portal(m->frag, &out->frag_portal_words) : NULL;
    if (!out->frag_portal_spv) out->frag_portal_words = 0;
    for (int b = 0; b < OPENPETE_MATERIAL_PORTAL_PIPES; b++)
        out->pipeline_portal_slot[b] = &m->pipeline_portal[b];
    shader_cdata(m->frag, 0, &out->frag_cdata);
    shader_cdata(m->frag, 2, &out->frag_portal_cdata);
    out->owner = m->owner;
    out->generation = m->generation;
    out->index = best;
    /* A per-instance slot always wins; otherwise fall back to this
     * material's static block (0 = no params at all). */
    out->uniform_slot = uniform_slot > 0 ? uniform_slot : mod_mat_view_static_slot(best);
    return best;
}

int openpete_mods_material_resolve(uint32_t tag, int contract,
                                   openpete_mod_material_gpu_t* out) {
    int slot = 0;
    const int best = mod_mat_resolve_index(tag, contract, &slot);
    if (best < 0) return -1;
    return material_fill_gpu(best, slot, out);
}

int openpete_mods_material_enum_count(void) { return mod_mat_count(); }

int openpete_mods_material_enum_at(int i, int* contract, int* terrain,
                                   openpete_mod_material_gpu_t* out) {
    if (!out || i < 0 || i >= mod_mat_count() || !mod_mat_at(i)->live) return -1;
    const mod_material_t* m = mod_mat_at(i);
    size_t words = 0;
    const int ter_sel = m->sel.channel == -1 || m->sel.channel == OP_CHAN_TERRAIN ||
        (m->sel_dirty && (m->sel_pending.channel == -1 ||
                          m->sel_pending.channel == OP_CHAN_TERRAIN));
    if (contract) *contract = m->contract;
    if (terrain)
        *terrain = m->contract == OPENPETE_MATERIAL_CONTRACT_SCENE && ter_sel && m->frag >= 0 &&
                   openpete_mods_shader_spv_terrain(m->frag, &words) != NULL && words > 0;
    material_fill_gpu(i, 0, out);
    return 0;
}

int openpete_mods_material_enum_terrain(int i, int pipe_idx,
                                        openpete_mod_material_terrain_t* out) {
    if (!out || i < 0 || i >= mod_mat_count() || !mod_mat_at(i)->live || mod_mat_at(i)->frag < 0) return -1;
    if (pipe_idx < 0 || pipe_idx >= OPENPETE_MATERIAL_TER_PIPES) return -1;
    mod_material_t* m = mod_mat_at(i);
    memset(out, 0, sizeof *out);
    out->frag_spv = openpete_mods_shader_spv_terrain(m->frag, &out->frag_words);
    if (!out->frag_spv || out->frag_words == 0) return -1;
    out->pipeline_slot = &m->pipeline_ter[pipe_idx];
    shader_cdata(m->frag, 1, &out->frag_cdata);
    out->owner = m->owner;
    out->generation = m->generation;
    out->index = i;
    return 0;
}

/** @brief Move a material's cached pipelines to the retirement list and clear them.
 *
 * @param m The material. */
static void material_retire_pipelines(mod_material_t* m) {
    for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++)
        retire_pipeline(m->pipeline[b]);
    retire_pipeline(m->pipeline_mark);
    for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++)
        retire_pipeline(m->pipeline_ter[b]);
    for (int b = 0; b < OPENPETE_MATERIAL_PORTAL_PIPES; b++) {
        retire_pipeline(m->pipeline_portal[b]);
        m->pipeline_portal[b] = NULL;
    }
    for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++) m->pipeline[b] = NULL;
    for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++) m->pipeline_ter[b] = NULL;
    m->pipeline_mark = NULL;
}

/** @brief Body of openpete_mods_material_remove_owner, run under the registry write lock.
 *
 * For each live material the mod owns: retires every cached pipeline, drops
 * a parked selector, marks the slot dead and bumps the terrain epoch. The
 * slot is not reused. Re-arms the tag split once. */
static void openpete_mods_material_remove_owner_locked(const char* mod_id) {
    for (int i = 0; i < mod_mat_count(); i++) {
        mod_material_t* m = mod_mat_at(i);
        if (!m->live || strcmp(m->owner, mod_id) != 0) continue;
        material_retire_pipelines(m);
        m->sel_dirty = 0;   /* a parked retune dies with the material */
        m->live = 0;
        m->generation = slot_generation_next();
        mod_mat_ter_epoch_bump();
        mod_mat_overlap_bump();
    }
    mod_mat_rearm_split();
}
void openpete_mods_material_remove_owner(const char* mod_id) { REG_WR(); openpete_mods_material_remove_owner_locked(mod_id); REG_WR_END(); }


/** @brief Body of openpete_mods_material_retire_all, run under the registry write lock.
 *
 * Moves the cached pipelines of every live material to the retirement list
 * and clears the slots; the materials stay live and rebuild on next use. */
static void openpete_mods_material_retire_all_locked(void) {
    for (int i = 0; i < mod_mat_count(); i++) {
        mod_material_t* m = mod_mat_at(i);
        if (!m->live) continue;
        material_retire_pipelines(m);
        m->generation = slot_generation_next();
    }
}
void openpete_mods_material_retire_all(void) { REG_WR(); openpete_mods_material_retire_all_locked(); REG_WR_END(); }
