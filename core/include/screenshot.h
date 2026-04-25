// core/include/screenshot.h — read pixels from the host renderer to PNG.

#ifndef OPENPETE_SCREENSHOT_H
#define OPENPETE_SCREENSHOT_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Save the current renderer's framebuffer to `path` as PNG.
 * Returns 0 on success, nonzero on error. */
int openpete_screenshot_save_png(SDL_Renderer* renderer, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SCREENSHOT_H */
