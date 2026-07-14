/* mod_shader.c — the mod GLSL compile service (shader tier S1).
 *
 * Mods ship GLSL source; the ENGINE compiles it to SPIR-V at load with
 * in-process glslang — the same compiler and dialect (vulkan1.0) that
 * builds the engine's own shaders — so no shader toolchain ships in any
 * dist. Compiles are content-hash cached under <mod>/.build/shaders/
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
 * The later tiers' entry points (postfx_*, pipeline_register/draw_mesh,
 * material_register) live here as refusing stubs until their engine
 * plumbing lands — the API struct slots exist from the same version
 * bump so the ABI never moves again.
 */

#include "openpete_mods.h"
#include "openpete_log.h"
#include "../host/sha256.h"

#ifndef OPENPETE_NO_GLSLANG
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <glslang/build_info.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#define shader_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define shader_mkdir(p) mkdir((p), 0755)
#endif

#define MOD_SHADER_MAX      256
#define MOD_SHADER_SRC_MAX  (1u << 20)   /* 1 MiB source cap */
#define MOD_SHADER_PATH_MAX 512

typedef struct {
    const char* owner;       /* mod id (loader-owned string) */
    char        path[MOD_SHADER_PATH_MAX]; /* as registered, mod-relative */
    int         stage;       /* GLSLANG_STAGE_VERTEX / _FRAGMENT */
    uint32_t*   spv;         /* malloc'd SPIR-V words */
    size_t      spv_words;
    int         live;        /* 0 = swept slot (stale handle) */
} mod_shader_t;

static mod_shader_t s_shaders[MOD_SHADER_MAX];
static int s_shader_count;

/* ------------------------------------------------------------------ */
/* small file helpers                                                  */

static char* read_whole_file(const char* path, size_t* len_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (size_t)n > MOD_SHADER_SRC_MAX) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) *len_out = got;
    return buf;
}

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

/* Stage values mirror glslang_stage_t so handles stay meaningful when
 * the engine is built without glslang (OPENPETE_NO_GLSLANG: prebuilt
 * .spv still loads; source compiles refuse). */
#define MOD_SHADER_STAGE_VERT 0   /* == GLSLANG_STAGE_VERTEX   */
#define MOD_SHADER_STAGE_FRAG 4   /* == GLSLANG_STAGE_FRAGMENT */

static int stage_from_path(const char* path) {
    size_t n = strlen(path);
    if (n >= 5 && strcmp(path + n - 5, ".vert") == 0) return MOD_SHADER_STAGE_VERT;
    if (n >= 5 && strcmp(path + n - 5, ".frag") == 0) return MOD_SHADER_STAGE_FRAG;
    return -1;
}

/* ------------------------------------------------------------------ */
/* include resolution: "openpete_shader_api.glsl" -> the staged SDK
 * copy; everything else resolves relative to the including shader's
 * own directory. */

typedef struct {
    char shader_dir[MOD_SHADER_PATH_MAX];  /* dir of the registered shader */
    char sdk_shader_dir[MOD_SHADER_PATH_MAX];
} include_ctx_t;

#ifndef OPENPETE_NO_GLSLANG
static glsl_include_result_t* include_load(const char* dir, const char* name) {
    char p[MOD_SHADER_PATH_MAX * 2];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    size_t len = 0;
    char* data = read_whole_file(p, &len);
    if (!data) return NULL;
    glsl_include_result_t* r = (glsl_include_result_t*)calloc(1, sizeof *r);
    if (!r) { free(data); return NULL; }
    r->header_name = strdup(name);
    r->header_data = data;
    r->header_length = len;
    return r;
}

static glsl_include_result_t* include_resolve(void* ctx_v, const char* name,
                                              const char* includer_name,
                                              size_t depth) {
    include_ctx_t* ctx = (include_ctx_t*)ctx_v;
    (void)includer_name;
    if (depth > 8) return NULL;                       /* cycle guard */
    glsl_include_result_t* r = include_load(ctx->sdk_shader_dir, name);
    if (!r) r = include_load(ctx->shader_dir, name);
    return r;
}

static glsl_include_result_t* include_system(void* ctx, const char* name,
                                             const char* inc, size_t depth) {
    return include_resolve(ctx, name, inc, depth);
}
static glsl_include_result_t* include_local(void* ctx, const char* name,
                                            const char* inc, size_t depth) {
    return include_resolve(ctx, name, inc, depth);
}
static int include_free(void* ctx, glsl_include_result_t* r) {
    (void)ctx;
    if (r) {
        free((void*)r->header_name);
        free((void*)r->header_data);
        free(r);
    }
    return 0;
}
#endif /* !OPENPETE_NO_GLSLANG */

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
    char* data = read_whole_file(path, &len);
    if (!data) return -1;
    openpete_sha256_update(hc, data, len);
    free(data);
    return 0;
}

/* Interface hash (also the prebuilt-sidecar contract): api version +
 * interface header bytes. Deliberately EXCLUDES the glslang version —
 * a prebuilt .spv doesn't depend on the engine's compiler. */
static void interface_hash(char out_hex[65]) {
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    const unsigned api_ver = OPENPETE_MOD_API_VERSION;
    openpete_sha256_update(&hc, &api_ver, sizeof api_ver);
    char p[MOD_SHADER_PATH_MAX * 2];
    snprintf(p, sizeof p, "%s/shaders/openpete_shader_api.glsl",
             openpete_mods_sdk_dir());
    (void)key_update_file(&hc, p);   /* absent = stable no-op */
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
    {
        char p[MOD_SHADER_PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/shaders/openpete_shader_api.glsl",
                 openpete_mods_sdk_dir());
        (void)key_update_file(&hc, p);
    }
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

static void write_log(const char* log_path, const char* phase,
                      const char* a, const char* b) {
    FILE* f = fopen(log_path, "wb");
    if (!f) return;
    fprintf(f, "[%s]\n%s\n%s\n", phase, a ? a : "", b ? b : "");
    fclose(f);
}

/* GLSL source -> malloc'd SPIR-V words. Returns word count, 0 on error
 * (details in log_path). */
#ifdef OPENPETE_NO_GLSLANG
static size_t compile_glsl(const char* src, int stage,
                           const include_ctx_t* inc_ctx,
                           const char* log_path, uint32_t** out_words) {
    (void)src; (void)stage; (void)inc_ctx; (void)out_words;
    write_log(log_path, "compile",
              "engine built without glslang (OPENPETE_BUILD_GLSLANG=OFF); "
              "ship a prebuilt .spv + .spv.ihash sidecar instead", NULL);
    return 0;
}
#else
static size_t compile_glsl(const char* src, int stage,
                           const include_ctx_t* inc_ctx,
                           const char* log_path, uint32_t** out_words) {
    static int s_glslang_up = 0;
    if (!s_glslang_up) {
        glslang_initialize_process();  /* process-lifetime; never finalized */
        s_glslang_up = 1;
    }

    glslang_input_t input;
    memset(&input, 0, sizeof input);
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = (glslang_stage_t)stage;
    input.client = GLSLANG_CLIENT_VULKAN;
    /* vulkan1.0 — matches the engine's own `glslangValidator -V` builds */
    input.client_version = GLSLANG_TARGET_VULKAN_1_0;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_0;
    input.code = src;
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = GLSLANG_MSG_DEFAULT_BIT;
    input.resource = glslang_default_resource();
    input.callbacks.include_system = include_system;
    input.callbacks.include_local = include_local;
    input.callbacks.free_include_result = include_free;
    input.callbacks_ctx = (void*)inc_ctx;

    glslang_shader_t* sh = glslang_shader_create(&input);
    if (!sh) return 0;
    if (!glslang_shader_preprocess(sh, &input) ||
        !glslang_shader_parse(sh, &input)) {
        write_log(log_path, "parse", glslang_shader_get_info_log(sh),
                  glslang_shader_get_info_debug_log(sh));
        glslang_shader_delete(sh);
        return 0;
    }
    glslang_program_t* prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT |
                                    GLSLANG_MSG_VULKAN_RULES_BIT)) {
        write_log(log_path, "link", glslang_program_get_info_log(prog),
                  glslang_program_get_info_debug_log(prog));
        glslang_program_delete(prog);
        glslang_shader_delete(sh);
        return 0;
    }
    glslang_program_SPIRV_generate(prog, (glslang_stage_t)stage);
    size_t words = glslang_program_SPIRV_get_size(prog);
    uint32_t* buf = words ? (uint32_t*)malloc(words * sizeof(uint32_t)) : NULL;
    if (buf) glslang_program_SPIRV_get(prog, buf);
    const char* spv_msg = glslang_program_SPIRV_get_messages(prog);
    if (spv_msg && *spv_msg)
        OPL_INFO_F("mods", "shader: glslang: %s", spv_msg);
    glslang_program_delete(prog);
    glslang_shader_delete(sh);
    if (!buf) return 0;
    *out_words = buf;
    return words;
}
#endif /* OPENPETE_NO_GLSLANG */

/* ------------------------------------------------------------------ */
/* the service                                                         */

int openpete_mods_shader_register(openpete_mod_t* self, const char* path) {
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
    int stage = stage_from_path(path);
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
    char spv_path[MOD_SHADER_PATH_MAX * 3], stamp_path[MOD_SHADER_PATH_MAX * 3];
    char log_path[MOD_SHADER_PATH_MAX * 3];
    snprintf(spv_path, sizeof spv_path, "%s/%s.spv", build_dir, flat);
    snprintf(stamp_path, sizeof stamp_path, "%s/%s.hash", build_dir, flat);
    snprintf(log_path, sizeof log_path, "%s/%s.log", build_dir, flat);

    char src_path[MOD_SHADER_PATH_MAX * 2];
    snprintf(src_path, sizeof src_path, "%s/%s", mod_dir, path);

    uint32_t* words = NULL;
    size_t n_words = 0;

    size_t src_len = 0;
    char* src = read_whole_file(src_path, &src_len);
    if (src) {
        /* Source-first: content-hash cache, stamp-not-object on failure. */
        char key[65], stamped[65] = "";
        if (shader_compute_key(mod_dir, path, key) != 0) { free(src); return -1; }
        {
            FILE* f = fopen(stamp_path, "rb");
            if (f) {
                size_t got = fread(stamped, 1, 64, f);
                stamped[got] = '\0';
                if (got && stamped[got - 1] == '\n') stamped[got - 1] = '\0';
                fclose(f);
            }
        }
        if (strcmp(stamped, key) == 0) {
            size_t len = 0;
            char* cached = read_whole_file(spv_path, &len);
            if (cached && len >= 4 && (len & 3u) == 0) {
                words = (uint32_t*)cached;
                n_words = len / 4;
            } else {
                free(cached);
            }
        }
        if (!words) {
            include_ctx_t ctx;
            snprintf(ctx.shader_dir, sizeof ctx.shader_dir, "%s/%s",
                     mod_dir, path);
            char* slash = strrchr(ctx.shader_dir, '/');
            if (slash) *slash = '\0';
            snprintf(ctx.sdk_shader_dir, sizeof ctx.sdk_shader_dir,
                     "%s/shaders", openpete_mods_sdk_dir());
            n_words = compile_glsl(src, stage, &ctx, log_path, &words);
            if (n_words == 0) {
                OPL_ERROR_F("mods", "%s: shader %s: compile FAILED (see %s) "
                            "— effect unavailable", id, path, log_path);
                remove(stamp_path);   /* stamp only; a cached .spv survives */
                free(src);
                return -1;
            }
            (void)write_whole_file(spv_path, words, n_words * 4);
            FILE* f = fopen(stamp_path, "wb");
            if (f) { fprintf(f, "%s\n", key); fclose(f); }
            OPL_INFO_F("mods", "%s: shader %s compiled (%zu words)", id, path,
                       n_words);
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
        char* side = read_whole_file(side_path, &side_len);
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
        char* pre = read_whole_file(pre_path, &len);
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
    sh->live = 1;
    return s_shader_count++;
}

const uint32_t* openpete_mods_shader_spv(int handle, size_t* words,
                                         int* stage) {
    if (handle < 0 || handle >= s_shader_count || !s_shaders[handle].live)
        return NULL;
    if (words) *words = s_shaders[handle].spv_words;
    if (stage) *stage = s_shaders[handle].stage;
    return s_shaders[handle].spv;
}

void openpete_mods_shader_remove_owner(const char* mod_id) {
    for (int i = 0; i < s_shader_count; i++) {
        if (!s_shaders[i].live || !s_shaders[i].owner) continue;
        if (strcmp(s_shaders[i].owner, mod_id) != 0) continue;
        free(s_shaders[i].spv);
        s_shaders[i].spv = NULL;
        s_shaders[i].spv_words = 0;
        s_shaders[i].live = 0;   /* slot stays dead: stale handles error */
    }
}

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

int openpete_mods_postfx_register(openpete_mod_t* self, int frag_shader,
                                  int point, int order) {
    (void)frag_shader; (void)point; (void)order;
    STUB_ONCE("postfx_register");
    return -1;
}
int openpete_mods_postfx_enable(openpete_mod_t* self, int fx, int enabled) {
    (void)fx; (void)enabled;
    STUB_ONCE("postfx_enable");
    return -1;
}
int openpete_mods_postfx_set_uniforms(openpete_mod_t* self, int fx,
                                      const void* data, uint32_t size) {
    (void)fx; (void)data; (void)size;
    STUB_ONCE("postfx_set_uniforms");
    return -1;
}
int openpete_mods_pipeline_register(openpete_mod_t* self,
                                    const openpete_mod_pipeline_desc_t* desc) {
    (void)desc;
    STUB_ONCE("pipeline_register");
    return -1;
}
int openpete_mods_draw_mesh(openpete_mod_t* self, int pipeline,
                            const void* verts, int vert_count,
                            uint16_t texture_id) {
    (void)pipeline; (void)verts; (void)vert_count; (void)texture_id;
    STUB_ONCE("draw_mesh");
    return -1;
}
int openpete_mods_material_register(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance) {
    (void)sel; (void)vert_shader; (void)frag_shader; (void)per_instance;
    STUB_ONCE("material_register");
    return -1;
}
