# 0013 - Tear down GL before destroying the SDL window

## Symptom

Under an EGL-backed context the game runs to completion - all frames render,
the exit line prints - and then segfaults during shutdown. The faulting
address equals RIP and is a plausible-looking mapped-range address rather than
0, and the backtrace is unambiguous:

```
#0  0x00007fffed9fdfc0 in ()
#1  GR_Shutdown() at src/render/PsyX_render.cpp:451
#2  PsyX_Shutdown() at src/PsyX_main.cpp:1153
```

`PsyX_render.cpp:451` is the first statement of `GR_Shutdown` -
`glDeleteVertexArrays`.

## Root cause

`PsyX_Shutdown()` called `SDL_DestroyWindow(g_window)` and only then
`GR_Shutdown()`. Destroying the last window makes SDL drop the GL context and
unload the GL library. Under EGL nothing else holds that library open, so the
`dlclose` actually unmaps it and every entry point resolved into it is left
dangling - `GR_Shutdown`'s first `glDelete*` jumps into an unmapped page.

Under GLX the identical ordering survives only by luck: the library stays
mapped because something else still references it. The ordering was always
wrong; EGL is just where it stops being survivable.

## Fix

Move the `GR_Shutdown()` call above the `SDL_DestroyWindow` block. GL objects
are now deleted while the context that owns them is still current, which is
the only correct order regardless of backend.

## Verification

Spyro 1, NTSC, openpete `linux-release`, 200-frame runs on WSL2 (Mesa
D3D12/GL 4.2). `SDL_VIDEO_FORCE_EGL=1` and `SDL_VIDEODRIVER=wayland` both went
from SIGSEGV in `GR_Shutdown` to a clean exit 0; GLX, `--headless` and the
default Vulkan display were exit 0 before and after.
