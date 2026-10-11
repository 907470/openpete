# glyph-v-fix

Restores the missing end-cap face on the 3D letter `V`.

## The defect

The game draws its gold 3D text - dragon-rescue names, `DEMO MODE`, the
pause and inventory menus, the gem and life counters - with one moby
class per character, `A`-`Z` at `426 + (c - 'A')`. Each glyph is a
SimpleModel.

The `V` model (class 447) carries 18 vertices and 17 faces: six front,
six back, and five side walls. The right arm has all three of the walls
an arm needs - outer edge, top end cap, inner edge - and the left arm
has only two. The quad closing the left arm's top is absent, so the
top-left of every 3D `V` opens into the glyph's unlit interior.

The face is missing from the asset, not from the way openpete draws it:
the reference PsyCross path and the native path render the hole
identically.

## The fix

The face stream cannot grow where it lies - the `W` model header begins
at the byte after the last `V` face - so the mod copies the 17 faces to
the mod arena, appends the missing quad, and points the model header's
face count and stream pointer at the copy.

The missing quad needs no new vertices: its four corners are already in
the model, shared with the two walls that meet at the arm's top. Nothing
is hardcoded from the asset. The faces are copied from whatever the game
loaded, and the new face's normal is computed from those four corners
and scaled to the length of 128 the stock faces use.

The font asset is evicted and reloaded across level and cutscene loads,
so the patch is applied from a per-tick hook that re-fires whenever a
freshly loaded model appears in the row, and is a no-op once the header
already points at the arena.

## Config

| Key | Default | Effect |
| --- | --- | --- |
| `glyph_v.enabled` | `true` | Install the end cap |
| `debug.dump` | `false` | Log the decoded vertex and face streams of the `V` model once |

## Verifying

`town-square-delvin-rescue.bk2` frame 2574 holds `RESCUED DEVLIN` with
the `V` filling a good part of the screen:

```
build/linux-release/openpete --headless --no-config \
  --disc games/spyro-1/spyro-1/build/SCUS-94228.cue --run-recomp 0x80012204 \
  --renderer vk --mute 1 --aspect 16:9 --mods-dir mods --mods glyph-v-fix \
  --bk2 games/spyro-1/bk2/town-square-delvin-rescue.bk2 \
  --screenshot-on-frame 2574 --screenshot-output /tmp/v{n}.png --frame-limit 2576
```

Against the same run without `--mods`, the cap changes ~50 pixels around
(390, 362) and nothing else in the frame, on the `-og` and `-vk` legs
alike.
