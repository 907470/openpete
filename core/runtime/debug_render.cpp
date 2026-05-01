// core/runtime/debug_render.cpp — Phase 1e infra-triangle.
//
// Issues a single hard-coded POLY_F3 through PsyCross every frame so we can
// verify the BeginScene → DrawOTag → EndScene → SwapWindow → SDL window
// pipeline end-to-end without depending on real game content (the actual
// Spyro draw path lives in the title-screen overlay, which is untranslated
// during Phase 1d). Toggled with `--debug-triangle` so it doesn't pollute
// the real-game render path once overlay translation lands.
//
// Compiled as C++ because PsyCross's libgpu.h uses static_assert on struct
// sizes; we expose a single C entry point for run.c.

#include "psx/libgte.h"  // SVECTOR forward referenced by libgpu.h
#include "psx/libgpu.h"
#include "PsyX/common/glad.h"
#include "PsyX/PsyX_public.h"
#include "screenshot.h"
#include "stb_image_write.h"
#include <SDL.h>

#include <cstdlib>
#include <cstring>

static POLY_F3 s_tri;
static u_long  s_ot[2];
static DRAWENV s_drawenv;
static int     s_initialised = 0;

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
     * whatever the game has set, since at Phase 1d Spyro's draw envs come
     * from the title-screen overlay (untranslated). */
    std::memset(&s_drawenv, 0, sizeof(s_drawenv));
    SetDefDrawEnv(&s_drawenv, 0, 0, 512, 240);
    s_drawenv.isbg = 1;
    setRGB0(&s_drawenv, 0x00, 0x40, 0x60);  /* dark teal */
}

/* Screenshot dumper. PsyCross ships a `PsyX_TakeScreenshot()` but it produces
 * an upside-down image: glReadPixels returns rows bottom-up (GL origin is
 * bottom-left), SDL_CreateRGBSurfaceFrom treats row 0 as the surface top,
 * and SDL_SaveBMP then re-flips during BMP encoding — net effect, one extra
 * flip. We re-implement here with an explicit row flip. */
extern "C" void openpete_psyx_take_screenshot(const char* path) {
    int w = 0, h = 0;
    PsyX_GetScreenSize(&w, &h);
    if (w <= 0 || h <= 0) return;

    const size_t pitch = (size_t)w * 4u;
    unsigned char* pixels = (unsigned char*)std::malloc(pitch * (size_t)h);
    if (!pixels) return;

    const int is_png = openpete_screenshot_path_is_png(path);
    /* PNG wants RGBA byte order; BMP path keeps the BGRA mask layout it
     * already used. Read in the format the destination needs to skip a
     * channel-swap. */
    glReadPixels(0, 0, w, h, is_png ? GL_RGBA : GL_BGRA, GL_UNSIGNED_BYTE, pixels);

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
        stbi_write_png(out_path, w, h, 4, pixels, (int)pitch);
    } else {
        SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
            pixels, w, h, /*depth=*/32, /*pitch=*/(int)pitch,
            /*Rmask=*/0x00FF0000u, /*Gmask=*/0x0000FF00u,
            /*Bmask=*/0x000000FFu, /*Amask=*/0xFF000000u);
        if (surface) {
            SDL_SaveBMP(surface, out_path);
            SDL_FreeSurface(surface);
        }
    }
    std::free(pixels);
}

extern "C" void openpete_debug_triangle_emit(void) {
    if (!s_initialised) openpete_debug_triangle_init();

    /* Force our own draw env each frame so the triangle has a known clip
     * rect and clear color — Spyro's draw env may not yet be set. */
    PutDrawEnv(&s_drawenv);

    /* Hand-build a single-prim OT. PsyCross's ClearOTag has a quirk where
     * its initialisation loop overwrites the terminator entry; safer to
     * build the chain ourselves: s_ot[0] (head, len=0) → s_tri → terminator.
     * `addPrim` modifies its prim's tag-address, so we re-issue both the
     * head and the prim's link target every frame. */
    setlen(&s_ot[0], 0);
    setaddr(&s_ot[0], &s_tri);
    termPrim(&s_tri);
    DrawOTag(s_ot);
}
