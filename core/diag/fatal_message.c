/* fatal_message.c — the player-facing half of a fatal exit. See
 * openpete_fatal.h. */

#include "openpete_fatal.h"
#include "openpete_log.h"

#include <SDL3/SDL_messagebox.h>
#include <stdio.h>

static int s_interactive = 0;

void openpete_fatal_set_interactive(int on) { s_interactive = on ? 1 : 0; }

void openpete_fatal_message(const char* title, const char* msg) {
    const char* log = openpete_log_file_path();
    fprintf(stderr, "openpete: %s: %s\n", title, msg);
#ifdef _WIN32
    if (s_interactive) {
        char text[2048];
        if (log && *log)
            snprintf(text, sizeof text, "%s\n\nDetails are in the log file:\n%s", msg, log);
        else
            snprintf(text, sizeof text, "%s", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, NULL);
    }
#else
    (void)log;
#endif
}
