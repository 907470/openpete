/* mod_shader_stamp_test - the mod shader compile cache's stamp
 * (core/mods/mod_shader_stamp.c) over real glslang compiles through the
 * compile-on-boot front end (core/mods/mod_glsl.c).
 *
 * Synthetic tree: an SDK include that includes a second SDK file (the
 * shape of terrain_shade.glsl including psx_hq_fog.glsl), a mod-local
 * include and an include from a subdirectory. After one compile the stamp
 * is current; an edit to each included file (the nested SDK one, the local
 * one, the subdirectory one) makes it stale and restoring the bytes makes it
 * current again; a file appearing in the SDK under a mod-local include's
 * name makes it stale; another key and a stamp without the deps line are
 * never current.
 *
 * Staged SDK (argv[1]) and a shipped material shader (argv[2]): the
 * shader's terrain profile compiled against a copy of the SDK records
 * psx_hq_fog.glsl, and an edit to that copy makes the stamp stale. The
 * prebuilt-module interface hash changes with an edit to every staged
 * `.glsl`, each one in turn.
 *
 * Usage: mod_shader_stamp_test <sdk-dir> <material.frag>
 * Exit 0 when every check passes, 1 otherwise. */

#include "mod_glsl.h"
#include "mod_shader_stamp.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_fail;

/** @brief Print a check's verdict and count a failure.
 * @param ok The check passed.
 * @param what What was checked. */
static void check(int ok, const char* what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) s_fail = 1;
}

/** @brief Write a whole file.
 * @param path The file.
 * @param text Its content. */
static void put(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

/** @brief Append text to a file.
 * @param path The file.
 * @param text The text. */
static void append(const char* path, const char* text) {
    FILE* f = fopen(path, "ab");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

/** @brief Read a whole file; exits on failure.
 * @param path The file.
 * @return The malloc'd content. */
static char* slurp(const char* path) {
    char* t = openpete_mod_glsl_read_file(path, NULL);
    if (!t) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
    return t;
}

/** @brief Compile a source with its include probes recorded and write its stamp.
 * @param src_path The source file.
 * @param sdk The SDK root.
 * @param preamble The profile preamble, or NULL.
 * @param stamp The stamp path.
 * @param key The build key.
 * @param deps Receives the probed files.
 * @return 0 when the compile and the stamp write succeed. */
static int compile_stamp(const char* src_path, const char* sdk, const char* preamble, const char* stamp,
                         const char* key, openpete_mod_glsl_deps_t* deps) {
    char* src = slurp(src_path);
    openpete_mod_glsl_include_t inc;
    openpete_mod_glsl_include_init(&inc, src_path, sdk);
    memset(deps, 0, sizeof *deps);
    inc.deps = deps;
    char log[1100];
    snprintf(log, sizeof log, "%s.log", stamp);
    uint32_t* words = NULL;
    const size_t n = openpete_mod_glsl_compile(src, OPENPETE_MOD_GLSL_STAGE_FRAG, &inc, preamble, log, &words);
    free(src);
    free(words);
    if (n == 0) { fprintf(stderr, "compile of %s failed, see %s\n", src_path, log); return -1; }
    return openpete_mod_shader_stamp_write(stamp, key, deps);
}

/** @brief Report whether a dependency list holds a path ending in a name with the given presence.
 * @param d The list.
 * @param tail The path's tail.
 * @param present 1 read, 0 probed absent.
 * @return 1 when listed. */
static int has_dep(const openpete_mod_glsl_deps_t* d, const char* tail, int present) {
    const size_t tl = strlen(tail);
    for (int i = 0; i < d->n; i++) {
        const size_t pl = strlen(d->path[i]);
        if (pl >= tl && !strcmp(d->path[i] + pl - tl, tail) && d->present[i] == present) return 1;
    }
    return 0;
}

/** @brief An edit to one file makes the stamp stale; restoring the bytes makes it current again.
 * @param stamp The stamp.
 * @param key Its key.
 * @param path The file edited.
 * @param what The check's name. */
static void edit_restore(const char* stamp, const char* key, const char* path, const char* what) {
    char* orig = slurp(path);
    append(path, "\n// edited\n");
    char msg[256];
    snprintf(msg, sizeof msg, "edit to %s makes the stamp stale", what);
    check(!openpete_mod_shader_stamp_current(stamp, key), msg);
    put(path, orig);
    free(orig);
    snprintf(msg, sizeof msg, "restoring %s makes it current again", what);
    check(openpete_mod_shader_stamp_current(stamp, key), msg);
}

/** @brief Copy every regular file of one directory into another (existing) one.
 * @param from The source directory.
 * @param to The destination directory. */
static void copy_dir(const char* from, const char* to) {
    DIR* d = opendir(from);
    if (!d) { perror(from); exit(1); }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char a[1400], b[1400];
        snprintf(a, sizeof a, "%.1100s/%.255s", from, e->d_name);
        snprintf(b, sizeof b, "%.1100s/%.255s", to, e->d_name);
        struct stat st;
        if (stat(a, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        char* t = slurp(a);
        put(b, t);
        free(t);
    }
    closedir(d);
}

static const char k_key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/** @brief The synthetic tree's checks.
 * @param root A fresh temporary directory. */
static void synthetic(const char* root) {
    char sdk[600], p[1100], src[1100], stamp[1100];
    snprintf(sdk, sizeof sdk, "%s/sdk", root);
    snprintf(p, sizeof p, "%s/sdk", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sdk/shaders", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mod", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mod/shaders", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mod/shaders/lib", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sdk/shaders/outer.glsl", root);
    put(p, "#include \"inner.glsl\"\nvec4 outer(vec4 c) { return inner(c); }\n");
    snprintf(p, sizeof p, "%s/sdk/shaders/inner.glsl", root);
    put(p, "vec4 inner(vec4 c) { return c * 0.5; }\n");
    snprintf(p, sizeof p, "%s/mod/shaders/local.glsl", root);
    put(p, "vec4 local(vec4 c) { return c + 0.25; }\n");
    snprintf(p, sizeof p, "%s/mod/shaders/lib/sub.glsl", root);
    put(p, "vec4 sub(vec4 c) { return c - 0.125; }\n");
    snprintf(src, sizeof src, "%s/mod/shaders/x.frag", root);
    put(src, "#version 450\n#extension GL_GOOGLE_include_directive : require\n"
             "#include \"outer.glsl\"\n#include \"local.glsl\"\n#include \"lib/sub.glsl\"\n"
             "layout(location = 0) out vec4 o;\nvoid main() { o = sub(local(outer(vec4(1.0)))); }\n");
    snprintf(stamp, sizeof stamp, "%s/x.hash", root);
    openpete_mod_glsl_deps_t* deps = (openpete_mod_glsl_deps_t*)calloc(1, sizeof *deps);
    check(compile_stamp(src, sdk, NULL, stamp, k_key, deps) == 0, "synthetic shader compiles and stamps");
    check(has_dep(deps, "/sdk/shaders/inner.glsl", 1), "nested SDK include recorded");
    check(has_dep(deps, "/mod/shaders/local.glsl", 1) && has_dep(deps, "/sdk/shaders/local.glsl", 0),
          "mod-local include recorded with its absent SDK probe");
    check(has_dep(deps, "/mod/shaders/lib/sub.glsl", 1), "subdirectory include recorded");
    free(deps);
    check(openpete_mod_shader_stamp_current(stamp, k_key), "stamp current after the compile");
    check(!openpete_mod_shader_stamp_current(stamp, "ff" "23456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
          "another key is not current");
    snprintf(p, sizeof p, "%s/sdk/shaders/inner.glsl", root);
    edit_restore(stamp, k_key, p, "the nested SDK include");
    snprintf(p, sizeof p, "%s/mod/shaders/local.glsl", root);
    edit_restore(stamp, k_key, p, "the mod-local include");
    snprintf(p, sizeof p, "%s/mod/shaders/lib/sub.glsl", root);
    edit_restore(stamp, k_key, p, "the subdirectory include");
    snprintf(p, sizeof p, "%s/sdk/shaders/local.glsl", root);
    put(p, "vec4 local(vec4 c) { return c; }\n");
    check(!openpete_mod_shader_stamp_current(stamp, k_key), "an SDK file shadowing a local include makes it stale");
    unlink(p);
    check(openpete_mod_shader_stamp_current(stamp, k_key), "removing the shadow makes it current again");
    char old[200];
    snprintf(old, sizeof old, "%s\n", k_key);
    put(stamp, old);
    check(!openpete_mod_shader_stamp_current(stamp, k_key), "a stamp without the deps line is not current");
}

/** @brief The staged SDK's checks: psx_hq_fog.glsl reaches a material shader's terrain profile and every staged file keys the interface hash.
 * @param root A fresh temporary directory.
 * @param staged The staged SDK root.
 * @param material A material fragment source. */
static void staged_sdk(const char* root, const char* staged, const char* material) {
    if (strlen(root) > 200 || strlen(staged) > 500) { check(0, "paths fit the test's buffers"); return; }
    char sdk[600], from[700], to[700], stamp[700], fog[800];
    snprintf(sdk, sizeof sdk, "%s/realsdk", root);
    mkdir(sdk, 0755);
    snprintf(to, sizeof to, "%s/shaders", sdk);
    mkdir(to, 0755);
    snprintf(from, sizeof from, "%s/shaders", staged);
    copy_dir(from, to);
    snprintf(stamp, sizeof stamp, "%s/material.terrain.hash", root);
    openpete_mod_glsl_deps_t* deps = (openpete_mod_glsl_deps_t*)calloc(1, sizeof *deps);
    check(compile_stamp(material, sdk, OPENPETE_MOD_GLSL_TERRAIN_PREAMBLE, stamp, k_key, deps) == 0,
          "material terrain profile compiles against the staged SDK");
    check(has_dep(deps, "/shaders/psx_hq_fog.glsl", 1), "terrain profile records psx_hq_fog.glsl");
    check(!deps->overflow, "dependency list did not overflow");
    free(deps);
    snprintf(fog, sizeof fog, "%s/psx_hq_fog.glsl", to);
    edit_restore(stamp, k_key, fog, "the staged psx_hq_fog.glsl");

    char names[128][128];
    const int n = openpete_mod_shader_list_dir_sorted(to, ".glsl", names, 128);
    check(n > 0, "staged SDK lists its shader files");
    int fog_listed = 0;
    for (int i = 0; i < n; i++) {
        char before[65], after[65], again[65], p[900], msg[300];
        snprintf(p, sizeof p, "%s/%.127s", to, names[i]);
        fog_listed |= !strcmp(names[i], "psx_hq_fog.glsl");
        openpete_mod_shader_interface_hash(sdk, 1u, before);
        char* orig = slurp(p);
        append(p, "\n// edited\n");
        openpete_mod_shader_interface_hash(sdk, 1u, after);
        put(p, orig);
        free(orig);
        openpete_mod_shader_interface_hash(sdk, 1u, again);
        snprintf(msg, sizeof msg, "interface hash keys %.127s", names[i]);
        check(strcmp(before, after) != 0 && strcmp(before, again) == 0, msg);
    }
    check(fog_listed, "psx_hq_fog.glsl is among the staged files");
}

/** @brief Run the synthetic and staged-SDK checks in a temporary directory.
 * @param argc The argument count.
 * @param argv The SDK directory and a material shader.
 * @return 0 when every check passes. */
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <sdk-dir> <material.frag>\n", argv[0]); return 2; }
    char root[] = "/tmp/mod_shader_stamp_test.XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    synthetic(root);
    staged_sdk(root, argv[1], argv[2]);
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf -- '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", root);
    printf("%s\n", s_fail ? "mod_shader_stamp_test: FAIL" : "mod_shader_stamp_test: PASS");
    return s_fail;
}
