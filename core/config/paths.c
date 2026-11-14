/* paths.c - portable-vs-user data layout. See openpete_paths.h. */

#include "openpete_paths.h"
#include "openpete_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
  #define OPP_MKDIR(p) _mkdir(p)
  #define OPP_SEP '\\'
  #define OPP_STAT _stat
  #define OPP_STAT_T struct _stat
  #define OPP_PORTABLE_DEFAULT 1
#else
  #include <fcntl.h>
  #include <pwd.h>
  #include <sys/file.h>
  #include <unistd.h>
  #define OPP_MKDIR(p) mkdir((p), 0755)
  #define OPP_SEP '/'
  #define OPP_STAT stat
  #define OPP_STAT_T struct stat
  #define OPP_PORTABLE_DEFAULT 0
#endif

#define OPP_PATH_MAX 1024

/* -1 = platform default, else the openpete.toml directive. */
static int s_pref = -1;
/* Memoized openpete_paths_portable(): -1 = not yet resolved. */
static int s_resolved = -1;
/* The sandbox (openpete_paths_sandbox), written at boot before any thread
 * starts: root is latched once from OPENPETE_SANDBOX and replaced by
 * openpete_paths_set_sandbox ("" = none); from_env is 1 when the variable
 * was set at the latch (the boot guard is enforced); bad is 1 when a root
 * was asked for but does not canonicalize or fit, which the guard
 * refuses. */
static struct {
    char root[OPP_PATH_MAX];
    int latched;
    int from_env;
    int bad;
} s_sb;

/* ------------------------------------------------------------------ */
/* primitives                                                          */
/* ------------------------------------------------------------------ */

int openpete_exe_path(char* out, size_t cap) {
    char buf[OPP_PATH_MAX];
#ifdef _WIN32
    /* The wide call is not limited to MAX_PATH; the narrow form of the
     * path is taken in the active code page (UTF-8 under the exe's
     * manifest), the encoding every narrow file API here interprets. */
    static wchar_t wbuf[32768];
    DWORD wn = GetModuleFileNameW(NULL, wbuf, (DWORD)(sizeof wbuf / sizeof wbuf[0]));
    if (wn == 0 || wn >= sizeof wbuf / sizeof wbuf[0]) return -1;
    if (WideCharToMultiByte(CP_ACP, 0, wbuf, -1, buf, (int)sizeof buf, NULL, NULL) <= 0)
        return -1;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
#endif
    if (strlen(buf) >= cap) return -1;
    snprintf(out, cap, "%s", buf);
    return 0;
}

int openpete_exe_dir(char* out, size_t cap) {
    char buf[OPP_PATH_MAX];
    if (openpete_exe_path(buf, sizeof buf) != 0) return -1;
    char* slash = strrchr(buf, '/');
#ifdef _WIN32
    char* back = strrchr(buf, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    if (!slash) return -1;
    *slash = '\0';
    if (strlen(buf) >= cap) return -1;
    snprintf(out, cap, "%s", buf);
    return 0;
}

int openpete_paths_lock(const char* path, intptr_t* handle) {
#ifdef _WIN32
    wchar_t w[OPP_PATH_MAX];
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, w, (int)(sizeof w / sizeof w[0]))) {
        OPL_ERROR_F("paths", "lock %s: path does not convert", path);
        return -2;
    }
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        OPL_ERROR_F("paths", "lock %s: cannot open (Windows error %lu)", path, (unsigned long)GetLastError());
        return -2;
    }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        const DWORD err = GetLastError();
        CloseHandle(h);
        if (err == ERROR_LOCK_VIOLATION) return -1;
        OPL_ERROR_F("paths", "lock %s: cannot lock (Windows error %lu)", path, (unsigned long)err);
        return -2;
    }
    *handle = (intptr_t)h;
#else
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        OPL_ERROR_F("paths", "lock %s: cannot open (%s)", path, strerror(errno));
        return -2;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        close(fd);
        if (err == EWOULDBLOCK) return -1;
        OPL_ERROR_F("paths", "lock %s: cannot lock (%s)", path, strerror(err));
        return -2;
    }
    *handle = (intptr_t)fd;
#endif
    return 0;
}

void openpete_paths_unlock(intptr_t handle) {
#ifdef _WIN32
    CloseHandle((HANDLE)handle);
#else
    close((int)handle);
#endif
}

long long openpete_process_id(void) {
#ifdef _WIN32
    return (long long)GetCurrentProcessId();
#else
    return (long long)getpid();
#endif
}

void openpete_paths_join(char* out, size_t cap, const char* dir, const char* leaf) {
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
        snprintf(out, cap, "%s%s", dir, leaf);
    else
        snprintf(out, cap, "%s%c%s", dir, OPP_SEP, leaf);
}

/** @brief Copy s into out, or return -1 when it does not fit in cap. */
static int copy_fit(char* out, size_t cap, const char* s) {
    if (strlen(s) >= cap) return -1;
    snprintf(out, cap, "%s", s);
    return 0;
}

/** @brief 1 when p names an existing directory, 0 for a file, another type, or a stat failure. */
static int dir_exists(const char* p) {
    OPP_STAT_T st;
    if (OPP_STAT(p, &st) != 0) return 0;
    /* The file-type field is an enum packed into st_mode, not a bitmask -
     * a plain `& S_IFDIR` also matches other types whose code shares that
     * bit. Mask to the field first. */
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

/** @brief 1 when a file can be created in dir, tested by creating and removing a probe file.
 *
 * A permission bit cannot answer this portably (Windows ACLs, the Program
 * Files virtualization store, read-only mounts), so the probe
 * ".openpete-write-probe" is written and removed. */
static int dir_writable(const char* dir) {
    char probe[OPP_PATH_MAX + 32];
    openpete_paths_join(probe, sizeof probe, dir, ".openpete-write-probe");
    FILE* f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}

/* ------------------------------------------------------------------ */
/* layout resolution                                                   */
/* ------------------------------------------------------------------ */

void openpete_paths_set_portable(int tri) {
    s_pref = (tri < 0) ? -1 : (tri != 0);
    s_resolved = -1;
}

int openpete_paths_portable(void) {
    if (s_resolved >= 0) return s_resolved;
    if (openpete_paths_sandbox()) { s_resolved = 0; return 0; }

    int want = (s_pref >= 0) ? s_pref : OPP_PORTABLE_DEFAULT;
    if (!want) { s_resolved = 0; return 0; }

    char exedir[OPP_PATH_MAX];
    if (openpete_exe_dir(exedir, sizeof exedir) != 0) {
        OPL_WARN_F("paths", "%s",
                   "cannot locate the executable; using the per-user directories");
        s_resolved = 0;
        return 0;
    }
    if (!dir_writable(exedir)) {
        OPL_WARN_F("paths",
                   "%s is not writable; config, cards and savestates go to the "
                   "per-user directories instead", exedir);
        s_resolved = 0;
        return 0;
    }
    s_resolved = 1;
    return 1;
}

/** @brief Take the canonical form of a sandbox root into out, creating nothing.
 *
 * The root is made only when a write path under it is created (user_base),
 * which the boot guard precedes, so a root it refuses is never made.
 * @param dir The directory.
 * @param out Receives the canonical path.
 * @param cap Capacity of @p out.
 * @return 0, or -1 when the path does not canonicalize or does not fit. */
static int take_root(const char* dir, char* out, size_t cap) {
    return openpete_paths_canonical(dir, out, cap);
}

const char* openpete_paths_sandbox(void) {
    if (!s_sb.latched) {
        s_sb.latched = 1;
        const char* e = getenv("OPENPETE_SANDBOX");
        if (e && *e) {
            s_sb.from_env = 1;
            if (take_root(e, s_sb.root, sizeof s_sb.root) != 0) {
                s_sb.root[0] = '\0';
                s_sb.bad = 1;
            }
        }
    }
    return s_sb.root[0] ? s_sb.root : NULL;
}

int openpete_paths_set_sandbox(const char* root) {
    (void)openpete_paths_sandbox();
    s_resolved = -1;
    s_sb.root[0] = '\0';
    s_sb.bad = 0;
    if (!root || !*root) return 0;
    if (take_root(root, s_sb.root, sizeof s_sb.root) == 0) return 0;
    s_sb.root[0] = '\0';
    s_sb.bad = 1;
    return -1;
}

/** @brief Base directory of the per-user layout for one class of data.
 *
 * Under a sandbox (openpete_paths_sandbox) and without @p real the base is
 * <sandbox>/<sandbox_leaf>, whatever the environment says. Otherwise the
 * platform's environment variable names the base; when it is unset or
 * empty the fallback is taken under %USERPROFILE% or $HOME. The variable
 * pairs differ per class: config (APPDATA, XDG_CONFIG_HOME), state
 * (LOCALAPPDATA, XDG_STATE_HOME) and data (LOCALAPPDATA, XDG_DATA_HOME).
 * With create the base is made, since a fallback spelling may not exist yet.
 * @param out Receives the base.
 * @param cap Capacity of @p out.
 * @param create Nonzero to make the base.
 * @param cls The class: 0 config, 1 state, 2 data.
 * @param real Nonzero to name the per-user base even under a sandbox.
 * @return 0, or -1 with an error logged when neither variable is set or the
 *         path does not fit. */
static int user_base(char* out, size_t cap, int create, int cls, int real) {
    static const char* const k_win_var[3] = {"APPDATA", "LOCALAPPDATA", "LOCALAPPDATA"};
    static const char* const k_win_fallback[3] = {"AppData\\Roaming", "AppData\\Local",
                                                  "AppData\\Local"};
    static const char* const k_posix_var[3] = {"XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME"};
    static const char* const k_posix_fallback[3] = {".config", ".local/state", ".local/share"};
    static const char* const k_sandbox_leaf[3] = {"config", "state", "data"};
    const char* sb = real ? NULL : openpete_paths_sandbox();
    if (sb) {
        char dir[OPP_PATH_MAX + 16];
        openpete_paths_join(dir, sizeof dir, sb, k_sandbox_leaf[cls]);
        if (copy_fit(out, cap, dir) != 0) {
            OPL_ERROR_F("paths", "%s", "sandbox base path too long");
            return -1;
        }
        if (create) {
            (void)OPP_MKDIR(sb);
            (void)OPP_MKDIR(out);
        }
        return 0;
    }
    const char* win_var = k_win_var[cls];
    const char* win_fallback = k_win_fallback[cls];
    const char* posix_var = k_posix_var[cls];
    const char* posix_fallback = k_posix_fallback[cls];
    char fallback[OPP_PATH_MAX];
    const char* base;
#ifdef _WIN32
    (void)posix_var; (void)posix_fallback;
    base = getenv(win_var);
    if (!base || !*base) {
        const char* up = getenv("USERPROFILE");
        if (!up || !*up) {
            OPL_ERROR_F("paths", "neither %s nor USERPROFILE set", win_var);
            return -1;
        }
        snprintf(fallback, sizeof fallback, "%s%c%s", up, OPP_SEP, win_fallback);
        base = fallback;
    }
#else
    (void)win_var; (void)win_fallback;
    base = getenv(posix_var);
    if (!base || !*base) {
        const char* home = getenv("HOME");
        if (!home || !*home) {
            OPL_ERROR_F("paths", "neither %s nor HOME set", posix_var);
            return -1;
        }
        snprintf(fallback, sizeof fallback, "%s/%s", home, posix_fallback);
        base = fallback;
    }
#endif
    if (strlen(base) >= cap) {
        OPL_ERROR_F("paths", "%s", "per-user base path too long");
        return -1;
    }
    /* The fallback spelling ($HOME/.config, %USERPROFILE%\AppData\Local) may
     * not exist yet; the environment-supplied one always does. */
    if (create) (void)OPP_MKDIR(base);
    snprintf(out, cap, "%s", base);
    return 0;
}

/** @brief Write "<user base>/openpete" for one class of data into out.
 *
 * The base comes from user_base for the class. With create the base and the
 * openpete directory are made, existing ones being left as they are and
 * mkdir failures ignored.
 * @param out Receives the path.
 * @param cap Size of out.
 * @param create Nonzero to make the directories.
 * @param cls The class: 0 config, 1 state, 2 data.
 * @param real Nonzero to name the per-user directory even under a sandbox.
 * @return 0 on success, -1 when no base is known or the path does not fit
 *         in cap. */
static int user_openpete_dir(char* out, size_t cap, int create, int cls, int real) {
    char base[OPP_PATH_MAX];
    if (user_base(base, sizeof base, create, cls, real) != 0)
        return -1;
    char dir[OPP_PATH_MAX + 16];
    openpete_paths_join(dir, sizeof dir, base, "openpete");
    if (strlen(dir) >= cap) {
        OPL_ERROR_F("paths", "%s", "per-user openpete path too long");
        return -1;
    }
    if (create) (void)OPP_MKDIR(dir);
    snprintf(out, cap, "%s", dir);
    return 0;
}

/** @brief Per-user openpete directory for state (%LOCALAPPDATA%, XDG_STATE_HOME); see user_openpete_dir. */
static int user_state_dir(char* out, size_t cap, int create) {
    return user_openpete_dir(out, cap, create, 1, 0);
}

/** @brief Per-user openpete directory for data (%LOCALAPPDATA%, XDG_DATA_HOME); see user_openpete_dir. */
static int user_data_dir(char* out, size_t cap, int create) {
    return user_openpete_dir(out, cap, create, 2, 0);
}

/* ------------------------------------------------------------------ */
/* public resolvers                                                    */
/* ------------------------------------------------------------------ */

int openpete_paths_user_config_dir(char* out, size_t cap, int create) {
    return user_openpete_dir(out, cap, create, 0, 0);
}

/** @brief Log where an earlier per-user layout left a state leaf that the portable layout now keeps elsewhere.
 *
 * Runs at most once per leaf (up to four leaves are remembered). The line
 * prints only when the portable directory does not exist yet and the
 * per-user state directory holds the same leaf. Nothing is moved; the log
 * tells the user where the old cards or savestates are. */
static void hint_legacy_state(const char* leaf, const char* portable_dir) {
    static const char* s_hinted[4];
    static int s_hinted_n;
    for (int i = 0; i < s_hinted_n; i++)
        if (!strcmp(s_hinted[i], leaf)) return;
    if (s_hinted_n < (int)(sizeof s_hinted / sizeof s_hinted[0]))
        s_hinted[s_hinted_n++] = leaf;

    if (dir_exists(portable_dir)) return;
    char base[OPP_PATH_MAX];
    if (user_state_dir(base, sizeof base, 0) != 0) return;
    char legacy[OPP_PATH_MAX + 16];
    openpete_paths_join(legacy, sizeof legacy, base, leaf);
    if (!dir_exists(legacy)) return;
    OPL_INFO_F("paths", "%s now live in %s; the previous location %s still "
                        "holds the old files", leaf, portable_dir, legacy);
}

int openpete_paths_state_dir(const char* leaf, char* out, size_t cap, int create) {
    char base[OPP_PATH_MAX];
    int portable = openpete_paths_portable();
    if (portable) {
        if (openpete_exe_dir(base, sizeof base) != 0) return -1;
    } else if (user_state_dir(base, sizeof base, create) != 0) {
        return -1;
    }

    char dir[OPP_PATH_MAX + 16];
    openpete_paths_join(dir, sizeof dir, base, leaf);
    if (strlen(dir) >= cap) {
        OPL_ERROR_F("paths", "%s-dir path too long", leaf);
        return -1;
    }
    if (create) {
        /* Only worth saying when this session is about to start writing
         * here - a report that merely named the directory has not moved
         * anything, so there is nothing to explain. */
        if (portable) hint_legacy_state(leaf, dir);
        (void)OPP_MKDIR(dir);
    }
    snprintf(out, cap, "%s", dir);
    return 0;
}

int openpete_paths_game_state_dir(const char* leaf, const char* game, char* out,
                                  size_t cap, int create) {
    char dir[OPP_PATH_MAX];
    if (openpete_paths_state_dir(leaf, dir, sizeof dir, create) != 0) return -1;
    if (!game || !*game) {
        if (strlen(dir) >= cap) return -1;
        snprintf(out, cap, "%s", dir);
        return 0;
    }
    char sub[OPP_PATH_MAX + 64];
    openpete_paths_join(sub, sizeof sub, dir, game);
    if (strlen(sub) >= cap || strlen(sub) >= sizeof sub - 1) {
        OPL_ERROR_F("paths", "%s/%s path too long", leaf, game);
        return -1;
    }
    if (create) (void)OPP_MKDIR(sub);
    snprintf(out, cap, "%s", sub);
    return 0;
}

/** @brief Adopt <exe dir>/<leaf> when it exists and is writable, writing it to out.
 *
 * The library, mods, user assets and recordings were exe-relative on every
 * platform in earlier layouts, so an older install holds content there. A
 * read-only install directory is not adopted. Returns 1 when adopted, 0
 * otherwise (also when the path does not fit in cap). */
static int adopt_install_dir(const char* leaf, char* out, size_t cap) {
    if (openpete_paths_sandbox()) return 0;
    char base[OPP_PATH_MAX];
    if (openpete_exe_dir(base, sizeof base) != 0) return 0;
    char dir[OPP_PATH_MAX + 16];
    openpete_paths_join(dir, sizeof dir, base, leaf);
    if (!dir_exists(dir) || strlen(dir) >= cap) return 0;
    if (!dir_writable(dir)) return 0;
    snprintf(out, cap, "%s", dir);
    return 1;
}

int openpete_paths_data_dir(const char* leaf, char* out, size_t cap, int create) {
    char base[OPP_PATH_MAX];
    if (openpete_paths_portable()) {
        if (openpete_exe_dir(base, sizeof base) != 0) return -1;
    } else {
        if (adopt_install_dir(leaf, out, cap)) return 0;
        if (user_data_dir(base, sizeof base, create) != 0) return -1;
    }

    char dir[OPP_PATH_MAX + 16];
    openpete_paths_join(dir, sizeof dir, base, leaf);
    if (strlen(dir) >= cap) {
        OPL_ERROR_F("paths", "%s-dir path too long", leaf);
        return -1;
    }
    if (create) (void)OPP_MKDIR(dir);
    snprintf(out, cap, "%s", dir);
    return 0;
}

int openpete_paths_user_state_dir(const char* leaf, char* out, size_t cap) {
    char base[OPP_PATH_MAX];
    if (user_openpete_dir(base, sizeof base, 0, 1, 1) != 0) return -1;
    char dir[OPP_PATH_MAX + 16];
    openpete_paths_join(dir, sizeof dir, base, leaf);
    if (strlen(dir) >= cap) return -1;
    snprintf(out, cap, "%s", dir);
    return 0;
}

/** @brief 1 when c separates path components on this platform. */
static int is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

#ifndef _WIN32
/** @brief Split path at its last separator into parent and leaf, trailing separators dropped.
 *
 * A path with no separator has the parent "."; one whose only separator
 * leads it has the parent "/".
 * @param path The path.
 * @param parent Receives the parent.
 * @param pcap Capacity of @p parent.
 * @param leaf Receives the last component.
 * @param lcap Capacity of @p leaf.
 * @return 0, or -1 when the path does not fit. */
static int split_last(const char* path, char* parent, size_t pcap, char* leaf, size_t lcap) {
    char buf[OPP_PATH_MAX];
    if (copy_fit(buf, sizeof buf, path) != 0) return -1;
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    char* slash = strrchr(buf, '/');
    if (!slash) {
        snprintf(parent, pcap, ".");
        return copy_fit(leaf, lcap, buf);
    }
    if (copy_fit(leaf, lcap, slash + 1) != 0) return -1;
    if (slash == buf) slash++;
    *slash = '\0';
    return copy_fit(parent, pcap, buf);
}

/** @brief Apply one path component to the canonical directory dir lexically: `.` and "" keep it, `..` drops its last component, any other name is joined. */
static int apply_leaf(const char* dir, const char* leaf, char* out, size_t cap) {
    char d[OPP_PATH_MAX];
    if (copy_fit(d, sizeof d, dir) != 0) return -1;
    if (!*leaf || strcmp(leaf, ".") == 0) return copy_fit(out, cap, d);
    if (strcmp(leaf, "..") == 0) {
        char* up = strrchr(d, '/');
        if (up && up != d) *up = '\0';
        else snprintf(d, sizeof d, "/");
        return copy_fit(out, cap, d);
    }
    char joined[OPP_PATH_MAX * 2];
    openpete_paths_join(joined, sizeof joined, d, leaf);
    return copy_fit(out, cap, joined);
}
#endif

int openpete_paths_canonical(const char* path, char* out, size_t cap) {
    if (!path || !*path || cap == 0) return -1;
#ifdef _WIN32
    char full[OPP_PATH_MAX];
    if (!_fullpath(full, path, sizeof full)) return -1;
    for (char* p = full; *p; p++) if (*p == '/') *p = '\\';
    size_t n = strlen(full);
    while (n > 3 && full[n - 1] == '\\') full[--n] = '\0';
    return copy_fit(out, cap, full);
#else
    char* real = realpath(path, NULL);
    if (real) {
        const int rc = copy_fit(out, cap, real);
        free(real);
        return rc;
    }
    /* The path does not resolve: canonicalize its parent and apply the
     * last component lexically, so a missing file or directory below an
     * existing one still compares against the parent's resolved form. */
    char parent[OPP_PATH_MAX], leaf[OPP_PATH_MAX], cparent[OPP_PATH_MAX];
    if (split_last(path, parent, sizeof parent, leaf, sizeof leaf) != 0) return -1;
    if (openpete_paths_canonical(parent, cparent, sizeof cparent) != 0) return -1;
    return apply_leaf(cparent, leaf, out, cap);
#endif
}

int openpete_paths_within(const char* path, const char* dir) {
    char cp[OPP_PATH_MAX], cd[OPP_PATH_MAX];
    if (openpete_paths_canonical(path, cp, sizeof cp) != 0) return 0;
    if (openpete_paths_canonical(dir, cd, sizeof cd) != 0) return 0;
    size_t n = strlen(cd);
    while (n > 1 && is_sep(cd[n - 1])) cd[--n] = '\0';
#ifdef _WIN32
    if (_strnicmp(cp, cd, n) != 0) return 0;
#else
    if (strncmp(cp, cd, n) != 0) return 0;
#endif
    return cp[n] == '\0' || is_sep(cp[n]) || (n == 1 && is_sep(cd[0]));
}

/* ------------------------------------------------------------------ */
/* sandbox guard                                                       */
/* ------------------------------------------------------------------ */

#define OPP_REAL_MAX 32

/** @brief The real write locations a sandboxed run must not touch.
 *
 * The per-user config, state and data openpete directories as the
 * environment names them; outside Windows the same three under the
 * account's passwd home, which covers a run whose HOME or XDG variables
 * point elsewhere; and every leaf the portable layout writes beside the
 * executable.
 * @param dirs Receives up to OPP_REAL_MAX paths.
 * @return The number of paths written. */
static int sandbox_real_dirs(char dirs[OPP_REAL_MAX][OPP_PATH_MAX]) {
    static const char* const k_exe_leaves[] = {
        "cards", "states", "logs", "cache", "library", "modules", "mods", "assets",
        "bk2", "screenshots", "bugreports", "openpete.toml", "openpete-devui.ini"};
    int n = 0;
    for (int cls = 0; cls < 3; cls++)
        if (user_openpete_dir(dirs[n], OPP_PATH_MAX, 0, cls, 1) == 0) n++;
#ifndef _WIN32
    static const char* const k_home_leaves[3] = {
        ".config/openpete", ".local/state/openpete", ".local/share/openpete"};
    const struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *pw->pw_dir)
        for (int i = 0; i < 3; i++)
            snprintf(dirs[n++], OPP_PATH_MAX, "%s/%s", pw->pw_dir, k_home_leaves[i]);
#endif
    char exe[OPP_PATH_MAX];
    if (openpete_exe_dir(exe, sizeof exe) == 0)
        for (size_t i = 0; i < sizeof k_exe_leaves / sizeof k_exe_leaves[0]; i++)
            openpete_paths_join(dirs[n++], OPP_PATH_MAX, exe, k_exe_leaves[i]);
    return n;
}

/** @brief Print the refusal and return 1 when path lies in one of the real locations, else 0.
 * @param path The write path tested.
 * @param what What names the path (a flag, a variable, a resolved leaf).
 * @param dirs The real locations.
 * @param n Their count.
 * @return 1 refused, 0 clear. */
static int sandbox_refuse_if_real(const char* path, const char* what,
                                  char dirs[OPP_REAL_MAX][OPP_PATH_MAX], int n) {
    if (!path || !*path) return 0;
    for (int i = 0; i < n; i++) {
        if (!openpete_paths_within(path, dirs[i])) continue;
        fprintf(stderr, "openpete: OPENPETE_SANDBOX: %s resolves to %s, inside the real "
                        "location %s; refusing to start. Point it inside the sandbox root %s\n",
                what, path, dirs[i], s_sb.root);
        return 1;
    }
    return 0;
}

/** @brief Refuse the resolved state and data leaves and the class directories that lie in a real location.
 * @param dirs The real locations.
 * @param n Their count.
 * @return 1 refused, 0 clear. */
static int sandbox_check_resolved(char dirs[OPP_REAL_MAX][OPP_PATH_MAX], int n) {
    static const char* const k_state[] = {"cards", "states", "logs", "cache"};
    static const char* const k_data[] = {"library", "modules", "mods", "assets",
                                         "bk2", "screenshots", "bugreports"};
    char p[OPP_PATH_MAX];
    for (int cls = 0; cls < 3; cls++)
        if (user_openpete_dir(p, sizeof p, 0, cls, 0) == 0 &&
            sandbox_refuse_if_real(p, "a per-user directory", dirs, n))
            return 1;
    for (size_t i = 0; i < sizeof k_state / sizeof k_state[0]; i++)
        if (openpete_paths_state_dir(k_state[i], p, sizeof p, 0) == 0 &&
            sandbox_refuse_if_real(p, k_state[i], dirs, n))
            return 1;
    for (size_t i = 0; i < sizeof k_data / sizeof k_data[0]; i++)
        if (openpete_paths_data_dir(k_data[i], p, sizeof p, 0) == 0 &&
            sandbox_refuse_if_real(p, k_data[i], dirs, n))
            return 1;
    return 0;
}

/** @brief Refuse an output path a variable or a command-line flag names in a real location.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param dirs The real locations.
 * @param n Their count.
 * @return 1 refused, 0 clear. */
static int sandbox_check_named(int argc, char** argv, char dirs[OPP_REAL_MAX][OPP_PATH_MAX], int n) {
    static const char* const k_env[] = {"OPENPETE_LOG_FILE", "OPENPETE_HANG_TRACE",
                                        "OPENPETE_HANG_RAM", "OPENPETE_CARD_DIR",
                                        "OPENPETE_CONFIG"};
    static const char* const k_flags[] = {
        "--log-file", "--card-dir", "--card1", "--card2", "--config", "--bk2-record",
        "--screenshot-output", "--state-save-output", "--ram-dump-output", "--record",
        "--profile-out", "--halt-cpu-dump-output", "--mods-dir"};
    for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; i++)
        if (sandbox_refuse_if_real(getenv(k_env[i]), k_env[i], dirs, n)) return 1;
    for (int a = 1; a < argc; a++) {
        for (size_t f = 0; f < sizeof k_flags / sizeof k_flags[0]; f++) {
            const size_t fl = strlen(k_flags[f]);
            if (strncmp(argv[a], k_flags[f], fl) != 0) continue;
            const char* v = NULL;
            if (argv[a][fl] == '=') v = argv[a] + fl + 1;
            else if (argv[a][fl] == '\0' && a + 1 < argc && strncmp(argv[a + 1], "--", 2) != 0)
                v = argv[a + 1];
            if (v && sandbox_refuse_if_real(v, k_flags[f], dirs, n)) return 1;
        }
    }
    return 0;
}

int openpete_paths_sandbox_guard(int argc, char** argv) {
    if (!openpete_paths_sandbox() && !s_sb.bad) return 0;
    if (!s_sb.from_env) return 0;
    if (s_sb.bad) {
        fprintf(stderr, "openpete: OPENPETE_SANDBOX=%s: the root does not resolve or is too long; "
                        "refusing to start\n", getenv("OPENPETE_SANDBOX") ? getenv("OPENPETE_SANDBOX") : "");
        return OPENPETE_EXIT_SANDBOX_GUARD;
    }
    char dirs[OPP_REAL_MAX][OPP_PATH_MAX];
    const int n = sandbox_real_dirs(dirs);
    if (sandbox_check_resolved(dirs, n) || sandbox_check_named(argc, argv, dirs, n))
        return OPENPETE_EXIT_SANDBOX_GUARD;
    return 0;
}
