// core/include/screenshot.h — read pixels from the host renderer to PNG.

#ifndef OPENPETE_SCREENSHOT_H
#define OPENPETE_SCREENSHOT_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Save the current renderer's framebuffer to `path`. The format is selected
 * by `path`'s extension: `.png` writes PNG via stb_image_write, anything
 * else writes BMP via SDL_SaveBMP. Returns 0 on success, nonzero on error. */
int openpete_screenshot_save_png(SDL_Renderer* renderer, const char* path);

/* Substitute `{n}` in `tmpl` with `frame` (decimal). If no `{n}` appears,
 * the template is copied verbatim. Output is always NUL-terminated. */
void openpete_screenshot_format_path(const char* tmpl, int frame,
                                     char* out, size_t out_sz);

/* True if `path`'s extension (case-insensitive) is `.png`. */
int  openpete_screenshot_path_is_png(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SCREENSHOT_H */
