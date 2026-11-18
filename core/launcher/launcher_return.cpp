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
//
// The relaunched image is given the environment the process was entered
// with (launcher_relaunch_env.h), not the session's: the config layer's
// full stage, flags that stand for a variable and live settings write the
// process environment, and an inherited value reads as one the user
// exported, which outranks every config file.
#include "launcher_relaunch_env.h"
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
#include <utility>
#include <vector>

#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>

#ifndef _WIN32
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

/* 1 between a return request and the relaunch; cleared by a cancel. */
std::atomic<int> s_pending{0};
/* 1 once a quit (window close, quit signal, quit key) arrived: it ends the
 * process whichever of it and EXIT GAME the process saw first. */
std::atomic<int> s_quit{0};
/* 1 once a return was requested, so the exit can say a cancel withdrew it:
 * stored by the request on the host loop's thread, loaded by the atexit
 * handler on the thread that calls exit. */
std::atomic<int> s_requested{0};

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

/* What OPENPETE_LAUNCHER_RETURN carried into this image. */
struct Incoming {
    unsigned long long armed_ms = 0;   /* steady_ms at entry */
    int count = 0;
    std::string game;
    int fullscreen = 0, maximized = 0, x = 0, y = 0, w = 0, h = 0;
    bool window = false;
};

/** @brief Read OPENPETE_LAUNCHER_RETURN's value.
 *
 * @param v The variable's value.
 * @param in Receives the fields; left as it is when the value names no return. */
void parse_incoming(const char* v, Incoming& in) {
    char game[32] = {};
    int c = 0, fs = 0, mx = 0, x = 0, y = 0, w = 0, h = 0;
    const int n = std::sscanf(v, "%d:%31[^:]:%d:%d:%d:%d:%d:%d", &c, game, &fs, &mx, &x, &y, &w, &h);
    if (n < 1 || c < 1) return;
    in.count = c;
    if (n >= 2) in.game = game;
    if (n == 8) {
        in.window = true;
        in.fullscreen = fs;
        in.maximized = mx;
        in.x = x;
        in.y = y;
        in.w = w;
        in.h = h;
    }
}

/* The process's state at entry, which the relaunch replays. */
struct EntryState {
    std::vector<std::string> argv;   /* the argv the process started with */
    /* The descriptors open at entry: inherited from the parent (stdio, a
     * launcher's keep-alive pipe), so the relaunched image inherits them too.
     * Every other descriptor is closed across the relaunch. */
    std::vector<int> inherited_fds;
    /* The process environment at entry without OPENPETE_LAUNCHER_RETURN,
     * "NAME=value" each: the relaunch starts from it (relaunch_environment). */
    std::vector<std::string> env;
    Incoming in;
};

/** @brief Capture the entry state: argv, the open descriptors, OPENPETE_LAUNCHER_RETURN (read, then removed from the environment) and the environment left.
 *
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The state. */
EntryState capture_entry(int argc, char** argv) {
    EntryState e;
    if (argv) e.argv.assign(argv, argv + argc);
    e.inherited_fds = open_fds();
    e.in.armed_ms = steady_ms();
    if (const char* v = std::getenv(kReturnEnv); v && *v) parse_incoming(v, e.in);
#ifdef _WIN32
    _putenv_s(kReturnEnv, "");   /* an empty value removes the variable */
#else
    unsetenv(kReturnEnv);
#endif
    e.env = relaunch_process_environment();
    return e;
}

/** @brief Return the entry state, captured by the first call and immutable after it.
 *
 * The first call is openpete_launcher_return_arm's, first in main, before
 * any other thread exists; the arguments of every later call are ignored.
 * The function-local static's initialization is the set-once publication:
 * every later reader, on any thread, sees the captured state. The state is
 * constructed before the arm registers relaunch_at_exit, so it is destroyed
 * after that handler runs.
 * @param argc The argument count, read by the first call only.
 * @param argv The arguments, read by the first call only.
 * @return The state. */
const EntryState& entry_state(int argc = 0, char** argv = nullptr) {
    static const EntryState e = capture_entry(argc, argv);
    return e;
}

/** @brief Compute the relaunched image's environment from the entry snapshot and log every variable it reverts.
 *
 * @param carried The OPENPETE_LAUNCHER_RETURN entry, "NAME=value".
 * @return The environment, "NAME=value" each. */
std::vector<std::string> relaunch_env(const std::string& carried) {
#ifdef _WIN32
    constexpr bool kFold = true;
#else
    constexpr bool kFold = false;
#endif
    RelaunchEnv r = relaunch_environment(entry_state().env, relaunch_process_environment(), carried, kFold);
    std::string names;
    for (const std::string& n : r.reverted) names += (names.empty() ? "" : " ") + n;
    std::fprintf(stderr, "openpete: return: %zu variable(s) the session set, changed or removed left at their entry state%s%s\n",
                 r.reverted.size(), names.empty() ? "" : ": ", names.c_str());
    return std::move(r.vars);
}

#ifndef _WIN32
/** @brief Mark every descriptor the process opened itself close-on-exec, so the relaunched image starts with only those it inherited at entry.
 *
 * A descriptor a library opened without O_CLOEXEC (a disc image, a wake
 * pipe) would otherwise survive every relaunch.
 * @return The count marked. */
int close_own_fds_on_exec() {
    int n = 0;
    const std::vector<int>& inherited = entry_state().inherited_fds;
    for (int fd : open_fds()) {
        if (std::binary_search(inherited.begin(), inherited.end(), fd)) continue;
        const int fl = fcntl(fd, F_GETFD);
        if (fl == -1 || (fl & FD_CLOEXEC)) continue;
        (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        n++;
    }
    return n;
}
#endif

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
    const std::vector<std::string>& argv = entry_state().argv;
    std::vector<std::string> out;
    for (size_t i = 0; i < argv.size(); i++) {
        const Dropped* d = i ? dropped_row(argv[i].c_str()) : nullptr;
        if (!d) {
            out.push_back(argv[i]);
            continue;
        }
        int skip = std::strchr(argv[i].c_str(), '=') ? 0 : d->values;
        if (skip < 0) skip = (i + 1 < argv.size() && argv[i + 1][0] != '-') ? 1 : 0;
        OPL_INFO_F("launcher", "return: %s left out of the relaunch (%s)", argv[i].c_str(), d->why);
        i += (size_t)skip;
    }
    return out;
}

/** @brief Compose OPENPETE_LAUNCHER_RETURN's value for the relaunched image: the next count, the exited game and the game window's last state.
 *
 * The game is the library entry's, which the launcher records once before
 * the session starts.
 * @return The value. */
std::string return_value() {
    int fs = 0, mx = 0, x = 0, y = 0, w = 0, h = 0;
    const bool win = openpete_native_window_final_state(&fs, &mx, &x, &y, &w, &h) != 0;
    const char* g = openpete_launcher_selected_game();
    if (!g) g = "";
    const int next = entry_state().in.count + 1;
    char v[160];
    if (win) std::snprintf(v, sizeof v, "%d:%s:%d:%d:%d:%d:%d:%d", next, g, fs, mx, x, y, w, h);
    else std::snprintf(v, sizeof v, "%d:%s", next, g);
    return v;
}

#ifdef _WIN32
/** @brief Start the relaunched image as a new process with SDL's process API: the arguments with the executable's path first, the given environment, the working directory inherited, stdout and stderr inherited and stdin closed.
 *
 * SDL builds the command line from the UTF-8 arguments with the quoting
 * CommandLineToArgvW reverses. The process is not waited for; this one
 * exits after it starts.
 * @param args The arguments, the program first.
 * @param vars The whole environment, "NAME=value" each.
 * @return true when the process started. */
bool start_relaunch_process(const std::vector<std::string>& args, const std::vector<std::string>& vars) {
    std::vector<const char*> av;
    for (const std::string& a : args) av.push_back(a.c_str());
    av.push_back(nullptr);
    SDL_Environment* env = SDL_CreateEnvironment(false);
    for (const std::string& kv : vars) {
        const size_t eq = kv.find('=');
        if (env && eq != std::string::npos && eq > 0)
            SDL_SetEnvironmentVariable(env, kv.substr(0, eq).c_str(), kv.c_str() + eq + 1, true);
    }
    const SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, av.data());
    if (env) SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_Process* proc = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (env) SDL_DestroyEnvironment(env);
    if (!proc) {
        std::fprintf(stderr, "openpete: return to the launcher failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_DestroyProcess(proc);   /* closes SDL's handles; the process runs on */
    return true;
}
#endif

/** @brief atexit handler: start the launcher again when a return is pending; registered first, so it runs after every other handler.
 *
 * POSIX replaces the process image (execve of the executable): the process
 * id, the parent's wait and inherited descriptors stay; every other thread
 * ends; the signal mask is cleared first so the new image starts with none
 * blocked. Windows starts a new process (start_relaunch_process) with the
 * same working directory, and this one exits. Both hand the new image the
 * environment of relaunch_env: the entry's, with OPENPETE_LAUNCHER_RETURN
 * set. A failure is logged and the process exits as it would have. */
void relaunch_at_exit() {
    if (!s_pending.load()) {
        if (s_requested.load())
            std::fprintf(stderr, "openpete: return to the launcher withdrawn: the process ends\n");
        return;
    }
    char exe[4096];
    if (openpete_exe_path(exe, sizeof exe) != 0) {
        std::fprintf(stderr, "openpete: return to the launcher failed: the executable's path is unknown\n");
        return;
    }
    const std::vector<std::string> args = relaunch_args();
    const std::string v = return_value();
    const std::vector<std::string> vars = relaunch_env(std::string(kReturnEnv) + "=" + v);
    std::fprintf(stderr, "openpete: returning to the launcher (%s=%s) at steady %llu ms: %s\n", kReturnEnv,
                 v.c_str(), steady_ms(), exe);
    std::fflush(nullptr);
#ifdef _WIN32
    std::vector<std::string> wargs = args;
    wargs[0] = exe;
    (void)start_relaunch_process(wargs, vars);
#else
    std::vector<char*> av;
    for (const std::string& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    std::vector<char*> ev;
    for (const std::string& kv : vars) ev.push_back(const_cast<char*>(kv.c_str()));
    ev.push_back(nullptr);
    const int marked = close_own_fds_on_exec();
    std::fprintf(stderr, "openpete: %d descriptor(s) opened by this image closed across the relaunch\n", marked);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    execve(exe, av.data(), ev.data());
    std::fprintf(stderr, "openpete: return to the launcher failed: execve %s\n", exe);
#endif
}

}  // namespace

void openpete_launcher_return_arm(int argc, char** argv) {
    (void)entry_state(argc, argv);
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
    s_requested.store(1);
    s_pending.store(1);
    if (s_quit.load()) s_pending.store(0);   /* a quit between the check and the store */
    OPL_INFO_F("launcher", "return: requested from %s at steady %llu ms: the launcher starts again after the teardown",
               g && *g ? g : "an unidentified entry", steady_ms());
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

int openpete_launcher_return_count(void) { return entry_state().in.count; }

const char* openpete_launcher_return_game(void) {
    const Incoming& in = entry_state().in;
    return in.count > 0 && !in.game.empty() ? in.game.c_str() : nullptr;
}

void openpete_launcher_return_apply(openpete_options_t* neutral) {
    const Incoming& in = entry_state().in;
    if (in.count < 1) return;
    OPL_INFO_F("launcher", "return %d: image entered at steady %llu ms; from %s, the game window %s", in.count,
               in.armed_ms,
               in.game.empty() ? "an unidentified entry" : in.game.c_str(),
               !in.window ? "not recorded" : in.fullscreen ? "fullscreen" : in.maximized ? "maximized"
                                                                                                : "windowed");
    if (!in.window) return;
    neutral->launcher_fullscreen = in.fullscreen ? 1 : 0;
    /* A fullscreen window carries the windowed geometry it returns to, or
     * size 0x0 at its display's origin when it had none: the launcher then
     * keeps its own windowed size, centred on that display, for when the
     * player leaves fullscreen. */
    if (!in.fullscreen && in.maximized) {
        neutral->launcher_window_maximized = 1;
    } else if (in.w > 0 && in.h > 0) {
        neutral->launcher_window_maximized = 0;
        neutral->launcher_window_w = in.w;
        neutral->launcher_window_h = in.h;
    }
    openpete_native_launcher_set_position(in.x, in.y, in.fullscreen && (in.w <= 0 || in.h <= 0));
}
