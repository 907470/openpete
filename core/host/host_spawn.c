// core/host/host_spawn.c — see host_spawn.h.
#include "host_spawn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * @param line The quoted wide command line; CreateProcessW may modify it.
 * @param log_path Log file for stdout and stderr, or NULL.
 * @param capture Capture buffer when log_path is NULL, or NULL.
 * @param cap Size of capture.
 * @return The exit code, or -1 when the process could not be started. */
static int spawn_line(wchar_t* line, const char* log_path, char* capture, size_t cap);

int openpete_host_spawn_argv(const char* const* argv, const char* log_path,
                             char* capture, size_t cap) {
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
    int code = spawn_line(line, log_path, capture, cap);
    free(line);
    return code;
}

static int spawn_line(wchar_t* line, const char* log_path, char* capture, size_t cap) {
    if (capture && cap) capture[0] = '\0';

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
        /* The read end stays with us — keep it out of the child. */
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
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
    BOOL ok = CreateProcessW(NULL, line, NULL, NULL, wr ? TRUE : FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (wr) CloseHandle(wr);   /* the child owns its copy now */
    if (!ok) {
        if (rd) CloseHandle(rd);
        return -1;
    }
    if (rd) {
        size_t got = 0;
        char b[4096];
        DWORD n = 0;
        while (ReadFile(rd, b, sizeof b, &n, NULL) && n > 0)
            capture_append(capture, cap, &got, b, n);
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

#else /* !_WIN32 */

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

int openpete_host_spawn_argv(const char* const* argv, const char* log_path,
                             char* capture, size_t cap) {
    if (capture && cap) capture[0] = '\0';
    int fd = -1, pipefd[2] = { -1, -1 };
    if (log_path) {
        fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return -1;
    } else if (capture) {
        if (pipe(pipefd) != 0) return -1;
        fd = pipefd[1];
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (fd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, fd, STDERR_FILENO);
        if (pipefd[0] >= 0) posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    }
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (fd >= 0) close(fd);
    if (rc != 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        return -1;
    }
    if (pipefd[0] >= 0) {
        size_t got = 0;
        char b[4096];
        ssize_t n;
        while ((n = read(pipefd[0], b, sizeof b)) != 0) {
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            capture_append(capture, cap, &got, b, (size_t)n);
        }
        close(pipefd[0]);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

#endif /* !_WIN32 */
