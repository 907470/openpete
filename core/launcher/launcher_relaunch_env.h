/* launcher_relaunch_env.h - the environment the return to the launcher
 * hands its relaunched image (launcher_return.cpp).
 *
 * A game session writes the process environment: the config layer's full
 * stage exports every [env] winner, its game and disc scopes included; a
 * flag that stands for a variable sets it; a live setting writes its
 * variable; SDL sets its own fallbacks. A relaunched image that inherited
 * those writes would read them as variables the user exported, which
 * outrank every config file, so a value of one game's scope would reach
 * the next game picked. The relaunch therefore starts from the environment
 * the process was entered with, plus the variable that carries the return.
 * Header-only, so the unit test compiles it without the launcher. */
#ifndef OPENPETE_LAUNCHER_RELAUNCH_ENV_H
#define OPENPETE_LAUNCHER_RELAUNCH_ENV_H

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <SDL3/SDL_stdinc.h>

/* The relaunch's environment and what it reverts of the session's. */
struct RelaunchEnv {
    std::vector<std::string> vars;       /* "NAME=value", sorted by name */
    std::vector<std::string> reverted;   /* names the session set, changed or removed, sorted */
};

/** @brief Return the name of one "NAME=value" entry, folded to upper case when names are case-insensitive.
 *
 * @param kv The entry.
 * @param fold_case Names compare without case (Windows).
 * @return The name; the whole entry when it holds no '='. */
inline std::string relaunch_env_name(const std::string& kv, bool fold_case) {
    std::string n = kv.substr(0, kv.find('='));
    if (fold_case)
        for (char& c : n) c = (char)std::toupper((unsigned char)c);
    return n;
}

/** @brief Return the value of one "NAME=value" entry.
 *
 * @param kv The entry.
 * @return The text after the first '='; empty when it holds none. */
inline std::string relaunch_env_value(const std::string& kv) {
    const size_t eq = kv.find('=');
    return eq == std::string::npos ? std::string() : kv.substr(eq + 1);
}

/** @brief Find the entry of a name in an environment list.
 *
 * @param env The list of "NAME=value" entries.
 * @param name The name, folded as relaunch_env_name folds it.
 * @param fold_case Names compare without case.
 * @return The entry, or NULL when the list does not hold the name. */
inline const std::string* relaunch_env_find(const std::vector<std::string>& env, const std::string& name,
                                            bool fold_case) {
    for (const std::string& kv : env)
        if (relaunch_env_name(kv, fold_case) == name) return &kv;
    return nullptr;
}

/** @brief Compute the relaunched image's environment: the entry snapshot with the carried variable set in it.
 *
 * Every variable the session set, changed or removed after entry is left at
 * its entry state (absent, or the entry's value); its name is listed in
 * `reverted`. The carried variable takes its new value whatever the entry
 * held, and is not listed.
 * @param entry The environment at process entry, "NAME=value" each.
 * @param now The environment at the relaunch.
 * @param carried The "NAME=value" the relaunch carries to the new image.
 * @param fold_case Names compare without case (Windows).
 * @return The environment and the reverted names. */
inline RelaunchEnv relaunch_environment(const std::vector<std::string>& entry, const std::vector<std::string>& now,
                                        const std::string& carried, bool fold_case) {
    RelaunchEnv r;
    const std::string carried_name = relaunch_env_name(carried, fold_case);
    for (const std::string& kv : entry)
        if (relaunch_env_name(kv, fold_case) != carried_name) r.vars.push_back(kv);
    r.vars.push_back(carried);
    std::sort(r.vars.begin(), r.vars.end(), [fold_case](const std::string& a, const std::string& b) {
        return relaunch_env_name(a, fold_case) < relaunch_env_name(b, fold_case);
    });
    auto note = [&](const std::string& kv) {
        const std::string n = relaunch_env_name(kv, fold_case);
        if (n == carried_name) return;
        const std::string* a = relaunch_env_find(r.vars, n, fold_case);
        const std::string* b = relaunch_env_find(now, n, fold_case);
        if ((a == nullptr) != (b == nullptr) || (a && relaunch_env_value(*a) != relaunch_env_value(*b)))
            r.reverted.push_back(n);
    };
    for (const std::string& kv : now) note(kv);
    for (const std::string& kv : r.vars) note(kv);
    std::sort(r.reverted.begin(), r.reverted.end());
    r.reverted.erase(std::unique(r.reverted.begin(), r.reverted.end()), r.reverted.end());
    return r;
}

/** @brief Read the process environment through SDL's populated environment, which reads environ on POSIX and the process block on Windows.
 *
 * Usable before SDL_Init and after SDL_Quit: the copy is SDL's own object,
 * not the global SDL_GetEnvironment, which SDL_Quit destroys.
 * @return The variables, "NAME=value" each, in no particular order; empty
 *         when SDL cannot allocate the copy. */
inline std::vector<std::string> relaunch_process_environment() {
    std::vector<std::string> out;
    SDL_Environment* env = SDL_CreateEnvironment(true);
    if (!env) return out;
    if (char** vars = SDL_GetEnvironmentVariables(env)) {
        for (char** v = vars; *v; v++) out.emplace_back(*v);
        SDL_free(vars);
    }
    SDL_DestroyEnvironment(env);
    return out;
}

#endif
