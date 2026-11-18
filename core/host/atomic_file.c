/* atomic_file.c - temp-file-and-rename replacement of persisted files. See
 * openpete_atomic_file.h. */

#include "openpete_atomic_file.h"
#include "openpete_log.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

/* The environment read guard (op_atomic_file_set_env_guard); NULL for none. */
static _Atomic(void (*)(int)) s_env_guard;
/* Failures OPENPETE_WRITE_FAIL injected (op_atomic_file_injected_count). */
static atomic_ulong s_injected;

void op_atomic_file_set_env_guard(void (*guard)(int begin)) { atomic_store(&s_env_guard, guard); }

/** @brief Report whether OPENPETE_WRITE_FAIL names path, and the byte count the injected failure keeps.
 *
 * The variable is `<substring>[:<bytes>]`; the substring must be non-empty
 * and occur in path.
 * @param path The target path of a commit.
 * @param keep Receives the byte count, 0 when none is given.
 * @return 1 when the commit must fail, else 0. */
static int write_fail_injected(const char* path, long* keep) {
    void (*guard)(int) = atomic_load(&s_env_guard);
    char pat[256];
    if (guard) guard(1);
    const char* spec = getenv("OPENPETE_WRITE_FAIL");
    snprintf(pat, sizeof pat, "%s", spec ? spec : "");
    if (guard) guard(0);
    *keep = 0;
    if (!pat[0]) return 0;
    char* colon = strrchr(pat, ':');
    if (colon) {
        *colon = '\0';
        *keep = strtol(colon + 1, NULL, 0);
    }
    if (!pat[0] || strstr(path, pat) == NULL) return 0;
    atomic_fetch_add(&s_injected, 1);
    return 1;
}

unsigned long op_atomic_file_injected_count(void) { return atomic_load(&s_injected); }

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

#ifdef _WIN32
/** @brief Move tmp over path with MoveFileExW, retrying once when the target is held.
 *
 * A target another process holds open without FILE_SHARE_DELETE (a viewer,
 * a scanner) refuses the replace with ERROR_ACCESS_DENIED or
 * ERROR_SHARING_VIOLATION; the refusal is logged and the move retried once
 * after 100 ms.
 * @param wtmp The temporary's path.
 * @param wpath The target's path.
 * @param path The target's narrow path, for the log.
 * @param mflags MoveFileExW flags.
 * @return Nonzero on success. */
static BOOL move_replace(const wchar_t* wtmp, const wchar_t* wpath, const char* path, DWORD mflags) {
    if (MoveFileExW(wtmp, wpath, mflags)) return TRUE;
    DWORD e = GetLastError();
    if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) return FALSE;
    OPL_WARN_F("atomic-file", "%s: replace refused (error %lu, target held open?) - retrying once",
               path, (unsigned long)e);
    Sleep(100);
    return MoveFileExW(wtmp, wpath, mflags);
}
#endif

/** @brief Rename the temporary over the target, replacing it in one step.
 *
 * On POSIX, rename replaces an existing target atomically and, for the
 * synced class, the directory is then synced so the new entry survives a
 * power loss. On Windows, MoveFileExW with MOVEFILE_REPLACE_EXISTING (plus
 * MOVEFILE_WRITE_THROUGH for the synced class) replaces the target without
 * removing it first, retried once when the target is held (move_replace);
 * the narrow paths convert through the active code page, the one fopen used
 * to create them.
 * @param tmp The temporary's path.
 * @param path The target's path.
 * @param flags OP_ATOMIC_NOSYNC skips the directory sync and write-through.
 * @return 0 on success, -1 on failure. */
static int replace_target(const char* tmp, const char* path, unsigned flags) {
#ifdef _WIN32
    wchar_t wtmp[OP_ATOMIC_PATH_MAX + 24], wpath[OP_ATOMIC_PATH_MAX];
    if (!MultiByteToWideChar(CP_ACP, 0, tmp, -1, wtmp, (int)(sizeof wtmp / sizeof wtmp[0])) ||
        !MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])))
        return -1;
    DWORD mflags = MOVEFILE_REPLACE_EXISTING;
    if (!(flags & OP_ATOMIC_NOSYNC)) mflags |= MOVEFILE_WRITE_THROUGH;
    if (!move_replace(wtmp, wpath, path, mflags)) {
        errno = EACCES;
        return -1;
    }
    return 0;
#else
    /* A target that exists but is not writable is refused, as MoveFileExW
     * refuses a read-only target on Windows; a writable target's mode
     * carries over to the replacement. */
    struct stat st;
    if (stat(path, &st) == 0) {
        if (access(path, W_OK) != 0) return -1;
        (void)chmod(tmp, st.st_mode & 07777);
    }
    if (rename(tmp, path) != 0) return -1;
    if (flags & OP_ATOMIC_NOSYNC) return 0;
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
    return op_atomic_file_open_ex(af, path, 0);
}

int op_atomic_file_open_ex(op_atomic_file_t* af, const char* path, unsigned flags) {
    af->f = NULL;
    af->tmp[0] = '\0';
    af->flags = flags;
    if (!path || strlen(path) >= sizeof af->path) {
        if (!(flags & OP_ATOMIC_QUIET))
            OPL_ERROR_F("atomic-file", "path too long: %s", path ? path : "(null)");
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(af->path, sizeof af->path, "%s", path);
    snprintf(af->tmp, sizeof af->tmp, "%s.tmp-%d", path, (int)op_getpid());
    af->f = fopen(af->tmp, "wb");
    if (!af->f) {
        if (!(flags & OP_ATOMIC_QUIET))
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
    } else if (!(af->flags & OP_ATOMIC_NOSYNC) && sync_stream(af->f) != 0) {
        why = "sync failed";
        err = errno;
    }
    int close_rc = fclose(af->f);
    af->f = NULL;
    if (!why && close_rc != 0) {
        why = "close failed";
        err = errno;
    }
    if (!why && replace_target(af->tmp, af->path, af->flags) != 0) {
        why = "rename failed";
        err = errno;
    }
    if (why) {
        remove(af->tmp);
        if (!(af->flags & OP_ATOMIC_QUIET))
            OPL_ERROR_F("atomic-file", "%s: %s%s%s - previous file kept", af->path, why,
                        err ? ": " : "", err ? strerror(err) : "");
        errno = err;
        return -1;
    }
    return 0;
}

int op_atomic_write_file(const char* path, const void* data, size_t n) {
    return op_atomic_write_file_ex(path, data, n, 0);
}

int op_atomic_write_file_ex(const char* path, const void* data, size_t n, unsigned flags) {
    op_atomic_file_t af;
    if (op_atomic_file_open_ex(&af, path, flags) != 0) return -1;
    if (n && fwrite(data, 1, n, af.f) != n) {
        if (!(flags & OP_ATOMIC_QUIET))
            OPL_ERROR_F("atomic-file", "%s: short write - previous file kept", path);
        op_atomic_file_abort(&af);
        return -1;
    }
    return op_atomic_file_commit(&af);
}

int op_atomic_copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) {
        OPL_ERROR_F("atomic-file", "cannot open %s for copying: %s", src, strerror(errno));
        return -1;
    }
    op_atomic_file_t af;
    if (op_atomic_file_open(&af, dst) != 0) {
        fclose(in);
        return -1;
    }
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, got, af.f) != got) break;
    }
    int read_err = ferror(in) || !feof(in);
    fclose(in);
    if (read_err) {
        OPL_ERROR_F("atomic-file", "%s: copy from %s incomplete - target unchanged", dst, src);
        op_atomic_file_abort(&af);
        return -1;
    }
    return op_atomic_file_commit(&af);
}

int op_atomic_rename(const char* src, const char* path, unsigned flags) {
    long keep = 0;
    const char* why = NULL;
    if (write_fail_injected(path, &keep)) why = "injected failure (OPENPETE_WRITE_FAIL)";
    else if (replace_target(src, path, flags) != 0) why = "rename failed";
    if (!why) return 0;
    OPL_ERROR_F("atomic-file", "%s: %s - previous file kept, new content left at %s", path, why,
                src);
    return -1;
}

/** @brief Report whether a path names an existing file or directory.
 *
 * @param path The path.
 * @return 1 when stat succeeds, else 0. */
static int path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int op_atomic_replace_dir(const char* staged, const char* dest, int (*rmtree)(const char*)) {
    char aside[OP_ATOMIC_PATH_MAX + 8];
    long keep = 0;
    if (strlen(dest) >= OP_ATOMIC_PATH_MAX) {
        OPL_ERROR_F("atomic-file", "path too long: %s", dest);
        return -1;
    }
    snprintf(aside, sizeof aside, "%s.old", dest);
    /* A process stopped between the two renames below left the previous
     * tree aside and none in place: put it back before anything else. */
    if (!path_exists(dest) && path_exists(aside) && rename(aside, dest) != 0) {
        OPL_ERROR_F("atomic-file", "%s: cannot restore %s: %s", dest, aside, strerror(errno));
        return -1;
    }
    (void)rmtree(aside);
    if (write_fail_injected(dest, &keep)) {
        OPL_ERROR_F("atomic-file", "%s: injected failure (OPENPETE_WRITE_FAIL) - previous tree kept",
                    dest);
        return -1;
    }
    int had = path_exists(dest);
    if (had && rename(dest, aside) != 0) {
        OPL_ERROR_F("atomic-file", "%s: cannot move aside: %s - previous tree kept", dest,
                    strerror(errno));
        return -1;
    }
    if (rename(staged, dest) != 0) {
        int err = errno;
        if (had && rename(aside, dest) != 0)
            OPL_ERROR_F("atomic-file", "%s: cannot restore from %s: %s", dest, aside,
                        strerror(errno));
        OPL_ERROR_F("atomic-file", "%s: cannot rename %s into place: %s - previous tree kept",
                    dest, staged, strerror(err));
        return -1;
    }
    if (had && rmtree(aside) != 0)
        OPL_WARN_F("atomic-file", "%s: previous tree left at %s", dest, aside);
    return 0;
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
