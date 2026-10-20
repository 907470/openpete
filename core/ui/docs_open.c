/* docs_open.c - open the User documentation in the system browser. See
 * openpete_docs_open.h. */

#include "openpete_docs_open.h"
#include "openpete_paths.h"
#include "openpete_log.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

/** @brief Report whether a regular file can be opened for reading.
 *
 * @param p The path.
 * @return 1 when fopen succeeds, else 0. */
static int readable(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int openpete_docs_target(char* out, size_t cap) {
    static const char* const k_rel[] = { "docs/index.html", "docs-site/index.html" };
    const char* appimage = getenv("APPIMAGE");
    char exe[1024];
    if (!(appimage && *appimage) && openpete_exe_dir(exe, sizeof exe) == 0) {
        for (size_t i = 0; i < sizeof k_rel / sizeof k_rel[0]; i++) {
            openpete_paths_join(out, cap, exe, k_rel[i]);
            if (readable(out)) return 1;
        }
    }
    snprintf(out, cap, "%s", OPENPETE_DOCS_URL);
    return 0;
}

int openpete_docs_open(void) {
    char target[1100];
    const int local = openpete_docs_target(target, sizeof target);
    /* SDL_OpenURL hands a plain path to the shell on Windows and to
     * xdg-open on Linux; macOS takes a file:// URL. */
    char url[1200];
#if defined(__APPLE__)
    snprintf(url, sizeof url, "%s%s", local ? "file://" : "", target);
#else
    (void)local;
    snprintf(url, sizeof url, "%s", target);
#endif
    if (SDL_OpenURL(url)) {
        OPL_INFO_F("menu", "opened the documentation: %s", url);
        return 0;
    }
    OPL_WARN_F("menu", "could not open the documentation %s: %s", url,
               SDL_GetError());
    return -1;
}
