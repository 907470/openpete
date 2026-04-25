// core/runtime/screenshot.c — read SDL renderer pixels and save as PNG.
//
// Phase 0b uses SDL_RenderReadPixels (no GL). Phase 1b will switch to
// glReadPixels once PsyCross owns the GL context, at which point this
// function will gain a parallel GL path.

#include "screenshot.h"
#include "openpete_log.h"

#include <stdint.h>
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

    /* PNG is top-down; SDL_RenderReadPixels already gives us top-down. No flip. */
    if (!stbi_write_png(path, w, h, channels, pixels, pitch)) {
        OPL_ERROR_F("screenshot", "stbi_write_png to %s failed", path);
        free(pixels);
        return -1;
    }

    free(pixels);
    OPL_INFO_F("screenshot", "saved %dx%d PNG to %s", w, h, path);
    return 0;
}
