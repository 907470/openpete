/* mod_shader_stamp.c - the mod shader compile cache's stamp and the
 * prebuilt-module interface hash (see mod_shader_stamp.h). */

#include "mod_shader_stamp.h"
#include "openpete_atomic_file.h"
#include "../host/sha256.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int openpete_mod_shader_list_dir_sorted(const char* dir, const char* exts, char names[][128], int cap) {
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
        snprintf(names[n++], 128, "%.127s", name);
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

/** @brief SHA-256 of a file's content as hex, read through openpete_mod_glsl_read_file (the reader include resolution uses).
 *
 * @param path The file.
 * @param hex Receives the digest.
 * @return 0, or -1 when the file does not read. */
static int file_hex(const char* path, char hex[65]) {
    size_t len = 0;
    char* data = openpete_mod_glsl_read_file(path, &len);
    if (!data) return -1;
    openpete_sha256_ctx hc;
    uint8_t digest[32];
    openpete_sha256_init(&hc);
    openpete_sha256_update(&hc, data, len);
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, hex);
    free(data);
    return 0;
}

int openpete_mod_shader_stamp_write(const char* stamp_path, const char* key,
                                    const openpete_mod_glsl_deps_t* deps) {
    const int n = deps ? deps->n : 0;
    if (deps && deps->overflow) { remove(stamp_path); return -1; }
    const size_t cap = 160 + (size_t)n * (sizeof deps->path[0] + 72);
    char* text = (char*)malloc(cap);
    if (!text) { remove(stamp_path); return -1; }
    size_t len = (size_t)snprintf(text, cap, "%s\ndeps %d\n", key, n);
    for (int i = 0; i < n && len < cap; i++) {
        char hex[65] = "-";
        if (deps->present[i] && file_hex(deps->path[i], hex) != 0) { free(text); remove(stamp_path); return -1; }
        len += (size_t)snprintf(text + len, cap - len, "%s %s\n", hex, deps->path[i]);
    }
    const int rc = len < cap ? op_atomic_write_file(stamp_path, text, len) : -1;
    free(text);
    if (rc != 0) remove(stamp_path);
    return rc;
}

/** @brief Report whether one stamp dependency line still holds: a hashed file reads to that hash, a `-` path is absent.
 *
 * @param line The line, without its newline: `<hex> <path>` or `- <path>`.
 * @return 1 when it holds, else 0 (also for a malformed line). */
static int dep_line_current(const char* line) {
    const char* sp = strchr(line, ' ');
    if (!sp || !sp[1]) return 0;
    const char* path = sp + 1;
    char hex[65];
    if (sp - line == 1 && line[0] == '-') {
        size_t n = 0;
        char* d = openpete_mod_glsl_read_file(path, &n);
        free(d);
        return d == NULL;
    }
    return sp - line == 64 && file_hex(path, hex) == 0 && memcmp(hex, line, 64) == 0;
}

int openpete_mod_shader_stamp_current(const char* stamp_path, const char* key) {
    size_t len = 0;
    char* text = openpete_mod_glsl_read_file(stamp_path, &len);
    if (!text) return 0;
    /* Lines: the key, "deps <n>", then exactly n dependency lines. */
    int ok = 1, n = -1, line_no = 0;
    for (char* line = text; ok && *line; line_no++) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line_no == 0) ok = strcmp(line, key) == 0;
        else if (line_no == 1) ok = sscanf(line, "deps %d", &n) == 1 && n >= 0;
        else ok = line_no - 2 < n && dep_line_current(line);
        line = nl ? nl + 1 : line + strlen(line);
    }
    free(text);
    return ok && n >= 0 && line_no == n + 2;
}

void openpete_mod_shader_interface_hash(const char* sdk_dir, unsigned api_ver, char out_hex[65]) {
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    openpete_sha256_update(&hc, &api_ver, sizeof api_ver);
    char dir[OPENPETE_MOD_GLSL_PATH_MAX * 2];
    snprintf(dir, sizeof dir, "%s/shaders", sdk_dir);
    char names[128][128];
    const int n = openpete_mod_shader_list_dir_sorted(dir, ".glsl", names, 128);
    for (int i = 0; i < n; i++) {
        char p[sizeof dir + 130];
        snprintf(p, sizeof p, "%s/%.127s", dir, names[i]);
        size_t len = 0;
        char* data = openpete_mod_glsl_read_file(p, &len);
        if (!data) continue;
        openpete_sha256_update(&hc, names[i], strlen(names[i]) + 1);
        openpete_sha256_update(&hc, data, len);
        free(data);
    }
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, out_hex);
}
