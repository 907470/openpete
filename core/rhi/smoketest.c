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
 * Run:   rhi_smoketest [--frames N] [--headless]
 *   --frames N   exit after N presented frames (for non-interactive CI);
 *                otherwise runs until the window is closed or Esc.
 *   --headless   run the self-checking cases on a surfaceless device and
 *                exit 0 on pass, 1 on a failed case, 77 without a device:
 *                the vertex stage reads its positions and colours from a
 *                retained storage buffer (rhi_storage, set 2), and each
 *                frame's readback must equal the expected image exactly.
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
    rhi_vertex tl = { x0, y0, 0.0f, u0, v0, rgba, 0, .w = 0.0f };
    rhi_vertex tr = { x1, y0, 0.0f, u1, v0, rgba, 0, .w = 0.0f };
    rhi_vertex br = { x1, y1, 0.0f, u1, v1, rgba, 0, .w = 0.0f };
    rhi_vertex bl = { x0, y1, 0.0f, u0, v1, rgba, 0, .w = 0.0f };
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

/* ---- headless case: vertex positions from a retained storage buffer ---- */

#define SC_W 256
#define SC_H 128

/* One storage record of storage_pull.vert: clip xy in pos, RGBA colour. */
typedef struct { float pos[4]; float color[4]; } sc_vert;

/* An axis-aligned rectangle in clip space and the pixel span it covers on
 * the SC_W x SC_H target. The clip edges sit on pixel edges, so coverage is
 * exact: columns [px0, px1) and rows [py0, py1). */
typedef struct {
    float x0, y0, x1, y1;
    int   px0, py0, px1, py1;
    uint8_t rgb[3];
} sc_rect;

static const sc_rect k_sc_red   = { -0.5f, -0.5f, 0.0f,  0.5f,  64, 32, 128, 96, { 255, 0, 0 } };
static const sc_rect k_sc_green = { 0.25f, -0.5f, 0.75f, 0.5f, 160, 32, 224, 96, { 0, 255, 0 } };

/** @brief Write the six storage records of one rectangle as two triangles.
 *
 * @param o Six records.
 * @param r Rectangle and its colour. */
static void sc_fill(sc_vert *o, const sc_rect *r)
{
    const float xy[6][2] = {
        { r->x0, r->y0 }, { r->x1, r->y0 }, { r->x1, r->y1 },
        { r->x0, r->y0 }, { r->x1, r->y1 }, { r->x0, r->y1 },
    };
    for (int i = 0; i < 6; i++) {
        o[i] = (sc_vert){ { xy[i][0], xy[i][1], 0.0f, 1.0f },
                          { r->rgb[0] / 255.0f, r->rgb[1] / 255.0f, r->rgb[2] / 255.0f, 1.0f } };
    }
}

/** @brief Return the colour a pixel must have: the first listed rectangle covering it, else black.
 *
 * @param rects Rectangles drawn in the frame.
 * @param n Their count.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param out Receives R, G, B. */
static void sc_expect(const sc_rect *const *rects, int n, int x, int y, uint8_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < n; i++) {
        const sc_rect *r = rects[i];
        if (x >= r->px0 && x < r->px1 && y >= r->py0 && y < r->py1) {
            memcpy(out, r->rgb, 3);
            return;
        }
    }
}

/** @brief Compare the offscreen readback with the expected image of a set of rectangles.
 *
 * The rectangles do not overlap, so the image is independent of draw order
 * and of the viewport's Y flip (each rectangle is symmetric about y = 0).
 * @param dev Device after the frame was submitted.
 * @param name Case name for the report line.
 * @param rects Rectangles the frame drew.
 * @param n Their count.
 * @param drawn Receives the number of non-black pixels read back.
 * @return true when every pixel matches. */
static bool sc_check(rhi_device *dev, const char *name, const sc_rect *const *rects, int n,
                     long *drawn)
{
    static uint8_t px[SC_W * SC_H * 4];
    int w = 0, h = 0;
    *drawn = 0;
    if (!rhi_read_offscreen(dev, px, sizeof px, &w, &h) || w != SC_W || h != SC_H) {
        printf("rhi_smoketest: %s: readback failed (%dx%d)\n", name, w, h);
        return false;
    }
    long bad = 0;
    for (int y = 0; y < SC_H; y++) {
        for (int x = 0; x < SC_W; x++) {
            const uint8_t *p = &px[(y * SC_W + x) * 4];
            uint8_t want[3];
            sc_expect(rects, n, x, y, want);
            *drawn += (p[0] | p[1] | p[2]) != 0;
            bad += memcmp(p, want, 3) != 0;
        }
    }
    long expect = 0;
    for (int i = 0; i < n; i++)
        expect += (long)(rects[i]->px1 - rects[i]->px0) * (rects[i]->py1 - rects[i]->py0);
    const bool ok = bad == 0 && *drawn == expect && *drawn > 0;
    printf("rhi_smoketest: %s: %s drawn=%ld expected=%ld mismatched=%ld\n",
           name, ok ? "PASS" : "FAIL", *drawn, expect, bad);
    return ok;
}

/** @brief Map, fill and commit one rectangle into the retained storage buffer.
 *
 * @param st Buffer of six records.
 * @param r Rectangle. */
static void sc_publish(rhi_storage *st, const sc_rect *r)
{
    sc_vert *v = rhi_storage_map(st);
    if (v) sc_fill(v, r);
    rhi_storage_commit(st);
}

/** @brief Record one draw of the six committed records.
 *
 * @param dev Device inside a frame and the offscreen pass.
 * @param pipe The storage_pull pipeline.
 * @param vb Six-vertex buffer bound for the pipeline's vertex input, which the stage does not read.
 * @param st Buffer. */
static void sc_draw(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb, rhi_storage *st)
{
    rhi_bind_pipeline(dev, pipe);
    rhi_bind_vertex_buffer(dev, vb);
    rhi_storage_bind(dev, st, 2);
    rhi_draw(dev, 6, 0);
}

/** @brief Run the four retained-storage frames and check each readback.
 *
 * Frame 1 commits the red rectangle and draws it. Frame 2 draws without a
 * write and must show the same image, so the contents persist across
 * frames. Frame 3 draws the committed red records, then commits the green
 * rectangle and draws again: both must show, so the same-frame commit
 * left the bytes of the recorded red draw intact. Frame 4 draws without a
 * write and must show green only. Every frame must draw a nonzero pixel
 * count equal to its rectangles' area.
 * @param dev Headless device.
 * @param pipe The storage_pull pipeline.
 * @param vb Six-vertex buffer for the vertex input.
 * @param st Buffer of six records.
 * @param drawn_total Receives the summed non-black pixel count.
 * @return true when every frame matches. */
static bool sc_run_frames(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb,
                          rhi_storage *st, long *drawn_total)
{
    static const sc_rect *const red[] = { &k_sc_red };
    static const sc_rect *const green[] = { &k_sc_green };
    static const sc_rect *const both[] = { &k_sc_red, &k_sc_green };
    static const struct {
        const char *name; int write_before, write_mid;
        const sc_rect *const *rects; int n;
    } k_frames[] = {
        { "storage commit",           1, 0, red,   1 },
        { "storage persist",          0, 0, red,   1 },
        { "storage same-frame write", 0, 1, both,  2 },
        { "storage persist after",    0, 0, green, 1 },
    };
    bool ok = true;
    *drawn_total = 0;
    for (size_t f = 0; f < sizeof k_frames / sizeof k_frames[0]; f++) {
        if (k_frames[f].write_before) sc_publish(st, &k_sc_red);
        if (!rhi_begin_frame(dev)) { printf("rhi_smoketest: begin_frame refused\n"); return false; }
        rhi_begin_offscreen(dev, 0.0f, 0.0f, 0.0f, 1.0f);
        sc_draw(dev, pipe, vb, st);
        if (k_frames[f].write_mid) {
            sc_publish(st, &k_sc_green);
            sc_draw(dev, pipe, vb, st);
        }
        rhi_end_offscreen(dev);
        rhi_end_frame(dev);
        long drawn;
        ok &= sc_check(dev, k_frames[f].name, k_frames[f].rects, k_frames[f].n, &drawn);
        *drawn_total += drawn;
    }
    return ok;
}

/** @brief Run the headless self-checking cases.
 *
 * @return 0 when every case passes, 1 on a failed case or resource
 *         failure, 77 when no Vulkan device can be created. */
static int run_headless(void)
{
    rhi_device *dev = rhi_create_headless("openpete-rhi-smoketest", SC_W, SC_H, NULL);
    if (!dev) { printf("rhi_smoketest: no headless device: SKIP\n"); return 77; }
    printf("rhi_smoketest: device='%s' (headless)\n", rhi_device_name(dev));
    const rhi_pipeline_desc desc = {
        .vert_spv = "storage_pull.vert.spv", .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false, .ssbo_set2 = true,
    };
    rhi_pipeline *pipe = rhi_create_pipeline(dev, &desc);
    rhi_buffer *vb = rhi_create_vertex_buffer(dev, 6 * sizeof(rhi_vertex));
    rhi_storage *st = rhi_storage_create(dev, 6 * sizeof(sc_vert));
    int rc = 1;
    if (pipe && vb && st) {
        const rhi_vertex zero[6] = { { 0 } };
        rhi_update_buffer(vb, zero, sizeof zero);
        const uint64_t over0 = rhi_ring_gen_overflows();
        long drawn = 0;
        const bool ok = sc_run_frames(dev, pipe, vb, st, &drawn);
        const uint64_t over = rhi_ring_gen_overflows() - over0;
        printf("rhi_smoketest: storage vertex stage: %s engaged_px=%ld overflows=%llu\n",
               ok && over == 0 ? "PASS" : "FAIL", drawn, (unsigned long long)over);
        rc = ok && over == 0 ? 0 : 1;
    } else {
        printf("rhi_smoketest: storage pipeline/buffer setup failed\n");
    }
    rhi_wait_idle(dev);
    rhi_storage_destroy(dev, st);
    if (vb) rhi_destroy_buffer(dev, vb);
    if (pipe) rhi_destroy_pipeline(dev, pipe);
    rhi_destroy(dev);
    return rc;
}

/** @brief Write the cycling clear colour of one iteration: a hue ramp over 768 steps, no libm.
 *
 * @param frame Iteration count.
 * @param rgb Receives red, green, blue in 0..1. */
static void hue_ramp(unsigned long frame, float rgb[3])
{
    unsigned p = (unsigned)(frame % 768u);
    unsigned q = p & 255u;
    float up = q / 255.0f, down = (255u - q) / 255.0f;
    unsigned seg = p >> 8;
    rgb[0] = seg == 0 ? down : seg == 2 ? up : 0.0f;
    rgb[1] = seg == 0 ? up : seg == 1 ? down : 0.0f;
    rgb[2] = seg == 1 ? up : seg == 2 ? down : 0.0f;
}

/** @brief Run the RHI smoke test: draw PSX-sampled and vertex-coloured quads until quit.
 *
 * Opens a resizable Vulkan window, fills a VRAM mirror with a 16bpp gradient,
 * an 8bpp grey ramp and a 4bpp colour-bar CLUT test pattern, and each
 * iteration draws three quads sampled through the PSX material push constant
 * (16, 8 and 4 bpp) and one vertex-coloured reference quad into an offscreen
 * target cleared to a cycling hue, then blits it to the swapchain. Exits on
 * window close or Escape, or after N presented frames with `--frames N`.
 * `--headless` runs the self-checking cases of run_headless instead.
 * @param argc Argument count.
 * @param argv Arguments; `--frames N` sets the presented-frame limit.
 * @return 0 on a clean run, 1 when SDL, the device or the GPU resources fail
 *         to initialise; under `--headless`, run_headless's status. */
int main(int argc, char **argv)
{
    long frame_limit = -1; /* -1 = run until quit */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) return run_headless();
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frame_limit = strtol(argv[++i], NULL, 10);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    rhi_loader_env_prepare(NULL);   /* before SDL_WINDOW_VULKAN loads the loader */
    SDL_Window *win = SDL_CreateWindow("openpete rhi smoketest", 1280, 720,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    rhi_device *dev = rhi_create(win, "openpete-rhi-smoketest", /*vsync=*/-1, NULL);
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
        { -0.45f, -0.18f, 0.0f, 0.0f, 0.0f, 0xFF0000FFu, 0, .w = 0.0f }, /* red    */
        {  0.45f, -0.18f, 0.0f, 1.0f, 0.0f, 0xFF00FF00u, 0, .w = 0.0f }, /* green  */
        {  0.45f, -0.92f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u, 0, .w = 0.0f }, /* blue   */
        { -0.45f, -0.18f, 0.0f, 0.0f, 0.0f, 0xFF0000FFu, 0, .w = 0.0f }, /* red    */
        {  0.45f, -0.92f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u, 0, .w = 0.0f }, /* blue   */
        { -0.45f, -0.92f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0, .w = 0.0f }, /* white  */
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

        /* Cycles through the hue wheel so the window is visibly live. */
        float rgb[3];
        hue_ramp(frame, rgb);
        frame++;

        if (!rhi_begin_frame(dev)) {
            /* Swapchain recreating or window minimized; try again. */
            SDL_Delay(8);
            continue;
        }
        rhi_begin_offscreen(dev, rgb[0], rgb[1], rgb[2], 1.0f);

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
