/* core/runtime/native_render.c — native Vulkan renderer A/B harness.
 *
 * Phase 1c of notes/plans/openpete-native-renderer.md. See
 * openpete_native_render.h for the contract + the two-window rationale.
 *
 * 1c.1 (this step): stand the RHI up inside the real runtime. A second SDL
 * window (Vulkan) presents a live cycling clear alongside the still-visible
 * PsyCross (OpenGL) window — proving GL and Vulkan coexist in one process,
 * which is the only real unknown before the native renderer can grow. 1c.2
 * replaces the clear with PsyCross's read-back frame (the reference); 1c.3
 * adds the native VRAM render + the native/reference/diff composite.
 *
 * Determinism: nothing here touches PSX RAM or the `.s` verdict producers.
 */

#include "openpete_native_render.h"

#include "openpete_log.h"
#include "rhi.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdbool.h>

/* Default A/B window size when the user didn't pin --window. Wide enough for
 * the native/reference/diff three-panel composite that lands in 1c.3. */
#define NR_DEFAULT_W 1280
#define NR_DEFAULT_H 720

/* Single native renderer instance (one window per process). */
static struct {
    bool          enabled;
    SDL_Window   *window;
    rhi_device   *dev;
    int           win_w, win_h; /* last-known pixel size, for resize detection */
    unsigned long frame;        /* presented native frames (drives the 1c.1 ramp) */
} s_nr;

int openpete_native_render_init(const openpete_options_t *opts)
{
    if (!opts || !opts->native_ab)
        return 0; /* not requested — inert no-op */

    if (opts->headless) {
        /* No display surface to present to; the A/B view is meaningless. A
         * future headless-diff mode (render both to memory + compare) would
         * be a separate path. */
        OPL_WARN_F("native", "--native-ab ignored under --headless (no display)");
        return 0;
    }

    /* SDL_INIT_VIDEO is already up (PsyX_Initialise). Create a SECOND window
     * for the Vulkan surface: a single SDL window can't carry both PsyCross's
     * GL context and our Vulkan surface, so the native renderer owns its own.
     * (1c.2 will hide PsyCross's window and this becomes the only visible one.) */
    int w = (opts->window_w > 0) ? opts->window_w : NR_DEFAULT_W;
    int h = (opts->window_h > 0) ? opts->window_h : NR_DEFAULT_H;

    SDL_Window *win = SDL_CreateWindow("openpete — native renderer (A/B)", w, h,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        OPL_ERROR_F("native", "SDL_CreateWindow(VULKAN) failed: %s", SDL_GetError());
        return 1;
    }

    rhi_device *dev = rhi_create(win, "openpete-native");
    if (!dev) {
        OPL_ERROR_F("native", "rhi_create failed; continuing without native A/B");
        SDL_DestroyWindow(win);
        return 1;
    }

    s_nr.enabled = true;
    s_nr.window  = win;
    s_nr.dev     = dev;
    s_nr.win_w   = w;
    s_nr.win_h   = h;
    s_nr.frame   = 0;
    OPL_INFO_F("native", "Vulkan A/B window up (%dx%d, device='%s')",
               w, h, rhi_device_name(dev));
    return 0;
}

bool openpete_native_render_enabled(void)
{
    return s_nr.enabled;
}

void openpete_native_render_frame(void)
{
    if (!s_nr.enabled)
        return;

    /* Detect window-size changes WITHOUT polling SDL events (the host loop
     * owns the event queue, and stealing events would starve PsyCross's
     * input). A per-frame size query is cheap. The RHI also recreates the
     * swapchain on its own when a present comes back out-of-date, so this is
     * belt-and-suspenders for a clean resize. */
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(s_nr.window, &pw, &ph) && pw > 0 && ph > 0 &&
        (pw != s_nr.win_w || ph != s_nr.win_h)) {
        s_nr.win_w = pw;
        s_nr.win_h = ph;
        rhi_notify_resize(s_nr.dev);
    }

    if (!rhi_begin_frame(s_nr.dev))
        return; /* swapchain recreating or window minimized — skip this present */

    /* 1c.1: a cycling clear color. Its only job is to be visibly LIVE next to
     * the PsyCross window, demonstrating that a Vulkan present loop runs in
     * the same process as PsyCross's GL render. Integer hue ramp (no libm). */
    unsigned p = (unsigned)(s_nr.frame % 768u);
    float r, g, b;
    if (p < 256u) {
        r = (255u - p) / 255.0f; g = p / 255.0f; b = 0.0f;
    } else if (p < 512u) {
        unsigned q = p - 256u; r = 0.0f; g = (255u - q) / 255.0f; b = q / 255.0f;
    } else {
        unsigned q = p - 512u; r = q / 255.0f; g = 0.0f; b = (255u - q) / 255.0f;
    }
    s_nr.frame++;

    rhi_begin_offscreen(s_nr.dev, r, g, b, 1.0f);
    rhi_end_offscreen(s_nr.dev);
    rhi_blit_to_swapchain(s_nr.dev);
    rhi_end_frame(s_nr.dev);
}

void openpete_native_render_shutdown(void)
{
    if (!s_nr.enabled)
        return;
    rhi_wait_idle(s_nr.dev);
    rhi_destroy(s_nr.dev);
    SDL_DestroyWindow(s_nr.window);
    s_nr.dev     = NULL;
    s_nr.window  = NULL;
    s_nr.enabled = false;
    OPL_INFO_F("native", "Vulkan A/B window shut down");
}
