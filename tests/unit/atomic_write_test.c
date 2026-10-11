/* atomic_write_test - each write class of the atomic-file helper against an
 * injected failure (OPENPETE_WRITE_FAIL): the previous file or tree must be
 * byte-identical afterwards, and an uninjected write must land.
 *
 * Classes, each on a target under the directory given as argv[1]:
 *   synced    op_atomic_write_file and op_atomic_file_open/commit (config,
 *             bk2, .args, meta.toml, disc.cue, imgui ini, mod stamp,
 *             fillpick session, extractors, build sources).
 *   copy      op_atomic_copy_file (mod hot-reload stage copy).
 *   unsynced  op_atomic_write_file_ex with OP_ATOMIC_NOSYNC (screenshot,
 *             harness and golden PNGs, texture-pack dump).
 *   rename    op_atomic_rename of a finished `.part` stream (video).
 *   dir       op_atomic_replace_dir (mod install), including the restore of
 *             a tree left aside by a process stopped between its renames.
 * The run fails when no failure was injected (op_atomic_file_injected_count).
 */

#include "openpete_atomic_file.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char s_root[512];

/** @brief Print a failure line and return 1. */
static int fail(const char* what) {
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

/** @brief Write n bytes to path with plain stdio. @return 0 on success. */
static int put(const char* path, const void* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, n, f);
    return (fclose(f) == 0 && w == n) ? 0 : -1;
}

/** @brief Read a whole file. @return A malloc'd buffer (NULL when unreadable); *n receives its length. */
static unsigned char* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = (unsigned char*)malloc((size_t)*n + 1);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

/** @brief Report whether path holds exactly n bytes equal to data. */
static int holds(const char* path, const void* data, size_t n) {
    long got = 0;
    unsigned char* b = slurp(path, &got);
    int ok = b && got == (long)n && memcmp(b, data, n) == 0;
    free(b);
    return ok;
}

/** @brief Report whether path exists. */
static int exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/** @brief Count the entries of a directory other than . and ..; -1 when it cannot be opened. */
static int dir_entries(const char* path) {
    DIR* d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n;
}

/** @brief Remove a directory holding only files (the test trees are one level deep). @return 0 when nothing remains. */
static int rmtree1(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return remove(path) == 0 ? 0 : -1;
    DIR* d = opendir(path);
    if (d) {
        struct dirent* e;
        char child[1200];
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            remove(child);
        }
        closedir(d);
    }
    return rmdir(path) == 0 ? 0 : -1;
}

static const char kOld[] = "previous content, whole\n";
static const char kNew[] = "replacement content, a different length entirely\n";

/** @brief One file-replacing class: seed kOld, inject a failure part way, check kOld and no temporary, then write kNew uninjected.
 *
 * @param label Class name for the output.
 * @param name File name under s_root.
 * @param write The class's write of kNew to a path.
 * @return 0 on pass, 1 on failure. */
static int case_file(const char* label, const char* name, int (*write)(const char* path)) {
    char path[700];
    snprintf(path, sizeof path, "%s/%s", s_root, name);
    if (put(path, kOld, sizeof kOld - 1) != 0) return fail("seeding the target failed");
    unsigned long before = op_atomic_file_injected_count();
    char spec[300];
    snprintf(spec, sizeof spec, "%s:8", name);
    setenv("OPENPETE_WRITE_FAIL", spec, 1);
    int rc_inj = write(path);
    unsetenv("OPENPETE_WRITE_FAIL");
    unsigned long injected = op_atomic_file_injected_count() - before;
    int kept = holds(path, kOld, sizeof kOld - 1);
    int dir_n = dir_entries(s_root);   /* the target, plus the .part of the rename class */
    int rc_ok = write(path);
    int landed = holds(path, kNew, sizeof kNew - 1);
    printf("%-8s injected=%lu rc=%d previous %s, entries=%d; uninjected rc=%d %s\n", label,
           injected, rc_inj, kept ? "byte-identical" : "CHANGED", dir_n, rc_ok,
           landed ? "landed" : "MISSING");
    int rc = 0;
    if (injected == 0) rc |= fail("no failure injected");
    if (rc_inj == 0) rc |= fail("the injected write reported success");
    if (!kept) rc |= fail("the previous file changed under an injected failure");
    if (rc_ok != 0 || !landed) rc |= fail("the uninjected write did not land");
    remove(path);
    return rc;
}

/** @brief Synced class through op_atomic_write_file. */
static int w_synced(const char* p) { return op_atomic_write_file(p, kNew, sizeof kNew - 1); }

/** @brief Synced class through the stream form, as the config and fillpick writers use it. */
static int w_stream(const char* p) {
    op_atomic_file_t af;
    if (op_atomic_file_open(&af, p) != 0) return -1;
    fputs(kNew, af.f);
    return op_atomic_file_commit(&af);
}

/** @brief Unsynced class through op_atomic_write_file_ex. */
static int w_unsynced(const char* p) {
    return op_atomic_write_file_ex(p, kNew, sizeof kNew - 1, OP_ATOMIC_NOSYNC);
}

/** @brief Copy class: kNew staged in a source file, then op_atomic_copy_file. */
static int w_copy(const char* p) {
    char src[720];
    snprintf(src, sizeof src, "%s.src", p);
    if (put(src, kNew, sizeof kNew - 1) != 0) return -1;
    int rc = op_atomic_copy_file(src, p);
    remove(src);
    return rc;
}

/** @brief Rename class: kNew written to `<p>.part`, then op_atomic_rename; the .part must survive a failed rename. */
static int w_rename(const char* p) {
    char part[720];
    snprintf(part, sizeof part, "%s.part", p);
    if (!exists(part) && put(part, kNew, sizeof kNew - 1) != 0) return -1;
    return op_atomic_rename(part, p, OP_ATOMIC_NOSYNC);
}

/** @brief Make a one-level tree holding file "f" with the given content. */
static int make_tree(const char* dir, const char* content) {
    char f[800];
    if (mkdir(dir, 0755) != 0) return -1;
    snprintf(f, sizeof f, "%s/f", dir);
    return put(f, content, strlen(content));
}

/** @brief Directory class: injected failure keeps the previous tree, an uninjected swap lands, a tree left aside is restored. */
static int case_dir(void) {
    char dest[700], staged[720], aside[720], f[800];
    snprintf(dest, sizeof dest, "%s/mod", s_root);
    snprintf(staged, sizeof staged, "%s.new", dest);
    snprintf(aside, sizeof aside, "%s.old", dest);
    snprintf(f, sizeof f, "%s/f", dest);
    if (make_tree(dest, kOld) != 0 || make_tree(staged, kNew) != 0)
        return fail("dir: seeding the trees failed");

    unsigned long before = op_atomic_file_injected_count();
    setenv("OPENPETE_WRITE_FAIL", "/mod", 1);
    int rc_inj = op_atomic_replace_dir(staged, dest, rmtree1);
    unsetenv("OPENPETE_WRITE_FAIL");
    unsigned long injected = op_atomic_file_injected_count() - before;
    int kept = holds(f, kOld, sizeof kOld - 1) && dir_entries(dest) == 1 && !exists(aside);
    int rc_ok = op_atomic_replace_dir(staged, dest, rmtree1);
    int landed = holds(f, kNew, sizeof kNew - 1) && !exists(staged) && !exists(aside);

    /* A process stopped between the renames: the previous tree aside, none
     * in place. The next swap must put it back before replacing, and an
     * injected failure on that swap must leave it restored. */
    rmtree1(dest);
    make_tree(aside, kOld);
    make_tree(staged, kNew);
    setenv("OPENPETE_WRITE_FAIL", "/mod", 1);
    int rc_rest = op_atomic_replace_dir(staged, dest, rmtree1);
    unsetenv("OPENPETE_WRITE_FAIL");
    injected = op_atomic_file_injected_count() - before;
    int restored = holds(f, kOld, sizeof kOld - 1) && !exists(aside);

    printf("dir      injected=%lu rc=%d previous tree %s; uninjected rc=%d %s; "
           "aside tree %s (rc=%d)\n", injected, rc_inj, kept ? "byte-identical" : "CHANGED",
           rc_ok, landed ? "landed" : "MISSING", restored ? "restored" : "LOST", rc_rest);
    int rc = 0;
    if (injected < 2) rc |= fail("dir: failures not injected");
    if (rc_inj == 0 || rc_rest == 0) rc |= fail("dir: an injected swap reported success");
    if (!kept) rc |= fail("dir: the previous tree changed under an injected failure");
    if (rc_ok != 0 || !landed) rc |= fail("dir: the uninjected swap did not land");
    if (!restored) rc |= fail("dir: the tree left aside was not restored");
    rmtree1(dest);
    rmtree1(staged);
    rmtree1(aside);
    return rc;
}

int main(int argc, char** argv) {
    snprintf(s_root, sizeof s_root, "%s", argc > 1 ? argv[1] : "/tmp/atomic_write_test.d");
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", s_root, s_root);
    if (system(cmd) != 0) return fail("cannot create the test directory");

    int rc = 0;
    rc |= case_file("synced", "config.toml", w_synced);
    rc |= case_file("stream", "fill-tags.session", w_stream);
    rc |= case_file("copy", "mod.so.hot1", w_copy);
    rc |= case_file("unsynced", "shot.png", w_unsynced);
    rc |= case_file("rename", "video.mkv", w_rename);
    rc |= case_dir();
    unsigned long total = op_atomic_file_injected_count();
    printf("failures injected: %lu\n", total);
    if (total == 0) rc |= fail("VACUOUS: no failure was injected");
    printf("%s\n", rc ? "atomic_write_test: FAIL" : "atomic_write_test: PASS");
    return rc;
}
