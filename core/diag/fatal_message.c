/* fatal_message.c - the player-facing half of a fatal exit. See
 * openpete_fatal.h. */

#include "openpete_fatal.h"
#include "openpete_log.h"

#include <SDL3/SDL_messagebox.h>
#include <stdio.h>

static int s_interactive = 0;

void openpete_fatal_set_interactive(int on) { s_interactive = on ? 1 : 0; }

void openpete_fatal_message(openpete_error_t err, const char* title, const char* msg) {
    const char* log = openpete_log_file_path();
    fprintf(stderr, "openpete: %s: %s %s\n", title, msg, openpete_error_ref(err));
#ifdef _WIN32
    if (s_interactive) {
        char text[2048];
        int n = snprintf(text, sizeof text, "%s\n\nError %s, explained at:\n%s", msg,
                         openpete_error_id(err), openpete_error_url(err));
        if (log && *log && n > 0 && (size_t)n < sizeof text)
            snprintf(text + n, sizeof text - (size_t)n,
                     "\n\nDetails are in the log file:\n%s", log);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, NULL);
    }
#else
    (void)log;
#endif
}
