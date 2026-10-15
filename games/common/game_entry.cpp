// games/common/game_entry.cpp - the process entry of every executable.
//
// Reconnects stdio on Windows, selects the game among the games the
// executable links (game_build.h, or the launcher's pick, game_pick.h),
// installs it, and runs its entry. SDL3's
// SDL_main.h re-#defines main -> SDL_main and supplies the platform entry
// shim; it must be included by the TU defining main().

#include <SDL3/SDL_main.h>

#include "game_build.h"
#include "game_pick.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>

/** @brief Bind one C stdio stream to the OS standard handle it belongs to, unbuffered.
 *
 * A distribution Windows build links the GUI subsystem (-mwindows), so its
 * CRT streams start unconnected; launched from a terminal, behind a
 * `> log` redirect, or through WSL2 interop (which passes inherited pipe
 * handles, not a console), the streams are bound here to whatever the
 * standard handle refers to (console, pipe or file). A stream already wired
 * to that handle (a console-subsystem build, or any CRT that inherited its
 * handles, which is every WSL-interop launch) is left connected:
 * _open_osfhandle would take a second CRT reference to the same HANDLE and
 * _dup2 would close the destination fd first, closing the one handle under
 * both references, after which every write to the stream is dropped.
 * @param which The standard handle (STD_OUTPUT_HANDLE, STD_ERROR_HANDLE,
 *        STD_INPUT_HANDLE).
 * @param stream The C stream to bind (stdout, stderr, stdin). */
static void reconnect_std(DWORD which, FILE* stream) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    int target = _fileno(stream);

    if (target >= 0 && (HANDLE)_get_osfhandle(target) == h) {
        setvbuf(stream, NULL, _IONBF, 0);
        return;
    }

    int fd = _open_osfhandle((intptr_t)h, _O_TEXT);
    if (fd == -1) return;
    if (fd != target) { _dup2(fd, target); _close(fd); }
    setvbuf(stream, NULL, _IONBF, 0);
}

/** @brief Attach the process to its parent console and rebind stdout, stderr and stdin to the OS handles.
 *
 * An AttachConsole failure (the parent has no console, or stdio is a pipe or
 * file) is ignored; each stream is then bound by reconnect_std to whatever
 * its standard handle refers to. */
static void attach_parent_console(void) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    reconnect_std(STD_OUTPUT_HANDLE, stdout);
    reconnect_std(STD_ERROR_HANDLE,  stderr);
    reconnect_std(STD_INPUT_HANDLE,  stdin);
}
#else
/** @brief Do nothing: outside Windows the C stdio streams are already connected. */
static void attach_parent_console(void) {}
#endif

/** @brief Select and install the game this process runs, then run its entry.
 *
 * stdio is reconnected first, before any output and whichever game runs.
 * The game is the one the command line names, else the launcher's pick
 * over every linked game's library entries (game_pick.h); it is installed
 * once.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The game entry's exit status, or 1 when the executable links no game. */
int main(int argc, char** argv) {
    attach_parent_console();
    if (openpete_game_build_count == 0) {
        std::fprintf(stderr, "openpete: this executable links no game\n");
        return 1;
    }
    const openpete_game_build_t* b = openpete_game_build_named(argc, argv);
    if (!b) {
        int rc = 0;
        b = openpete_game_pick(argc, argv, &rc);
        if (!b) return rc;
    }
    openpete_game_build_install(b);
    return b->main(argc, argv);
}
