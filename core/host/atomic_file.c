/* atomic_file.c — temp-file-and-rename replacement of persisted files. See
 * openpete_atomic_file.h. */

#include "openpete_atomic_file.h"
#include "openpete_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #include <io.h>
  #include <process.h>
  #define op_getpid() _getpid()
#else
  #include <fcntl.h>
  #include <unistd.h>
  #define op_getpid() getpid()
#endif

/** @brief Report whether OPENPETE_WRITE_FAIL names path, and the byte count the injected failure keeps.
 *
 * The variable is `<substring>[:<bytes>]`; the substring must be non-empty
 * and occur in path.
 * @param path The target path of a commit.
 * @param keep Receives the byte count, 0 when none is given.
 * @return 1 when the commit must fail, else 0. */
static int write_fail_injected(const char* path, long* keep) {
    const char* spec = getenv("OPENPETE_WRITE_FAIL");
    *keep = 0;
    if (!spec || !*spec) return 0;
    char pat[256];
    snprintf(pat, sizeof pat, "%s", spec);
    char* colon = strrchr(pat, ':');
    if (colon) {
        *colon = '\0';
        *keep = strtol(colon + 1, NULL, 0);
    }
    return pat[0] && strstr(path, pat) != NULL;
}

/** @brief Flush a stdio stream's written data to the device.
 *
 * @param f An open stream whose buffer was already flushed.
 * @return 0 on success, -1 on failure with errno set. */
static int sync_stream(FILE* f) {
#ifdef _WIN32
    return _commit(_fileno(f)) == 0 ? 0 : -1;
#else
    return fsync(fileno(f)) == 0 ? 0 : -1;
#endif
}

/** @brief Rename the temporary over the target, replacing it in one step.
 *
 * On POSIX, rename replaces an existing target atomically and the
 * directory is then synced so the new entry survives a power loss. On
 * Windows, MoveFileExW with MOVEFILE_REPLACE_EXISTING | WRITE_THROUGH
 * replaces the target without removing it first; the narrow paths convert
 * through the active code page, the one fopen used to create them.
 * @param tmp The temporary's path.
 * @param path The target's path.
 * @return 0 on success, -1 on failure. */
static int replace_target(const char* tmp, const char* path) {
#ifdef _WIN32
    wchar_t wtmp[OP_ATOMIC_PATH_MAX + 24], wpath[OP_ATOMIC_PATH_MAX];
    if (!MultiByteToWideChar(CP_ACP, 0, tmp, -1, wtmp, (int)(sizeof wtmp / sizeof wtmp[0])) ||
        !MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])))
        return -1;
    if (!MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        errno = EACCES;
        return -1;
    }
    return 0;
#else
    if (rename(tmp, path) != 0) return -1;
    char dir[OP_ATOMIC_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = '\0';
    else if (slash) *slash = '\0';
    else snprintf(dir, sizeof dir, ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        (void)fsync(dfd);   /* best effort: the rename itself already landed */
        close(dfd);
    }
    return 0;
#endif
}

int op_atomic_file_open(op_atomic_file_t* af, const char* path) {
    af->f = NULL;
    af->tmp[0] = '\0';
    if (!path || strlen(path) >= sizeof af->path) {
        OPL_ERROR_F("atomic-file", "path too long: %s", path ? path : "(null)");
        return -1;
    }
    snprintf(af->path, sizeof af->path, "%s", path);
    snprintf(af->tmp, sizeof af->tmp, "%s.tmp-%d", path, (int)op_getpid());
    af->f = fopen(af->tmp, "wb");
    if (!af->f) {
        OPL_ERROR_F("atomic-file", "cannot create %s: %s", af->tmp, strerror(errno));
        return -1;
    }
    return 0;
}

void op_atomic_file_abort(op_atomic_file_t* af) {
    if (!af->f) return;
    fclose(af->f);
    af->f = NULL;
    remove(af->tmp);
}

int op_atomic_file_commit(op_atomic_file_t* af) {
    if (!af->f) return -1;
    long keep = 0;
    const char* why = NULL;
    int err = 0;
    if (write_fail_injected(af->path, &keep)) {
        /* The temporary is cut to the kept length, as a write that failed
         * part way would leave it, and the target is never touched. */
        fflush(af->f);
        if (fseek(af->f, keep, SEEK_SET) == 0) {
#ifdef _WIN32
            int cut = _chsize(_fileno(af->f), keep);
#else
            int cut = ftruncate(fileno(af->f), (off_t)keep);
#endif
            (void)cut;   /* the temporary is removed below either way */
        }
        why = "injected failure (OPENPETE_WRITE_FAIL)";
    } else if (ferror(af->f)) {
        why = "write failed";
        err = errno;
    } else if (fflush(af->f) != 0) {
        why = "flush failed";
        err = errno;
    } else if (sync_stream(af->f) != 0) {
        why = "sync failed";
        err = errno;
    }
    int close_rc = fclose(af->f);
    af->f = NULL;
    if (!why && close_rc != 0) {
        why = "close failed";
        err = errno;
    }
    if (!why && replace_target(af->tmp, af->path) != 0) {
        why = "rename failed";
        err = errno;
    }
    if (why) {
        remove(af->tmp);
        OPL_ERROR_F("atomic-file", "%s: %s%s%s — previous file kept", af->path, why,
                    err ? ": " : "", err ? strerror(err) : "");
        return -1;
    }
    return 0;
}

int op_atomic_write_file(const char* path, const void* data, size_t n) {
    op_atomic_file_t af;
    if (op_atomic_file_open(&af, path) != 0) return -1;
    if (n && fwrite(data, 1, n, af.f) != n) {
        OPL_ERROR_F("atomic-file", "%s: short write — previous file kept", path);
        op_atomic_file_abort(&af);
        return -1;
    }
    return op_atomic_file_commit(&af);
}

int op_file_write_at(const char* path, long offset, const void* data, size_t n) {
    long keep = 0;
    if (write_fail_injected(path, &keep)) {
        OPL_ERROR_F("atomic-file", "%s: injected failure (OPENPETE_WRITE_FAIL)", path);
        return -1;
    }
    FILE* f = fopen(path, "r+b");
    if (!f) {
        OPL_ERROR_F("atomic-file", "cannot open %s for update: %s", path, strerror(errno));
        return -1;
    }
    int ok = fseek(f, offset, SEEK_SET) == 0 && fwrite(data, 1, n, f) == n &&
             fflush(f) == 0 && sync_stream(f) == 0;
    int err = errno;
    if (fclose(f) != 0 && ok) {
        ok = 0;
        err = errno;
    }
    if (!ok) {
        OPL_ERROR_F("atomic-file", "%s: write of %zu bytes at %ld failed: %s", path, n,
                    offset, strerror(err));
        return -1;
    }
    return 0;
}
