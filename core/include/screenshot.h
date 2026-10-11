// core/include/screenshot.h - read pixels from the host renderer to PNG.

#ifndef OPENPETE_SCREENSHOT_H
#define OPENPETE_SCREENSHOT_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Save the SDL renderer's current target to an image file.
 *
 * Reads the pixels with SDL_RenderReadPixels and converts them to RGBA32
 * when the renderer's format differs; the rows are already top-down. A path
 * ending in `.png` (any case) is written with
 * openpete_write_png_rgba_stride, any other path as BMP with SDL_SaveBMP. Used by run.c's SDL-renderer screenshot
 * path.
 * @param renderer Renderer to read.
 * @param path Output path.
 * @return 0 on success; -1 on a NULL argument or a read, convert or write
 *         failure. */
int openpete_screenshot_save_png(SDL_Renderer* renderer, const char* path);

/** @brief Expand the `{n}` and `{p}` placeholders of a screenshot path template.
 *
 * Each `{n}` becomes @p frame and each `{p}` becomes @p present in decimal;
 * every other character is copied. Output longer than @p out_sz is
 * truncated and always NUL-terminated. A NULL template yields an empty
 * string. A `{` that does not open a complete `{n}` or `{p}` is copied
 * as is; the scan never reads past the terminator.
 * @param tmpl Template, such as the --screenshot-output value.
 * @param frame VBL number substituted for `{n}`.
 * @param present Present index substituted for `{p}`; callers capturing one
 *        present per VBL pass 0.
 * @param out Output buffer; nothing is written when NULL.
 * @param out_sz Size of @p out; nothing is written when 0. */
void openpete_screenshot_format_path(const char* tmpl, int frame,
                                     int present,
                                     char* out, size_t out_sz);

/** @brief Report whether a path ends in `.png`, ignoring case.
 *
 * @param path Path to test; NULL returns 0.
 * @return 1 for a `.png` suffix, else 0. */
int  openpete_screenshot_path_is_png(const char* path);

/** @brief Write a top-down RGBA8 buffer with a row stride to a PNG file, replacing it atomically.
 *
 * The PNG is encoded with stb_image_write into a temporary beside @p path
 * and renamed over it without a device flush (OP_ATOMIC_NOSYNC, the
 * capture class), so a failed or interrupted write leaves any previous
 * file whole. No flip or channel swap is applied; alpha is written as
 * given.
 * @param path Output path.
 * @param pixels h rows of w*4 bytes, @p stride bytes apart.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @param stride Bytes from one row's start to the next.
 * @return 0 on success; 1 on a NULL argument, a non-positive dimension,
 *         or a write failure. */
int  openpete_write_png_rgba_stride(const char* path, const void* pixels, int w, int h,
                                    int stride);

/** @brief Write a top-down, packed RGBA8 buffer to a PNG file, replacing it atomically.
 *
 * openpete_write_png_rgba_stride with a stride of w*4. The native
 * renderer's readbacks already return top-down R,G,B,A. Alpha is written
 * as given. Used by the native screenshot paths and the identity audit.
 * @param path Output path.
 * @param pixels w*h*4 bytes, rows packed.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return 0 on success; 1 on a NULL argument, a non-positive dimension,
 *         or a write failure. */
int  openpete_write_png_rgba(const char* path, const void* pixels, int w, int h);

/** @brief Name the next player-facing screenshot and create its directory.
 *
 * The name is `<dir>/openpete-<YYYYmmdd-HHMMSS>.png` in local time, then
 * `-2` to `-999` suffixes when the name exists. The directory is the
 * per-user or portable `screenshots` data directory
 * (openpete_paths_data_dir), else the relative path `screenshots`. Used by
 * run.c's screenshot key.
 * @param out Receives the path; emptied on failure.
 * @param cap Size of @p out.
 * @return 0 on success; -1 when @p out is NULL or @p cap is 0, the
 *         directory cannot be created, or every suffix is taken. */
int  openpete_screenshot_user_path(char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SCREENSHOT_H */
