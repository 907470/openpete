# 0004 — Renderer bad-rect clamp (don't abort on degenerate VRAM upload)

`PsyX_render.cpp::GR_CopyRGBAFramebufferToVRAM` originally asserted
`x >= 0`, `y >= 0`, `x + w <= VRAM_WIDTH`, `y + h <= VRAM_WIDTH`. The
function is called once per `PsyX_EndScene` with `activeDispEnv.disp.{x,y,w,h}`
as parameters.

In Spyro 1 we observed two failure modes that aborted with this assertion:

1. **Degenerate `(x=0, y=0, w=0, h=0)`** during the initial frames before
   the game has set up its first `PutDispEnv`. The default
   `g_PreviousFramebuffer = {0,0,0,0}` initializer falls into this case
   if `framebuffer_need_update` was set without a prior store. Harmless;
   the right answer is to skip the upload, not abort.

2. **Garbage rect like `(x=-4327, y=-4239, w=-4036, h=-3873)`** after the
   recompiled code went off the rails (uninitialized stack memory leaking
   into `activeDispEnv`). The root cause turned out to be missing forced
   entry points in psxrecomp's main-EXE pass (`0x80024054` is a computed-
   jump target inside `r_pete`, `0x80032AB0` is a small util only reached
   from the titlescreen overlay) — those were added as `--forced-entry=`
   flags in the regen step. But even with the underlying recomp gap fixed,
   we keep this clamp as a generic safety net: **a degenerate rect should
   skip-and-warn, not abort**, so any future codegen / state-machine
   surprise leaves a recoverable trail rather than a SIGABRT.

The clamp warns (rate-limited to 6 messages) and returns early without
touching VRAM. `g_PreviousFramebuffer` retains whatever it had previously,
which is what we want for the no-update path.

## Files changed

- `src/render/PsyX_render.cpp` — replace the four `assert(...)` calls in
  `GR_CopyRGBAFramebufferToVRAM` with a single early-return guard that
  also rejects `w <= 0` / `h <= 0` (originally allowed by the asserts but
  meaningless: `malloc(0 * 0)` then a no-op double loop).
