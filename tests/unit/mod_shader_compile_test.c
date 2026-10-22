/* mod_shader_compile_test - compiles every shipped mod shader against the
 * staged mod SDK, through the compile-on-boot front end (core/mods/
 * mod_glsl.c): the same glslang settings, the same include resolution
 * (<sdk>/shaders, then the shader's own dir) and the same profiles (every
 * stage once; a material fragment a second time under the retained-terrain
 * preamble, a third under the portal preamble and a fourth under the actor
 * preamble).
 *
 * Every fragment profile then goes through the constant-table move the
 * engine applies at registration (core/mods/mod_spv_cdata.c); a module it
 * rewrites is written to a temporary file and checked with spirv-val
 * (--target-env vulkan1.0) when a validator path is given.
 *
 * Usage: mod_shader_compile_test <mods-dir> <sdk-dir> [<spirv-val> [--expect-moved]]
 * Exit 0 when every profile compiles, every rewritten module validates,
 * and at least one material fragment was compiled; 1 on any compile or
 * validation error, on a missing SDK, when no material fragment was found
 * (a run that compiled no material shader proves nothing about the
 * material contract), or, with --expect-moved, when no table moved. */

#include "mod_glsl.h"
#include "mod_spv_cdata.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_SHADERS 512

typedef struct {
    char mod[128];
    char path[OPENPETE_MOD_GLSL_PATH_MAX * 2];   /* full path */
} shader_ref_t;

static shader_ref_t s_found[MAX_SHADERS];
static int s_n_found;
static const char* s_spirv_val;   /* validator path, NULL = no validation */
static int s_moved_modules;       /* rewritten modules */

static int is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Every .vert/.frag under `dir`, skipping dot entries (.build holds the
 * engine's own compile cache). */
static void collect(const char* mod, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[OPENPETE_MOD_GLSL_PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) { collect(mod, p); continue; }
        if (openpete_mod_glsl_stage_from_path(p) < 0) continue;
        if (s_n_found >= MAX_SHADERS) break;
        snprintf(s_found[s_n_found].mod, sizeof s_found[s_n_found].mod, "%s", mod);
        snprintf(s_found[s_n_found].path, sizeof s_found[s_n_found].path, "%s", p);
        s_n_found++;
    }
    closedir(d);
}

static int cmp_ref(const void* a, const void* b) {
    return strcmp(((const shader_ref_t*)a)->path, ((const shader_ref_t*)b)->path);
}

static void print_file(const char* path) {
    char* txt = openpete_mod_glsl_read_file(path, NULL);
    if (txt) { fputs(txt, stdout); free(txt); }
}

/** @brief Write a rewritten module to a temporary file and run spirv-val on it.
 *
 * @return 0 when it validates or no validator was given, 1 otherwise. */
static int validate(const uint32_t* w, size_t n) {
    if (!s_spirv_val) return 0;
    const char* path = "mod_shader_compile_test.spv";
    FILE* f = fopen(path, "wb");
    if (!f) return 1;
    const size_t put = fwrite(w, sizeof *w, n, f);
    fclose(f);
    if (put != n) return 1;
    char cmd[OPENPETE_MOD_GLSL_PATH_MAX * 2];
    snprintf(cmd, sizeof cmd, "\"%s\" --target-env vulkan1.0 %s", s_spirv_val, path);
    const int rc = system(cmd);
    remove(path);
    return rc != 0;
}

/** @brief Run the constant-table move on a fragment profile and validate the module it rewrites.
 *
 * @return 0 when nothing moved or the rewrite validates, 1 otherwise. */
static int move_one(const shader_ref_t* r, const char* profile, const uint32_t* w,
                    size_t n) {
    uint32_t* out = NULL;
    size_t out_n = 0;
    openpete_mod_spv_cdata_t data;
    const int moved = openpete_mod_spv_cdata_extract(w, n, &out, &out_n, &data);
    if (moved < 0) {
        printf("FAIL %s %s [%s] table move: malformed module\n", r->mod, r->path, profile);
        return 1;
    }
    if (moved == 0) return 0;
    const int bad = validate(out, out_n);
    printf("%s %s %s [%s] %d table(s) moved, %zu B, %zu words%s\n", bad ? "FAIL" : "ok  ",
           r->mod, r->path, profile, moved, data.len, out_n,
           s_spirv_val ? (bad ? ", spirv-val FAILED" : ", spirv-val ok") : "");
    s_moved_modules++;
    free(out);
    free(data.bytes);
    return bad;
}

/* One profile; prints the verdict line, and the glslang log on failure. */
static int compile_one(const shader_ref_t* r, const char* src, int stage,
                       const openpete_mod_glsl_include_t* inc,
                       const char* profile, const char* preamble,
                       const char* log_path) {
    uint32_t* words = NULL;
    remove(log_path);
    size_t n = openpete_mod_glsl_compile(src, stage, inc, preamble, log_path,
                                         &words);
    if (n == 0) {
        printf("FAIL %s %s [%s]\n", r->mod, r->path, profile);
        print_file(log_path);
        return 1;
    }
    printf("ok   %s %s [%s] %zu words\n", r->mod, r->path, profile, n);
    const int bad = stage == OPENPETE_MOD_GLSL_STAGE_FRAG ? move_one(r, profile, words, n) : 0;
    free(words);
    return bad;
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: %s <mods-dir> <sdk-dir> [<spirv-val> [--expect-moved]]\n",
                argv[0]);
        return 1;
    }
    const char* mods_dir = argv[1];
    const char* sdk_dir = argv[2];
    s_spirv_val = argc >= 4 && argv[3][0] ? argv[3] : NULL;
    const int expect_moved = argc == 5 && strcmp(argv[4], "--expect-moved") == 0;
    char probe[OPENPETE_MOD_GLSL_PATH_MAX * 2];
    snprintf(probe, sizeof probe, "%s/shaders/openpete_psx_material.glsl", sdk_dir);
    char* probe_txt = openpete_mod_glsl_read_file(probe, NULL);
    if (!probe_txt) {
        printf("FAIL: no staged SDK shader include at %s\n", probe);
        return 1;
    }
    free(probe_txt);

    DIR* d = opendir(mods_dir);
    if (!d) { printf("FAIL: cannot open %s\n", mods_dir); return 1; }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[OPENPETE_MOD_GLSL_PATH_MAX * 2];
        snprintf(p, sizeof p, "%s/%s", mods_dir, e->d_name);
        if (is_dir(p)) collect(e->d_name, p);
    }
    closedir(d);
    qsort(s_found, (size_t)s_n_found, sizeof s_found[0], cmp_ref);

    const char* log_path = "mod_shader_compile_test.log";
    int failed = 0, profiles = 0, materials = 0;
    char material_mods[1024] = "";
    for (int i = 0; i < s_n_found; i++) {
        const shader_ref_t* r = &s_found[i];
        char* src = openpete_mod_glsl_read_file(r->path, NULL);
        if (!src) { printf("FAIL %s %s: unreadable\n", r->mod, r->path); failed++; continue; }
        int stage = openpete_mod_glsl_stage_from_path(r->path);
        openpete_mod_glsl_include_t inc;
        openpete_mod_glsl_include_init(&inc, r->path, sdk_dir);
        failed += compile_one(r, src, stage, &inc, "stock", NULL, log_path);
        profiles++;
        if (openpete_mod_glsl_has_terrain_profile(src, stage)) {
            failed += compile_one(r, src, stage, &inc, "terrain",
                                  OPENPETE_MOD_GLSL_TERRAIN_PREAMBLE, log_path);
            failed += compile_one(r, src, stage, &inc, "portal",
                                  OPENPETE_MOD_GLSL_PORTAL_PREAMBLE, log_path);
            failed += compile_one(r, src, stage, &inc, "actor",
                                  OPENPETE_MOD_GLSL_ACTOR_PREAMBLE, log_path);
            profiles += 3;
            materials++;
            if (!strstr(material_mods, r->mod)) {
                strncat(material_mods, " ", sizeof material_mods - strlen(material_mods) - 1);
                strncat(material_mods, r->mod, sizeof material_mods - strlen(material_mods) - 1);
            }
        }
        free(src);
    }
    remove(log_path);

    printf("mod_shader_compile: %d shader(s), %d profile(s), %d material "
           "fragment(s), %d failed (mods %s, sdk %s)\n",
           s_n_found, profiles, materials, failed, mods_dir, sdk_dir);
    printf("mod_shader_compile: material mods:%s\n",
           material_mods[0] ? material_mods : " (none)");
    printf("mod_shader_compile: %d module(s) had constant tables moved%s\n", s_moved_modules,
           s_spirv_val ? " (each validated)" : " (no validator)");
    if (materials == 0) {
        printf("FAIL: VACUOUS - no material fragment found under %s\n", mods_dir);
        return 1;
    }
    if (expect_moved && s_moved_modules == 0) {
        printf("FAIL: VACUOUS - no constant table moved under %s\n", mods_dir);
        return 1;
    }
    return failed ? 1 : 0;
}
