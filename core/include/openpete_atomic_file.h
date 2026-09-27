/* openpete_atomic_file.h — replace a file so a reader sees the old or the new
 * content, never a mix.
 *
 * The new content is written to a temporary file in the target's own
 * directory ("<path>.tmp-<pid>"), every write and the close are checked, the
 * data is flushed to the device (fsync, or _commit on Windows), and the
 * temporary is then renamed over the target (rename on POSIX, MoveFileExW
 * with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows). A
 * failure at any step removes the temporary and leaves the target as it
 * was; a process killed before the rename leaves the target as it was and
 * at most a stale temporary beside it.
 *
 * Fault injection: OPENPETE_WRITE_FAIL=<substring>[:<bytes>] makes every
 * commit whose target path contains <substring> fail after <bytes> bytes
 * (default 0) have reached the temporary, with the target untouched, and
 * every op_file_write_at on such a path fail before writing.
 */
#ifndef OPENPETE_ATOMIC_FILE_H
#define OPENPETE_ATOMIC_FILE_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OP_ATOMIC_PATH_MAX 1400

/** An atomic replacement in progress: stdio writes go to f, the temporary. */
typedef struct op_atomic_file {
    FILE* f;                            /* the temporary, open "wb" */
    char  path[OP_ATOMIC_PATH_MAX];     /* the target */
    char  tmp[OP_ATOMIC_PATH_MAX + 24]; /* the temporary's path */
} op_atomic_file_t;

/** @brief Open a temporary file beside path for an atomic replacement of path.
 *
 * On failure the error is logged and af->f is NULL.
 * @param af Receives the open replacement.
 * @param path The target file; its directory must exist.
 * @return 0 on success, -1 when the path is too long or the temporary cannot be created. */
int op_atomic_file_open(op_atomic_file_t* af, const char* path);

/** @brief Finish an atomic replacement: flush, sync and close the temporary, then rename it over the target.
 *
 * A stdio error recorded on af->f by any earlier write (ferror), a failed
 * fflush, fsync or fclose, a failed rename, or an injected failure
 * (OPENPETE_WRITE_FAIL) removes the temporary, logs the cause and leaves
 * the target unchanged. af->f is closed in every case.
 * @param af A replacement from op_atomic_file_open.
 * @return 0 when the target now holds the new content, -1 otherwise. */
int op_atomic_file_commit(op_atomic_file_t* af);

/** @brief Abandon an atomic replacement: close and remove the temporary, leaving the target unchanged.
 *
 * @param af A replacement from op_atomic_file_open; a NULL af->f is a no-op. */
void op_atomic_file_abort(op_atomic_file_t* af);

/** @brief Replace path with n bytes from data atomically.
 *
 * @param path The target file; its directory must exist.
 * @param data The new content.
 * @param n Its length in bytes.
 * @return 0 when the target holds exactly the n bytes, -1 with the target unchanged otherwise. */
int op_atomic_write_file(const char* path, const void* data, size_t n);

/** @brief Overwrite n bytes at offset of an existing file in place and flush them to the device.
 *
 * For a fixed-layout image whose unit of change is a record no larger than
 * a device sector (a 128-byte memory-card frame), where rewriting the whole
 * file per record costs the file size in writes. The open, seek, write,
 * flush, sync and close are each checked; OPENPETE_WRITE_FAIL applies as
 * for op_atomic_file_commit.
 * @param path An existing file at least offset + n bytes long.
 * @param offset Byte offset of the record.
 * @param data The record.
 * @param n Its length in bytes.
 * @return 0 when the bytes reached the file, -1 otherwise (the file may be missing or shorter). */
int op_file_write_at(const char* path, long offset, const void* data, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_ATOMIC_FILE_H */
