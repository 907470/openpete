/*
 * rhi_smoketest - standalone RHI verification.
 *
 * Opens a Vulkan window and, over a cycling clear color, draws:
 *   - three quads sampled from a synthetic PSX VRAM test pattern
 *     through the ported 4/8/16bpp + CLUT decoder (shaders/psx_sample.*):
 *       left   = 16bpp direct  (red->right / green->down gradient)
 *       middle =  8bpp + 256-entry CLUT (grayscale ramp left->right)
 *       right  =  4bpp +  16-entry CLUT (16 color bars)
 *   - a vertex-colored reference quad (the non-VRAM pipeline path).
 *
 * Proves the whole foundation in isolation: instance/device/swapchain,
 * offscreen target, clear/blit/present, vertex buffers, pipelines, the
 * R16_UINT VRAM image + sampler descriptor set + fragment push constants,
 * and the PSX texture decode.
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

#define VRAM_W 1024
#define VRAM_H 512
#define TS       64   /* test texture size (texels per side) */

/* Per-draw PSX material - must match the push_constant block in
 * shaders/psx_sample.frag (five tightly-packed 32-bit ints). */
typedef struct {
    int32_t tpage_x, tpage_y; /* texture page origin (VRAM 16-bit-pixel units) */
    int32_t clut_x,  clut_y;  /* CLUT origin (paletted modes only)             */
    int32_t bpp;              /* 4, 8, or 16                                   */
} psx_material;

/** @brief Pack 5-bit r, g, b and a 1-bit stp flag into a PSX 16-bit texel.
 *
 * Each channel is masked to its low 5 bits: r in bits 0-4, g in bits 5-9,
 * b in bits 10-14; the low bit of stp goes to bit 15. */
static uint16_t bgr555(int r, int g, int b, int stp)
{
    return (uint16_t)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10) | ((stp & 1) << 15));
}

/** @brief Store an 8-bit CLUT index at texel (u,v) of an 8bpp texture in the VRAM mirror.
 *
 * Two indices share one 16-bit VRAM word, the even texel in the low byte:
 * the PSX packing the sampler decodes. The other byte of the word is kept.
 * @param vram The 1024x512 VRAM mirror.
 * @param bx Texture origin x in 16-bit VRAM words.
 * @param by Texture origin y in VRAM rows.
 * @param u Texel column.
 * @param v Texel row.
 * @param idx CLUT index; the low 8 bits are stored. */
static void put8(uint16_t *vram, int bx, int by, int u, int v, int idx)
{
    uint16_t *w = &vram[(by + v) * VRAM_W + bx + (u >> 1)];
    if (u & 1) *w = (uint16_t)((*w & 0x00FF) | ((idx & 0xFF) << 8));
    else       *w = (uint16_t)((*w & 0xFF00) |  (idx & 0xFF));
}

/** @brief Store a 4-bit CLUT index at texel (u,v) of a 4bpp texture in the VRAM mirror.
 *
 * Four indices share one 16-bit VRAM word, texel u in nibble u & 3
 * counted from the low end. The other nibbles of the word are kept.
 * @param vram The 1024x512 VRAM mirror.
 * @param bx Texture origin x in 16-bit VRAM words.
 * @param by Texture origin y in VRAM rows.
 * @param u Texel column.
 * @param v Texel row.
 * @param idx CLUT index; the low 4 bits are stored. */
static void put4(uint16_t *vram, int bx, int by, int u, int v, int idx)
{
    uint16_t *w = &vram[(by + v) * VRAM_W + bx + (u >> 2)];
    int sh = 4 * (u & 3);
    *w = (uint16_t)((*w & ~(0xF << sh)) | ((idx & 0xF) << sh));
}

/** @brief Write the six vertices of an axis-aligned quad as two triangles.
 *
 * Triangles tl-tr-br and tl-br-bl, z 0, all with one modulate colour.
 * @param o Output array of six vertices.
 * @param x0 Left edge in clip space.
 * @param y0 Top edge in clip space.
 * @param x1 Right edge in clip space.
 * @param y1 Bottom edge in clip space.
 * @param u0 Left texture coordinate in texels.
 * @param v0 Top texture coordinate in texels.
 * @param u1 Right texture coordinate in texels.
 * @param v1 Bottom texture coordinate in texels.
 * @param rgba Modulate colour. */
static void make_quad(rhi_vertex *o, float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1, uint32_t rgba)
{
    rhi_vertex tl = { x0, y0, 0.0f, u0, v0, rgba, 0 };
    rhi_vertex tr = { x1, y0, 0.0f, u1, v0, rgba, 0 };
    rhi_vertex br = { x1, y1, 0.0f, u1, v1, rgba, 0 };
    rhi_vertex bl = { x0, y1, 0.0f, u0, v1, rgba, 0 };
    o[0] = tl; o[1] = tr; o[2] = br;
    o[3] = tl; o[4] = br; o[5] = bl;
}

/** @brief Fill the VRAM mirror with one 64x64 test texture per sampler depth and their CLUTs.
 *
 * 16bpp direct at words (0,0): red rises with u, green with v, blue fixed
 * at 1 so no texel is the transparent all-zero value. 8bpp at words (64,0):
 * index ramps 0..255 across u into a 256-entry grey CLUT at row 256, whose
 * blue channel has bit 0 set so entry 0 is not zero. 4bpp at words (128,0):
 * index u * 16 / 64 into a 16-colour bar CLUT at row 257.
 * @param vram The zeroed 1024x512 VRAM mirror. */
static void build_vram(uint16_t *vram)
{
    /* 16bpp texture at tpage (0,0): R across, G down, a dim blue floor so
     * the (0,0) corner isn't the transparent sentinel. */
    for (int v = 0; v < TS; v++)
        for (int u = 0; u < TS; u++)
            vram[v * VRAM_W + u] = bgr555(u * 31 / (TS - 1), v * 31 / (TS - 1), 1, 0);

    /* 8bpp texture at tpage (64,0) + 256-entry grayscale CLUT at (0,256).
     * idx ramps 0..255 across u; CLUT entry 0 gets a blue floor (non-zero). */
    for (int i = 0; i < 256; i++) {
        int g = i >> 3;
        vram[256 * VRAM_W + i] = bgr555(g, g, g | 1, 0);
    }
    for (int v = 0; v < TS; v++)
        for (int u = 0; u < TS; u++)
            put8(vram, 64, 0, u, v, u * 255 / (TS - 1));

    /* 4bpp texture at tpage (128,0) + 16 color bars CLUT at (0,257). */
    static const unsigned char bars[16][3] = {
        {31,0,0},{31,15,0},{31,31,0},{15,31,0},{0,31,0},{0,31,15},{0,31,31},{0,15,31},
        {0,0,31},{15,0,31},{31,0,31},{31,0,15},{31,31,31},{20,20,20},{10,10,10},{31,16,8},
    };
    for (int i = 0; i < 16; i++)
        vram[257 * VRAM_W + i] = bgr555(bars[i][0], bars[i][1], bars[i][2], 0);
    for (int v = 0; v < TS; v++)
        for (int u = 0; u < TS; u++)
            put4(vram, 128, 0, u, v, u * 16 / TS);
}

/** @brief Run the RHI smoke test: draw PSX-sampled and vertex-coloured quads until quit.
 *
 * Opens a resizable Vulkan window, fills a VRAM mirror with a 16bpp gradient,
 * an 8bpp grey ramp and a 4bpp colour-bar CLUT test pattern, and each
 * iteration draws three quads sampled through the PSX material push constant
 * (16, 8 and 4 bpp) and one vertex-coloured reference quad into an offscreen
 * target cleared to a cycling hue, then blits it to the swapchain. Exits on
 * window close or Escape, or after N presented frames with `--frames N`.
 * @param argc Argument count.
 * @param argv Arguments; `--frames N` sets the presented-frame limit.
 * @return 0 on a clean run, 1 when SDL, the device or the GPU resources fail
 *         to initialise. */
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

    SDL_Window *win = SDL_CreateWindow("openpete rhi smoketest", 1280, 720,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    rhi_device *dev = rhi_create(win, "openpete-rhi-smoketest", /*vsync=*/-1);
    if (!dev) {
        fprintf(stderr, "rhi_create failed\n");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    printf("rhi_smoketest: device='%s'\n", rhi_device_name(dev));
    fflush(stdout);

    /* VRAM model + test pattern. */
    uint16_t *mirror = calloc((size_t)VRAM_W * VRAM_H, sizeof(uint16_t));
    rhi_texture *vram = rhi_create_vram(dev, VRAM_W, VRAM_H);
    if (!mirror || !vram) {
        fprintf(stderr, "rhi_smoketest: VRAM setup failed\n");
        free(mirror); rhi_destroy(dev); SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    build_vram(mirror);
    rhi_update_vram(dev, vram, mirror);

    /* Three sampled quads (top) sharing one buffer; one reference quad
     * (bottom). Y is up (the offscreen viewport is Y-flipped). */
    rhi_vertex tex_verts[18];
    make_quad(&tex_verts[0],  -0.95f, 0.92f, -0.35f, 0.18f, 0, 0, TS, TS, 0xFFFFFFFFu);
    make_quad(&tex_verts[6],  -0.30f, 0.92f,  0.30f, 0.18f, 0, 0, TS, TS, 0xFFFFFFFFu);
    make_quad(&tex_verts[12],  0.35f, 0.92f,  0.95f, 0.18f, 0, 0, TS, TS, 0xFFFFFFFFu);
    rhi_buffer *tex_vb = rhi_create_vertex_buffer(dev, sizeof(tex_verts));

    const rhi_vertex ref[6] = {
        { -0.45f, -0.18f, 0.0f, 0.0f, 0.0f, 0xFF0000FFu, 0 }, /* red    */
        {  0.45f, -0.18f, 0.0f, 1.0f, 0.0f, 0xFF00FF00u, 0 }, /* green  */
        {  0.45f, -0.92f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u, 0 }, /* blue   */
        { -0.45f, -0.18f, 0.0f, 0.0f, 0.0f, 0xFF0000FFu, 0 }, /* red    */
        {  0.45f, -0.92f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u, 0 }, /* blue   */
        { -0.45f, -0.92f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0 }, /* white  */
    };
    rhi_buffer *ref_vb = rhi_create_vertex_buffer(dev, sizeof(ref));

    rhi_pipeline_desc tex_desc = {
        .vert_spv = "psx_sample.vert.spv", .frag_spv = "psx_sample.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    rhi_pipeline_desc ref_desc = {
        .vert_spv = "quad.vert.spv", .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
    };
    rhi_pipeline *tex_pipe = rhi_create_pipeline(dev, &tex_desc);
    rhi_pipeline *ref_pipe = rhi_create_pipeline(dev, &ref_desc);
    if (!tex_vb || !ref_vb || !tex_pipe || !ref_pipe) {
        fprintf(stderr, "rhi_smoketest: pipeline/buffer setup failed\n");
        rhi_destroy(dev); SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    rhi_update_buffer(tex_vb, tex_verts, sizeof(tex_verts));
    rhi_update_buffer(ref_vb, ref, sizeof(ref));

    const psx_material mat16 = {   0, 0,   0,   0, 16 };
    const psx_material mat8  = {  64, 0,   0, 256,  8 };
    const psx_material mat4  = { 128, 0,   0, 257,  4 };

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

        /* PSX-sampled quads. */
        rhi_bind_pipeline(dev, tex_pipe);
        rhi_bind_vram(dev, vram);
        rhi_bind_vertex_buffer(dev, tex_vb);
        rhi_push_constants(dev, &mat16, sizeof(mat16)); rhi_draw(dev, 6, 0);
        rhi_push_constants(dev, &mat8,  sizeof(mat8));  rhi_draw(dev, 6, 6);
        rhi_push_constants(dev, &mat4,  sizeof(mat4));  rhi_draw(dev, 6, 12);

        /* Vertex-colored reference quad (non-VRAM pipeline). */
        rhi_bind_pipeline(dev, ref_pipe);
        rhi_bind_vertex_buffer(dev, ref_vb);
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
    rhi_destroy_pipeline(dev, tex_pipe);
    rhi_destroy_pipeline(dev, ref_pipe);
    rhi_destroy_buffer(dev, tex_vb);
    rhi_destroy_buffer(dev, ref_vb);
    rhi_destroy_texture(dev, vram);
    rhi_destroy(dev);
    free(mirror);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
