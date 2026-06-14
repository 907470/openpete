/*
 * rhi_smoketest — milestone 1a verification.
 *
 * Opens a Vulkan window and clears the offscreen target to a cycling
 * color, then blits + presents. Proves the RHI Vulkan bringup (instance,
 * device, swapchain, offscreen target, clear, blit, present, resize) in
 * isolation, before the RHI is wired into openpete's fiber loop.
 *
 * Run:   rhi_smoketest [--frames N]
 *   --frames N   exit after N presented frames (for non-interactive CI);
 *                otherwise runs until the window is closed or Esc.
 */

#include "rhi.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    long frame_limit = -1; /* -1 = run until quit */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frame_limit = strtol(argv[++i], NULL, 10);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("openpete rhi smoketest (1a)", 1280, 720,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    rhi_device *dev = rhi_create(win, "openpete-rhi-smoketest");
    if (!dev) {
        fprintf(stderr, "rhi_create failed\n");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    printf("rhi_smoketest: device='%s'\n", rhi_device_name(dev));
    fflush(stdout);

    /* 1b.1: a centered, vertex-colored quad drawn through the pipeline on
     * top of the cycling clear — exercises buffer + pipeline + draw. Colors
     * are packed byte order R,G,B,A (matches the R8G8B8A8_UNORM attribute). */
    const rhi_vertex quad[6] = {
        { -0.6f, -0.6f, 0.0f, 0.0f, 0.0f, 0xFF0000FFu, 0 }, /* red    */
        {  0.6f, -0.6f, 0.0f, 1.0f, 0.0f, 0xFF00FF00u, 0 }, /* green  */
        {  0.6f,  0.6f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u, 0 }, /* blue   */
        { -0.6f, -0.6f, 0.0f, 0.0f, 0.0f, 0xFF0000FFu, 0 }, /* red    */
        {  0.6f,  0.6f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u, 0 }, /* blue   */
        { -0.6f,  0.6f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0 }, /* white  */
    };
    rhi_buffer *quad_vb = rhi_create_vertex_buffer(dev, sizeof(quad));
    rhi_pipeline_desc pdesc = {
        .vert_spv = "quad.vert.spv", .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
    };
    rhi_pipeline *quad_pipe = rhi_create_pipeline(dev, &pdesc);
    if (!quad_vb || !quad_pipe) {
        fprintf(stderr, "rhi_smoketest: quad setup failed\n");
        rhi_destroy(dev); SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    rhi_update_buffer(quad_vb, quad, sizeof(quad));

    bool running = true;
    unsigned long frame = 0;
    unsigned long presented = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (e.type == SDL_EVENT_WINDOW_RESIZED ||
                       e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                rhi_notify_resize(dev);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }
        if (!running) break;

        /* Integer RGB ramp (no libm): cycles through the hue wheel so the
         * window is visibly live. */
        unsigned p = (unsigned)(frame % 768u);
        float r, g, b;
        if (p < 256u)      { r = (255u - p) / 255.0f;        g = p / 255.0f;             b = 0.0f; }
        else if (p < 512u) { unsigned q = p - 256u; r = 0.0f; g = (255u - q) / 255.0f;   b = q / 255.0f; }
        else               { unsigned q = p - 512u; r = q / 255.0f; g = 0.0f;            b = (255u - q) / 255.0f; }
        frame++;

        if (!rhi_begin_frame(dev)) {
            /* Swapchain recreating or window minimized; try again. */
            SDL_Delay(8);
            continue;
        }
        rhi_begin_offscreen(dev, r, g, b, 1.0f);
        rhi_bind_pipeline(dev, quad_pipe);
        rhi_bind_vertex_buffer(dev, quad_vb);
        rhi_draw(dev, 6, 0);
        rhi_end_offscreen(dev);
        rhi_blit_to_swapchain(dev);
        rhi_end_frame(dev);

        presented++;
        if (presented == 1 || presented % 120 == 0) {
            printf("rhi_smoketest: presented %lu frames\n", presented);
            fflush(stdout);
        }
        if (frame_limit >= 0 && (long)presented >= frame_limit) {
            running = false;
        }
    }

    printf("rhi_smoketest: shutting down after %lu presented frames\n", presented);
    rhi_wait_idle(dev);
    rhi_destroy_pipeline(dev, quad_pipe);
    rhi_destroy_buffer(dev, quad_vb);
    rhi_destroy(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
