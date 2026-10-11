// core/renderer/debug_render.cpp - the `--debug-triangle` infra-triangle.
//
// Issues a single hard-coded POLY_F3 through PsyCross every frame to verify
// the BeginScene -> DrawOTag -> EndScene -> SwapWindow -> SDL window pipeline
// end-to-end without depending on game content. Toggled with
// `--debug-triangle` so it doesn't pollute the real-game render path.
//
// Compiled as C++ because PsyCross's libgpu.h uses static_assert on struct
// sizes; we expose a single C entry point for run.c.

#include "psx/libgte.h"  // SVECTOR forward referenced by libgpu.h
#include "psx/libgpu.h"
#include "PsyX/common/glad.h"
#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_render.h"
#include "screenshot.h"
#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>

static POLY_F3 s_tri;
static u_long  s_ot[2];
static DRAWENV s_drawenv;
static int     s_initialised = 0;

/** @brief Build the debug triangle primitive and its draw environment once.
 *
 * Fills a flat red-orange POLY_F3 inside the 512x240 viewport and a
 * DRAWENV with a dark teal background clear, so a black screenshot means
 * nothing rendered and a teal one without the triangle means it was
 * clipped. Later calls
 * return at once; openpete_debug_triangle_emit calls it on first use. */
extern "C" void openpete_debug_triangle_init(void) {
    if (s_initialised) return;
    s_initialised = 1;

    /* Big red-orange triangle in PSX coordinate space. Spyro's draw envs
     * clip to (0, 8, 512, 224); we keep all three vertices inside that
     * rectangle (relative to the draw env's `ofs`, which Spyro puts at
     * (0,0) and (0,240) for the two buffers). */
    std::memset(&s_tri, 0, sizeof(s_tri));
    setPolyF3(&s_tri);
    setRGB0(&s_tri, 0xFF, 0x40, 0x20);
    setXY3(&s_tri,
           256, 24,    /* top */
            48, 200,   /* bottom-left */
           464, 200);  /* bottom-right */

    /* Default draw env covering the standard 512x240 PSX viewport with a
     * teal background (so a black screenshot means "nothing rendered" vs
     * "rendered but clipped"). We `PutDrawEnv` this every frame to override
     * whatever the game has set. */
    std::memset(&s_drawenv, 0, sizeof(s_drawenv));
    SetDefDrawEnv(&s_drawenv, 0, 0, 512, 240);
    s_drawenv.isbg = 1;
    setRGB0(&s_drawenv, 0x00, 0x40, 0x60);  /* dark teal */
}

/** @brief Save the PsyCross scene to a PNG or BMP file chosen by the path's extension.
 *
 * Reads the present FBO at internal resolution, cropped to the PSX visible
 * region, so the image carries no window bars and does not depend on the
 * window size; with no present FBO it presents the cropped scene to the
 * default framebuffer and reads the window instead. Rows are flipped to
 * top-down, since glReadPixels returns them bottom-up. A PNG is read as
 * RGBA with every alpha forced to 0xFF; any other extension is read as
 * BGRA and written with SDL_SaveBMP. Writes nothing in PsyCross null-GPU
 * mode, which has no GL context.
 * @param path Output path; NULL writes SCREENSHOT.BMP.
 * @param out_w Receives the image width, 0 when nothing was read; may be
 *        NULL.
 * @param out_h Receives the image height, 0 when nothing was read; may be
 *        NULL. */
extern "C" void openpete_psyx_take_screenshot_ext(const char* path,
                                                 int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    /* PsyCross in null-GPU library mode has no GL context - every gl* pointer
     * is unloaded and GR_PresentCroppedToWindow/glReadPixels would segfault.
     * The -og reference screenshot simply doesn't exist in that mode. */
    if (g_psyx_null_gpu) {
        SDL_Log("[screenshot] -og skipped (%s): PsyCross null-GPU library mode",
                path ? path : "?");
        return;
    }
    const int is_png = openpete_screenshot_path_is_png(path);
    /* PNG wants RGBA byte order; BMP path keeps the BGRA mask layout it
     * already used. Read in the format the destination needs to skip a
     * channel-swap. */
    const GLenum fmt = is_png ? GL_RGBA : GL_BGRA;

    int w = 0, h = 0;
    unsigned char* pixels = nullptr;

    /* Capture the rendered scene at its native internal resolution, cropped
     * to the canonical PSX visible region - read straight from the present
     * FBO, NOT the window backbuffer. The backbuffer carries letter/pillarbox
     * bars whenever the window aspect doesn't match the content, and the
     * screenshot fires before PsyX_EndScene (when the scene still lives in
     * the present FBO and fb 0 is stale). Reading the FBO makes the saved
     * image independent of how the window happens to be sized. */
    if (GR_GetPresentSceneSize(&w, &h) && w > 0 && h > 0) {
        const size_t pitch = (size_t)w * 4u;
        pixels = (unsigned char*)std::malloc(pitch * (size_t)h);
        if (!pixels) return;
        if (!GR_ReadPresentSceneRGBA(pixels, w, h, (unsigned int)fmt)) {
            std::free(pixels);
            pixels = nullptr;
        }
    }

    if (!pixels) {
        /* Fallback: no present FBO (no-FBO / headless path). Present the
         * cropped scene to fb 0 and read the window backbuffer as before. */
        PsyX_GetScreenSize(&w, &h);
        if (w <= 0 || h <= 0) return;
        const size_t pitch = (size_t)w * 4u;
        pixels = (unsigned char*)std::malloc(pitch * (size_t)h);
        if (!pixels) return;
        GR_PresentCroppedToWindow();
        glReadPixels(0, 0, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
    }

    const size_t pitch = (size_t)w * 4u;
    /* In-place row flip: swap row i with row h-1-i. */
    unsigned char* tmp = (unsigned char*)std::malloc(pitch);
    if (tmp) {
        for (int i = 0; i < h / 2; ++i) {
            unsigned char* a = pixels + (size_t)i * pitch;
            unsigned char* b = pixels + (size_t)(h - 1 - i) * pitch;
            std::memcpy(tmp, a, pitch);
            std::memcpy(a, b, pitch);
            std::memcpy(b, tmp, pitch);
        }
        std::free(tmp);
    }

    const char* out_path = path ? path : "SCREENSHOT.BMP";
    if (is_png) {
        /* Force every pixel opaque before encoding. glReadPixels returns
         * the framebuffer's blended scene alpha (typically 0x80..0xFF -
         * only ~1/6 of pixels are fully opaque), which is meaningless for
         * a flat screenshot but makes image viewers alpha-composite the
         * PNG over their transparency checkerboard. The live window is
         * opaque, so the saved PNG looked checkerboarded while the running
         * exe did not. RGBA byte order: alpha is byte 3 of each pixel. */
        for (size_t i = 0; i < (size_t)w * (size_t)h; ++i)
            pixels[i * 4u + 3u] = 0xFFu;
        (void)openpete_write_png_rgba_stride(out_path, pixels, w, h, (int)pitch);
    } else {
        /* glReadPixels(GL_BGRA, GL_UNSIGNED_BYTE) stores bytes B,G,R,A -
         * the 32-bit little-endian value 0xAARRGGBB, i.e. ARGB8888. */
        SDL_Surface* surface = SDL_CreateSurfaceFrom(
            w, h, SDL_PIXELFORMAT_ARGB8888, pixels, (int)pitch);
        if (surface) {
            SDL_SaveBMP(surface, out_path);
            SDL_DestroySurface(surface);
        }
    }
    std::free(pixels);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/** @brief Save the PsyCross scene to a PNG or BMP file, discarding the image size.
 *
 * Calls openpete_psyx_take_screenshot_ext with no size outputs.
 * @param path Output path, PNG or BMP by extension; NULL writes
 *        SCREENSHOT.BMP. */
extern "C" void openpete_psyx_take_screenshot(const char* path) {
    openpete_psyx_take_screenshot_ext(path, nullptr, nullptr);
}

/** @brief Submit the debug triangle for the current scene through PsyCross.
 *
 * Puts the private draw environment, so the clip rect and clear colour
 * are known whatever the game set, then draws a one-primitive ordering
 * table built by hand (head, triangle, terminator) with DrawOTag. The
 * links are rebuilt every call, since PsyCross's ClearOTag would
 * overwrite the terminator. */
extern "C" void openpete_debug_triangle_emit(void) {
    if (!s_initialised) openpete_debug_triangle_init();

    /* Force our own draw env each frame so the triangle has a known clip
     * rect and clear color - Spyro's draw env may not yet be set. */
    PutDrawEnv(&s_drawenv);

    /* Hand-build a single-prim OT. PsyCross's ClearOTag has a quirk where
     * its initialisation loop overwrites the terminator entry; safer to
     * build the chain ourselves: s_ot[0] (head, len=0) -> s_tri -> terminator.
     * `addPrim` modifies its prim's tag-address, so we re-issue both the
     * head and the prim's link target every frame. */
    setlen(&s_ot[0], 0);
    setaddr(&s_ot[0], &s_tri);
    termPrim(&s_tri);
    DrawOTag(s_ot);
}
