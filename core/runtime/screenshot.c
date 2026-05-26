// core/runtime/screenshot.c — read SDL renderer pixels and save as PNG.
//
// Phase 0b uses SDL_RenderReadPixels (no GL). Phase 1b will switch to
// glReadPixels once PsyCross owns the GL context, at which point this
// function will gain a parallel GL path.

#include "screenshot.h"
#include "openpete_log.h"

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
        if (tmpl[i] == '{' && tmpl[i+2] == '}'
                && (tmpl[i+1] == 'n' || tmpl[i+1] == 'p')) {
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
