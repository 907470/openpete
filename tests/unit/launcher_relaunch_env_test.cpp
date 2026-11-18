/* tests/unit/launcher_relaunch_env_test.cpp - the environment the return to
 * the launcher hands its relaunched image (launcher_relaunch_env.h), without
 * a window.
 *
 * relaunch_environment over constructed lists: a variable the session set,
 * changed or removed is left at its entry state and named as reverted; the
 * carried variable takes its new value; Windows names fold case. Then two
 * sessions through the real config layer: the entry snapshot is taken with
 * relaunch_process_environment, game A's full stage exports its global and
 * [game.spyro-1.env] values into the process, and the relaunch environment
 * holds neither. The process environment is then replaced by that
 * environment (as execve gives it) and game B's full stage resolves its own
 * [game.spyro-2.env] value, and leaves a variable only A's scope sets
 * unset; the inherited environment, the control, shows A's value outranking
 * B's. On POSIX the computed list is handed to execve of /bin/sh, which
 * prints what the new image sees. */
#include "launcher_relaunch_env.h"
#include "openpete_config.h"
#include "openpete_keymap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

int g_bad = 0;

/** @brief Count a failed check and print it.
 *
 * @param ok The check's result.
 * @param what What was checked. */
void check(bool ok, const std::string& what) {
    if (ok) return;
    std::printf("FAIL %s\n", what.c_str());
    g_bad++;
}

/** @brief Join a list with spaces, for the failure messages.
 *
 * @param v The list.
 * @return The joined text. */
std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& e : v) s += (s.empty() ? "" : " ") + e;
    return s;
}

/** @brief Check relaunch_environment over constructed entry and session lists, case-sensitive and case-folding. */
void case_lists() {
    const std::vector<std::string> entry = {"HOME=/h", "KEEP=1", "CHANGED=old", "REMOVED=r",
                                            "OPENPETE_LAUNCHER_RETURN=1:spyro-1"};
    const std::vector<std::string> now = {"HOME=/h", "KEEP=1", "CHANGED=new", "ADDED=a",
                                          "OPENPETE_LAUNCHER_RETURN=stale"};
    const RelaunchEnv r = relaunch_environment(entry, now, "OPENPETE_LAUNCHER_RETURN=2:spyro-2", false);
    const std::vector<std::string> want_vars = {"CHANGED=old", "HOME=/h", "KEEP=1",
                                                "OPENPETE_LAUNCHER_RETURN=2:spyro-2", "REMOVED=r"};
    const std::vector<std::string> want_rev = {"ADDED", "CHANGED", "REMOVED"};
    check(r.vars == want_vars, "lists: vars are the entry's with the carried value: got " + join(r.vars));
    check(r.reverted == want_rev, "lists: reverted names: got " + join(r.reverted));

    const RelaunchEnv w = relaunch_environment({"Path=C:\\a", "Temp=t"}, {"PATH=C:\\a", "TEMP=u"},
                                               "OPENPETE_LAUNCHER_RETURN=1", true);
    check(w.reverted == std::vector<std::string>{"TEMP"},
          "fold: a name differing only in case is one variable: got " + join(w.reverted));
    check(relaunch_env_find(w.vars, "TEMP", true) && *relaunch_env_find(w.vars, "TEMP", true) == "Temp=t",
          "fold: the entry's spelling and value are kept");
}

#ifndef _WIN32
char g_toml[512];

/** @brief Key-name lookup for the config layer's binding store: each name parses to its own code, so the built-in bindings resolve without SDL's key tables.
 *
 * @param kind The source kind, unused.
 * @param name The token's name.
 * @return A code from the name's FNV-1a hash, distinct for the built-in names. */
int any_name_to_code(int kind, const char* name) {
    (void)kind;
    unsigned h = 2166136261u;
    for (const char* c = name; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    return (int)(h & 0xFFFFFu) + 1;
}

/** @brief Run the config layer's full stage for one game over a config file body, as a game session does after identification.
 *
 * @param game The game id.
 * @param body The config file's text.
 * @return The stage's status. */
int full_stage(const char* game, const char* body) {
    openpete_config_reset();
    FILE* f = std::fopen(g_toml, "wb");
    if (!f) return -1;
    std::fputs(body, f);
    std::fclose(f);
    char* argv[] = {(char*)"openpete", (char*)"--config", g_toml, nullptr};
    openpete_config_set_game_id(game);
    int oc = 0;
    char** ov = nullptr;
    return openpete_config_apply(3, argv, 3, &oc, &ov);
}

/** @brief Replace the process environment with a list, as execve would give it to a new image.
 *
 * @param vars The list, "NAME=value" each. */
void set_process_environment(const std::vector<std::string>& vars) {
    clearenv();
    for (const std::string& kv : vars) putenv(strdup(kv.c_str()));
}

/** @brief Return a variable's value in the process, or "(unset)".
 *
 * @param name The variable.
 * @return The value. */
std::string env_of(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "(unset)";
}

/* The config file both sessions read: a global [env] value, one game A's
 * scope alone sets, and one both games' scopes set differently. */
constexpr const char* kBody =
    "[env]\nOPENPETE_NOTEX = \"1\"\n"
    "[game.spyro-1.env]\nOPENPETE_TEXPACK_DIR = \"packs-a\"\nOPENPETE_MODS = \"mods-a\"\n"
    "[game.spyro-2.env]\nOPENPETE_MODS = \"mods-b\"\n";

/** @brief Check two sessions through the config layer: A's exports are not in the relaunch environment, and B resolves its own values in it. */
void case_two_sessions() {
    for (const char* n : {"OPENPETE_NOTEX", "OPENPETE_TEXPACK_DIR", "OPENPETE_MODS", "OPENPETE_LAUNCHER_RETURN"})
        unsetenv(n);
    const std::vector<std::string> entry = relaunch_process_environment();
    check(!entry.empty(), "two sessions: the entry snapshot holds the process environment");

    check(full_stage("spyro-1", kBody) >= 0, "session A: the full stage resolves");
    check(env_of("OPENPETE_TEXPACK_DIR") == "packs-a" && env_of("OPENPETE_MODS") == "mods-a" &&
              env_of("OPENPETE_NOTEX") == "1",
          "session A: the full stage exports A's scope and the global [env] into the process");
    const std::vector<std::string> after_a = relaunch_process_environment();

    const RelaunchEnv r = relaunch_environment(entry, after_a, "OPENPETE_LAUNCHER_RETURN=1:spyro-1", false);
    check(r.reverted == std::vector<std::string>{"OPENPETE_MODS", "OPENPETE_NOTEX", "OPENPETE_TEXPACK_DIR"},
          "relaunch: the reverted names are A's exports: got " + join(r.reverted));
    for (const char* n : {"OPENPETE_NOTEX", "OPENPETE_TEXPACK_DIR", "OPENPETE_MODS"})
        check(!relaunch_env_find(r.vars, n, false), std::string("relaunch: ") + n + " is not handed to the new image");
    check(r.vars.size() == entry.size() + 1, "relaunch: the entry's variables and the carried one");

    /* Control: the session environment inherited whole, as before the fix. */
    set_process_environment(after_a);
    check(full_stage("spyro-2", kBody) >= 0, "control: session B resolves");
    const bool control_leaks = env_of("OPENPETE_MODS") == "mods-a" && env_of("OPENPETE_TEXPACK_DIR") == "packs-a";
    check(control_leaks, "control: the inherited environment carries A's scope into B (mods " +
                             env_of("OPENPETE_MODS") + ", texpack " + env_of("OPENPETE_TEXPACK_DIR") + ")");

    set_process_environment(r.vars);
    check(env_of("OPENPETE_LAUNCHER_RETURN") == "1:spyro-1", "session B: the carried variable arrives");
    check(full_stage("spyro-2", kBody) >= 0, "session B resolves");
    check(env_of("OPENPETE_MODS") == "mods-b", "session B: its own scope's value wins, got " + env_of("OPENPETE_MODS"));
    check(env_of("OPENPETE_TEXPACK_DIR") == "(unset)",
          "session B: a variable only A's scope sets stays unset, got " + env_of("OPENPETE_TEXPACK_DIR"));
    check(env_of("OPENPETE_NOTEX") == "1", "session B: the global [env] value still applies");

    /* What execve hands the new image, read back by a child shell. */
    std::vector<char*> ev;
    for (const std::string& kv : r.vars) ev.push_back(const_cast<char*>(kv.c_str()));
    ev.push_back(nullptr);
    int fds[2];
    if (pipe(fds) != 0) {
        check(false, "exec: pipe");
        return;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        char* av[] = {(char*)"sh", (char*)"-c",
                      (char*)"printf '%s|%s|%s' \"${OPENPETE_MODS-unset}\" \"${OPENPETE_TEXPACK_DIR-unset}\" "
                             "\"${OPENPETE_LAUNCHER_RETURN-unset}\"",
                      nullptr};
        execve("/bin/sh", av, ev.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[256] = {};
    size_t got = 0;
    for (ssize_t n; (n = read(fds[0], buf + got, sizeof buf - 1 - got)) > 0;) got += (size_t)n;
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    check(std::string(buf) == "unset|unset|1:spyro-1", std::string("exec: the new image sees ") + buf);
}
#endif

}  // namespace

/** @brief Run the cases and report.
 *
 * @return 0 when every check passed, 1 otherwise. */
int main() {
    case_lists();
#ifndef _WIN32
    const char* tmp = std::getenv("TMPDIR");
    std::snprintf(g_toml, sizeof g_toml, "%s/openpete-relaunch-env-%d.toml", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    openpete_keymap_set_lookup(any_name_to_code, nullptr, nullptr);
    case_two_sessions();
    std::remove(g_toml);
#endif
    std::printf("launcher_relaunch_env_test: %s (%d failure(s))\n", g_bad ? "FAIL" : "PASS", g_bad);
    return g_bad ? 1 : 0;
}
