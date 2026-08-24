# 0012 — Load GL entry points through SDL, not glad's GLX-only loader

## Symptom

On an EGL-backed GL context the process dies with `SIGSEGV, host RIP=0x0,
fault addr=(nil)`, the host backtrace one frame deep, `addr2line` placing the
caller in `GR_Clear` (`PsyX_render.cpp`) — i.e. `glClearColor`/`glClear` are
NULL function pointers. The context itself is fine; the log prints a real
`*OpenGL version: 4.6 (Core Profile) Mesa …` line immediately before.

Reproduces on any EGL-only host: Wayland sessions without Xwayland, a
display-less ssh session, and X11 with `SDL_VIDEO_FORCE_EGL=1`. It is EGL,
not Wayland — forcing EGL on X11 reproduces it and Wayland with a Vulkan
display does not.

## Root cause

`GR_InitialiseGLExt()` called the **argless** `gladLoadGL()`. glad implements
that (`glad.c`, `open_gl()`) by `dlopen`ing `libGL.so.1` and resolving every
entry point through `glXGetProcAddressARB`, with a `dlsym` fallback. Under EGL
there is no GLX at all, so whether any given symbol resolves depends entirely
on whether that Mesa build happens to export it from `libGL.so.1` directly —
which is why some entry points come back valid and others NULL on the same
machine.

## Fix

Use the loader-taking variant, `gladLoadGLLoader()`, with
`SDL_GL_GetProcAddress`. SDL resolves through whichever of GLX, EGL or WGL
actually backs the context it created, so the loader matches the context by
construction. It also drops glad's private `dlopen`/`dlclose` of `libGL`
entirely — that pairing left the resolved pointers owned by a library glad had
already closed.

Strictly better on every platform, not a Linux special case: SDL's Windows
implementation already handles the `wglGetProcAddress`-returns-NULL-for-GL-1.1
case that glad's `GetProcAddress` fallback was there for.

`SDL3/SDL.h` is already in scope via `../platform.h`; `gladLoadGLLoader` and
`GLADloadproc` are declared in `include/PsyX/common/glad.h`.

See also 0013 — the same EGL configuration exposed a second, independent bug
at shutdown.

## Verification

Spyro 1, NTSC, openpete `linux-release`, 200-frame runs. The null-entry-point
crash itself needs an EGL host whose `libGL.so.1` does not export the core
symbols; the WSL2 dev box's Mesa does, so it cannot reproduce that signature
and this patch is validated there only as a no-regression change:

| config | before | after |
|---|---|---|
| X11 + GLX + `--renderer psycross` | exit 0 | exit 0 |
| X11 + `SDL_VIDEO_FORCE_EGL=1` + psycross | SEGV (0013's) | exit 0 |
| Wayland + psycross | SEGV (0013's) | exit 0 |
| `--headless` | exit 0 | exit 0 |
| default `--renderer vk`, GLX and EGL | exit 0 | exit 0 |
