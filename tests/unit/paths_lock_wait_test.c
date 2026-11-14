/* paths_lock_wait_test - openpete_paths_lock_wait (core/config/paths.c),
 * the lock a first-boot module build takes on `<module dir>/build.lock`
 * and a mod save sidecar write on `<sidecar>.lock`.
 *
 * Another process (a forked child) holds the lock: a waiter calls its
 * waiting callback once, does not take the lock while the child holds it,
 * and takes it (result 1, waited) once the child exits; a third process
 * then cannot take it until the waiter releases it. A waiter whose
 * cancelled callback answers yes returns -1 without the lock. A free lock
 * is taken at once (result 0). A bounded waiter (the mod-save sidecar's
 * use) returns -1 once the bound passes with the lock held, and a zero
 * bound tries once. A lock file that cannot be created returns -2. POSIX
 * only (fork). */

#include "openpete_paths.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int s_fail;

/** @brief Print a check's verdict and count a failure.
 * @param ok The check passed.
 * @param what What was checked. */
static void check(int ok, const char* what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) s_fail = 1;
}

/** @brief Fork a child that takes the lock, reports on a pipe and holds it until killed.
 * @param path The lock file.
 * @return The child's pid, after it holds the lock. */
static pid_t hold_in_child(const char* path) {
    int fd[2];
    if (pipe(fd) != 0) { perror("pipe"); exit(1); }
    const pid_t pid = fork();
    if (pid == 0) {
        intptr_t h = 0;
        const char c = openpete_paths_lock(path, &h) == 0 ? 'y' : 'n';
        if (write(fd[1], &c, 1) != 1) _exit(1);
        for (;;) pause();
    }
    char c = 0;
    if (read(fd[0], &c, 1) != 1 || c != 'y') { fprintf(stderr, "child did not take the lock\n"); exit(1); }
    close(fd[0]);
    close(fd[1]);
    return pid;
}

typedef struct {
    int   waiting_calls;
    int   cancel_polls;     /* cancelled answers yes after this many polls; -1 never */
    int   polls;
    pid_t release_child;    /* killed on the third poll, 0 none */
} wait_state_t;

/** @brief Count the waiting callback's calls. */
static void on_waiting(void* user) { ((wait_state_t*)user)->waiting_calls++; }

/** @brief Count polls, release the holder on the third and answer cancelled after cancel_polls. */
static int on_cancelled(void* user) {
    wait_state_t* w = (wait_state_t*)user;
    w->polls++;
    if (w->release_child && w->polls == 3) {
        kill(w->release_child, SIGKILL);
        waitpid(w->release_child, NULL, 0);
        w->release_child = 0;
    }
    return w->cancel_polls >= 0 && w->polls >= w->cancel_polls;
}

/** @brief Report whether another process can take the lock now.
 * @param path The lock file.
 * @return 1 when a child process took it. */
static int free_for_other_process(const char* path) {
    const pid_t pid = fork();
    if (pid == 0) {
        intptr_t h = 0;
        _exit(openpete_paths_lock(path, &h) == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/** @brief Run the lock checks in a temporary directory.
 * @return 0 when every check passes. */
int main(void) {
    char dir[] = "/tmp/paths_lock_wait_test.XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char path[128];
    snprintf(path, sizeof path, "%s/build.lock", dir);

    intptr_t h = 0;
    check(openpete_paths_lock_wait(path, &h, -1, 10, NULL, NULL, NULL) == 0, "a free lock is taken at once");
    check(!free_for_other_process(path), "another process cannot take a held lock");
    openpete_paths_unlock(h);
    check(free_for_other_process(path), "the lock is free after the release");

    wait_state_t w = {0, -1, 0, hold_in_child(path)};
    const int rc = openpete_paths_lock_wait(path, &h, -1, 10, on_cancelled, on_waiting, &w);
    check(rc == 1, "a waiter takes the lock after the holding process exits (result 1)");
    check(w.waiting_calls == 1, "the waiting callback ran once");
    check(w.polls >= 3, "the waiter polled while the lock was held");
    check(!free_for_other_process(path), "the waiter holds the lock");
    openpete_paths_unlock(h);

    const pid_t holder = hold_in_child(path);
    wait_state_t c = {0, 4, 0, 0};
    check(openpete_paths_lock_wait(path, &h, -1, 10, on_cancelled, on_waiting, &c) == -1,
          "a cancelled waiter returns -1");
    check(c.polls == 4 && c.waiting_calls == 1, "it stopped at the poll that answered cancelled");
    wait_state_t t = {0, -1, 0, 0};
    check(openpete_paths_lock_wait(path, &h, 30, 10, on_cancelled, on_waiting, &t) == -1,
          "a bounded waiter returns -1 when the lock stays held for the whole wait");
    check(t.waiting_calls == 1 && t.polls == 3, "it retried for the bound (three polls of 10 ms in 30 ms)");
    check(openpete_paths_lock_wait(path, &h, 0, 10, on_cancelled, on_waiting, &t) == -1 && t.waiting_calls == 1,
          "a zero bound tries once, without waiting");
    kill(holder, SIGKILL);
    waitpid(holder, NULL, 0);

    char bad[160];
    snprintf(bad, sizeof bad, "%s/missing/build.lock", dir);
    check(openpete_paths_lock_wait(bad, &h, -1, 10, NULL, NULL, NULL) == -2, "an uncreatable lock file returns -2");

    unlink(path);
    rmdir(dir);
    printf("%s\n", s_fail ? "paths_lock_wait_test: FAIL" : "paths_lock_wait_test: PASS");
    return s_fail;
}
