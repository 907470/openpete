/* rhi_pcache_file.h - per-build pipeline cache files: naming and aging
 * (private to the Vulkan RHI).
 *
 * A cache file is named vk-pipelines-<build>-<vendor>-<device>-<uuid>.bin,
 * <build> eight hex digits of a digest of the engine's build string and its
 * embedded SPIR-V, so builds that share the per-user state directory keep
 * their own files. A file's mtime is its last-use time: every open refreshes
 * it. A file name ending in -headless.bin is of the headless class (headless
 * and other synchronous runs), any other of the windowed class. At an open,
 * cache files of the opening run's class unused for RHI_PCF_MAX_AGE_DAYS
 * days or ranked beyond the RHI_PCF_KEEP most recently used are removed;
 * the opening run's own file ranks first and is never removed. A file is
 * never removed because its build differs, and an open never removes a
 * file of the other class. */
#ifndef RHI_PCACHE_FILE_H
#define RHI_PCACHE_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RHI_PCF_MAX_AGE_DAYS 30
#define RHI_PCF_KEEP         4

/** @brief Return the eight-hex-digit build component of a cache file name.
 *
 * FNV-1a 32 over the build string, then over every embedded SPIR-V blob's
 * name and bytes.
 * @param build The engine build string, or NULL for "".
 * @param out Receives the component (9 bytes with the terminator). */
void rhi_pcf_build_tag(const char *build, char out[9]);

/** @brief Refresh a cache file's mtime to now, marking it used; a missing file is left missing.
 *
 * @param path The file. */
void rhi_pcf_touch(const char *path);

/** @brief Remove the cache files of one class in a directory that are unused for RHI_PCF_MAX_AGE_DAYS or ranked beyond RHI_PCF_KEEP by last use.
 *
 * The headless class is the files matching vk-pipelines-*-headless.bin, the
 * windowed class every other vk-pipelines-*.bin. @p own ranks first whether
 * or not it exists and is never removed. Each removal is logged.
 * @param dir The cache directory.
 * @param own The opening run's own file path.
 * @param headless true to prune the headless class, false the windowed class.
 * @return The number of files removed. */
int rhi_pcf_prune(const char *dir, const char *own, bool headless);

#endif /* RHI_PCACHE_FILE_H */
