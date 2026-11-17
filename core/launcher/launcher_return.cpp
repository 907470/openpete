// core/launcher/launcher_return.cpp - the settings menu's EXIT GAME back to
// the launcher (contract in openpete_launcher.h).
//
// A game session cannot be torn down and started again inside one process:
// openpete_run seals the time and game-layout groups for the process's life,
// the mod api closes with a one-way latch, and the launcher hands its window
// and device to the game with no way back. The return therefore ends the
// process through its ordinary teardown and starts the launcher again from a
// fresh process image, run by an atexit handler registered before any other
// so that it runs after them all.
//
// The relaunch replays the process's own argv with the one-shot flags of
// kDropped removed, and sets OPENPETE_LAUNCHER_RETURN, which the returned
// image reads at entry; under it the launcher also ignores the inputs that
// boot an entry without the game page and are not argv flags (the config
// file's game key, a bare image argument; library.cpp, resolve_pick).
#include "openpete_launcher.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_paths.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

/* The environment variable that carries the return to the relaunched image:
 * "<count>:<game>:<fullscreen>:<maximized>:<x>:<y>:<w>:<h>". */
constexpr const char* kReturnEnv = "OPENPETE_LAUNCHER_RETURN";

/* A command-line flag the relaunch leaves out: it applies to one game
 * session, and the relaunched launcher starts none until the player picks.
 * `values` is how many following tokens it takes (-1: the next one when it
 * does not start with '-'); `prefix` matches every flag that begins with
 * the spelling. The returned launcher also ignores the boot selectors that
 * reach it another way (the config file's game key, a bare image argument;
 * resolve_pick). */
struct Dropped {
    const char* flag;
    int values;
    bool prefix;
    const char* why;
};

constexpr Dropped kDropped[] = {
    {"--game", 1, false, "boots its game's entry without the game page"},
    {"--ingest", 1, false, "adds a disc and boots it without the game page"},
    {"--skip-to-level", 1, false, "jumps one session to a level"},
    {"--skip-to-area", 1, false, "jumps one session to an area"},
    {"--skip-to-cutscene", 1, false, "jumps one session to a cutscene"},
    {"--bk2", 1, false, "replays one movie in one session"},
    {"--bk2-record", -1, false, "records one session's movie, which a second session would overwrite"},
    {"--bk2-frame-offset", 1, false, "offsets one movie"},
    {"--bk2", 0, true, "a movie flag of one session"},
    {"--record", 1, false, "records one session's video, which a second session would overwrite"},
    {"--record-frames", 1, false, "bounds one recording"},
    {"--frame-limit", 1, false, "bounds one session's length"},
    {"--max-runtime-seconds", 1, false, "bounds one session's length"},
    {"--screenshot-on-frame", 1, false, "captures from one session"},
    {"--screenshot-on-tick", 1, false, "captures from one session"},
    {"--screenshot-intermediate-on-frame", 1, false, "captures from one session"},
    {"--state-load-on-frame", 1, false, "loads a savestate into one session"},
    {"--state-save-on-frame", 1, false, "saves a savestate from one session"},
    {"--state-load-input", 1, false, "loads a savestate into one session"},
    {"--ram-dump-on-frame", 1, false, "dumps RAM from one session"},
    {"--freecam-pose", 1, false, "poses one session's camera"},
    {"--freecam-path", 1, false, "drives one session's camera"},
    {"--player-pose", 1, false, "poses one session's player"},
};

/* The argv the process started with, for the relaunch. */
std::vector<std::string> s_argv;
/* 1 between a return request and the relaunch; cleared by a cancel. */
std::atomic<int> s_pending{0};
/* 1 once a quit (window close, quit signal, quit key) arrived: it ends the
 * process whichever of it and EXIT GAME the process saw first. */
std::atomic<int> s_quit{0};
/* 1 once a return was requested, so the exit can say a cancel withdrew it. */
bool s_requested = false;
/* The game a return request names. */
char s_game_out[32] = {};

/* The descriptors open at entry: inherited from the parent (stdio, a
 * launcher's keep-alive pipe), so the relaunched image inherits them too.
 * Every other descriptor is closed across the relaunch. */
std::vector<int> s_inherited_fds;

/** @brief Return the system-wide monotonic clock in ms, the clock the return's log lines share across the process images.
 *
 * @return Milliseconds. */
unsigned long long steady_ms() {
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** @brief List the process's open descriptors: /proc/self/fd where it exists, else a probe of the first 1024.
 *
 * @return The descriptors, ascending. */
std::vector<int> open_fds() {
    std::vector<int> fds;
#ifndef _WIN32
    if (DIR* d = opendir("/proc/self/fd")) {
        const int self = dirfd(d);
        while (const dirent* e = readdir(d))
            if (e->d_name[0] != '.' && std::atoi(e->d_name) != self) fds.push_back(std::atoi(e->d_name));
        closedir(d);
    } else {
        for (int fd = 0; fd < 1024; fd++)
            if (fcntl(fd, F_GETFD) != -1) fds.push_back(fd);
    }
    std::sort(fds.begin(), fds.end());
#endif
    return fds;
}

#ifndef _WIN32
/** @brief Mark every descriptor the process opened itself close-on-exec, so the relaunched image starts with only those it inherited at entry.
 *
 * A descriptor a library opened without O_CLOEXEC (a disc image, a wake
 * pipe) would otherwise survive every relaunch.
 * @return The count marked. */
int close_own_fds_on_exec() {
    int n = 0;
    for (int fd : open_fds()) {
        if (std::binary_search(s_inherited_fds.begin(), s_inherited_fds.end(), fd)) continue;
        const int fl = fcntl(fd, F_GETFD);
        if (fl == -1 || (fl & FD_CLOEXEC)) continue;
        (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        n++;
    }
    return n;
}
#endif

/* What OPENPETE_LAUNCHER_RETURN carried into this image. */
struct Incoming {
    unsigned long long armed_ms = 0;   /* steady_ms at entry */
    int count = 0;
    std::string game;
    int fullscreen = 0, maximized = 0, x = 0, y = 0, w = 0, h = 0;
    bool window = false;
} s_in;

/** @brief Return the kDropped row a token matches, or NULL.
 *
 * A `--flag=value` token matches its flag's row.
 * @param tok The token.
 * @return The row, or NULL to keep the token. */
const Dropped* dropped_row(const char* tok) {
    const char* eq = std::strchr(tok, '=');
    const size_t n = eq ? (size_t)(eq - tok) : std::strlen(tok);
    for (const Dropped& d : kDropped) {
        const size_t dn = std::strlen(d.flag);
        if (d.prefix ? n >= dn && !std::strncmp(tok, d.flag, dn) : n == dn && !std::strncmp(tok, d.flag, n))
            return &d;
    }
    return nullptr;
}

/** @brief Build the relaunch's arguments: the process's argv without the kDropped flags and their values, each drop logged.
 *
 * @return The arguments, argv[0] first. */
std::vector<std::string> relaunch_args() {
    std::vector<std::string> out;
    for (size_t i = 0; i < s_argv.size(); i++) {
        const Dropped* d = i ? dropped_row(s_argv[i].c_str()) : nullptr;
        if (!d) {
            out.push_back(s_argv[i]);
            continue;
        }
        int skip = std::strchr(s_argv[i].c_str(), '=') ? 0 : d->values;
        if (skip < 0) skip = (i + 1 < s_argv.size() && s_argv[i + 1][0] != '-') ? 1 : 0;
        OPL_INFO_F("launcher", "return: %s left out of the relaunch (%s)", s_argv[i].c_str(), d->why);
        i += (size_t)skip;
    }
    return out;
}

/** @brief Set OPENPETE_LAUNCHER_RETURN for the relaunched image: the next count, the exited game and the game window's last state.
 *
 * @return The value set. */
std::string export_return() {
    int fs = 0, mx = 0, x = 0, y = 0, w = 0, h = 0;
    const bool win = openpete_native_window_final_state(&fs, &mx, &x, &y, &w, &h) != 0;
    char v[160];
    if (win) std::snprintf(v, sizeof v, "%d:%s:%d:%d:%d:%d:%d:%d", s_in.count + 1, s_game_out, fs, mx, x, y, w, h);
    else std::snprintf(v, sizeof v, "%d:%s", s_in.count + 1, s_game_out);
#ifdef _WIN32
    SetEnvironmentVariableA(kReturnEnv, v);
#else
    setenv(kReturnEnv, v, 1);
#endif
    return v;
}

#ifdef _WIN32
/** @brief Convert UTF-8 text to UTF-16 for a wide Win32 call.
 *
 * @param u The UTF-8 text.
 * @return The UTF-16 text; empty on a conversion failure. */
std::wstring widen(const char* u) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u, -1, w.data(), n);
    w.resize((size_t)n - 1);
    return w;
}

/** @brief Quote one argument for a Windows command line as CommandLineToArgvW reads it.
 *
 * @param a The argument.
 * @return The quoted argument. */
std::string win_quote(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    size_t bs = 0;
    for (char c : a) {
        if (c == '\\') { bs++; continue; }
        q.append(c == '"' ? bs * 2 + 1 : bs, '\\');
        bs = 0;
        q.push_back(c);
    }
    q.append(bs * 2, '\\');
    q.push_back('"');
    return q;
}
#endif

/** @brief atexit handler: start the launcher again when a return is pending; registered first, so it runs after every other handler.
 *
 * POSIX replaces the process image (execv of the executable): the process id,
 * the parent's wait and inherited descriptors stay; every other thread ends;
 * the signal mask is cleared first so the new image starts with none
 * blocked. Windows starts a new process with the same working directory and
 * environment, and this one exits. A failure is logged and the process exits
 * as it would have. */
void relaunch_at_exit() {
    if (!s_pending.load()) {
        if (s_requested)
            std::fprintf(stderr, "openpete: return to the launcher withdrawn: the process ends\n");
        return;
    }
    char exe[4096];
    if (openpete_exe_path(exe, sizeof exe) != 0) {
        std::fprintf(stderr, "openpete: return to the launcher failed: the executable's path is unknown\n");
        return;
    }
    const std::vector<std::string> args = relaunch_args();
    const std::string v = export_return();
    std::fprintf(stderr, "openpete: returning to the launcher (%s=%s) at steady %llu ms: %s\n", kReturnEnv,
                 v.c_str(), steady_ms(), exe);
    std::fflush(nullptr);
#ifdef _WIN32
    std::string cmd;
    for (const std::string& a : args) cmd += (cmd.empty() ? "" : " ") + win_quote(a);
    const std::wstring wexe = widen(exe), wcmd = widen(cmd.c_str());
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(wcmd.begin(), wcmd.end());
    line.push_back(L'\0');
    if (!CreateProcessW(wexe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "openpete: return to the launcher failed: CreateProcess error %lu\n", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    std::vector<char*> av;
    for (const std::string& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    const int marked = close_own_fds_on_exec();
    std::fprintf(stderr, "openpete: %d descriptor(s) opened by this image closed across the relaunch\n", marked);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    execv(exe, av.data());
    std::fprintf(stderr, "openpete: return to the launcher failed: execv %s\n", exe);
#endif
}

/** @brief Read OPENPETE_LAUNCHER_RETURN into s_in.
 *
 * @param v The variable's value. */
void parse_incoming(const char* v) {
    char game[32] = {};
    int c = 0, fs = 0, mx = 0, x = 0, y = 0, w = 0, h = 0;
    const int n = std::sscanf(v, "%d:%31[^:]:%d:%d:%d:%d:%d:%d", &c, game, &fs, &mx, &x, &y, &w, &h);
    if (n < 1 || c < 1) return;
    s_in.count = c;
    if (n >= 2) s_in.game = game;
    if (n == 8) {
        s_in.window = true;
        s_in.fullscreen = fs;
        s_in.maximized = mx;
        s_in.x = x;
        s_in.y = y;
        s_in.w = w;
        s_in.h = h;
    }
}

}  // namespace

void openpete_launcher_return_arm(int argc, char** argv) {
    s_argv.assign(argv, argv + argc);
    s_inherited_fds = open_fds();
    s_in.armed_ms = steady_ms();
    if (const char* v = std::getenv(kReturnEnv); v && *v) parse_incoming(v);
#ifdef _WIN32
    SetEnvironmentVariableA(kReturnEnv, nullptr);
    _putenv_s(kReturnEnv, "");
#else
    unsetenv(kReturnEnv);
#endif
    std::atexit(relaunch_at_exit);
}

int openpete_launcher_return_request(void) {
    if (!openpete_launcher_picked_on_page()) {
        OPL_INFO_F("launcher", "%s", "return: this session did not start from the game page: the process exits");
        return 0;
    }
    if (s_quit.load()) {
        OPL_INFO_F("launcher", "%s", "return: a quit arrived before EXIT GAME: the process exits");
        return 0;
    }
    const char* g = openpete_launcher_selected_game();
    std::snprintf(s_game_out, sizeof s_game_out, "%s", g ? g : "");
    s_requested = true;
    s_pending.store(1);
    if (s_quit.load()) s_pending.store(0);   /* a quit between the check and the store */
    OPL_INFO_F("launcher", "return: requested from %s at steady %llu ms: the launcher starts again after the teardown",
               s_game_out[0] ? s_game_out : "an unidentified entry", steady_ms());
    return 1;
}

void openpete_launcher_return_cancel(void) {
    s_quit.store(1);
    s_pending.store(0);
}

void openpete_launcher_return_settle(int rc) {
    if (rc == 0 || !s_pending.load()) return;
    s_pending.store(0);
    OPL_WARN_F("launcher", "return: withdrawn: the session ended with status %d", rc);
}

int openpete_launcher_return_count(void) { return s_in.count; }

const char* openpete_launcher_return_game(void) {
    return s_in.count > 0 && !s_in.game.empty() ? s_in.game.c_str() : nullptr;
}

void openpete_launcher_return_apply(openpete_options_t* neutral) {
    if (s_in.count < 1) return;
    OPL_INFO_F("launcher", "return %d: image entered at steady %llu ms; from %s, the game window %s", s_in.count,
               s_in.armed_ms,
               s_in.game.empty() ? "an unidentified entry" : s_in.game.c_str(),
               !s_in.window ? "not recorded" : s_in.fullscreen ? "fullscreen" : s_in.maximized ? "maximized"
                                                                                                : "windowed");
    if (!s_in.window) return;
    neutral->launcher_fullscreen = s_in.fullscreen ? 1 : 0;
    /* A fullscreen window's size is its display's: the launcher keeps its
     * own windowed size for when the player leaves fullscreen. */
    if (!s_in.fullscreen && s_in.maximized) {
        neutral->launcher_window_maximized = 1;
    } else if (!s_in.fullscreen && s_in.w > 0 && s_in.h > 0) {
        neutral->launcher_window_maximized = 0;
        neutral->launcher_window_w = s_in.w;
        neutral->launcher_window_h = s_in.h;
    }
    openpete_native_launcher_set_position(s_in.x, s_in.y);
}
