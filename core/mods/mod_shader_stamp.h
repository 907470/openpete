/* mod_shader_stamp.h - the mod shader compile cache's stamp and the
 * prebuilt-module interface hash.
 *
 * A cached `.spv` is current when its `.hash` stamp names the build key and
 * every file the compile that wrote it probed is unchanged: each file it
 * read still hashes to the recorded SHA-256, and each SDK path it found
 * absent is still absent. The stamp text is
 *
 *     <key>
 *     deps <n>
 *     <sha256 hex> <path>      one per file read
 *     - <path>                 one per path probed absent
 *
 * so an edit to any include, in the SDK or beside the shader, in any
 * directory, invalidates exactly the modules that read it. A stamp
 * without the deps line (written before it existed) is never current.
 *
 * The interface hash a prebuilt `<path>.spv.ihash` sidecar must hold covers
 * every `.glsl` staged under `<sdk>/shaders`, since a prebuilt module's
 * includes are not known. Depends on SHA-256, the atomic writer and
 * dirent only. */
#ifndef OPENPETE_MOD_SHADER_STAMP_H
#define OPENPETE_MOD_SHADER_STAMP_H

#include "mod_glsl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief List the files in a directory whose extension is in a `;`-separated list, sorted by strcmp.
 *
 * The extension is the name from its last `.`; a name with no `.` or of
 * 128 bytes or more is skipped, and the scan stops at `cap` entries. The
 * strcmp order makes a hash over the list independent of readdir order.
 * @param dir Directory to scan; a missing one lists nothing.
 * @param exts Extensions with their dots, such as `.vert;.frag`.
 * @param names Receives the names.
 * @param cap Capacity of @p names.
 * @return The number of names written. */
int openpete_mod_shader_list_dir_sorted(const char* dir, const char* exts, char names[][128], int cap);

/** @brief Write a compile's stamp: the build key and the files its include resolution probed, each with its current content hash.
 *
 * @param stamp_path The stamp file, written atomically.
 * @param key The build key, 64 hex characters.
 * @param deps The probed files; NULL for a compile that read none.
 * @return 0, or -1 when the list overflowed, a recorded file no longer reads, or the write fails (no stamp is left then). */
int openpete_mod_shader_stamp_write(const char* stamp_path, const char* key,
                                    const openpete_mod_glsl_deps_t* deps);

/** @brief Report whether a stamp names a build key and every file it records is unchanged.
 *
 * @param stamp_path The stamp file.
 * @param key The build key.
 * @return 1 when current, 0 otherwise (missing, another key, no deps line, a file changed, read or appeared). */
int openpete_mod_shader_stamp_current(const char* stamp_path, const char* key);

/** @brief Hash the shader interface a prebuilt module must match: the mod API version and every `.glsl` under `<sdk_dir>/shaders`, name and content, in strcmp order.
 *
 * @param sdk_dir The staged SDK root.
 * @param api_ver The mod API version.
 * @param out_hex Receives the 64-character hex digest and a NUL. */
void openpete_mod_shader_interface_hash(const char* sdk_dir, unsigned api_ver, char out_hex[65]);

#ifdef __cplusplus
}
#endif

#endif
