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

/* PsyCross's screenshot dumper. Implemented in PsyX_main.cpp without extern
 * "C", so we declare it in C++ linkage and surface a C-callable wrapper for
 * run.c. Writes ./SCREENSHOT.BMP via glReadPixels. */
void PsyX_TakeScreenshot();
extern "C" void openpete_psyx_take_screenshot(void) {
    PsyX_TakeScreenshot();
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
