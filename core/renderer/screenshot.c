// core/renderer/screenshot.c — read renderer pixels and save as PNG.

#include "screenshot.h"
#include "openpete_log.h"
#include "openpete_paths.h"

#include <time.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int openpete_screenshot_save_png(SDL_Renderer* renderer, const char* path) {
    if (!renderer || !path) return -1;

    /* SDL3 SDL_RenderReadPixels allocates and returns a surface in the
     * renderer's native format. Normalise to RGBA8888 so stb_image_write
     * gets 4 channels per pixel and the BMP path has a known layout. */
    SDL_Surface* raw = SDL_RenderReadPixels(renderer, NULL);
    if (!raw) {
        OPL_ERROR_F("screenshot", "SDL_RenderReadPixels: %s", SDL_GetError());
        return -1;
    }
    SDL_Surface* surface = (raw->format == SDL_PIXELFORMAT_RGBA32)
        ? raw
        : SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    if (surface != raw) SDL_DestroySurface(raw);
    if (!surface) {
        OPL_ERROR_F("screenshot", "SDL_ConvertSurface: %s", SDL_GetError());
        return -1;
    }

    const int w = surface->w, h = surface->h;
    int rc = 0;

    /* SDL_RenderReadPixels gives top-down; both PNG and SDL_SaveBMP want
     * top-down, so no flip needed. */
    if (openpete_screenshot_path_is_png(path)) {
        if (!stbi_write_png(path, w, h, 4, surface->pixels, surface->pitch)) {
            OPL_ERROR_F("screenshot", "stbi_write_png to %s failed", path);
            rc = -1;
        } else {
            OPL_INFO_F("screenshot", "saved %dx%d PNG to %s", w, h, path);
        }
    } else {
        if (!SDL_SaveBMP(surface, path)) {
            OPL_ERROR_F("screenshot", "SDL_SaveBMP to %s failed: %s",
                        path, SDL_GetError());
            rc = -1;
        } else {
            OPL_INFO_F("screenshot", "saved %dx%d BMP to %s", w, h, path);
        }
    }

    SDL_DestroySurface(surface);
    return rc;
}

void openpete_screenshot_format_path(const char* tmpl, int frame,
                                     int present,
                                     char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!tmpl) { out[0] = '\0'; return; }
    size_t o = 0;
    for (size_t i = 0; tmpl[i] && o + 1 < out_sz; ) {
        /* tmpl[i+2] is read only after tmpl[i+1] matched a non-NUL byte, so
         * a template ending in '{' never reads past its terminator. */
        if (tmpl[i] == '{' && (tmpl[i+1] == 'n' || tmpl[i+1] == 'p')
                && tmpl[i+2] == '}') {
            int val = (tmpl[i+1] == 'n') ? frame : present;
            int n = snprintf(out + o, out_sz - o, "%d", val);
            if (n < 0) break;
            o += (size_t)n;
            if (o >= out_sz) o = out_sz - 1;
            i += 3;
        } else {
            out[o++] = tmpl[i++];
        }
    }
    out[o < out_sz ? o : out_sz - 1] = '\0';
}

int openpete_screenshot_path_is_png(const char* path) {
    if (!path) return 0;
    size_t n = strlen(path);
    if (n < 4) return 0;
    const char* e = path + n - 4;
    return (e[0] == '.'
            && tolower((unsigned char)e[1]) == 'p'
            && tolower((unsigned char)e[2]) == 'n'
            && tolower((unsigned char)e[3]) == 'g');
}

int openpete_screenshot_user_path(char* out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = '\0';

    /* Screenshots are output, so the directory has to be writable: next to the
     * exe in a portable install, under the per-user data directory otherwise —
     * which is also where a read-only install medium (an AppImage mount,
     * Program Files) lands. A relative path is the last resort, so a run whose
     * exe location cannot be determined still saves something. */
    char dir[1024];
    if (openpete_paths_data_dir("screenshots", dir, sizeof dir, 0) != 0)
        snprintf(dir, sizeof dir, "screenshots");
    if (!SDL_CreateDirectory(dir)) {  /* mkdir -p; true if it already exists */
        OPL_ERROR_F("screenshot", "could not create %s: %s", dir, SDL_GetError());
        return -1;
    }

    char stamp[24];
    time_t now = time(NULL);
    /* localtime() (not _r/_s): this runs once per keypress on the host loop,
     * so the shared-buffer caveat doesn't apply, and it avoids the
     * platform-specific reentrant variants. */
    struct tm* tmv = localtime(&now);
    if (!tmv || strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tmv) == 0)
        snprintf(stamp, sizeof stamp, "screenshot");

    /* One-second stamp resolution, so a second shot within the same second
     * would overwrite the first: suffix until the name is free. */
    for (int n = 1; n < 1000; n++) {
        char leaf[64];
        if (n == 1) snprintf(leaf, sizeof leaf, "openpete-%s.png", stamp);
        else        snprintf(leaf, sizeof leaf, "openpete-%s-%d.png", stamp, n);
        openpete_paths_join(out, cap, dir, leaf);
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(out, &info)) return 0;   /* nothing there: take it */
    }
    OPL_ERROR_F("screenshot", "no free filename for %s/openpete-%s", dir, stamp);
    out[0] = '\0';
    return -1;
}
