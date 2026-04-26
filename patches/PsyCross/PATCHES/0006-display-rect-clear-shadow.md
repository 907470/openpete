# 0006 — Display-rect clear shadow (replay at BeginScene)

PsyCross is direct-rasterize-to-GL: PSX primitives are converted to GL
triangles drawn straight into the default framebuffer's color buffer.
There is no FBO-backed "VRAM" that gets blit-presented at vblank — the
GL backbuffer **is** the "front-displayable VRAM rect" for the current
frame.

Two consequences:

1. After `SDL_GL_SwapWindow` the new GL backbuffer's contents are
   driver-defined and unspecified. None of the possible states correspond
   to PSX semantics, where each scanout is a fresh read from VRAM at
   `activeDispEnv.disp`.
2. `PsyX_BeginScene` clears depth/stencil unconditionally but only clears
   color when `activeDrawEnv.isbg` is set (the upstream `// TODO: clear
   all affected backbuffers` comment in `PsyX_main.cpp:817` flags exactly
   this gap).

Spyro's main render path keeps `isbg=1`, so the implicit clear hides the
problem. Loading transitions and a handful of other spots use `isbg=0`
with manual `ClearImage` calls that update PsyCross's emulated VRAM but
land on a *different* GL backbuffer than the one the next frame draws
into — so prior pixels persist and smear with new content.

## Fix

Shadow the most recent solid-color clear that touched the active display
rect, and replay it at every `BeginScene` regardless of `isbg`:

- `ClearImage` / `ClearImage2` call `PsyX_NoteClearForDispRect` after
  the existing `GR_Clear` so the shadow tracks the latest user-driven
  fill.
- `PsyX_BeginScene` calls `PsyX_ReplayDispRectClearShadow` immediately
  after `GR_BeginScene()`, before the existing `if (activeDrawEnv.isbg)`
  block. The replay is a no-op until the first arming clear lands.
- The existing `isbg=1` path keeps its `GR_Clear` and additionally
  refreshes the shadow, so a subsequent `isbg=0` frame has the right
  colour to replay.
- `PutDispEnv` invalidates the shadow whenever the rect changes —
  Spyro re-issues `ClearImage` immediately afterwards, so this is just
  defence-in-depth against re-stamping a colour into a now-dead VRAM
  region.

## What this didn't conclusively verify

The headless A/B harness (`--screenshot-on-frame`) captures
`glReadPixels` of the active framebuffer rather than what would actually
appear after `SwapWindow`, so a screenshot at any single frame can't see
the post-flip stale-backbuffer artifact. Frame 4500 (gameplay) is
byte-identical pre/post — confirms zero regression on the `isbg=1`
gameplay path. The smear visible in the loading transition is best
verified interactively in `scripts/run-windows-mingw.sh`.

## Files changed

- `include/PsyX/PsyX_render.h` — public API for the shadow.
- `src/render/PsyX_render.cpp` — storage + the three helpers
  (`PsyX_NoteClearForDispRect`, `PsyX_InvalidateDispRectClearShadow`,
  `PsyX_ReplayDispRectClearShadow`).
- `src/psx/LIBGPU.C` — arm the shadow from `ClearImage` /
  `ClearImage2`; invalidate from `PutDispEnv`.
- `src/PsyX_main.cpp` — replay before, refresh after the existing
  isbg block in `PsyX_BeginScene`.

## Plan reference

See `notes/plans/psycross-loading-smear.md` for the full diagnosis,
PSX scanout semantics cross-referenced against PSYQ 4.0, and the
out-of-scope notes (multi-buffer, interlace, true VRAM-as-FBO).
