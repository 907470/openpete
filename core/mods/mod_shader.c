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
 * The postfx pass registry and the material-selector tier (registration,
 * per-present slot interning, draw-time resolve, the retained-terrain
 * bucket queries) are implemented below the shader service in this file.
 */

#include "openpete_mods.h"
#include "openpete_atomic_file.h"
#include "openpete_log.h"
#include "mod_vec.h"
#include "openpete_native_render.h"   /* set_tag_split arming (material selectors) */
#include "../host/sha256.h"
#include "mod_glsl.h"

#ifndef OPENPETE_NO_GLSLANG
#include <glslang/build_info.h>
#endif

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
static SDL_ThreadID s_reg_reader_tid;
/** @brief Take the registry write lock unless the caller is the compose reader thread.
 *
 * The thread recorded by openpete_mods_registry_read_lock() already holds
 * the read side, so it proceeds unlocked instead of deadlocking.
 *
 * @return 1 when the write lock was taken, 0 when it was not. */
static int reg_wr_begin(void) {
    if (s_reg_reader_tid && s_reg_reader_tid == SDL_GetCurrentThreadID()) return 0;
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
    s_reg_reader_tid = SDL_GetCurrentThreadID();
}
void openpete_mods_registry_read_unlock(void) {
    s_reg_reader_tid = 0;
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
    int         live;        /* 0 = swept slot (stale handle) */
} mod_shader_t;

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
    if (src) {
        char key[65];
        if (shader_compute_key(mod_dir, path, key) != 0) { free(src); return -1; }
        openpete_mod_glsl_include_t ctx;
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
        free(src);
    } else {
        /* No source: author-shipped prebuilt "<path>.spv" + mandatory
         * "<path>.spv.ihash" interface sidecar. */
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
        if (strcmp(side, want) != 0) {
            OPL_ERROR_F("mods", "%s: shader %s: prebuilt .spv was built "
                        "against a DIFFERENT shader interface - refused "
                        "(re-issue with the current SDK)", id, path);
            free(side);
            return -1;
        }
        free(side);
        size_t len = 0;
        char* pre = openpete_mod_glsl_read_file(pre_path, &len);
        if (!pre || len < 4 || (len & 3u) != 0) {
            OPL_ERROR_F("mods", "%s: shader %s: missing/malformed %s.spv",
                        id, path, path);
            free(pre);
            return -1;
        }
        words = (uint32_t*)pre;
        n_words = len / 4;
        OPL_INFO_F("mods", "%s: shader %s: prebuilt .spv accepted (%zu words)",
                   id, path, n_words);
    }

    if (!s_shaders.elem_size)
        mod_cvec_init(&s_shaders, sizeof(mod_shader_t), MOD_SHADER_BLOCK,
                      "shaders", MOD_SHADER_BLOCK);
    mod_shader_t* sh = (mod_shader_t*)mod_cvec_push(&s_shaders);
    if (!sh) {
        OPL_ERROR_F("mods", "%s: shader table allocation failed", id);
        free(words);
        free(prof.spv_ter);
        free(prof.spv_portal);
        return -1;
    }
    sh->owner = id;
    snprintf(sh->path, sizeof sh->path, "%s", path);
    sh->stage = stage;
    sh->spv = words;
    sh->spv_words = n_words;
    sh->spv_ter = prof.spv_ter;
    sh->spv_ter_words = prof.spv_ter_words;
    sh->spv_portal = prof.spv_portal;
    sh->spv_portal_words = prof.spv_portal_words;
    sh->live = 1;
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

/** @brief Body of openpete_mods_shader_remove_owner, run under the registry write lock.
 *
 * Frees both SPIR-V profiles of every live shader the mod owns and marks
 * the slot dead; the slot is not reused, so a stale handle reads as swept. */
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
    unsigned char uniforms[OPENPETE_POSTFX_UNIFORMS_MAX];
    uint32_t uniforms_len;
} mod_postfx_t;

/* Chunked: the renderer holds &fx->pipeline as its executor-cached
 * pipeline slot across presents. */
static mod_cvec s_fx;               /* mod_postfx_t */
static mod_fvec s_retired_pipelines; /* void*, drained by the renderer */

/** @brief Append a cached pipeline to the retirement list the renderer drains.
 *
 * NULL and OPENPETE_POSTFX_PIPE_FAILED are ignored. The list grows, so a
 * sweep that retires every variant of a material at once keeps them all;
 * an allocation failure logs an error and leaks that pipeline. The
 * renderer takes the list with openpete_mods_postfx_take_retired() and
 * destroys each pipeline. */
static void retire_pipeline(void* pipe) {
    void** slot;
    if (!pipe || pipe == OPENPETE_POSTFX_PIPE_FAILED) return;
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
    /* The whole enabled set is collected and sorted before the cap is
     * applied, so the passes the renderer runs are the highest-priority
     * ones rather than whichever registered first. Static, cleared per
     * call: this runs per present and the storage is worth keeping. */
    static mod_fvec s_idx;   /* int, indices into s_fx */
    int n;
    if (!s_idx.elem_size)
        mod_fvec_init(&s_idx, sizeof(int), MOD_POSTFX_BLOCK, NULL, 0);
    mod_fvec_clear(&s_idx);
    for (int i = 0; i < fx_count(); i++) {
        const mod_postfx_t* f = fx_at(i);
        int* slot;
        if (!f->live || !f->enabled || f->point != point) continue;
        if (!openpete_mods_is_enabled(f->owner)) continue;  /* live toggle */
        slot = (int*)mod_fvec_push(&s_idx);
        if (!slot) break;
        *slot = i;
    }
    n = (int)mod_fvec_count(&s_idx);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            int* pj1 = (int*)mod_fvec_at(&s_idx, (size_t)j - 1);
            int* pj  = (int*)mod_fvec_at(&s_idx, (size_t)j);
            mod_postfx_t *a = fx_at(*pj1), *b = fx_at(*pj);
            int swap = (a->load_index > b->load_index) ||
                       (a->load_index == b->load_index && a->order > b->order) ||
                       (a->load_index == b->load_index && a->order == b->order &&
                        *pj1 > *pj);
            if (!swap) break;
            { int t = *pj1; *pj1 = *pj; *pj = t; }
        }
    if (n > cap) {
        postfx_truncation_notice(point, n, cap);
        n = cap;
    }
    for (int i = 0; i < n; i++) {
        mod_postfx_t* fx = fx_at(*(int*)mod_fvec_at(&s_idx, (size_t)i));
        out[i].frag_spv = openpete_mods_shader_spv(fx->shader,
                                                   &out[i].frag_words, NULL);
        out[i].uniforms = fx->uniforms;
        out[i].uniforms_len = fx->uniforms_len;
        out[i].pipeline_slot = &fx->pipeline;
        out[i].owner = fx->owner;
    }
    return n;
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
    }
}
void openpete_mods_postfx_remove_owner(const char* mod_id) { REG_WR(); openpete_mods_postfx_remove_owner_locked(mod_id); REG_WR_END(); }


/* ------------------------------------------------------------------ */
/* material selectors (declarative + per-instance).
 *
 * Declarative: whole-class overrides matched at DRAW time against the
 * provenance tag - extractors stay selector-agnostic. Per-instance: a
 * callback runs at EXTRACTION (the moby walk, cpu
 * available) per matching instance, returns a per-draw uniform block
 * or declines. Its outcome is interned to a small per-present SLOT id
 * stamped into the tag's spare bits, so the batcher splits draws by
 * outcome (two instances of one class with different callback results
 * land in different batches); the slot's uniform block binds at the
 * override draw. Conflicts: enabled-list priority, the later-loaded mod
 * wins.
 *
 * Per-present, not identity-replayed: reextract_record_pass re-walks
 * every present, and the callback reads TICK-STABLE guest RAM (the
 * sub-tick spine interpolates render-side only, never mutating
 * gameplay RAM - the oracle guarantees it), so per-present invocation
 * yields identical results across a canonical+intermediates span (no
 * strobing). Canonical-compute + identity-replay would only save the
 * callback cost on intermediates; deferred as a pure optimization. */

/* A real bound, not a table size: the bucket id stamped into the baked
 * face material is the index + 1, and it has 8 bits
 * (OPENPETE_MATERIAL_TER_BUCKET_MAX). A material registered above index
 * 254 would succeed and then never shade a terrain face. */
#define MOD_MATERIAL_MAX OPENPETE_MATERIAL_TER_BUCKET_MAX

typedef struct {
    const char* owner;
    openpete_mod_material_selector_t sel;
    int  vert, frag;       /* shader handles; -1 = stock stage */
    openpete_mod_material_fn per_instance;   /* NULL = declarative */
    int  load_index;
    /* Executor-cached rhi_pipeline*, one per BLEND CLASS (0 opaque, 1
     * average, 2 additive, 3 subtract). A material reshades inside
     * whatever group it lands in and must not change how that group
     * composites, so the same shader needs a pipeline per blend. */
    /* One built pipeline per draw-state variant this material can be
     * asked for: [0..3] = PSX blend classes, [4] = opaque with depth
     * write off (portal sky), [5] = the DECAL PROFILE (moby shadows:
     * a different vertex stage, descriptor sets and push block - see
     * openpete_psx_decal.glsl). */
    void* pipeline[OPENPETE_MATERIAL_PIPE_VARIANTS];
    void* pipeline_mark;   /* stencil-MARK twin (hole-fill WORLD/ACTOR groups) */
    /* Retained-terrain pipelines, one per (arena vertex stage x colour
     * slot). Separate from `pipeline` because the terrain layout, the
     * vertex input and the engine half of the contract all differ. */
    void* pipeline_ter[OPENPETE_MATERIAL_TER_PIPES];
    /* Portal-profile pipelines (OPENPETE_MATERIAL_PORTAL_PIPES). */
    void* pipeline_portal[OPENPETE_MATERIAL_PORTAL_PIPES];
    int   user_enabled;    /* material_enable - the mod's own on/off */
    /* Static uniform block (material_set_params). Declarative materials have
     * no per-instance callback to mint a slot, so the block is interned into
     * one at every frame reset and reused for the whole present. */
    unsigned char params[OPENPETE_MATERIAL_UNIFORMS_MAX];
    uint32_t      params_len;
    int           static_slot;
    /* Per-instance TERRAIN materials: the refine outcome for this present,
     * memoized so the callback runs ONCE however many faces need it and
     * whichever residency they are in. A terrain refine selector has exactly
     * one key (its Tiledef id, or the untextured band), so one slot covers
     * the whole material. */
    int           refine_slot;
    unsigned      refine_present;
    /* A selector retuned from inside a present (material_set_selector) parks
     * here until the tick boundary. Selection has to be constant for a whole
     * present or half the frame shades under the old key. */
    openpete_mod_material_selector_t sel_pending;
    int           sel_dirty;
    int  live;
} mod_material_t;

static mod_material_t s_mats[MOD_MATERIAL_MAX];
static int s_mat_count;
/* Census only: the table cannot grow past the bucket-id bound, so the
 * row reports how much of that bound a run's content uses. */
static mod_fixed s_mat_census;
static int s_mat_sel_dirty;      /* any material carries a parked selector */
static int s_mat_has_instance;   /* cached: any live selector has a callback */

/* Per-present interned per-instance slots. Index 0 reserved
 * (= "defer to draw-time class resolve"); 1.. are interned outcomes. */
#define MOD_MATERIAL_SLOTS 128
_Static_assert(MOD_MATERIAL_SLOTS == OPENPETE_MODS_MATERIAL_VIEW_SLOTS, "view mirrors the slot table");
_Static_assert(MOD_MATERIAL_MAX == OPENPETE_MODS_MATERIAL_VIEW_MATS, "view mirrors the material table");
typedef struct {
    int      mat;            /* winning selector index */
    uint32_t len;
    unsigned char bytes[OPENPETE_MATERIAL_UNIFORMS_MAX];
} mat_slot_t;
static mat_slot_t s_mat_slots[MOD_MATERIAL_SLOTS];
static int s_mat_slot_n = 1;    /* next free; 0 is the reserved sentinel */

/* The present thread's view of the slot table (NULL = the live table, for
 * the single-threaded path). See openpete_mods_material_view_t. */
static const openpete_mods_material_view_t* s_mat_view;
/** @brief Return the slot count of the installed view, else of the live table. */
static int view_slot_n(void) { return s_mat_view ? s_mat_view->slot_n : s_mat_slot_n; }
/** @brief Return the material index of an interned slot in the installed view, else in the live table.
 *
 * @param s Slot index below view_slot_n(). */
static int view_slot_mat(int s) { return s_mat_view ? s_mat_view->slots[s].mat : s_mat_slots[s].mat; }
/** @brief Return the static uniform slot of a material in the installed view, else in the live table.
 *
 * @param m Material index below s_mat_count.
 * @return The slot, or 0 for no params. */
static int view_static_slot(int m) { return s_mat_view ? s_mat_view->static_slot[m] : s_mats[m].static_slot; }

/* Slot ids are minted from ONE global counter but stamped into per-path
 * tag fields of differing width, so the table can never outgrow the
 * NARROWEST field that carries an id. Terrain's is the narrowest at 9
 * bits, and its decode masks (tag >> 16) & 0x1FF: an id of 512 would
 * come back as 0, which reads as "defer to declarative resolve" - the
 * wrong material, silently, with no crash and no log. 0x1FF itself is
 * the declined->stock sentinel, so the last usable id is 510. Raising
 * MOD_MATERIAL_SLOTS means widening that field first. */
typedef char nr_assert_mat_slots_fit_narrowest_tag_field[
    (MOD_MATERIAL_SLOTS <= OPENPETE_MATERIAL_TERRAIN_STOCK) ? 1 : -1];

/* native_render arms the batcher tag-split while any selector lives;
 * s_mat_has_instance gates the moby walk's per-instance resolve.
 *
 * Arming keys on REGISTRATION (live), never on the owner's live-toggle
 * state: entry - and therefore material_register - runs before the
 * loader marks a mod enabled, so an enabled-keyed arm would read 0 at
 * boot and never re-arm. A disabled owner's selectors therefore still
 * cost a batch split and a winner scan, but can produce no override:
 * every winner/resolve path consults openpete_mods_is_enabled. */
/* The retained terrain cache bakes each face's winning selector into the face
 * itself, so it must re-bake whenever the TERRAIN-relevant selector set moves.
 * This counter is that key, and it is deliberately NARROW: a counter that also
 * moved for moby, sky, flame and particle materials - and for every enable
 * toggle of one - re-baked the whole cache for changes that cannot alter a
 * single terrain bucket, which cost 2.7x the frame rate while a mod was
 * toggling rows (terrain.build 33% -> 73% of sim_tick). Move it only for
 * changes terrain_winner() could actually see. */
static unsigned s_mat_ter_epoch;

/** @brief Test whether a material could bucket a terrain face in some enable state and level.
 *
 * True for a live material on channel -1 or OP_CHAN_TERRAIN. The mutators
 * use it to decide whether a change bumps s_mat_ter_epoch, so it ignores
 * the enable state, which is the change being reacted to, and the level,
 * which is not known at every bump site. A wider answer costs a spurious
 * re-bake; a narrower one would leave faces stamped for a selector that no
 * longer wins. */
static int terrain_sel_relevant(const mod_material_t* m) {
    return m->live && (m->sel.channel == -1 || m->sel.channel == OP_CHAN_TERRAIN);
}

/** @brief Recompute the per-instance flag and arm the batcher tag split from the live materials.
 *
 * The split is armed while any material is live, whatever its owner's
 * enabled state, and s_mat_has_instance is set while any live material has
 * a per-instance callback. Runs under the registry write lock or on the
 * compose reader thread. */
static void material_rearm_split(void) {
    int any = 0, inst = 0;
    for (int i = 0; i < s_mat_count; i++)
        if (s_mats[i].live) { any = 1; if (s_mats[i].per_instance) inst = 1; }
    s_mat_has_instance = inst;
    openpete_native_set_tag_split(any);
}

int openpete_mods_material_has_instance(void) { return s_mat_has_instance; }

unsigned openpete_mods_material_terrain_epoch(void) {
    /* Sum of two monotonic counters - strictly increases on any change the
     * terrain bake cares about; +1 keeps 0 free as the latch's "unset". The
     * mod toggle epoch rides along because terrain_sel_usable() consults
     * openpete_mods_is_enabled(owner). */
    return 1u + s_mat_ter_epoch + openpete_mods_toggle_epoch();
}

static int openpete_mods_current_level(void);   /* defined with s_cur_level */

/** @brief Test whether a material can bucket terrain faces in a level.
 *
 * True when the material is live, user-enabled and owned by an enabled
 * mod, is on channel -1 or OP_CHAN_TERRAIN, and has no level key or one
 * equal to @p level; a negative @p level matches every level key. The
 * level test mirrors the draw-time resolve's, so a level-scoped selector
 * buckets faces only in its level. */
static int terrain_sel_usable(const mod_material_t* m, int level) {
    if (!m->live || !m->user_enabled || !openpete_mods_is_enabled(m->owner))
        return 0;
    if (m->sel.channel != -1 && m->sel.channel != OP_CHAN_TERRAIN) return 0;
    if (m->sel.level != -1 && level >= 0 && m->sel.level != level) return 0;
    return 1;
}

/** @brief Return the index of the winning terrain material for a face key, or -1.
 *
 * Applies the draw-time resolve's predicates and precedence over the
 * materials terrain_sel_usable() accepts for the current level. A
 * per-instance material whose texture key matches (for the untextured
 * band, one with OP_SEL_UNTEXTURED and no texture key) outranks every
 * declarative one, as the walked path's tag carries its slot and skips the
 * declarative scan; within each kind the highest load_index wins.
 * Declarative materials test the texture key and the OP_SEL_ENV_ANIM,
 * tier and textured/untextured flags against the face.
 * @param texid Tiledef id, or -1 for an untextured face.
 * @param env Nonzero for a face in the env-rewrite set.
 * @param lp Nonzero for an LP-tier face. */
static int terrain_winner(int texid, int env, int lp) {
    if (s_mat_count == 0) return -1;
    const int untex = texid < 0;
    const int level = openpete_mods_current_level();
    int best = -1, best_inst = -1;
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        if (!terrain_sel_usable(m, level)) continue;
        if (m->per_instance) {
            /* Mirror the winner scans exactly: the untex band resolve keys
             * on OP_SEL_UNTEXTURED + texture=-1, the texture-keyed resolve on
             * texture==texid with the flag clear - neither reads the
             * env/tier refinements. */
            if (untex ? ((m->sel.flags & OP_SEL_UNTEXTURED) && m->sel.texture == -1)
                      : (!(m->sel.flags & OP_SEL_UNTEXTURED) && m->sel.texture == texid))
                if (best_inst < 0 || m->load_index > s_mats[best_inst].load_index)
                    best_inst = i;
            continue;
        }
        /* Declarative: the resolve's predicate over the face's tag bits. */
        if (m->sel.texture != -1 && (untex || m->sel.texture != texid)) continue;
        if ((m->sel.flags & OP_SEL_ENV_ANIM) && !env) continue;
        if ((m->sel.flags & OP_SEL_HP_TIER) && lp) continue;
        if ((m->sel.flags & OP_SEL_LP_TIER) && !lp) continue;
        if ((m->sel.flags & OP_SEL_TEXTURED) && untex) continue;
        if ((m->sel.flags & OP_SEL_UNTEXTURED) && !untex) continue;
        if (best < 0 || m->load_index > s_mats[best].load_index) best = i;
    }
    return best_inst >= 0 ? best_inst : best;
}

int openpete_mods_material_terrain_bucket(int texid, int env, int lp) {
    int w = terrain_winner(texid, env, lp);
    if (w < 0) return 0;
    return (w + 1 <= OPENPETE_MATERIAL_TER_BUCKET_MAX) ? w + 1 : 0;
}

int openpete_mods_material_terrain_bucket_list(int* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (s_mat_view) {
        int n = s_mat_view->ter_bucket_n;
        if (n > cap) n = cap;
        for (int i = 0; i < n; i++) out[i] = s_mat_view->ter_bucket[i];
        return n;
    }
    int n = 0, level = openpete_mods_current_level();
    /* Ascending by construction: the scan runs in index order and the id
     * is the index + 1. */
    for (int i = 0; i < s_mat_count && n < cap; i++) {
        if (!terrain_sel_usable(&s_mats[i], level)) continue;
        if (i + 1 > OPENPETE_MATERIAL_TER_BUCKET_MAX) break;
        out[n++] = i + 1;
    }
    return n;
}

static int terrain_refine_slot(int w);   /* defined with the refine tier */

int openpete_mods_material_terrain_bucket_gpu(
        int bucket, int pipe_idx, openpete_mod_material_terrain_t* out) {
    if (!out || bucket <= 0 || bucket > s_mat_count) return -1;
    if (pipe_idx < 0 || pipe_idx >= OPENPETE_MATERIAL_TER_PIPES) return -1;
    mod_material_t* m = &s_mats[bucket - 1];
    if (!m->live || !m->user_enabled || !openpete_mods_is_enabled(m->owner))
        return -1;
    if (m->frag < 0) return -1;
    if (m->per_instance) {
        /* The refine callback decides this present. Declining is not an
         * error: those faces are already claimed out of the stock pass, so
         * the caller draws the bucket's pass with the STOCK pipeline. */
        int slot = terrain_refine_slot(bucket - 1);
        if (slot == OPENPETE_MATERIAL_TERRAIN_STOCK || slot <= 0) return -1;
        out->uniform_slot = slot;
    } else {
        out->uniform_slot = view_static_slot(bucket - 1);
    }
    out->frag_spv = openpete_mods_shader_spv_terrain(m->frag, &out->frag_words);
    if (!out->frag_spv || out->frag_words == 0) return -1;
    out->pipeline_slot = &m->pipeline_ter[pipe_idx];
    out->owner = m->owner;
    return 0;
}

/** @brief Check that this engine can honour a material selector exactly as written.
 *
 * Refuses, with an error log, a struct_size other than this SDK's, flag
 * bits outside OP_SEL_FLAGS_KNOWN, a channel other than -1 outside
 * OP_CHAN_MOBY..OP_CHAN_PORTAL, and a texture key other than a Tiledef id
 * 0..127 on channel -1 or OP_CHAN_TERRAIN without OP_SEL_UNTEXTURED. The flag check exists because a new flag bit never
 * changes sizeof. Register and set_selector share it, so a retune cannot
 * install a key the register path would refuse.
 * @param id Mod id for log lines.
 * @param sel The selector.
 * @return 1 when valid, 0 when refused. */
static int material_sel_valid(const char* id,
                              const openpete_mod_material_selector_t* sel) {
    if (sel->struct_size != (uint32_t)sizeof *sel) {
        OPL_ERROR_F("mods", "%s: material: selector size %u, engine expects %u "
                    "- rebuild the mod against this SDK",
                    id, sel->struct_size, (unsigned)sizeof *sel);
        return 0;
    }
    if (sel->flags & ~(unsigned)OP_SEL_FLAGS_KNOWN) {
        OPL_ERROR_F("mods", "%s: material: selector carries unknown flag bits "
                    "%#x - this engine would match wider than you asked",
                    id, sel->flags & ~(unsigned)OP_SEL_FLAGS_KNOWN);
        return 0;
    }
    if (sel->channel != -1 &&
        (sel->channel < OP_CHAN_MOBY || sel->channel > OP_CHAN_PORTAL)) {
        OPL_ERROR_F("mods", "%s: material: unknown channel %d", id,
                    sel->channel);
        return 0;
    }
    if (sel->texture != -1 &&
        (sel->texture < 0 || sel->texture > 0x7F ||
         (sel->channel != -1 && sel->channel != OP_CHAN_TERRAIN) ||
         (sel->flags & (unsigned)OP_SEL_UNTEXTURED))) {
        OPL_ERROR_F("mods", "%s: material texture selector must be a terrain "
                    "Tiledef id 0..127 on a textured path (got %d)", id,
                    sel->texture);
        return 0;
    }
    return 1;
}

/** @brief Body of openpete_mods_material_register, run under the registry write lock.
 *
 * Validates the selector and shader handles, appends the material enabled,
 * bumps the terrain epoch when it could bucket terrain, and re-arms the tag
 * split. */
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
    if (!material_sel_valid(id, sel)) return -1;
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
    if (vert_shader < 0 && frag_shader < 0) return -1;   /* nothing to do */
    if (s_mat_count >= MOD_MATERIAL_MAX) {
        OPL_ERROR_F("mods", "%s: material table full (%d)", id,
                    MOD_MATERIAL_MAX);
        return -1;
    }
    mod_material_t* m = &s_mats[s_mat_count];
    memset(m, 0, sizeof *m);
    m->owner = id;
    m->sel = *sel;
    m->vert = vert_shader;
    m->frag = frag_shader;
    m->per_instance = per_instance;
    m->load_index = openpete_mods_load_index_of(self);
    m->live = 1;
    m->user_enabled = 1;          /* registration arms it; enable() toggles */
    int handle = s_mat_count++;
    if (!s_mat_census.storage)
        mod_fixed_init(&s_mat_census, "materials", 32, MOD_MATERIAL_MAX);
    mod_fixed_set(&s_mat_census, (size_t)s_mat_count);
    if (terrain_sel_relevant(m)) s_mat_ter_epoch++;
    material_rearm_split();
    OPL_INFO_F("mods", "%s: material selector armed (channel %d ident %d class %d "
               "flags %#x%s)", id, sel->channel, sel->ident, sel->moby_class, sel->flags,
               per_instance ? ", per-instance" : "");
    return handle;
}
int openpete_mods_material_register(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance) { REG_WR(); int r_ = openpete_mods_material_register_locked(self, sel, vert_shader, frag_shader, per_instance); REG_WR_END(); return r_; }


/** @brief Turn one registered material on or off, under the registry write lock.
 *
 * Sets user_enabled, bumps the terrain epoch when the material could
 * bucket a terrain face, and re-arms the batcher tag split. `self` is not
 * checked against the owner.
 * @return 0, or -1 for an unknown or swept material. */
static int openpete_mods_material_enable_locked(openpete_mod_t* self, int material, int on) {
    (void)self;
    if (material < 0 || material >= s_mat_count || !s_mats[material].live)
        return -1;
    s_mats[material].user_enabled = on ? 1 : 0;
    if (terrain_sel_relevant(&s_mats[material])) s_mat_ter_epoch++;
    material_rearm_split();
    return 0;
}
int openpete_mods_material_enable(openpete_mod_t* self, int material, int on) { REG_WR(); int r_ = openpete_mods_material_enable_locked(self, material, on); REG_WR_END(); return r_; }


/** @brief Set a material's static uniform block, under the registry write lock.
 *
 * The block is copied; openpete_mods_material_frame_reset interns it into
 * a slot at each present for materials whose owner is enabled. `self` is
 * not checked against the owner.
 * @return 0, or -1 for an unknown or swept material, @p len above
 *         OPENPETE_MATERIAL_UNIFORMS_MAX, or a NULL block with nonzero
 *         @p len. */
static int openpete_mods_material_set_params_locked(openpete_mod_t* self, int material,
                                      const void* block, uint32_t len) {
    (void)self;
    if (material < 0 || material >= s_mat_count || !s_mats[material].live)
        return -1;
    if (len > OPENPETE_MATERIAL_UNIFORMS_MAX) return -1;
    if (len && !block) return -1;
    s_mats[material].params_len = len;
    if (len) memcpy(s_mats[material].params, block, len);
    return 0;
}
int openpete_mods_material_set_params(openpete_mod_t* self, int material,
                                      const void* block, uint32_t len) { REG_WR(); int r_ = openpete_mods_material_set_params_locked(self, material, block, len); REG_WR_END(); return r_; }


/** @brief Replace a live material's selector, keeping its handle, shaders, callback and enabled state.
 *
 * Runs under the registry write lock. Only the owning mod may retune, and
 * the selector must pass material_sel_valid. Inside a present the new
 * selector is parked for openpete_mods_material_apply_pending, since
 * selection must stay constant across a present and the terrain epoch may
 * move only between ticks; outside one it applies at once, bumping the
 * terrain epoch when the old or new selector could bucket terrain.
 * Re-registering instead would use a table slot per change, leave the old
 * selector matching, and is refused inside a present.
 * @return 0, or -1 for a NULL argument, an unknown or swept material, a
 *         material another mod owns, or an invalid selector. */
static int openpete_mods_material_set_selector_locked(
        openpete_mod_t* self, int material,
        const openpete_mod_material_selector_t* sel) {
    const char* id = openpete_mods_id_of(self);
    if (!id || !sel) return -1;
    if (material < 0 || material >= s_mat_count || !s_mats[material].live)
        return -1;
    mod_material_t* m = &s_mats[material];
    if (m->owner != id && strcmp(m->owner, id) != 0) {
        OPL_ERROR_F("mods", "%s: material %d belongs to \"%s\"", id, material,
                    m->owner);
        return -1;
    }
    if (!material_sel_valid(id, sel)) return -1;
    /* Inside a present the new key is parked: a selector that changed mid-
     * present would shade part of the image under each key, and the retained
     * terrain bake keys off the material epoch, which may only move between
     * ticks. Outside one there is nothing to straddle, so it lands now. */
    if (openpete_mods_in_present) {
        m->sel_pending = *sel;
        m->sel_dirty = 1;
        s_mat_sel_dirty = 1;
        return 0;
    }
    const int was_ter = terrain_sel_relevant(m);
    m->sel = *sel;
    m->sel_dirty = 0;
    if (was_ter || terrain_sel_relevant(m)) s_mat_ter_epoch++;
    material_rearm_split();
    return 0;
}
int openpete_mods_material_set_selector(
        openpete_mod_t* self, int material,
        const openpete_mod_material_selector_t* sel) { REG_WR(); int r_ = openpete_mods_material_set_selector_locked(self, material, sel); REG_WR_END(); return r_; }


/** @brief Install the selectors parked by retunes made inside a present.
 *
 * Runs under the registry write lock. Bumps the terrain epoch for each
 * material whose old or new selector could bucket terrain, and re-arms the
 * batcher tag split when any selector moved. */
static void openpete_mods_material_apply_pending_locked(void) {
    if (!s_mat_sel_dirty) return;
    s_mat_sel_dirty = 0;
    int n = 0;
    for (int i = 0; i < s_mat_count; i++) {
        if (!s_mats[i].sel_dirty) continue;
        const int was_ter = terrain_sel_relevant(&s_mats[i]);
        s_mats[i].sel = s_mats[i].sel_pending;
        s_mats[i].sel_dirty = 0;
        if (was_ter || terrain_sel_relevant(&s_mats[i])) s_mat_ter_epoch++;
        n++;
    }
    if (n) material_rearm_split();
}
void openpete_mods_material_apply_pending(void) {
    if (!s_mat_sel_dirty) return;     /* nothing parked: never take the lock */
    REG_WR(); openpete_mods_material_apply_pending_locked(); REG_WR_END();
}


/** @brief Return the winning material for a moby class, or -1.
 *
 * Considers live, user-enabled materials of enabled mods on channel -1 or
 * OP_CHAN_MOBY whose class key is -1 or `cls`. A texture key
 * or an env, HP-tier, LP-tier or untextured flag excludes the material,
 * since those refine terrain only. The level key is not tested. Among
 * matches the highest load_index, the later-loaded mod, wins. */
static int material_moby_winner(int cls) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        mod_material_t* m = &s_mats[i];
        if (!m->live || !m->user_enabled ||
            !openpete_mods_is_enabled(m->owner)) continue;
        if (m->sel.channel != -1 && m->sel.channel != OP_CHAN_MOBY) continue;
        if (m->sel.moby_class != -1 && m->sel.moby_class != cls) continue;
        if (m->sel.texture != -1) continue;   /* texture keys are terrain-only */
        if (m->sel.flags & (OP_SEL_ENV_ANIM | OP_SEL_HP_TIER | OP_SEL_LP_TIER |
                            OP_SEL_UNTEXTURED))
            continue;   /* terrain-only refinements never match a moby */
        if (best < 0 || m->load_index > s_mats[best].load_index)
            best = i;
    }
    return best;
}

/* Level id for level-scoped selector keys. The mods layer cannot read it
 * itself - the symbol is game-specific - so the game pushes it while it
 * renders. Draw-time resolve and the terrain bucket queries
 * (terrain_sel_usable) consume it, and -1 (nothing has pushed yet) makes
 * every level key inert rather than wrongly matching. */
static int s_cur_level = -1;
/** @brief Record the level for level-scoped selectors and bump the terrain epoch when it changes. */
static void openpete_mods_set_level_locked(int level_id) {
    /* The retained-terrain claim latches its verdict against
     * openpete_mods_material_terrain_epoch(), and that verdict depends on
     * the level (selectors are level-scoped, because sector and texture ids
     * are per-level). So a level change has to move the epoch, or a sector
     * latched in one level keeps that answer into the next. */
    if (level_id != s_cur_level) {
        s_cur_level = level_id; s_mat_ter_epoch++;
    }
}
/* No lock: a compose blocked on the display never delays the push. */
void openpete_mods_set_level(int level_id) { openpete_mods_set_level_locked(level_id); }

/** @brief Return the level id the game last pushed, or -1 before any push. */
static int openpete_mods_current_level(void) { return s_cur_level; }

/* Bumped every present, so a per-present memo (the terrain refine slot) can
 * tell "computed this present" from "computed under the last one". */
static unsigned s_mat_present = 1;

void openpete_mods_material_frame_reset(void) {
    s_mat_slot_n = 1;
    s_mat_present++;
    /* Re-intern every live material's static block, so a declarative
     * material has a slot to point at for the rest of this present. */
    for (int i = 0; i < s_mat_count; i++) {
        mod_material_t* m = &s_mats[i];
        m->static_slot = 0;
        if (!m->live || !m->params_len) continue;
        if (!m->user_enabled || !openpete_mods_is_enabled(m->owner)) continue;
        if (s_mat_slot_n >= MOD_MATERIAL_SLOTS) continue;   /* full -> no params */
        int sl = s_mat_slot_n++;
        s_mat_slots[sl].mat = i;
        s_mat_slots[sl].len = m->params_len;
        memcpy(s_mat_slots[sl].bytes, m->params, m->params_len);
        m->static_slot = sl;
    }
}
int  openpete_mods_material_slot_count(void) { return s_mat_slot_n; }

/* Per-scene snapshot of the interned per-instance slots (see the ring in
 * native_render.c): slots 1 up to the live count, the static slot of every
 * material index, and the terrain bucket list. Each slot's bytes sit in a
 * fixed OPENPETE_MATERIAL_UNIFORMS_MAX array, so the compose indexes them
 * the same way as the live table. */
void openpete_mods_material_view_capture(openpete_mods_material_view_t* out) {
    out->slot_n = s_mat_slot_n;
    for (int i = 1; i < s_mat_slot_n; i++) {
        out->slots[i].mat = s_mat_slots[i].mat;
        out->slots[i].len = s_mat_slots[i].len;
        memcpy(out->slots[i].bytes, s_mat_slots[i].bytes, s_mat_slots[i].len);
    }
    for (int i = 0; i < MOD_MATERIAL_MAX; i++) out->static_slot[i] = s_mats[i].static_slot;
    /* Capture the live list, not the view's: a capture runs on the sim
     * thread with no view installed. */
    out->ter_bucket_n = openpete_mods_material_terrain_bucket_list(
        out->ter_bucket, OPENPETE_MODS_MATERIAL_VIEW_MATS);
}
void openpete_mods_material_view_set(const openpete_mods_material_view_t* v) { s_mat_view = v; }

const void* openpete_mods_material_slot_uniforms(int slot, uint32_t* len) {
    if (slot <= 0 || slot >= s_mat_slot_n) { if (len) *len = 0; return NULL; }
    if (len) *len = s_mat_slots[slot].len;
    return s_mat_slots[slot].bytes;
}

int openpete_mods_material_moby_slot(CPUState* cpu, int moby_class,
                                     uint32_t moby_vaddr) {
    if (!s_mat_has_instance) return 0;
    int w = material_moby_winner(moby_class);
    if (w < 0 || !s_mats[w].per_instance) return 0;   /* declarative -> draw-time */

    (void)cpu;   /* refine reads guest RAM through the mod api's accessors */
    openpete_mod_material_key_t key;
    memset(&key, 0, sizeof key);
    key.struct_size    = (uint32_t)sizeof key;
    key.version        = OPENPETE_MATERIAL_KEY_VER_MOBY;
    key.channel        = OP_CHAN_MOBY;
    key.u.moby.vaddr   = moby_vaddr;
    key.u.moby.moby_class = moby_class;
    unsigned char buf[OPENPETE_MATERIAL_UNIFORMS_MAX];
    int rc = s_mats[w].per_instance(&key, buf, sizeof buf);
    if (rc < 0) return OPENPETE_MATERIAL_SLOT_STOCK;   /* declined -> stock */
    if (rc > (int)sizeof buf) rc = (int)sizeof buf;

    /* Intern (winner, bytes): identical outcomes share a slot so they
     * stay in ONE batch. Linear scan - the interned set is small. */
    for (int s = 1; s < s_mat_slot_n; s++)
        if (s_mat_slots[s].mat == w && s_mat_slots[s].len == (uint32_t)rc &&
            memcmp(s_mat_slots[s].bytes, buf, (size_t)rc) == 0)
            return s;
    if (s_mat_slot_n >= MOD_MATERIAL_SLOTS)
        return OPENPETE_MATERIAL_SLOT_STOCK;   /* table full -> graceful stock */
    int s = s_mat_slot_n++;
    s_mat_slots[s].mat = w;
    s_mat_slots[s].len = (uint32_t)rc;
    memcpy(s_mat_slots[s].bytes, buf, (size_t)rc);
    return s;
}

/** @brief Return the winning per-instance terrain material for the untextured band, or -1.
 *
 * The untextured band is the beyond-LOD gouraud terrain. Considers live,
 * user-enabled per-instance materials of enabled mods on channel -1 or
 * OP_CHAN_TERRAIN with OP_SEL_UNTEXTURED set and no texture key;
 * the highest load_index wins. */
static int material_terrain_untex_winner(void) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        mod_material_t* m = &s_mats[i];
        if (!m->live || !m->user_enabled ||
            !openpete_mods_is_enabled(m->owner)) continue;
        if (!m->per_instance) continue;
        if (m->sel.channel != -1 && m->sel.channel != OP_CHAN_TERRAIN) continue;
        if (m->sel.texture != -1) continue;   /* untex band has no texture */
        if (!(m->sel.flags & OP_SEL_UNTEXTURED)) continue;
        if (best < 0 || m->load_index > s_mats[best].load_index) best = i;
    }
    return best;
}

/** @brief Return the winning per-instance terrain material for a textured face's Tiledef id, or -1.
 *
 * Considers live, user-enabled per-instance materials of enabled mods on
 * channel -1 or OP_CHAN_TERRAIN without OP_SEL_UNTEXTURED whose
 * texture key equals `texid`; the highest load_index wins. */
static int material_terrain_tex_winner(int texid) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        mod_material_t* m = &s_mats[i];
        if (!m->live || !m->user_enabled ||
            !openpete_mods_is_enabled(m->owner)) continue;
        if (!m->per_instance) continue;
        if (m->sel.channel != -1 && m->sel.channel != OP_CHAN_TERRAIN) continue;
        if (m->sel.flags & OP_SEL_UNTEXTURED) continue;
        /* Per-instance terrain selectors are texture-keyed: texture=-1
         * per-instance stays the untex band's business, never a
         * blanket all-textured-terrain override. */
        if (m->sel.texture == -1 || m->sel.texture != texid) continue;
        if (best < 0 || m->load_index > s_mats[best].load_index) best = i;
    }
    return best;
}

/** @brief Return the refine outcome of one per-instance terrain material as an interned uniform slot, computed once per present.
 *
 * A terrain refine selector carries one key, a Tiledef id or the
 * untextured band, so the material's outcome is the outcome of every face
 * it reaches and one memo per material serves the walked and the retained
 * terrain alike. The memo is keyed on the present counter
 * openpete_mods_material_frame_reset() advances. The callback receives a
 * terrain key with the selector's texture (-1 for the untextured band);
 * its block, capped at OPENPETE_MATERIAL_UNIFORMS_MAX, reuses an interned
 * slot of the same material with identical bytes, else takes a new slot.
 * @param w Material index, or -1 for none.
 * @return 0 for no material or a declarative one, an interned slot, or
 *         OPENPETE_MATERIAL_TERRAIN_STOCK when the callback declined or
 *         the slot table is full. */
static int terrain_refine_slot(int w) {
    if (w < 0 || w >= s_mat_count) return 0;
    mod_material_t* m = &s_mats[w];
    if (!m->per_instance) return 0;
    if (m->refine_present == s_mat_present) return m->refine_slot;
    m->refine_present = s_mat_present;
    m->refine_slot = OPENPETE_MATERIAL_TERRAIN_STOCK;

    const int untex = (m->sel.flags & OP_SEL_UNTEXTURED) != 0;
    openpete_mod_material_key_t key;
    memset(&key, 0, sizeof key);
    key.struct_size       = (uint32_t)sizeof key;
    key.version           = OPENPETE_MATERIAL_KEY_VER_TERRAIN;
    key.channel           = OP_CHAN_TERRAIN;
    key.u.terrain.texture = untex ? -1 : m->sel.texture;
    key.u.terrain.flags   = untex ? OP_SEL_UNTEXTURED : m->sel.flags;
    unsigned char buf[OPENPETE_MATERIAL_UNIFORMS_MAX];
    int rc = m->per_instance(&key, buf, sizeof buf);
    if (rc < 0) return m->refine_slot;          /* declined -> stock */
    if (rc > (int)sizeof buf) rc = (int)sizeof buf;
    for (int s = 1; s < s_mat_slot_n; s++)
        if (s_mat_slots[s].mat == w && s_mat_slots[s].len == (uint32_t)rc &&
            memcmp(s_mat_slots[s].bytes, buf, (size_t)rc) == 0)
            return (m->refine_slot = s);
    if (s_mat_slot_n >= MOD_MATERIAL_SLOTS)
        return m->refine_slot;                  /* table full -> graceful stock */
    int s = s_mat_slot_n++;
    s_mat_slots[s].mat = w;
    s_mat_slots[s].len = (uint32_t)rc;
    memcpy(s_mat_slots[s].bytes, buf, (size_t)rc);
    return (m->refine_slot = s);
}

/* Per-instance material slot for one textured terrain Tiledef id. */
int openpete_mods_material_terrain_tex_slot(CPUState* cpu, int texid) {
    if (!s_mat_has_instance) return 0;
    (void)cpu;
    return terrain_refine_slot(material_terrain_tex_winner(texid));
}

/* Per-instance material slot for the untextured terrain band. */
int openpete_mods_material_terrain_untex_slot(CPUState* cpu) {
    if (!s_mat_has_instance) return 0;
    (void)cpu;
    return terrain_refine_slot(material_terrain_untex_winner());
}

/** @brief Test whether an interned slot's material is still live and its owner enabled.
 *
 * An interned slot can outlive its material's usability: extraction
 * interns the slot, the tick boundary (live-toggle apply and hot-reload
 * sweep) runs next, and only then does the present draw. A disabled owner
 * must stop that present. A swept material has freed SPIR-V and a
 * retired pipeline slot no later sweep revisits, so a rebuild through it
 * would cache a pipeline nothing destroys. Both cases draw stock.
 * @param slot Slot index in the current view.
 * @return Nonzero when usable. */
static int material_slot_usable(int slot) {
    int m = view_slot_mat(slot);
    return m >= 0 && m < s_mat_count && s_mats[m].live &&
           openpete_mods_is_enabled(s_mats[m].owner);
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
    mod_material_t* m = &s_mats[best];
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
    out->owner = m->owner;
    /* A per-instance slot always wins; otherwise fall back to this
     * material's static block (0 = no params at all). */
    out->uniform_slot = uniform_slot > 0 ? uniform_slot : view_static_slot(best);
    return best;
}

/* Draw-time resolve (native_render draw_tex_runs): the run's provenance tag ->
 * the winning override, or -1. Tag layout: [31:28] path+1; moby class
 * [15:0] + per-instance slot [27:16]; terrain env bit 27 / LP
 * bit 26 + sector [8:0]. Enabled-list priority: the later-loaded mod
 * wins. */
/* The selector keys a tag carries, decoded once. `path` is 0 moby,
 * 1 terrain; the other members are -1 when the tag's path does not carry
 * that key. */
typedef struct {
    int path;
    int cls;     /* moby class */
    int tex;     /* terrain texture id */
    int chan;    /* raw path nibble: the feeder's channel */
    int ident;   /* channel-local identity */
    int level;
    int env, lp, untex;
} material_key_t;

/** @brief Decode a provenance tag into the selector keys it carries.
 *
 * Bits 31:28 hold the path plus one (a nibble of 0 gives path -1), and the
 * channel is that raw nibble. Identity is bits 15:0 for every path. A moby
 * tag (path 0) carries its class in bits 15:0. A terrain tag (path 1)
 * carries the env-rewrite flag in bit 27, the LP-tier flag in bit 26, the
 * untextured flag in bit 25, and, when textured, the Tiledef id in bits
 * 15:9. The level is the one last pushed.
 *
 * @param tag Provenance tag.
 * @param k Receives the decoded keys; a key the path does not carry is -1
 *        or 0. */
static void material_key_from_tag(uint32_t tag, material_key_t* k) {
    k->path  = (int)(tag >> 28) - 1;
    k->untex = (k->path == 1) && (tag & 0x02000000u);
    k->cls   = (k->path == 0) ? (int)(tag & 0xFFFFu) : -1;
    k->env   = (k->path == 1) && (tag & 0x08000000u);
    k->lp    = (k->path == 1) && (tag & 0x04000000u);
    k->tex   = (k->path == 1 && !k->untex) ? (int)((tag >> 9) & 0x7Fu) : -1;
    /* Channel == the raw path nibble. The other feeders (player, flame,
     * particle, sky, shadow, tracer) all carry their identity in [15:0],
     * so one comparison serves every one of them. */
    k->chan  = (int)(tag >> 28);
    k->ident = (int)(tag & 0xFFFFu);
    k->level = openpete_mods_current_level();
}

/** @brief Read the interned material slot a tag names directly.
 *
 * A moby tag carries a per-instance slot in bits [27:16]
 * (OPENPETE_MATERIAL_SLOT_STOCK = declined); a terrain tag carries a
 * per-present slot in bits [24:16] (OPENPETE_MATERIAL_TERRAIN_STOCK =
 * declined). Slot 0 defers to the declarative resolve. A slot at or past
 * the view's slot count is stale, and one whose material is swept or whose
 * owner is disabled no longer resolves.
 * @param tag Provenance tag.
 * @param path 0 moby, 1 terrain; other paths carry no slot.
 * @param slot Receives the slot on a hit.
 * @return 1 with the slot, 0 to fall through to the declarative resolve,
 *         -1 when the slot declines the draw or no longer resolves. */
static int material_slot_from_tag(uint32_t tag, int path, int* slot) {
    int s;
    if (path == 0) {
        s = (int)((tag >> 16) & 0xFFFu);
        if (s == OPENPETE_MATERIAL_SLOT_STOCK) return -1;    /* declined */
    } else if (path == 1) {
        s = (int)((tag >> 16) & 0x1FFu);
        if (s == OPENPETE_MATERIAL_TERRAIN_STOCK) return -1; /* declined */
    } else {
        return 0;
    }
    if (s == 0) return 0;                        /* declarative resolve */
    if (s >= view_slot_n()) return -1;           /* stale (post-reset) */
    if (!material_slot_usable(s)) return -1;     /* toggled/swept */
    *slot = s;
    return 1;
}

/** @brief Test whether a material's selector accepts a decoded tag key.
 *
 * Each selector field narrows only when set: channel, level (inert while
 * no level has been pushed), identity (not on terrain, whose [15:0] holds
 * sector and texture id), moby class (moby path only), texture (textured
 * terrain only), and the env, HP-tier, LP-tier, textured and untextured
 * flags. Setting neither textured nor untextured selects both.
 * @return 1 when accepted, else 0. */
static int material_selector_matches(const mod_material_t* m,
                                     const material_key_t* k) {
    if (m->sel.channel != -1 && m->sel.channel != k->chan) return 0;
    if (m->sel.level != -1 && k->level >= 0 && m->sel.level != k->level)
        return 0;
    /* Identity is meaningful for every channel except terrain, whose
     * [15:0] is sector|texid rather than a channel-local id and which
     * has its own keys below. */
    if (m->sel.ident != -1 && k->path != 1 && m->sel.ident != k->ident)
        return 0;
    if (k->path == 0 && m->sel.moby_class != -1 &&
        m->sel.moby_class != k->cls)
        return 0;
    /* Texture key: terrain textured faces only. */
    if (m->sel.texture != -1 && (k->tex < 0 || m->sel.texture != k->tex))
        return 0;
    if ((m->sel.flags & OP_SEL_ENV_ANIM) && !k->env) return 0;
    if ((m->sel.flags & OP_SEL_HP_TIER) && k->lp) return 0;
    if ((m->sel.flags & OP_SEL_LP_TIER) && !k->lp) return 0;
    /* Texturedness narrows: setting neither member selects both, so an
     * unqualified terrain selector reaches the gouraud faces (the LP
     * tier and the beyond-LOD band) as well as the textured ones. */
    if ((m->sel.flags & OP_SEL_TEXTURED) && k->untex) return 0;
    if ((m->sel.flags & OP_SEL_UNTEXTURED) && !k->untex) return 0;
    return 1;
}

/** @brief Return the declarative material that wins a decoded tag key, or -1.
 *
 * Considers live, user-enabled materials of enabled mods without a
 * per-instance callback, which resolve through slots only; among those
 * material_selector_matches accepts, the highest load_index, the
 * later-loaded mod, wins. */
static int material_winner(const material_key_t* k) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        mod_material_t* m = &s_mats[i];
        if (!m->live || !m->user_enabled ||
            !openpete_mods_is_enabled(m->owner)) continue;
        if (m->per_instance) continue;   /* per-instance winners resolve via slots only */
        if (!material_selector_matches(m, k)) continue;
        if (best < 0 ||
            m->load_index > s_mats[best].load_index)  /* later = higher prio */
            best = i;
    }
    return best;
}

int openpete_mods_material_resolve(uint32_t tag,
                                   openpete_mod_material_gpu_t* out) {
    material_key_t k;
    int slot = 0, best;
    if (!tag || s_mat_count == 0) return -1;
    material_key_from_tag(tag, &k);
    switch (material_slot_from_tag(tag, k.path, &slot)) {
    case 1:  return material_fill_gpu(view_slot_mat(slot), slot, out);
    case -1: return -1;
    default: break;   /* declarative resolve */
    }
    best = material_winner(&k);
    if (best < 0) return -1;
    return material_fill_gpu(best, 0, out);
}

/** @brief Body of openpete_mods_material_remove_owner, run under the registry write lock.
 *
 * For each live material the mod owns: retires every cached pipeline, drops
 * a parked selector, marks the slot dead and bumps the terrain epoch. The
 * slot is not reused. Re-arms the tag split once. */
static void openpete_mods_material_remove_owner_locked(const char* mod_id) {
    for (int i = 0; i < s_mat_count; i++) {
        if (!s_mats[i].live || strcmp(s_mats[i].owner, mod_id) != 0) continue;
        for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++)
            retire_pipeline(s_mats[i].pipeline[b]);
        retire_pipeline(s_mats[i].pipeline_mark);
        for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++)
            retire_pipeline(s_mats[i].pipeline_ter[b]);
        for (int b = 0; b < OPENPETE_MATERIAL_PORTAL_PIPES; b++) {
            retire_pipeline(s_mats[i].pipeline_portal[b]);
            s_mats[i].pipeline_portal[b] = NULL;
        }
        for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++) s_mats[i].pipeline[b] = NULL;
        for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++) s_mats[i].pipeline_ter[b] = NULL;
        s_mats[i].pipeline_mark = NULL;
        s_mats[i].sel_dirty = 0;   /* a parked retune dies with the material */
        s_mats[i].live = 0;
        s_mat_ter_epoch++;
    }
    material_rearm_split();
}
void openpete_mods_material_remove_owner(const char* mod_id) { REG_WR(); openpete_mods_material_remove_owner_locked(mod_id); REG_WR_END(); }


/** @brief Body of openpete_mods_material_retire_all, run under the registry write lock.
 *
 * Moves the cached pipelines of every live material to the retirement list
 * and clears the slots; the materials stay live and rebuild on next use. */
static void openpete_mods_material_retire_all_locked(void) {
    for (int i = 0; i < s_mat_count; i++) {
        if (!s_mats[i].live) continue;
        for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++)
            retire_pipeline(s_mats[i].pipeline[b]);
        retire_pipeline(s_mats[i].pipeline_mark);
        for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++)
            retire_pipeline(s_mats[i].pipeline_ter[b]);
        for (int b = 0; b < OPENPETE_MATERIAL_PORTAL_PIPES; b++) {
            retire_pipeline(s_mats[i].pipeline_portal[b]);
            s_mats[i].pipeline_portal[b] = NULL;
        }
        for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++) s_mats[i].pipeline[b] = NULL;
        for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++) s_mats[i].pipeline_ter[b] = NULL;
        s_mats[i].pipeline_mark = NULL;
    }
}
void openpete_mods_material_retire_all(void) { REG_WR(); openpete_mods_material_retire_all_locked(); REG_WR_END(); }

