// core/runtime/run.c — Phase 0b run loop.
//
// Owns: SDL setup, render loop, frame counter, screenshot trigger,
// exit-condition tracking. Game-agnostic — knows nothing about Spyro.
//
// Phase 1b will replace this with the recompiled-game dispatcher; for
// now, the loop just clears to a colored background each frame.

#include "openpete.h"
#include "openpete_log.h"
#include "screenshot.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 640
#define WIN_H 480

/* A faint blue-gray so we can visually distinguish "Phase 0b binary
 * actually drew something" vs "garbage / never ran". Also keeps the PNG
 * non-zero-byte. RGB 0x10/0x14/0x20. */
#define CLEAR_R 0x10
#define CLEAR_G 0x14
#define CLEAR_B 0x20

int openpete_run(const openpete_options_t* opts) {
    openpete_log_init(opts->log_format, opts->log_level_spec);
    OPL_INFO_F("boot", "starting %s (frame_limit=%d max_seconds=%d headless=%d)",
               opts->display_name, opts->frame_limit, opts->max_runtime_seconds,
               (int)opts->headless);

    if (opts->headless) {
        /* Try the offscreen driver first (renders to memory; correct semantics).
         * Some Linux distro packages don't ship it — fall back to "dummy",
         * which is always present. Honor a pre-existing SDL_VIDEODRIVER. */
        SDL_setenv("SDL_VIDEODRIVER", "offscreen", 0);
        OPL_INFO_F("boot", "headless mode requested (preferred driver: %s)",
                   SDL_getenv("SDL_VIDEODRIVER"));
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        if (opts->headless) {
            OPL_WARN_F("boot", "SDL_Init under '%s' failed (%s); retrying with dummy",
                       SDL_getenv("SDL_VIDEODRIVER"), SDL_GetError());
            SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
                OPL_ERROR_F("boot", "SDL_Init dummy fallback also failed: %s", SDL_GetError());
                return OPENPETE_EXIT_PANIC;
            }
        } else {
            OPL_ERROR_F("boot", "SDL_Init: %s", SDL_GetError());
            return OPENPETE_EXIT_PANIC;
        }
    }
    OPL_INFO_F("boot", "SDL video driver: %s", SDL_GetCurrentVideoDriver());

    Uint32 window_flags = SDL_WINDOW_SHOWN;
    if (opts->headless) {
        /* Offscreen driver still creates a window; hidden flag is harmless. */
        window_flags = SDL_WINDOW_HIDDEN;
    }
    SDL_Window* window = SDL_CreateWindow(
        opts->display_name,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, window_flags);
    if (!window) {
        OPL_ERROR_F("boot", "SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return OPENPETE_EXIT_PANIC;
    }

    /* Headless: skip GPU acceleration (the offscreen/dummy drivers don't
     * expose one) and go straight to software so SDL_RenderReadPixels works. */
    Uint32 renderer_flags = opts->headless ? SDL_RENDERER_SOFTWARE
                                           : SDL_RENDERER_ACCELERATED;
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    if (!renderer && !opts->headless) {
        OPL_WARN_F("boot", "accelerated renderer unavailable, falling back: %s",
                   SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        OPL_ERROR_F("boot", "SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return OPENPETE_EXIT_PANIC;
    }

    Uint32 start = SDL_GetTicks();
    int frame = 0;
    int exit_code = OPENPETE_EXIT_OK;
    const char* exit_reason = "window-closed";
    bool quit = false;
    int  screenshot_status = 0; /* 0 ok, 1 failed; only meaningful if requested */

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                quit = true;
                exit_reason = "sdl-quit";
            }
        }

        SDL_SetRenderDrawColor(renderer, CLEAR_R, CLEAR_G, CLEAR_B, 0xFF);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);

        if (opts->screenshot_on_frame >= 0 && frame == opts->screenshot_on_frame) {
            int rc = openpete_screenshot_save_png(renderer, opts->screenshot_output);
            if (rc != 0) {
                screenshot_status = 1;
                exit_code = OPENPETE_EXIT_PANIC;
                exit_reason = "screenshot-failed";
                quit = true;
            }
        }

        frame++;

        if (opts->frame_limit > 0 && frame >= opts->frame_limit) {
            quit = true;
            exit_reason = "frame-limit";
        }
        if (opts->max_runtime_seconds > 0 &&
            (SDL_GetTicks() - start) > (Uint32)opts->max_runtime_seconds * 1000) {
            quit = true;
            exit_reason = "max-runtime-seconds";
        }
    }

    Uint32 elapsed_ms = SDL_GetTicks() - start;
    OPL_INFO_F("exit", "reason=%s frames=%d elapsed_ms=%u screenshot_status=%d",
               exit_reason, frame, elapsed_ms, screenshot_status);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    fprintf(stdout, "openpete-exit: reason=%s frames=%d ms=%u code=%d\n",
            exit_reason, frame, elapsed_ms, exit_code);
    return exit_code;
}
