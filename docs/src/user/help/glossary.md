# Glossary

The User pages use the terms players use. Each entry names the term the
program, its flags and its developer documentation use for the same thing,
and for a timing term, which clock it counts.

OpenPete runs three clocks:

| Clock | Rate | Counts |
| --- | --- | --- |
| tick | 1125000/37609 Hz, about 29.913 per second | steps of the game's own logic |
| VBL | 2250000/37609 Hz, about 59.826 per second | the console's display refreshes; two per tick |
| present | the render rate, up to your monitor's refresh rate by default | images OpenPete shows in the window |

"Frame" can mean any of the three, so the developer documentation does not
use it for a clock.

## Timing

### Tick

Codebase term: tick. Clock: the tick. One step of the game's logic: Spyro
moves, enemies act and the game reads the controller once per tick. The
game's speed is fixed at the tick rate whatever your frame rate.

### VBL

Codebase term: VBL (vertical blank). Clock: the VBL. One display refresh of
the original console. The game waits for two VBLs per tick. Timings in
input recordings and many developer flags count VBLs.

### Frame rate

Codebase term: present rate. Clock: the present. How many images per second
OpenPete shows. `--render-fps`<!-- id: flag:--render-fps --> (the **FPS**
row of the settings menu) sets the target; the default is your monitor's
refresh rate. The frame rate is independent of the game's speed.

### FPS

Codebase term: presents per second. Clock: the present. The number the
**SHOW FPS** row and `--perf-stats`<!-- id: flag:--perf-stats --> display.

### Frame pacing

Codebase term: present pacing. Clock: the present, measured against the
VBL. How evenly the images are spaced in time. Uneven spacing shows as
stutter even at a high frame rate.

### Frame interpolation

Codebase term: interpolation (the `interp_toggle` key action). Clock: the
present, between two ticks. Drawing images between two game ticks, so
motion is smooth at frame rates above the tick rate. It changes only the
image; the game runs the same.

### Frame advance

Codebase term: tick step (the `frame_step` key action). Clock: the tick.
While paused, ++f8++ advances the game by one tick.

### Fast-forward

Codebase term: turbo (the `turbo` key action), or unlocked running. Clock:
the tick. Holding ++tab++ runs ticks as fast as the computer allows.

### Slow motion

Codebase term: tick scale (`--tick-scale`<!-- id: flag:--tick-scale -->).
Clock: the tick and the present together. Scales the game's speed.

### Runahead

Codebase term: runahead (`--runahead`<!-- id: flag:--runahead -->). Clock:
the tick. Shows the image a number of ticks ahead of the game's state, to
reduce input lag. The game's state is unchanged.

## Display

### Extended detail

Codebase term: extended LOD (`--lod extended`<!-- id: flag:--lod -->, the
`lod_toggle` key action, the **LOD** row). Draws every visible part of a
level at full detail, instead of the original's lower-detail distant
scenery.

### Supersampling

Codebase term: SSAA (`--ssaa`<!-- id: flag:--ssaa -->). Renders at a
multiple of the window's resolution and scales down, which smooths edges.

### Vertex wobble

Codebase term: integer vertex precision
(`--vertex-precision integer`<!-- id: flag:--vertex-precision -->, the
**WOBBLE** row). Snaps polygons to the original console's pixel grid,
which makes them jitter as on the console.

### Texture warping

Codebase term: affine texture mapping
(`--texture-mapping affine`<!-- id: flag:--texture-mapping -->, the
**TEXTURES** row). Maps textures as the original console did, which bends
them across large polygons.

### Screenshot

Codebase term: screenshot (the `screenshot` key action, ++f12++). A PNG of
the image in the window, as shown.

## Play

### Settings menu

Codebase term: the O menu (the `menu` key action). The in-game **SETTINGS**
panel.

### Dev overlay

Codebase term: devui (the `devui` key action). The panel listing every
setting, the cheat buttons, the level warp and the mods.

### Free camera

Codebase term: freecam (the `freecam` key action). A camera you fly
independently of Spyro.

### Savestate

Codebase term: savestate. A snapshot of the whole game at one tick, saved
and loaded with OpenPete's keys. Separate from the game's own saves on the
memory card.

### Input recording

Codebase term: bk2 (a BizHawk movie file). A record of the controller input,
one entry per VBL, that replays the same game.

### Library

Codebase term: disc library. Where OpenPete keeps its copy of your disc
image and the game code compiled from it.

### First-run compile

Codebase terms: ingest and the first-boot module build. Copying your disc
into the library and compiling the game's code for your computer, once per
disc and once per OpenPete release.

### Code mod

Codebase term: source mod. A mod with a `src/` folder, compiled on your
computer.

## Ids that contain "frame"

These flags and key actions keep "frame" in their names. The table gives the
clock each one counts.

| Id | Counts |
| --- | --- |
| `frame_step` (key action) | ticks: advances one tick |
| `--record-frames N`<!-- id: flag:--record-frames --> | encoded video images of a recording |
| `--frame-limit N`<!-- id: flag:--frame-limit --> | VBLs: exits after N |
| `--bk2-frame-offset N`<!-- id: flag:--bk2-frame-offset --> | VBLs: the VBL at which a recording's first input entry plays |
| `--screenshot-on-frame N`<!-- id: flag:--screenshot-on-frame --> | VBLs |
| `--screenshot-intermediate-on-frame N`<!-- id: flag:--screenshot-intermediate-on-frame --> | VBLs; captures an image between two ticks |
| `--state-save-on-frame N`, `--state-load-on-frame N`<!-- id: flag:--state-save-on-frame --> | VBLs |
| `--ram-dump-on-frame N`<!-- id: flag:--ram-dump-on-frame --> | VBLs |
| `--unlock-frames LIST`<!-- id: flag:--unlock-frames --> | VBLs: ranges to fast-forward |
| `--frame-time-log PATH`<!-- id: flag:--frame-time-log --> | presents: one record per image shown |
| `--wireframe`<!-- id: flag:--wireframe --> | no clock: draws polygon edges only |

`--bk2-frame-offset` is listed in `--help`. `--record-frames` exists only in
builds with video recording, which no release package includes. The others
are developer flags, which a release package's `--help` does not list.
