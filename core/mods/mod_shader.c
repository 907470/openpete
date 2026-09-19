/* mod_shader.c — the mod GLSL compile service (shader tier S1).
 *
 * Mods ship GLSL source; the ENGINE compiles it to SPIR-V at load through
 * mod_glsl.c (in-process glslang, the dialect of the engine's own
 * shaders). Compiles are content-hash cached under <mod>/.build/shaders/
 * with the same stamp mechanics as the C compile-on-boot cache
 * (mod_loader.c mod_compute_key): stamp removed on failure, object kept.
 * A prebuilt "<path>.spv" beside a MISSING source loads instead
 * (binary-mod parity) but only with a "<path>.spv.ihash" sidecar
 * matching the current interface hash — a stale prebuilt refuses
 * cleanly instead of misrendering.
 *
 * A broken shader is a log line + an unavailable effect, never a
 * bricked boot. Registration is entry/tick context (file IO — refused
 * in present hooks).
 *
 * The later tiers' entry points (postfx_*,
 * material_register) live here as refusing stubs until their engine
 * plumbing lands — the API struct slots exist from the same version
 * bump so the ABI never moves again.
 */

#include "openpete_mods.h"
#include "openpete_log.h"
#include "openpete_native_render.h"   /* set_tag_split arming (S8) */
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
static int reg_wr_begin(void) {
    if (s_reg_reader_tid && s_reg_reader_tid == SDL_GetCurrentThreadID()) return 0;
    SDL_LockRWLockForWriting(reg_lock());
    return 1;
}
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

#define MOD_SHADER_MAX      256
#define MOD_SHADER_PATH_MAX OPENPETE_MOD_GLSL_PATH_MAX

typedef struct {
    const char* owner;       /* mod id (loader-owned string) */
    char        path[MOD_SHADER_PATH_MAX]; /* as registered, mod-relative */
    int         stage;       /* GLSLANG_STAGE_VERTEX / _FRAGMENT */
    uint32_t*   spv;         /* malloc'd SPIR-V words */
    size_t      spv_words;
    /* The same source compiled for the retained terrain pipelines
     * (OP_PROFILE_TERRAIN). NULL for anything that is not a material
     * fragment, and for one whose terrain profile failed to compile —
     * terrain draws then stay stock rather than the mod failing. */
    uint32_t*   spv_ter;
    size_t      spv_ter_words;
    int         live;        /* 0 = swept slot (stale handle) */
} mod_shader_t;

static mod_shader_t s_shaders[MOD_SHADER_MAX];
static int s_shader_count;

/* ------------------------------------------------------------------ */
/* small file helpers                                                  */

static int write_whole_file(const char* path, const void* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t put = fwrite(data, 1, len, f);
    fclose(f);
    return put == len ? 0 : -1;
}

/* Sorted names of files in `dir` whose suffix appears in the ;-list
 * `exts`. Local, deterministic (strcmp order) — feeds hashes. */
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

/* "shaders/crt.frag" -> "shaders_crt.frag" (flat cache filename) */
static void flatten_path(const char* path, char* out, size_t cap) {
    size_t i = 0;
    for (; path[i] && i + 1 < cap; i++)
        out[i] = (path[i] == '/' || path[i] == '\\') ? '_' : path[i];
    out[i] = '\0';
}

#define MOD_SHADER_STAGE_VERT OPENPETE_MOD_GLSL_STAGE_VERT
#define MOD_SHADER_STAGE_FRAG OPENPETE_MOD_GLSL_STAGE_FRAG

/* ------------------------------------------------------------------ */
/* build key: api version + glslang version + interface header bytes +
 * every shader-source file in the shader's own directory (sorted walk
 * not needed — hash the registered file THEN its siblings by name
 * via the directory key helper below). Sibling hashing makes an edit
 * to a mod-local include invalidate its includers (over-invalidation
 * across the dir is accepted: GLSL compiles are milliseconds). */

static void key_update_str(openpete_sha256_ctx* hc, const char* s) {
    openpete_sha256_update(hc, s, strlen(s));
}

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
 * any of it changes the SPIR-V a source produces — a cached .spv that
 * outlives an edit to one of these is a shader lying about its interface. */
static const char* const k_iface_files[] = {
    "openpete_shader_api.glsl",     /* postfx tier                        */
    "openpete_psx_material.glsl",   /* material tier: both engine halves  */
    "openpete_psx_decal.glsl",      /* decal draw path                    */
    "psx_key_contest.glsl",         /* og painter verdicts (both halves)  */
    "terrain_shade.glsl",           /* retained-terrain engine half       */
};

static void key_update_interface(openpete_sha256_ctx* hc) {
    for (size_t i = 0; i < sizeof k_iface_files / sizeof *k_iface_files; i++) {
        char p[MOD_SHADER_PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/shaders/%s", openpete_mods_sdk_dir(),
                 k_iface_files[i]);
        (void)key_update_file(hc, p);   /* absent = stable no-op */
    }
}

/* Interface hash (also the prebuilt-sidecar contract): api version +
 * interface header bytes. Deliberately EXCLUDES the glslang version —
 * a prebuilt .spv doesn't depend on the engine's compiler. */
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


/* One profile of one shader source: reuse the cached SPIR-V when the build
 * key still matches, else compile and cache it. `suffix` names the variant's
 * cache files ("" for the stock profile, ".terrain" for the retained terrain
 * engine half) and `preamble` is what glslang injects after #version.
 * Returns word count, 0 on failure — and on failure removes only the STAMP,
 * so a previously good .spv survives to be re-validated next boot. */
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
    (void)write_whole_file(spv_path, *out_words, n * 4);
    f = fopen(stamp_path, "wb");
    if (f) { fprintf(f, "%s\n", key); fclose(f); }
    OPL_INFO_F("mods", "%s: shader %s%s compiled (%zu words)", id, path,
               suffix, n);
    return n;
}

/* ------------------------------------------------------------------ */
/* the service                                                         */

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
    if (s_shader_count >= MOD_SHADER_MAX) {
        OPL_ERROR_F("mods", "%s: shader table full (%d)", id, MOD_SHADER_MAX);
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
    uint32_t* ter_words = NULL;
    size_t n_ter_words = 0;

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
                        "— effect unavailable", id, path, log_path);
            free(src);
            return -1;
        }
        /* A material fragment is compiled a second time for the retained
         * terrain pipelines, whose engine half of the contract takes flat
         * per-face varyings instead of a per-draw push block. The source is
         * the same source: only the profile define differs. Anything else
         * (postfx, decal, mesh vertex stages) has one profile. */
        if (openpete_mod_glsl_has_terrain_profile(src, stage)) {
            n_ter_words = compile_profile(src, stage, &ctx, key, build_dir, flat,
                                          ".terrain",
                                          OPENPETE_MOD_GLSL_TERRAIN_PREAMBLE,
                                          log_path, id, path, &ter_words);
            if (n_ter_words == 0)
                OPL_ERROR_F("mods", "%s: shader %s: terrain profile FAILED "
                            "(see %s) — terrain draws stay stock", id, path,
                            log_path);
        }
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
                        "sidecar — refusing the prebuilt (unverifiable "
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
                        "against a DIFFERENT shader interface — refused "
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

    mod_shader_t* sh = &s_shaders[s_shader_count];
    sh->owner = id;
    snprintf(sh->path, sizeof sh->path, "%s", path);
    sh->stage = stage;
    sh->spv = words;
    sh->spv_words = n_words;
    sh->spv_ter = ter_words;
    sh->spv_ter_words = n_ter_words;
    sh->live = 1;
    return s_shader_count++;
}
int openpete_mods_shader_register(openpete_mod_t* self, const char* path) { REG_WR(); int r_ = openpete_mods_shader_register_locked(self, path); REG_WR_END(); return r_; }


const uint32_t* openpete_mods_shader_spv(int handle, size_t* words,
                                         int* stage) {
    if (handle < 0 || handle >= s_shader_count || !s_shaders[handle].live)
        return NULL;
    if (words) *words = s_shaders[handle].spv_words;
    if (stage) *stage = s_shaders[handle].stage;
    return s_shaders[handle].spv;
}

/* The retained-terrain profile of a material fragment. NULL when the shader
 * has none (not a material, or its terrain compile failed) — the caller then
 * draws terrain stock rather than dropping the frame. */
const uint32_t* openpete_mods_shader_spv_terrain(int handle, size_t* words) {
    if (handle < 0 || handle >= s_shader_count || !s_shaders[handle].live)
        return NULL;
    if (words) *words = s_shaders[handle].spv_ter_words;
    return s_shaders[handle].spv_ter;
}

static void openpete_mods_shader_remove_owner_locked(const char* mod_id) {
    for (int i = 0; i < s_shader_count; i++) {
        if (!s_shaders[i].live || !s_shaders[i].owner) continue;
        if (strcmp(s_shaders[i].owner, mod_id) != 0) continue;
        free(s_shaders[i].spv);
        s_shaders[i].spv = NULL;
        s_shaders[i].spv_words = 0;
        free(s_shaders[i].spv_ter);
        s_shaders[i].spv_ter = NULL;
        s_shaders[i].spv_ter_words = 0;
        s_shaders[i].live = 0;   /* slot stays dead: stale handles error */
    }
}
void openpete_mods_shader_remove_owner(const char* mod_id) { REG_WR(); openpete_mods_shader_remove_owner_locked(mod_id); REG_WR_END(); }


/* Hot-reload watch companion (mod_loader.c hot_reload_poll): content
 * hash of every shader source under <mod>/shaders/. "-" when the dir
 * is absent. Sources elsewhere in the mod dir are keyed per-register
 * (shader_compute_key) but not watched — document shaders/ as THE
 * shader home. */
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
/* later-tier stubs: the ABI slots exist from api v2; each logs once
 * and refuses until its engine plumbing lands (postfx: S3/S4,
 * pipelines: S7, materials: S8 in plans/openpete-mod-shaders.md). */

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
/* postfx tier (S3): registry + the renderer-facing collect. The GPU
 * pipeline object is executor-owned (native_render.c) but cached in
 * the slot via pass.pipeline_slot; a sweep moves it to the retirement
 * list, drained by the renderer under its own frame discipline. */

#define MOD_POSTFX_MAX 64

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

static mod_postfx_t s_fx[MOD_POSTFX_MAX];
static int s_fx_count;
static void* s_retired_pipelines[MOD_POSTFX_MAX];
static int s_retired_n;

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
    if (s_fx_count >= MOD_POSTFX_MAX) {
        OPL_ERROR_F("mods", "%s: postfx table full (%d)", id, MOD_POSTFX_MAX);
        return -1;
    }
    mod_postfx_t* fx = &s_fx[s_fx_count];
    memset(fx, 0, sizeof *fx);
    fx->owner = id;
    fx->shader = frag_shader;
    fx->point = point;
    fx->order = order;
    fx->load_index = openpete_mods_load_index_of(self);
    fx->enabled = 0;             /* explicit postfx_enable arms it */
    fx->live = 1;
    return s_fx_count++;
}
int openpete_mods_postfx_register(openpete_mod_t* self, int frag_shader,
                                  int point, int order) { REG_WR(); int r_ = openpete_mods_postfx_register_locked(self, frag_shader, point, order); REG_WR_END(); return r_; }


static int openpete_mods_postfx_enable_locked(openpete_mod_t* self, int fx, int enabled) {
    const char* id = openpete_mods_id_of(self);
    if (!id || fx < 0 || fx >= s_fx_count || !s_fx[fx].live) return -1;
    if (strcmp(s_fx[fx].owner, id) != 0) return -1;   /* not yours */
    s_fx[fx].enabled = enabled ? 1 : 0;
    return 0;
}
int openpete_mods_postfx_enable(openpete_mod_t* self, int fx, int enabled) { REG_WR(); int r_ = openpete_mods_postfx_enable_locked(self, fx, enabled); REG_WR_END(); return r_; }


static int openpete_mods_postfx_set_uniforms_locked(openpete_mod_t* self, int fx,
                                      const void* data, uint32_t size) {
    const char* id = openpete_mods_id_of(self);
    if (!id || fx < 0 || fx >= s_fx_count || !s_fx[fx].live || !data) return -1;
    if (strcmp(s_fx[fx].owner, id) != 0) return -1;
    if (size > OPENPETE_POSTFX_UNIFORMS_MAX) size = OPENPETE_POSTFX_UNIFORMS_MAX;
    memcpy(s_fx[fx].uniforms, data, size);   /* copy-on-call, any context */
    s_fx[fx].uniforms_len = size;
    return 0;
}
int openpete_mods_postfx_set_uniforms(openpete_mod_t* self, int fx,
                                      const void* data, uint32_t size) { REG_WR(); int r_ = openpete_mods_postfx_set_uniforms_locked(self, fx, data, size); REG_WR_END(); return r_; }


/* Renderer-facing: enabled passes at `point`, ordered by enabled-list
 * priority (load index) then per-mod order then registration. The fx
 * table is small — selection sort into the out array. */
int openpete_mods_postfx_collect(int point, openpete_postfx_pass_t* out,
                                 int cap) {
    int idx[MOD_POSTFX_MAX];
    int n = 0;
    for (int i = 0; i < s_fx_count; i++) {
        if (!s_fx[i].live || !s_fx[i].enabled || s_fx[i].point != point)
            continue;
        if (!openpete_mods_is_enabled(s_fx[i].owner)) continue;  /* live toggle */
        if (n < cap) idx[n++] = i;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            mod_postfx_t *a = &s_fx[idx[j - 1]], *b = &s_fx[idx[j]];
            int swap = (a->load_index > b->load_index) ||
                       (a->load_index == b->load_index && a->order > b->order) ||
                       (a->load_index == b->load_index && a->order == b->order &&
                        idx[j - 1] > idx[j]);
            if (!swap) break;
            int t = idx[j - 1]; idx[j - 1] = idx[j]; idx[j] = t;
        }
    for (int i = 0; i < n; i++) {
        mod_postfx_t* fx = &s_fx[idx[i]];
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
    int n = s_retired_n < cap ? s_retired_n : cap;
    memcpy(out, s_retired_pipelines, (size_t)n * sizeof(void*));
    memmove(s_retired_pipelines, s_retired_pipelines + n,
            (size_t)(s_retired_n - n) * sizeof(void*));
    s_retired_n -= n;
    return n;
}

/* Shutdown sweep: every cached pipeline (enabled or not) to the
 * retirement list; registrations stay live (process is exiting or the
 * renderer is about to drain). */
static void openpete_mods_postfx_retire_all_locked(void) {
    for (int i = 0; i < s_fx_count; i++) {
        if (!s_fx[i].live) continue;
        if (s_fx[i].pipeline && s_fx[i].pipeline != OPENPETE_POSTFX_PIPE_FAILED &&
            s_retired_n < MOD_POSTFX_MAX)
            s_retired_pipelines[s_retired_n++] = s_fx[i].pipeline;
        s_fx[i].pipeline = NULL;
    }
}
void openpete_mods_postfx_retire_all(void) { REG_WR(); openpete_mods_postfx_retire_all_locked(); REG_WR_END(); }


static void openpete_mods_postfx_remove_owner_locked(const char* mod_id) {
    for (int i = 0; i < s_fx_count; i++) {
        if (!s_fx[i].live || strcmp(s_fx[i].owner, mod_id) != 0) continue;
        if (s_fx[i].pipeline && s_fx[i].pipeline != OPENPETE_POSTFX_PIPE_FAILED &&
            s_retired_n < MOD_POSTFX_MAX)
            s_retired_pipelines[s_retired_n++] = s_fx[i].pipeline;
        s_fx[i].pipeline = NULL;
        s_fx[i].live = 0;
    }
}
void openpete_mods_postfx_remove_owner(const char* mod_id) { REG_WR(); openpete_mods_postfx_remove_owner_locked(mod_id); REG_WR_END(); }


/* ------------------------------------------------------------------ */
/* material-selector tier (S8a declarative + S8b per-instance).
 *
 * S8a: whole-class overrides matched at DRAW time against the S6
 * provenance tag — extractors stay selector-agnostic. S8b: a
 * per-instance callback runs at EXTRACTION (the moby walk, cpu
 * available) per matching instance, returns a per-draw uniform block
 * or declines. Its outcome is interned to a small per-present SLOT id
 * stamped into the tag's spare bits, so the batcher splits draws by
 * outcome (two instances of one class with different callback results
 * land in different batches); the slot's uniform block binds at the
 * override draw. Conflicts: enabled-list priority, FIRST match wins.
 *
 * Per-present, not identity-replayed: reextract_record_pass re-walks
 * every present, and the callback reads TICK-STABLE guest RAM (the
 * sub-tick spine interpolates render-side only, never mutating
 * gameplay RAM — the oracle guarantees it), so per-present invocation
 * yields identical results across a canonical+intermediates span (no
 * strobing). Canonical-compute + identity-replay would only save the
 * callback cost on intermediates; deferred as a pure optimization. */

#define MOD_MATERIAL_MAX 32

typedef struct {
    const char* owner;
    openpete_mod_material_selector_t sel;
    int  vert, frag;       /* shader handles; -1 = stock stage */
    openpete_mod_material_fn per_instance;   /* NULL = declarative (S8a) */
    int  load_index;
    /* Executor-cached rhi_pipeline*, one per BLEND CLASS (0 opaque, 1
     * average, 2 additive, 3 subtract). A material reshades inside
     * whatever group it lands in and must not change how that group
     * composites, so the same shader needs a pipeline per blend. */
    /* One built pipeline per draw-state variant this material can be
     * asked for: [0..3] = PSX blend classes, [4] = opaque with depth
     * write off (portal sky), [5] = the DECAL PROFILE (moby shadows:
     * a different vertex stage, descriptor sets and push block — see
     * openpete_psx_decal.glsl). */
    void* pipeline[OPENPETE_MATERIAL_PIPE_VARIANTS];
    void* pipeline_mark;   /* stencil-MARK twin (hole-fill WORLD/ACTOR groups) */
    /* Retained-terrain pipelines, one per (arena vertex stage x colour
     * slot). Separate from `pipeline` because the terrain layout, the
     * vertex input and the engine half of the contract all differ. */
    void* pipeline_ter[OPENPETE_MATERIAL_TER_PIPES];
    int   user_enabled;    /* material_enable — the mod's own on/off */
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
static int s_mat_sel_dirty;      /* any material carries a parked selector */
static int s_mat_has_instance;   /* cached: any live selector has a callback */

/* Per-present interned per-instance slots (S8b). Index 0 reserved
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
static int view_slot_n(void) { return s_mat_view ? s_mat_view->slot_n : s_mat_slot_n; }
static int view_slot_mat(int s) { return s_mat_view ? s_mat_view->slots[s].mat : s_mat_slots[s].mat; }
static int view_static_slot(int m) { return s_mat_view ? s_mat_view->static_slot[m] : s_mats[m].static_slot; }

/* Slot ids are minted from ONE global counter but stamped into per-path
 * tag fields of differing width, so the table can never outgrow the
 * NARROWEST field that carries an id. Terrain's is the narrowest at 9
 * bits, and its decode masks (tag >> 16) & 0x1FF: an id of 512 would
 * come back as 0, which reads as "defer to declarative resolve" — the
 * wrong material, silently, with no crash and no log. 0x1FF itself is
 * the declined->stock sentinel, so the last usable id is 510. Raising
 * MOD_MATERIAL_SLOTS means widening that field first. */
typedef char nr_assert_mat_slots_fit_narrowest_tag_field[
    (MOD_MATERIAL_SLOTS <= OPENPETE_MATERIAL_TERRAIN_STOCK) ? 1 : -1];

/* native_render arms the batcher tag-split while any selector lives;
 * s_mat_has_instance gates the moby walk's per-instance resolve.
 *
 * Arming keys on REGISTRATION (live), never on the owner's live-toggle
 * state: entry — and therefore material_register — runs before the
 * loader marks a mod enabled, so an enabled-keyed arm would read 0 at
 * boot and never re-arm. A disabled owner's selectors therefore still
 * cost a batch split and a winner scan, but can produce no override:
 * every winner/resolve path consults openpete_mods_is_enabled. */
/* The retained terrain cache bakes each face's winning selector into the face
 * itself, so it must re-bake whenever the TERRAIN-relevant selector set moves.
 * This counter is that key, and it is deliberately NARROW: a counter that also
 * moved for moby, sky, flame and particle materials — and for every enable
 * toggle of one — re-baked the whole cache for changes that cannot alter a
 * single terrain bucket, which cost 2.7x the frame rate while a mod was
 * toggling rows (terrain.build 33% -> 73% of sim_tick). Move it only for
 * changes terrain_winner() could actually see. */
static unsigned s_mat_ter_epoch;

/* Could this material ever bucket a terrain face — ignoring whether it is
 * enabled right now? Deliberately NOT terrain_sel_usable(): the enable state
 * is what we are deciding whether to react to, and the level is not known at
 * every bump site. Wider than the truth is safe here (a spurious re-bake);
 * narrower would leave faces stamped for a selector that no longer wins. */
static int terrain_sel_relevant(const mod_material_t* m) {
    return m->live && (m->sel.channel == -1 || m->sel.channel == OP_CHAN_TERRAIN);
}

static void material_rearm_split(void) {
    int any = 0, inst = 0;
    for (int i = 0; i < s_mat_count; i++)
        if (s_mats[i].live) { any = 1; if (s_mats[i].per_instance) inst = 1; }
    s_mat_has_instance = inst;
    openpete_native_set_tag_split(any);
}

int openpete_mods_material_has_instance(void) { return s_mat_has_instance; }

unsigned openpete_mods_material_terrain_epoch(void) {
    /* Sum of two monotonic counters — strictly increases on any change the
     * terrain bake cares about; +1 keeps 0 free as the latch's "unset". The
     * mod toggle epoch rides along because terrain_sel_usable() consults
     * openpete_mods_is_enabled(owner). */
    return 1u + s_mat_ter_epoch + openpete_mods_toggle_epoch();
}

static int openpete_mods_current_level(void);   /* defined with s_cur_level */

/* Is this material live, enabled, on the terrain channel, and in scope for
 * the level being rendered? The level test mirrors the resolve's: without
 * it a level-scoped row would bucket faces in EVERY level. */
static int terrain_sel_usable(const mod_material_t* m, int level) {
    if (!m->live || !m->user_enabled || !openpete_mods_is_enabled(m->owner))
        return 0;
    if (m->sel.channel != -1 && m->sel.channel != OP_CHAN_TERRAIN) return 0;
    if (m->sel.level != -1 && level >= 0 && m->sel.level != level) return 0;
    return 1;
}

/* The winning terrain selector for a face key (-1 = none), by the same
 * predicates and the same precedence as the draw-time resolve: a
 * PER-INSTANCE selector wins outright (the walked path's tag carries its
 * slot, which short-circuits the declarative scan), and among selectors of
 * one kind the later-loaded one wins. */
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
             * on OP_SEL_UNTEXTURED + texture=-1, the S9 texture resolve on
             * texture==texid with the flag clear — neither reads the
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

int openpete_mods_material_terrain_bucket_max(void) {
    if (s_mat_view) return s_mat_view->ter_bucket_max;
    int max = 0, level = openpete_mods_current_level();
    for (int i = 0; i < s_mat_count; i++) {
        if (!terrain_sel_usable(&s_mats[i], level)) continue;
        if (i + 1 > max && i + 1 <= OPENPETE_MATERIAL_TER_BUCKET_MAX) max = i + 1;
    }
    return max;
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

/* Refuse a selector this engine cannot fully honour, rather than matching
 * wider than its author asked for. Size alone is not enough: a new flag bit
 * never changes sizeof, so the flags are checked against the known mask too.
 *
 * Shared by register and set_selector so a retune can never install a key the
 * register path would have refused. */
static int material_sel_valid(const char* id,
                              const openpete_mod_material_selector_t* sel) {
    if (sel->struct_size != (uint32_t)sizeof *sel) {
        OPL_ERROR_F("mods", "%s: material: selector size %u, engine expects %u "
                    "— rebuild the mod against this SDK",
                    id, sel->struct_size, (unsigned)sizeof *sel);
        return 0;
    }
    if (sel->flags & ~(unsigned)OP_SEL_FLAGS_KNOWN) {
        OPL_ERROR_F("mods", "%s: material: selector carries unknown flag bits "
                    "%#x — this engine would match wider than you asked",
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

static int openpete_mods_material_register_locked(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance) {
    const char* id = openpete_mods_id_of(self);
    if (!id || !sel) return -1;
    if (openpete_mods_in_present) {
        OPL_ERROR_F("mods", "%s: material_register refused inside a present "
                    "hook — register at entry, and retune a live selector "
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


/* Turn one registered material on or off. */
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


/* Set the uniform block a material's shader reads. */
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


/* Retune a live material's selector, keeping its handle, shader, callback and
 * enabled state. Re-registering instead would burn a table slot per change
 * and leave the old selector matching forever, and is refused outright inside
 * a present — which is exactly where a mod reacting to its own UI runs. */
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


/* Install selectors parked by a present-context retune. */
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


/* The winning selector index for a moby of class `cls` (enabled-list
 * priority, first match wins), or -1. Mobys have no env/tier, so those
 * flags exclude a moby match. */
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
 * itself — the symbol is game-specific — so the game pushes it while it
 * renders. Draw-time resolve is the only consumer, and -1 (nothing has
 * pushed yet) makes every level key inert rather than wrongly matching. */
static int s_cur_level = -1;
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
/* One int, written between scenes and read at draw: no lock, and never a
 * wait behind a compose blocked on the display. */
void openpete_mods_set_level(int level_id) { openpete_mods_set_level_locked(level_id); }

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
 * native_render.c). `cap` is the caller's slot capacity; returns the count
 * copied (1 + interned). Bytes are packed at OPENPETE_MATERIAL_UNIFORMS_MAX
 * stride so the compose can index them the same way as the live table. */
void openpete_mods_material_view_capture(openpete_mods_material_view_t* out) {
    out->slot_n = s_mat_slot_n;
    for (int i = 1; i < s_mat_slot_n; i++) {
        out->slots[i].mat = s_mat_slots[i].mat;
        out->slots[i].len = s_mat_slots[i].len;
        memcpy(out->slots[i].bytes, s_mat_slots[i].bytes, s_mat_slots[i].len);
    }
    for (int i = 0; i < MOD_MATERIAL_MAX; i++) out->static_slot[i] = s_mats[i].static_slot;
    out->ter_bucket_max = openpete_mods_material_terrain_bucket_max();
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
    if (w < 0 || !s_mats[w].per_instance) return 0;   /* declarative → draw-time */

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
    if (rc < 0) return OPENPETE_MATERIAL_SLOT_STOCK;   /* declined → stock */
    if (rc > (int)sizeof buf) rc = (int)sizeof buf;

    /* Intern (winner, bytes): identical outcomes share a slot so they
     * stay in ONE batch. Linear scan — the interned set is small. */
    for (int s = 1; s < s_mat_slot_n; s++)
        if (s_mat_slots[s].mat == w && s_mat_slots[s].len == (uint32_t)rc &&
            memcmp(s_mat_slots[s].bytes, buf, (size_t)rc) == 0)
            return s;
    if (s_mat_slot_n >= MOD_MATERIAL_SLOTS)
        return OPENPETE_MATERIAL_SLOT_STOCK;   /* table full → graceful stock */
    int s = s_mat_slot_n++;
    s_mat_slots[s].mat = w;
    s_mat_slots[s].len = (uint32_t)rc;
    memcpy(s_mat_slots[s].bytes, buf, (size_t)rc);
    return s;
}

/* Winner among per-instance TERRAIN selectors carrying OP_SEL_UNTEXTURED —
 * the untextured (beyond-LOD gouraud) band resolve. */
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

/* Winner among per-instance TERRAIN selectors keyed on a textured face's
 * Tiledef id (S9). */
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
         * per-instance stays the untex band's business (S8c), never a
         * blanket all-textured-terrain override. */
        if (m->sel.texture == -1 || m->sel.texture != texid) continue;
        if (best < 0 || m->load_index > s_mats[best].load_index) best = i;
    }
    return best;
}

/* The refine outcome for one per-instance TERRAIN material, as an interned
 * uniform slot, computed ONCE per present.
 *
 * A terrain refine selector carries exactly one key — a Tiledef id, or the
 * untextured band — so "the material's outcome" and "this key's outcome" are
 * the same thing, and one memo per material covers every face it reaches.
 * That is what lets a RETAINED face have a refine material at all: the
 * outcome is not derivable from a baked face, but it does not have to be,
 * because it does not vary across the faces of one material.
 *
 * Computing it in one place is also what keeps the two residencies on one
 * answer. It used to be driven from the walk's emit loop, which a retained
 * face never enters; both paths now read this, in the same per-present
 * context, from the same key.
 *
 * Returns 0 (no winner / not per-instance) or an interned slot, or
 * OPENPETE_MATERIAL_TERRAIN_STOCK when the callback declined. */
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

/* Resolve the texture-keyed material for one textured terrain face id. The
 * callback receives the Tiledef id in the key. Returns the interned slot for
 * OPENPETE_TAG_TERRAIN_TEX (0 = no per-instance winner -> declarative
 * resolve; OPENPETE_MATERIAL_TERRAIN_STOCK = the callback declined). */
int openpete_mods_material_terrain_tex_slot(CPUState* cpu, int texid) {
    if (!s_mat_has_instance) return 0;
    (void)cpu;
    return terrain_refine_slot(material_terrain_tex_winner(texid));
}

/* Resolve the untextured-terrain material once per extraction (terrain has
 * no instances — the whole band shares one outcome). The callback receives
 * moby_vaddr 0. Returns the interned slot for OPENPETE_TAG_TERRAIN_UNTEX
 * (0 = no per-instance winner -> declarative resolve;
 * OPENPETE_MATERIAL_TERRAIN_STOCK = the callback declined). */
int openpete_mods_material_terrain_untex_slot(CPUState* cpu) {
    if (!s_mat_has_instance) return 0;
    (void)cpu;
    return terrain_refine_slot(material_terrain_untex_winner());
}

/* An interned slot can outlive its winner's usability: extraction interns
 * the slot, the tick boundary (live-toggle apply AND hot-reload sweep)
 * runs next, and only then does this present draw. A disabled owner must
 * stop THIS present, not the next one; a swept selector is worse — its
 * SPIR-V is freed and its retired pipeline slot is one no later sweep
 * revisits, so a rebuild through it would cache a pipeline nothing can
 * ever destroy. Both cases fall back to the stock draw. */
static int material_slot_usable(int slot) {
    int m = view_slot_mat(slot);
    return m >= 0 && m < s_mat_count && s_mats[m].live &&
           openpete_mods_is_enabled(s_mats[m].owner);
}

/* Fill `out` from selector index `best` + per-instance uniform slot. */
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
    out->owner = m->owner;
    /* A per-instance slot always wins; otherwise fall back to this
     * material's static block (0 = no params at all). */
    out->uniform_slot = uniform_slot > 0 ? uniform_slot : view_static_slot(best);
    return best;
}

/* Draw-time resolve (native_render draw_tex_runs): the run's S6 tag →
 * the winning override, or -1. Tag layout: [31:28] path+1; moby class
 * [15:0] + per-instance slot [27:16] (S8b); terrain env bit 27 / LP
 * bit 26 + sector [8:0]. Enabled-list priority, FIRST match wins. */
int openpete_mods_material_resolve(uint32_t tag,
                                   openpete_mod_material_gpu_t* out) {
    if (!tag || s_mat_count == 0) return -1;
    int path = (int)(tag >> 28) - 1;              /* 0 = moby, 1 = terrain */

    if (path == 0) {   /* moby: check the per-instance slot first (S8b) */
        int slot = (int)((tag >> 16) & 0xFFFu);
        if (slot == OPENPETE_MATERIAL_SLOT_STOCK) return -1;   /* declined */
        if (slot != 0) {
            if (slot >= view_slot_n()) return -1;   /* stale (post-reset) */
            if (!material_slot_usable(slot)) return -1;   /* toggled/swept */
            return material_fill_gpu(view_slot_mat(slot), slot, out);
        }
        /* slot 0 → fall through to declarative class resolve */
    }

    int untex = (path == 1) && (tag & 0x02000000u);
    if (path == 1) {   /* terrain: per-present slot (S8c untex / S9 texture) */
        int slot = (int)((tag >> 16) & 0x1FFu);
        if (slot == OPENPETE_MATERIAL_TERRAIN_STOCK) return -1;   /* declined */
        if (slot != 0) {
            if (slot >= view_slot_n()) return -1;   /* stale (post-reset) */
            if (!material_slot_usable(slot)) return -1;   /* toggled/swept */
            return material_fill_gpu(view_slot_mat(slot), slot, out);
        }
        /* slot 0 → fall through to declarative resolve */
    }

    int cls  = (path == 0) ? (int)(tag & 0xFFFFu) : -1;
    int env  = (path == 1) && (tag & 0x08000000u);
    int lp   = (path == 1) && (tag & 0x04000000u);
    int tex  = (path == 1 && !untex) ? (int)((tag >> 9) & 0x7Fu) : -1;
    /* Channel == the raw path nibble. The other feeders (player, flame,
     * particle, sky, shadow, tracer) all carry their identity in [15:0],
     * so one comparison serves every one of them. */
    int chan  = (int)(tag >> 28);
    int ident = (int)(tag & 0xFFFFu);
    int level = openpete_mods_current_level();
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        mod_material_t* m = &s_mats[i];
        if (!m->live || !m->user_enabled ||
            !openpete_mods_is_enabled(m->owner)) continue;
        if (m->per_instance) continue;   /* per-instance winners resolve via slots only */
        if (m->sel.channel != -1 && m->sel.channel != chan) continue;
        if (m->sel.level != -1 && level >= 0 && m->sel.level != level) continue;
        /* Identity is meaningful for every channel EXCEPT terrain, whose
         * [15:0] is sector|texid rather than a channel-local id and which
         * has its own keys below. */
        if (m->sel.ident != -1 && path != 1 && m->sel.ident != ident) continue;
        if (path == 0 && m->sel.moby_class != -1 && m->sel.moby_class != cls)
            continue;
        /* Texture key (S9): terrain textured faces only. */
        if (m->sel.texture != -1 && (tex < 0 || m->sel.texture != tex))
            continue;
        if ((m->sel.flags & OP_SEL_ENV_ANIM) && !env) continue;
        if ((m->sel.flags & OP_SEL_HP_TIER) && lp) continue;
        if ((m->sel.flags & OP_SEL_LP_TIER) && !lp) continue;
        /* Texturedness NARROWS: setting neither member selects both, so an
         * unqualified terrain selector reaches the gouraud faces (the LP
         * tier and the beyond-LOD band) as well as the textured ones. */
        if ((m->sel.flags & OP_SEL_TEXTURED) && untex) continue;
        if ((m->sel.flags & OP_SEL_UNTEXTURED) && !untex) continue;
        if (best < 0 ||
            m->load_index > s_mats[best].load_index)  /* later = higher prio */
            best = i;
    }
    if (best < 0) return -1;
    return material_fill_gpu(best, 0, out);
}

static void openpete_mods_material_remove_owner_locked(const char* mod_id) {
    for (int i = 0; i < s_mat_count; i++) {
        if (!s_mats[i].live || strcmp(s_mats[i].owner, mod_id) != 0) continue;
        for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++)
            if (s_mats[i].pipeline[b] &&
                s_mats[i].pipeline[b] != OPENPETE_POSTFX_PIPE_FAILED &&
                s_retired_n < MOD_POSTFX_MAX)
                s_retired_pipelines[s_retired_n++] = s_mats[i].pipeline[b];
        if (s_mats[i].pipeline_mark &&
            s_mats[i].pipeline_mark != OPENPETE_POSTFX_PIPE_FAILED &&
            s_retired_n < MOD_POSTFX_MAX)
            s_retired_pipelines[s_retired_n++] = s_mats[i].pipeline_mark;
        for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++)
            if (s_mats[i].pipeline_ter[b] &&
                s_mats[i].pipeline_ter[b] != OPENPETE_POSTFX_PIPE_FAILED &&
                s_retired_n < MOD_POSTFX_MAX)
                s_retired_pipelines[s_retired_n++] = s_mats[i].pipeline_ter[b];
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


static void openpete_mods_material_retire_all_locked(void) {
    for (int i = 0; i < s_mat_count; i++) {
        if (!s_mats[i].live) continue;
        for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++)
            if (s_mats[i].pipeline[b] &&
                s_mats[i].pipeline[b] != OPENPETE_POSTFX_PIPE_FAILED &&
                s_retired_n < MOD_POSTFX_MAX)
                s_retired_pipelines[s_retired_n++] = s_mats[i].pipeline[b];
        if (s_mats[i].pipeline_mark &&
            s_mats[i].pipeline_mark != OPENPETE_POSTFX_PIPE_FAILED &&
            s_retired_n < MOD_POSTFX_MAX)
            s_retired_pipelines[s_retired_n++] = s_mats[i].pipeline_mark;
        for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++)
            if (s_mats[i].pipeline_ter[b] &&
                s_mats[i].pipeline_ter[b] != OPENPETE_POSTFX_PIPE_FAILED &&
                s_retired_n < MOD_POSTFX_MAX)
                s_retired_pipelines[s_retired_n++] = s_mats[i].pipeline_ter[b];
        for (int b = 0; b < OPENPETE_MATERIAL_PIPE_VARIANTS; b++) s_mats[i].pipeline[b] = NULL;
        for (int b = 0; b < OPENPETE_MATERIAL_TER_PIPES; b++) s_mats[i].pipeline_ter[b] = NULL;
        s_mats[i].pipeline_mark = NULL;
    }
}
void openpete_mods_material_retire_all(void) { REG_WR(); openpete_mods_material_retire_all_locked(); REG_WR_END(); }

