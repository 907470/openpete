# egg-hud-3d - 3D egg icons

The game draws its collected-egg icons (the HUD row after a pickup, the
inventory's global row, and the egg marks beside each level name) as a
nine-cell flipbook: a pre-rendered egg, one cell per tick. At the game's
30 Hz that reads as a rotating egg; at a high `--render-fps` it steps,
because there is nothing between the cells to interpolate.

This mod puts the game's own dragon egg (moby class 34) in the sprite's
place. The model is one of the shared PETE.WAD models the game keeps
resident in every level, so nothing is extracted or shipped. Each egg is
drawn as a HUD billboard at the sprite's exact rectangle, rotated to the
cell the sprite would have shown, and the engine's HUD-moby interpolation
spins and slides it smoothly between ticks.

The inventory's dark "uncollected" marks are static and keep their sprite.

## How it works

- `func_8001919C` (the egg sprite builder) is overridden. A live egg is
  recorded (rectangle + cell) and the original is not called, so no sprite
  is drawn. Everything else passes through.
- `func_80022A2C` (the shaded-moby renderer, the one call every screen
  makes) is pre-hooked: one parked `Moby` per record is appended to the
  render list, then the original runs.
- Mobys live in a 42-slot park in the guest arena, one fixed slot per
  emitter position, so each egg keeps a stable address for the engine's
  per-object interpolation.

Guest writes: the render list (scratch, rebuilt every tick) and the park.
The game's egg state machine and layout are untouched.

## Cost

About 1.75 KB of polybuf per egg (the original renderer still builds the
prims). A full 12-egg HUD row costs ~21 KB against a measured worst-case
headroom of ~37 KB in the corpus; appends are capped at the list's free
entries.

## Repro

```
openpete-spyro1 --mods egg-hud-3d --bk2 games/spyro-1/bk2/egg-inventory.bk2
```

HUD egg from ~frame 3603; inventory open from ~frame 3760.
