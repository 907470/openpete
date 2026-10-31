/* paths.c - portable-vs-user data layout. See openpete_paths.h. */

#include "openpete_paths.h"
#include "openpete_log.h"

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
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, w, (int)(sizeof w / sizeof w[0]))) return -1;
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        CloseHandle(h);
        return -1;
    }
    *handle = (intptr_t)h;
#else
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
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

/** @brief Base directory of the per-user layout for one class of data.
 *
 * The platform's environment variable names the base; when it is unset or
 * empty the fallback is taken under %USERPROFILE% or $HOME. The variable
 * pairs differ per class: config (APPDATA, XDG_CONFIG_HOME), state
 * (LOCALAPPDATA, XDG_STATE_HOME) and data (LOCALAPPDATA, XDG_DATA_HOME).
 * With create the base is made, since a fallback spelling may not exist yet.
 * Returns 0, or -1 with an error logged when neither variable is set or the
 * path does not fit. */
static int user_base(char* out, size_t cap, int create, const char* win_var,
                     const char* win_fallback, const char* posix_var,
                     const char* posix_fallback) {
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
 * The base comes from user_base with the given variable pair. With create
 * the base and the openpete directory are made, existing ones being left as
 * they are and mkdir failures ignored.
 * @param out Receives the path.
 * @param cap Size of out.
 * @param create Nonzero to make the directories.
 * @param win_var Windows environment variable naming the base.
 * @param win_fallback Path under %USERPROFILE% used when win_var is unset.
 * @param posix_var POSIX environment variable naming the base.
 * @param posix_fallback Path under $HOME used when posix_var is unset.
 * @return 0 on success, -1 when no base is known or the path does not fit
 *         in cap. */
static int user_openpete_dir(char* out, size_t cap, int create,
                             const char* win_var, const char* win_fallback,
                             const char* posix_var, const char* posix_fallback) {
    char base[OPP_PATH_MAX];
    if (user_base(base, sizeof base, create, win_var, win_fallback, posix_var,
                  posix_fallback) != 0)
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
    return user_openpete_dir(out, cap, create, "LOCALAPPDATA", "AppData\\Local",
                             "XDG_STATE_HOME", ".local/state");
}

/** @brief Per-user openpete directory for data (%LOCALAPPDATA%, XDG_DATA_HOME); see user_openpete_dir. */
static int user_data_dir(char* out, size_t cap, int create) {
    return user_openpete_dir(out, cap, create, "LOCALAPPDATA", "AppData\\Local",
                             "XDG_DATA_HOME", ".local/share");
}

/* ------------------------------------------------------------------ */
/* public resolvers                                                    */
/* ------------------------------------------------------------------ */

int openpete_paths_user_config_dir(char* out, size_t cap, int create) {
    return user_openpete_dir(out, cap, create, "APPDATA", "AppData\\Roaming",
                             "XDG_CONFIG_HOME", ".config");
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
