/* openpete_docs_open.h - open the User documentation in the system browser.
 *
 * A package carries the documentation site as docs/ beside the executable
 * (docs/index.html); a build tree carries it as docs-site/ once the `docs`
 * target has run. The settings menu's DOCUMENTATION row opens the first of
 * those that exists. Two cases open the published site instead:
 *
 *   - no local copy exists (a build without the docs target), and
 *   - the program runs from an AppImage. Its files live on a mount that
 *     the runtime removes when the program exits, so a tab left open loses
 *     every page it has not loaded; and a browser confined by snap sees a
 *     private /tmp and no hidden directory of the home folder, so it can
 *     read neither the mount nor a copy under ~/.cache.
 */
#ifndef OPENPETE_DOCS_OPEN_H
#define OPENPETE_DOCS_OPEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The published User section, opened when no local copy is usable. */
#define OPENPETE_DOCS_URL "https://openpete.com/docs/user/index.html"

/** @brief Resolve what the documentation row opens: a local index.html path or the published URL.
 *
 * Tries <exe_dir>/docs/index.html, then <exe_dir>/docs-site/index.html,
 * unless the APPIMAGE environment variable is set (the AppImage runtime
 * sets it), in which case only the URL is written.
 * @param out Receives the path or URL.
 * @param cap Capacity of @p out.
 * @return 1 when @p out names a local file, 0 when it holds the URL. */
int openpete_docs_target(char* out, size_t cap);

/** @brief Open the documentation in the system's browser.
 *
 * Resolves the target with openpete_docs_target and hands it to
 * SDL_OpenURL (a file:// URL for a local file). The outcome is logged
 * under the "menu" channel.
 * @return 0 when SDL_OpenURL accepted the target, -1 otherwise. */
int openpete_docs_open(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_DOCS_OPEN_H */
