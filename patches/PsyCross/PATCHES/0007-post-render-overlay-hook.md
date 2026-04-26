# 0007: post-render overlay hook

## Symptom

There's no clean external seam in PsyCross for openpete to draw on top of
the just-finished scene before it's presented. We need this for the host's
FPS counter (`core/runtime/fps_overlay.cpp`) — text rendered via direct GL
on the GL backbuffer, sitting above the game's rasterised output, drawn
once per `PsyX_EndScene` regardless of game state.

Without a hook the only options were:

- Inject a primitive into the game's OT chain. Subject to the game's
  current draw-env clip/ofs and double-buffer alternation, which made the
  overlay flicker on Spyro and required the chain to fight Spyro's HUD/menu
  layers for visibility.
- Replace `PsyX_EndScene` with our own `GR_EndScene` + `GR_StoreFrameBuffer`
  + custom-draw + `GR_SwapWindow` sequence. Doesn't work without exposing
  PsyCross's static `begin_scene_flag` / `activeDispEnv` state.

## Fix

Add a function-pointer hook `g_psyx_postRenderOverlay` (declared in
`PsyX_public.h`, defined in `PsyX_main.cpp`), invoked inside
`PsyX_EndScene` at exactly one spot:

- AFTER `GR_StoreFrameBuffer` — the VRAM mirror snapshot is taken first,
  so the overlay never bleeds into the game's emulated VRAM.
- BEFORE `GR_SwapWindow` — the overlay lands on the backbuffer that's
  about to be presented.

Default value is `NULL`, so the hook is opt-in and a no-op for any
PsyCross consumer that doesn't register a callback. The callback is
responsible for save/restore of any GL state it touches (matching the
expectations of the existing `g_dbg_gameDebug*` callbacks PsyCross
already exposes).

## Verification

Spyro 1, NTSC, openpete linux-release.

- Boot via `scripts/run-windows-mingw.sh` defaults; observe "FPS:60" in
  the top-left of the GL window in yellow. Screenshot at frame 893
  (Insomniac splash) confirms the overlay is rendered into the presented
  backbuffer.
- Disable hook (set `g_psyx_postRenderOverlay = NULL`) and rebuild —
  game renders identically to pre-patch behaviour, no overlay drawn.
  Confirmed bit-identical render path when callback is unset.

## Notes

- The overlay implementation in `core/runtime/fps_overlay.cpp` saves
  every GL knob it touches (`GL_CURRENT_PROGRAM`,
  `GL_VERTEX_ARRAY_BINDING`, `GL_ARRAY_BUFFER_BINDING`, viewport,
  blend/depth/cull/scissor enables) and restores them before returning,
  so PsyCross's next frame `BeginScene` / `DrawAllSplits` sees an
  untouched pipeline.
- This is a generic extension point — any PsyCross consumer can register
  its own overlay (debug HUDs, frame-time graphs, watermarks).
