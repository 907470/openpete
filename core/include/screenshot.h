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

/* Substitute `{n}` (sim frame) and `{p}` (present index) in `tmpl` with the
 * given decimals. If a placeholder doesn't appear, that field is omitted.
 * Output is always NUL-terminated. Pass present=0 when capturing a single
 * present per frame (the historical screenshot-on-frame behaviour).  */
void openpete_screenshot_format_path(const char* tmpl, int frame,
                                     int present,
                                     char* out, size_t out_sz);

/* True if `path`'s extension (case-insensitive) is `.png`. */
int  openpete_screenshot_path_is_png(const char* path);

/* Write a raw RGBA8 buffer (byte order R,G,B,A; `pixels` is w*h*4 bytes,
 * top-down) to `path` as PNG via stb_image_write. Returns 0 on success. */
int  openpete_write_png_rgba(const char* path, const void* pixels, int w, int h);

/* Name the next player-facing screenshot: "<screenshots dir>/openpete-<local
 * timestamp>.png", with a numeric suffix when that name is taken. Creates the
 * directory. Returns 0 on success, -1 on failure (nothing written to `out`). */
int  openpete_screenshot_user_path(char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SCREENSHOT_H */
