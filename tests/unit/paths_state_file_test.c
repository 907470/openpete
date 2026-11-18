/* paths_state_file_test - openpete_paths_state_file (core/config/paths.c),
 * the location of a file kept directly in the state directory (the dev
 * overlay's openpete-devui.ini).
 *
 * In each of the three layouts the file sits in the directory that holds
 * openpete_paths_state_dir's leaves: under a sandbox root
 * <root>/state/openpete/<name>, in the per-user layout
 * $XDG_STATE_HOME/openpete/<name>, in the portable layout
 * <exe dir>/<name>. With create the directory exists afterwards and the
 * file does not. POSIX only (mkdtemp, setenv). */

#include "openpete_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_fail;

/** @brief Print a check's verdict and count a failure.
 * @param ok The check passed.
 * @param what What was checked. */
static void check(int ok, const char* what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) s_fail = 1;
}

/** @brief Return 1 when p names an existing directory. */
static int is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/** @brief Return 1 when p exists in any form. */
static int exists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/** @brief Check one layout: the file path, its directory made, the file absent, and the cards leaf beside it.
 * @param want_dir The directory the file must be in.
 * @param what The layout's name for the verdict lines. */
static void check_layout(const char* want_dir, const char* what) {
    char got[4096], want[4096 + 64], cards[4096], want_cards[4096 + 64], line[4096 + 128];
    int rc = openpete_paths_state_file("openpete-devui.ini", got, sizeof got, 1);
    snprintf(want, sizeof want, "%s/openpete-devui.ini", want_dir);
    snprintf(line, sizeof line, "%s: resolves (%s)", what, rc == 0 ? got : "-1");
    check(rc == 0 && strcmp(got, want) == 0, line);
    snprintf(line, sizeof line, "%s: directory made, file not", what);
    check(is_dir(want_dir) && !exists(want), line);
    rc = openpete_paths_state_dir("cards", cards, sizeof cards, 0);
    snprintf(want_cards, sizeof want_cards, "%s/cards", want_dir);
    snprintf(line, sizeof line, "%s: beside the cards leaf (%s)", what, rc == 0 ? cards : "-1");
    check(rc == 0 && strcmp(cards, want_cards) == 0, line);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    char tmpl[] = "/tmp/openpete-paths-state-file-XXXXXX";
    char* made = mkdtemp(tmpl);
    if (!made) {
        printf("FAIL: mkdtemp\n");
        return 1;
    }
    char root[4096];
    if (!realpath(made, root)) {
        printf("FAIL: realpath\n");
        return 1;
    }
    unsetenv("OPENPETE_SANDBOX");

    char want[4096 + 64];
    if (openpete_paths_set_sandbox(root) != 0) check(0, "sandbox root accepted");
    snprintf(want, sizeof want, "%s/state/openpete", root);
    check_layout(want, "sandbox");

    openpete_paths_set_sandbox(NULL);
    char xdg[4096 + 16];
    snprintf(xdg, sizeof xdg, "%s/xdg-state", root);
    setenv("XDG_STATE_HOME", xdg, 1);
    openpete_paths_set_portable(0);
    snprintf(want, sizeof want, "%s/openpete", xdg);
    check_layout(want, "per-user");

    char exe[4096];
    if (openpete_exe_dir(exe, sizeof exe) != 0) {
        check(0, "portable: exe dir resolves");
    } else {
        openpete_paths_set_portable(1);
        if (openpete_paths_portable()) {
            size_t n = strlen(exe);
            if (n > 1 && exe[n - 1] == '/') exe[n - 1] = '\0';
            check_layout(exe, "portable");
        } else {
            printf("SKIP: portable: the test's directory is not writable\n");
        }
    }

    char cmd[4096 + 32];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) printf("note: could not remove %s\n", root);
    printf("%s\n", s_fail ? "paths_state_file_test: FAIL" : "paths_state_file_test: PASS");
    return s_fail;
}
