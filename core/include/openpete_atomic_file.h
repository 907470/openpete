/* openpete_atomic_file.h - replace a file so a reader sees the old or the new
 * content, never a mix.
 *
 * The new content is written to a temporary file in the target's own
 * directory ("<path>.tmp-<pid>"), every write and the close are checked, the
 * data is flushed to the device (fsync, or _commit on Windows), and the
 * temporary is then renamed over the target (rename on POSIX, MoveFileExW
 * with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows,
 * retried once when another process holds the target). A failure at any
 * step removes the temporary and leaves the target as it was; a process
 * killed before the rename leaves the target as it was and at most a stale
 * temporary beside it.
 *
 * Two write classes. Synced (flags 0) is for user data: the data and the
 * directory entry are flushed to the device. Unsynced (OP_ATOMIC_NOSYNC) is
 * for disposable captures (screenshots, harness PNGs, dumps, video): the
 * temporary is renamed without a device flush, so a power loss may lose the
 * new file, but no reader ever sees a partial one.
 *
 * Fault injection: OPENPETE_WRITE_FAIL=<substring>[:<bytes>] makes every
 * commit whose target path contains <substring> fail after <bytes> bytes
 * (default 0) have reached the temporary, with the target untouched, and
 * every op_file_write_at, op_atomic_rename and op_atomic_replace_dir on
 * such a path fail before touching it. op_atomic_file_injected_count counts
 * the failures injected.
 */
#ifndef OPENPETE_ATOMIC_FILE_H
#define OPENPETE_ATOMIC_FILE_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OP_ATOMIC_PATH_MAX 1400

/** Write-class flag: rename without flushing data or directory to the device. */
#define OP_ATOMIC_NOSYNC 1u
/** Flag: log nothing on failure; the caller reports it, with errno holding
 * the failed step's error (0 for an injected failure). Combines with either
 * write class. */
#define OP_ATOMIC_QUIET 2u

/** An atomic replacement in progress: stdio writes go to f, the temporary. */
typedef struct op_atomic_file {
    FILE* f;                            /* the temporary, open "wb" */
    char  path[OP_ATOMIC_PATH_MAX];     /* the target */
    char  tmp[OP_ATOMIC_PATH_MAX + 24]; /* the temporary's path */
    unsigned flags;                     /* OP_ATOMIC_NOSYNC, OP_ATOMIC_QUIET or 0 */
} op_atomic_file_t;

/** @brief Open a temporary file beside path for an atomic replacement of path.
 *
 * On failure the error is logged and af->f is NULL.
 * @param af Receives the open replacement.
 * @param path The target file; its directory must exist.
 * @return 0 on success, -1 when the path is too long or the temporary cannot be created. */
int op_atomic_file_open(op_atomic_file_t* af, const char* path);

/** @brief Open a temporary file beside path for an atomic replacement of path in a given write class.
 *
 * As op_atomic_file_open, which is this with flags 0 (synced).
 * @param af Receives the open replacement.
 * @param path The target file; its directory must exist.
 * @param flags OP_ATOMIC_NOSYNC for the unsynced class, else 0; OP_ATOMIC_QUIET or-ed in to log nothing.
 * @return 0 on success, -1 when the path is too long or the temporary cannot be created. */
int op_atomic_file_open_ex(op_atomic_file_t* af, const char* path, unsigned flags);

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

/** @brief Replace path with n bytes from data atomically, in a given write class.
 *
 * @param path The target file; its directory must exist.
 * @param data The new content.
 * @param n Its length in bytes.
 * @param flags OP_ATOMIC_NOSYNC for the unsynced class, else 0; OP_ATOMIC_QUIET or-ed in to log nothing.
 * @return 0 when the target holds exactly the n bytes, -1 with the target unchanged otherwise. */
int op_atomic_write_file_ex(const char* path, const void* data, size_t n, unsigned flags);

/** @brief Rename a finished file over path in one step, for a writer that streams to its own temporary.
 *
 * For an encoder that owns its output stream (video to `<path>.part`): the
 * caller closes @p src, then this replaces @p path with it as
 * op_atomic_file_commit renames. On failure @p src is left in place and
 * @p path unchanged.
 * @param src The finished file, in @p path's directory.
 * @param path The target file.
 * @param flags OP_ATOMIC_NOSYNC for the unsynced class, else 0.
 * @return 0 when path now names src's content, -1 otherwise. */
int op_atomic_rename(const char* src, const char* path, unsigned flags);

/** @brief Replace directory dest with directory staged by renaming the previous tree aside.
 *
 * dest is renamed to `<dest>.old`, staged to dest, then the aside tree is
 * removed with @p rmtree. A failed second rename renames the aside tree
 * back. A process stopped between the two renames leaves `<dest>.old` and
 * no dest; the next call renames it back before replacing. On failure
 * @p staged is left for the caller to remove.
 * @param staged The new tree, on dest's filesystem.
 * @param dest The directory to replace; it may be absent.
 * @param rmtree Removes a directory tree, returning 0 on success; called on absent paths too.
 * @return 0 when dest holds the staged tree, -1 with the previous tree in place otherwise. */
int op_atomic_replace_dir(const char* staged, const char* dest, int (*rmtree)(const char*));

/** @brief Count the failures OPENPETE_WRITE_FAIL injected in this process.
 *
 * @return The count since process start. */
unsigned long op_atomic_file_injected_count(void);

/** @brief Replace dst with a copy of src atomically.
 *
 * The bytes of @p src are streamed into a temporary beside @p dst, which
 * op_atomic_file_commit renames over @p dst, so @p dst is either absent (or
 * its old content) or the whole copy, never a prefix. @p src is only read.
 * @param src The file to copy.
 * @param dst The target file; its directory must exist.
 * @return 0 when dst holds the whole of src, -1 with dst unchanged otherwise. */
int op_atomic_copy_file(const char* src, const char* dst);

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

/** @brief Install the guard the fault-injection environment read runs inside: guard(1) before the read, guard(0) after it.
 *
 * A process that writes the environment while other threads commit files
 * installs the read side of its environment write guard here, so the
 * OPENPETE_WRITE_FAIL lookup never walks an environment a setenv is
 * moving. Without a guard the lookup runs unguarded.
 * @param guard The guard, or NULL for none. */
void op_atomic_file_set_env_guard(void (*guard)(int begin));

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_ATOMIC_FILE_H */
