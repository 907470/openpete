/* mod_shader_compile_test - compiles every shipped mod shader against the
 * staged mod SDK, through the compile-on-boot front end (core/mods/
 * mod_glsl.c): the same glslang settings, the same include resolution
 * (<sdk>/shaders, then the shader's own dir) and the same profiles (every
 * stage once; a material fragment a second time under the retained-terrain
 * preamble).
 *
 * Usage: mod_shader_compile_test <mods-dir> <sdk-dir>
 * Exit 0 when every profile compiles and at least one material fragment
 * was compiled; 1 on any compile error, on a missing SDK, or when no
 * material fragment was found (a run that compiled no material shader
 * proves nothing about the material contract). */

#include "mod_glsl.h"

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

/* One profile; prints the verdict line, and the glslang log on failure. */
static int compile_one(const shader_ref_t* r, const char* src, int stage,
                       const openpete_mod_glsl_include_t* inc,
                       const char* profile, const char* preamble,
                       const char* log_path) {
    uint32_t* words = NULL;
    remove(log_path);
    size_t n = openpete_mod_glsl_compile(src, stage, inc, preamble, log_path,
                                         &words);
    free(words);
    if (n == 0) {
        printf("FAIL %s %s [%s]\n", r->mod, r->path, profile);
        print_file(log_path);
        return 1;
    }
    printf("ok   %s %s [%s] %zu words\n", r->mod, r->path, profile, n);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <mods-dir> <sdk-dir>\n", argv[0]);
        return 1;
    }
    const char* mods_dir = argv[1];
    const char* sdk_dir = argv[2];
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
            profiles++;
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
    if (materials == 0) {
        printf("FAIL: VACUOUS - no material fragment found under %s\n", mods_dir);
        return 1;
    }
    return failed ? 1 : 0;
}
