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

    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0) {
        OPL_ERROR_F("screenshot", "SDL_GetRendererOutputSize: %s", SDL_GetError());
        return -1;
    }
    if (w <= 0 || h <= 0) {
        OPL_ERROR_F("screenshot", "renderer has zero size (%dx%d)", w, h);
        return -1;
    }

    /* RGBA8888 keeps stb_image_write happy at 4 channels per pixel. */
    const int channels = 4;
    const Uint32 fmt   = SDL_PIXELFORMAT_RGBA32;
    const int   pitch  = w * channels;

    uint8_t* pixels = (uint8_t*)malloc((size_t)pitch * (size_t)h);
    if (!pixels) {
        OPL_ERROR_F("screenshot", "malloc %d bytes failed", pitch * h);
        return -1;
    }

    if (SDL_RenderReadPixels(renderer, NULL, fmt, pixels, pitch) != 0) {
        OPL_ERROR_F("screenshot", "SDL_RenderReadPixels: %s", SDL_GetError());
        free(pixels);
        return -1;
    }

    /* SDL_RenderReadPixels gives top-down; both PNG and SDL_SaveBMP want
     * top-down with these masks, so no flip needed. */
    if (openpete_screenshot_path_is_png(path)) {
        if (!stbi_write_png(path, w, h, channels, pixels, pitch)) {
            OPL_ERROR_F("screenshot", "stbi_write_png to %s failed", path);
            free(pixels);
            return -1;
        }
        OPL_INFO_F("screenshot", "saved %dx%d PNG to %s", w, h, path);
    } else {
        SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
            pixels, w, h, /*depth=*/32, /*pitch=*/pitch,
            /*Rmask=*/0x000000FFu, /*Gmask=*/0x0000FF00u,
            /*Bmask=*/0x00FF0000u, /*Amask=*/0xFF000000u);
        if (!surface || SDL_SaveBMP(surface, path) != 0) {
            OPL_ERROR_F("screenshot", "SDL_SaveBMP to %s failed: %s",
                        path, SDL_GetError());
            if (surface) SDL_FreeSurface(surface);
            free(pixels);
            return -1;
        }
        SDL_FreeSurface(surface);
        OPL_INFO_F("screenshot", "saved %dx%d BMP to %s", w, h, path);
    }

    free(pixels);
    return 0;
}

void openpete_screenshot_format_path(const char* tmpl, int frame,
                                     char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!tmpl) { out[0] = '\0'; return; }
    size_t o = 0;
    for (size_t i = 0; tmpl[i] && o + 1 < out_sz; ) {
        if (tmpl[i] == '{' && tmpl[i+1] == 'n' && tmpl[i+2] == '}') {
            int n = snprintf(out + o, out_sz - o, "%d", frame);
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
