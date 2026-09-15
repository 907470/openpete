// core/host/host_spawn.c — see host_spawn.h.
#include "host_spawn.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>

/* Spawn `cmd.exe /C <cmd>` and wait, with NO console window. When
 * `capture` is non-NULL the child's stdout+stderr are collected through
 * a pipe (the _popen replacement). Grandchildren inherit the hidden
 * console, so a whole compiler pipeline runs silently. Mirrors
 * win_run_cmd in games/spyro-1/dist/recomp_build.cpp. */
static int win_spawn(const char* cmd, char* capture, size_t cap) {
    HANDLE rd = NULL, wr = NULL;
    if (capture) {
        SECURITY_ATTRIBUTES sa;
        memset(&sa, 0, sizeof sa);
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
        /* The read end stays with us — keep it out of the child. */
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }
    /* cmd.exe /C strips the outermost quotes when the command contains
     * more than one quoted token — the classic system() trap. Wrapping
     * the whole command in one extra pair of quotes defeats it. */
    char full[8192];
    snprintf(full, sizeof full, "cmd.exe /C \"%s\"", cmd);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (capture) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError  = wr;
    }
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(NULL, full, NULL, NULL,
                             capture ? TRUE : FALSE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);
    if (wr) CloseHandle(wr);   /* the child owns the write end now */
    if (!ok) {
        if (rd) CloseHandle(rd);
        return -1;
    }
    if (capture) {
        size_t len = 0;
        char b[4096];
        DWORD n = 0;
        while (ReadFile(rd, b, sizeof b, &n, NULL) && n > 0) {
            size_t room = (len + 1 < cap) ? cap - 1 - len : 0;
            size_t take = (size_t)n < room ? (size_t)n : room;
            memcpy(capture + len, b, take);
            len += take;
        }
        capture[len] = '\0';
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int openpete_host_spawn(const char* cmd) {
    return win_spawn(cmd, NULL, 0);
}

int openpete_host_spawn_capture(const char* cmd, char* out, size_t cap) {
    if (out && cap) out[0] = '\0';
    return win_spawn(cmd, out, cap);
}

#else /* !_WIN32 */

#include <stdlib.h>

int openpete_host_spawn(const char* cmd) {
    return system(cmd);
}

int openpete_host_spawn_capture(const char* cmd, char* out, size_t cap) {
    if (out && cap) out[0] = '\0';
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    size_t len = 0;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, p)) > 0) {
        size_t room = (len + 1 < cap) ? cap - 1 - len : 0;
        size_t take = n < room ? n : room;
        memcpy(out + len, b, take);
        len += take;
    }
    out[len] = '\0';
    return pclose(p);
}

#endif /* !_WIN32 */
