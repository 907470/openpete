/* rhi_pcache_file.c - per-build pipeline cache file naming and aging
 * (contract in rhi_pcache_file.h). */
#include "rhi_pcache_file.h"

#include "rhi_embedded_shaders.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <utime.h>

#define PCF_LOG(...) fprintf(stderr, "[rhi/vk] " __VA_ARGS__)
#define PCF_MAX_FILES 256

/** @brief Fold bytes into an FNV-1a 32 hash.
 *
 * @param h Running hash.
 * @param p The bytes.
 * @param n Their count.
 * @return The updated hash. */
static uint32_t pcf_fnv32(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

void rhi_pcf_build_tag(const char *build, char out[9])
{
    uint32_t h = 2166136261u;
    if (build) h = pcf_fnv32(h, build, strlen(build));
    for (unsigned i = 0; i < g_rhi_embedded_shader_count; i++) {
        const rhi_embedded_shader *e = &g_rhi_embedded_shaders[i];
        h = pcf_fnv32(h, e->name, strlen(e->name) + 1);
        h = pcf_fnv32(h, e->data, e->size);
    }
    snprintf(out, 9, "%08x", (unsigned)h);
}

void rhi_pcf_touch(const char *path)
{
    if (path && path[0]) (void)utime(path, NULL);
}

/* One candidate file of the prune: its path and last-use time. */
typedef struct {
    char    *path;
    SDL_Time used;
} pcf_entry;

/** @brief Order prune candidates most recently used first.
 *
 * @param a First entry.
 * @param b Second entry.
 * @return qsort order. */
static int pcf_cmp_recent(const void *a, const void *b)
{
    const SDL_Time ta = ((const pcf_entry *)a)->used, tb = ((const pcf_entry *)b)->used;
    return ta > tb ? -1 : ta < tb ? 1 : 0;
}

/** @brief Collect the cache files of one class in a directory other than the run's own, with their mtimes.
 *
 * @param dir The directory.
 * @param own The run's own file path.
 * @param headless true for the -headless.bin class, false for the windowed class.
 * @param out Receives up to PCF_MAX_FILES entries; each path is malloc'd.
 * @return The number of entries. */
static int pcf_collect(const char *dir, const char *own, bool headless, pcf_entry *out)
{
    int count = 0, n = 0;
    char **names = SDL_GlobDirectory(dir, "vk-pipelines-*.bin", 0, &count);
    for (int i = 0; names && i < count && n < PCF_MAX_FILES; i++) {
        if ((strstr(names[i], "-headless.bin") != NULL) != headless) continue;
        const size_t len = strlen(dir) + strlen(names[i]) + 2;
        char *path = malloc(len);
        if (!path) break;
        snprintf(path, len, "%s/%s", dir, names[i]);
        SDL_PathInfo info;
        if (strcmp(path, own) == 0 || !SDL_GetPathInfo(path, &info) ||
            info.type != SDL_PATHTYPE_FILE) {
            free(path);
            continue;
        }
        out[n++] = (pcf_entry){ path, info.modify_time };
    }
    SDL_free(names);
    return n;
}

int rhi_pcf_prune(const char *dir, const char *own, bool headless)
{
    if (!dir || !dir[0] || !own || !own[0]) return 0;
    pcf_entry *e = calloc(PCF_MAX_FILES, sizeof *e);
    if (!e) return 0;
    const int n = pcf_collect(dir, own, headless, e);
    qsort(e, (size_t)n, sizeof *e, pcf_cmp_recent);
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    const SDL_Time max_age = (SDL_Time)RHI_PCF_MAX_AGE_DAYS * 86400 * SDL_NS_PER_SECOND;
    int removed = 0;
    for (int i = 0; i < n; i++) {
        /* Rank i + 1: the run's own file holds rank 0. */
        const bool stale = now - e[i].used > max_age;
        if ((i + 1 >= RHI_PCF_KEEP || stale) && SDL_RemovePath(e[i].path)) {
            PCF_LOG("pipeline cache: removed %s (%s)\n", e[i].path,
                    stale ? "unused past the age limit" : "beyond the most recently used");
            removed++;
        }
        free(e[i].path);
    }
    free(e);
    return removed;
}
