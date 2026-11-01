// core/host/host_spawn.c - see host_spawn.h.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* pipe2 */
#endif
#include "host_spawn.h"
#include "openpete_log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The predicate the stoppable form polls (openpete_host_spawn_set_stop). */
static int (*s_stop)(void);

/* The stoppable form's poll period while a child runs, in ms. */
enum { STOP_POLL_MS = 2 };

/* The mode bits spawn_argv and spawn_line take: poll the stop predicate;
 * run below normal priority (OPENPETE_HOST_SPAWN_BELOW_NORMAL). */
enum { SPAWN_STOPPABLE = 1, SPAWN_BELOW_NORMAL = 2 };
/* The niceness a below-normal child runs at above the caller's (POSIX). */
enum { BELOW_NORMAL_NICE = 10 };

/* Set by the first below-normal spawn's priority log line. */
static atomic_flag s_priority_logged = ATOMIC_FLAG_INIT;

void openpete_host_spawn_set_stop(int (*stop)(void)) { s_stop = stop; }

/** @brief Log, once per process, the priority the first below-normal child was read back at.
 *
 * @param what The readback's name (Windows priority class, POSIX niceness).
 * @param value The value read back from the running child. */
static void log_priority_once(const char* what, long value) {
    if (atomic_flag_test_and_set(&s_priority_logged)) return;
    OPL_INFO_F("host-spawn", "below-normal child: %s %#lx (%ld)", what, (unsigned long)value, value);
}

/** @brief Report whether the registered stop predicate asks the calling thread's spawn to stop.
 *
 * @param stoppable 0 for the plain form, which never stops.
 * @return Non-zero to stop. */
static int stop_requested(int stoppable) {
    return stoppable && s_stop && s_stop();
}

/** @brief Append src to the capture buffer, discarding what does not fit before the final NUL.
 *
 * @param capture The buffer.
 * @param cap Its size.
 * @param len The bytes already held; advanced by what was kept.
 * @param src New output.
 * @param n Its length. */
static void capture_append(char* capture, size_t cap, size_t* len, const char* src, size_t n) {
    size_t room = (*len + 1 < cap) ? cap - 1 - *len : 0;
    size_t take = n < room ? n : room;
    memcpy(capture + *len, src, take);
    *len += take;
    capture[*len] = '\0';
}

#ifdef _WIN32

#include <windows.h>

/** @brief Append one argument to a wide command line, quoted so CommandLineToArgvW returns it unchanged.
 *
 * The argument is wrapped in double quotes; a run of backslashes is
 * doubled when a quote or the closing quote follows it, and an embedded
 * quote is written as \".
 * @param out The command line.
 * @param cap Its capacity in wide characters.
 * @param len Characters already in out; advanced.
 * @param arg The argument, wide.
 * @return 0, or -1 when the line would not fit. */
static int append_quoted(wchar_t* out, size_t cap, size_t* len, const wchar_t* arg) {
    size_t n = *len;
#define PUT(c) do { if (n + 1 >= cap) return -1; out[n++] = (c); } while (0)
    if (n) PUT(L' ');
    PUT(L'"');
    for (const wchar_t* p = arg;; p++) {
        size_t bs = 0;
        while (*p == L'\\') { bs++; p++; }
        if (!*p) {                      /* backslashes before the closing quote */
            for (size_t i = 0; i < bs * 2; i++) PUT(L'\\');
            break;
        }
        if (*p == L'"') {               /* backslashes before an embedded quote */
            for (size_t i = 0; i < bs * 2 + 1; i++) PUT(L'\\');
        } else {
            for (size_t i = 0; i < bs; i++) PUT(L'\\');
        }
        PUT(*p);
    }
    PUT(L'"');
#undef PUT
    out[n] = 0;
    *len = n;
    return 0;
}

/** @brief Start the command line with no console window, route its stdout and stderr to the log file or the capture buffer, and wait for it.
 *
 * The stoppable form starts the child suspended, assigns it to a job object
 * that kills its processes when closed, resumes it, and waits in
 * STOP_POLL_MS steps, reading the capture pipe only when it holds bytes; on
 * a stop it terminates the job and waits for the child.
 * @param line The quoted wide command line; CreateProcessW may modify it.
 * @param log_path Log file for stdout and stderr, or NULL.
 * @param capture Capture buffer when log_path is NULL, or NULL.
 * @param cap Size of capture.
 * @param stoppable SPAWN_* mode bits; non-zero polls the stop predicate.
 * @return The exit code, -1 when the process could not be started, or
 *         OPENPETE_HOST_SPAWN_STOPPED. */
static int spawn_line(wchar_t* line, const char* log_path, char* capture, size_t cap, int stoppable);

/** @brief Build the quoted command line from argv and run it (spawn_line).
 *
 * @param argv The argument vector.
 * @param log_path Log file, or NULL.
 * @param capture Capture buffer, or NULL.
 * @param cap Its size.
 * @param stoppable SPAWN_* mode bits; non-zero polls the stop predicate.
 * @return As spawn_line, or -1 when the line cannot be built. */
static int spawn_argv(const char* const* argv, const char* log_path, char* capture, size_t cap,
                      int stoppable) {
    enum { LINE_MAX_W = 32767 };   /* CreateProcessW's command-line limit */
    wchar_t* line = (wchar_t*)malloc(sizeof(wchar_t) * (LINE_MAX_W + 1));
    if (!line) return -1;
    size_t len = 0;
    line[0] = 0;
    for (int i = 0; argv[i]; i++) {
        wchar_t w[4096];
        if (!MultiByteToWideChar(CP_ACP, 0, argv[i], -1, w, (int)(sizeof w / sizeof w[0])) ||
            append_quoted(line, LINE_MAX_W + 1, &len, w) != 0) {
            free(line);
            return -1;
        }
    }
    int code = spawn_line(line, log_path, capture, cap, stoppable);
    free(line);
    return code;
}

/** @brief Read what the capture pipe holds without blocking.
 *
 * @param rd The pipe's read end.
 * @param capture The buffer.
 * @param cap Its size.
 * @param got Bytes held; advanced. */
static void drain_available(HANDLE rd, char* capture, size_t cap, size_t* got) {
    DWORD avail = 0;
    char b[4096];
    while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        DWORD n = 0;
        if (!ReadFile(rd, b, avail < sizeof b ? avail : (DWORD)sizeof b, &n, NULL) || n == 0) break;
        capture_append(capture, cap, got, b, n);
    }
}

static int spawn_line(wchar_t* line, const char* log_path, char* capture, size_t cap, int stoppable) {
    if (capture && cap) capture[0] = '\0';
    if (stop_requested(stoppable)) return OPENPETE_HOST_SPAWN_STOPPED;

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (log_path) {
        wchar_t wlog[4096];
        if (!MultiByteToWideChar(CP_ACP, 0, log_path, -1, wlog, (int)(sizeof wlog / sizeof wlog[0])))
            return -1;
        wr = CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
        if (wr == INVALID_HANDLE_VALUE) return -1;
    } else if (capture) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
        /* The read end stays with us - keep it out of the child. */
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }

    /* The stoppable form's job: closing or terminating it ends the child
     * and every process the child started. */
    HANDLE job = NULL;
    if (stoppable) {
        job = CreateJobObjectW(NULL, NULL);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
            memset(&li, 0, sizeof li);
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        }
    }

    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (wr) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError  = wr;
    }
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    const DWORD flags = CREATE_NO_WINDOW | (job ? CREATE_SUSPENDED : 0) |
                        ((stoppable & SPAWN_BELOW_NORMAL) ? BELOW_NORMAL_PRIORITY_CLASS : 0);
    BOOL ok = CreateProcessW(NULL, line, NULL, NULL, wr ? TRUE : FALSE, flags, NULL, NULL, &si, &pi);
    if (wr) CloseHandle(wr);   /* the child owns its copy now */
    if (!ok) {
        if (rd) CloseHandle(rd);
        if (job) CloseHandle(job);
        return -1;
    }
    if (stoppable & SPAWN_BELOW_NORMAL)
        log_priority_once("priority class", (long)GetPriorityClass(pi.hProcess));
    if (job) {
        /* Assigned before it runs, so every process it starts is in the job.
         * Without the job (assignment refused) a stop terminates the child
         * alone. */
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = NULL;
        }
        ResumeThread(pi.hThread);
    }
    size_t got = 0;
    int stopped = 0;
    if (!stoppable) {
        if (rd) {
            char b[4096];
            DWORD n = 0;
            while (ReadFile(rd, b, sizeof b, &n, NULL) && n > 0)
                capture_append(capture, cap, &got, b, n);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else {
        while (WaitForSingleObject(pi.hProcess, STOP_POLL_MS) == WAIT_TIMEOUT) {
            if (rd) drain_available(rd, capture, cap, &got);
            if (stop_requested(stoppable)) {
                stopped = 1;
                if (job) TerminateJobObject(job, 1);
                else TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, INFINITE);
                break;
            }
        }
        if (rd && !stopped) {
            char b[4096];
            DWORD n = 0;
            while (ReadFile(rd, b, sizeof b, &n, NULL) && n > 0)
                capture_append(capture, cap, &got, b, n);
        }
    }
    if (rd) CloseHandle(rd);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return stopped ? OPENPETE_HOST_SPAWN_STOPPED : (int)code;
}

int openpete_host_spawn_argv(const char* const* argv, const char* log_path,
                             char* capture, size_t cap) {
    return spawn_argv(argv, log_path, capture, cap, 0);
}

int openpete_host_spawn_argv_stoppable(const char* const* argv, const char* log_path,
                                       char* capture, size_t cap) {
    return spawn_argv(argv, log_path, capture, cap, SPAWN_STOPPABLE);
}

int openpete_host_spawn_argv_stoppable_flags(const char* const* argv, const char* log_path,
                                             char* capture, size_t cap, unsigned flags) {
    return spawn_argv(argv, log_path, capture, cap,
                      SPAWN_STOPPABLE | ((flags & OPENPETE_HOST_SPAWN_BELOW_NORMAL) ? SPAWN_BELOW_NORMAL : 0));
}

#else /* !_WIN32 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

/** @brief Read the capture pipe until it closes, polling the stop predicate every STOP_POLL_MS.
 *
 * @param fd The pipe's read end.
 * @param capture The buffer.
 * @param cap Its size.
 * @param stoppable Non-zero to poll the stop predicate.
 * @return 1 when a stop ended the read, else 0. */
static int read_capture(int fd, char* capture, size_t cap, int stoppable) {
    size_t got = 0;
    char b[4096];
    for (;;) {
        if (stoppable) {
            struct pollfd p = { fd, POLLIN, 0 };
            const int r = poll(&p, 1, STOP_POLL_MS);
            if (r == 0 || (r < 0 && errno == EINTR)) {
                if (stop_requested(stoppable)) return 1;
                continue;
            }
        }
        const ssize_t n = read(fd, b, sizeof b);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        capture_append(capture, cap, &got, b, (size_t)n);
    }
}

/* How long a stopped group has to exit on SIGTERM before SIGKILL, in ms. */
enum { STOP_GRACE_MS = 500 };

/** @brief End a stopped child's process group and reap the child.
 *
 * SIGTERM first: the compiler driver, cc1 and the linker remove their
 * temporary and partial output files on it (LLVM's remove-on-signal
 * handlers). A group whose leader has not exited STOP_GRACE_MS later is
 * sent SIGKILL.
 * @param pid The child, leader of its own process group. */
static void kill_group(pid_t pid) {
    const struct timespec step = { 0, STOP_POLL_MS * 1000000L };
    int st = 0;
    kill(-pid, SIGTERM);
    for (int waited = 0; waited < STOP_GRACE_MS; waited += STOP_POLL_MS) {
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return;
        nanosleep(&step, NULL);
    }
    kill(-pid, SIGKILL);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}

/** @brief Wait for the child, polling the stop predicate every STOP_POLL_MS in the stoppable form, and kill its process group on a stop.
 *
 * @param pid The child, leader of its own group in the stoppable form.
 * @param stoppable Non-zero to poll the stop predicate.
 * @param stop Non-zero when a stop was already seen: the group is ended at once (kill_group).
 * @return The exit code, -1 when it did not exit normally, or
 *         OPENPETE_HOST_SPAWN_STOPPED. */
static int wait_child(pid_t pid, int stoppable, int stop) {
    int st = 0;
    if (stoppable) {
        const struct timespec step = { 0, STOP_POLL_MS * 1000000L };
        for (;;) {
            if (stop || stop_requested(stoppable)) {
                kill_group(pid);
                return OPENPETE_HOST_SPAWN_STOPPED;
            }
            const pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) break;
            if (r < 0 && errno != EINTR) return -1;
            nanosleep(&step, NULL);
        }
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/** @brief Lower the process group a stoppable child leads to BELOW_NORMAL_NICE above the caller's niceness, clamped to 19; a refusal leaves it as it is.
 *
 * @param pgid The child, leader of its own group. */
static void lower_group(pid_t pgid) {
    errno = 0;
    const int base = getpriority(PRIO_PROCESS, 0);
    if (errno) return;
    const int level = base + BELOW_NORMAL_NICE > 19 ? 19 : base + BELOW_NORMAL_NICE;
    (void)setpriority(PRIO_PGRP, (id_t)pgid, level);
    errno = 0;
    const int now = getpriority(PRIO_PGRP, (id_t)pgid);
    if (!errno) log_priority_once("group niceness", now);
}

/** @brief Spawn argv with stdout and stderr to the log file or the capture pipe and wait for it; the stoppable form makes the child a process-group leader and kills the group on a stop.
 *
 * With SPAWN_BELOW_NORMAL the child's group is lowered by BELOW_NORMAL_NICE
 * right after the spawn (lower_group).
 * @param argv The argument vector.
 * @param log_path Log file, or NULL.
 * @param capture Capture buffer, or NULL.
 * @param cap Its size.
 * @param stoppable SPAWN_* mode bits; non-zero polls the stop predicate.
 * @return The exit code, -1, or OPENPETE_HOST_SPAWN_STOPPED. */
static int spawn_argv(const char* const* argv, const char* log_path, char* capture, size_t cap,
                      int stoppable) {
    if (capture && cap) capture[0] = '\0';
    if (stop_requested(stoppable)) return OPENPETE_HOST_SPAWN_STOPPED;
    int fd = -1, pipefd[2] = { -1, -1 };
    if (log_path) {
        fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return -1;
    } else if (capture) {
        /* Both ends close-on-exec, so a child another thread spawns
         * meanwhile holds neither end; the dup2 below gives this child its
         * own inheritable copy. */
#ifdef __linux__
        if (pipe2(pipefd, O_CLOEXEC) != 0) return -1;
#else
        if (pipe(pipefd) != 0) return -1;
        (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
#endif
        fd = pipefd[1];
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (fd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, fd, STDERR_FILENO);
        if (pipefd[0] >= 0) posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    }
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    if (stoppable) {
        /* Its own group (pgid = its pid), so a stop ends the driver and
         * every process it started with one signal. The child starts with
         * no signal blocked and SIGTERM at its default action: a spawning
         * thread that blocks SIGTERM would otherwise pass that mask on, and
         * the compiler would not see the stop's SIGTERM. */
        sigset_t none, term;
        sigemptyset(&none);
        sigemptyset(&term);
        sigaddset(&term, SIGTERM);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        posix_spawnattr_setpgroup(&at, 0);
        posix_spawnattr_setsigmask(&at, &none);
        posix_spawnattr_setsigdefault(&at, &term);
    }
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, &at, (char* const*)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (fd >= 0) close(fd);
    if (rc != 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        return -1;
    }
    if (stoppable & SPAWN_BELOW_NORMAL) lower_group(pid);
    int stop = 0;
    if (pipefd[0] >= 0) {
        stop = read_capture(pipefd[0], capture, cap, stoppable);
        close(pipefd[0]);
    }
    return wait_child(pid, stoppable, stop);
}

int openpete_host_spawn_argv(const char* const* argv, const char* log_path,
                             char* capture, size_t cap) {
    return spawn_argv(argv, log_path, capture, cap, 0);
}

int openpete_host_spawn_argv_stoppable(const char* const* argv, const char* log_path,
                                       char* capture, size_t cap) {
    return spawn_argv(argv, log_path, capture, cap, SPAWN_STOPPABLE);
}

int openpete_host_spawn_argv_stoppable_flags(const char* const* argv, const char* log_path,
                                             char* capture, size_t cap, unsigned flags) {
    return spawn_argv(argv, log_path, capture, cap,
                      SPAWN_STOPPABLE | ((flags & OPENPETE_HOST_SPAWN_BELOW_NORMAL) ? SPAWN_BELOW_NORMAL : 0));
}

#endif /* !_WIN32 */
