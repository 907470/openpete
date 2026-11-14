# Glossary

Terms of the codebase as the developer pages and the source use them. The
player-facing terms are in the User [glossary](../user/help/glossary.md);
the mod-facing ones in the Modding [glossary](../modding/glossary.md).

## Clocks

The codebase has several clocks, and "frame" names three of them, so the
clock is always named.

| Term | Meaning |
|---|---|
| tick | One simulation step of the game: 1125000/37609 Hz (about 29.913 per second). Gameplay state advances only on a tick; the oracle and the gameplay hash log compare state per tick. |
| VBL | The PSX vertical blank: 2250000/37609 Hz (about 59.826 per second), twice the tick rate. `--frame-limit` counts feed keys: a VBL on Spyro 2 and 3, one of the game's yields on Spyro the Dragon ([Unattended runs](unattended-runs.md)). |
| present | One image the host submits to the display. A windowed run targets `--render-fps` presents per second, by default the display's refresh rate, independent of the tick rate; a headless run emits no intermediate presents. |
| canonical, sub-tick | The scene extracted at a tick is canonical. A scene re-extracted between two ticks for a present is sub-tick, at an alpha between 0 and 1 computed from the present clock and the tick stamps. |

## Moby models

A moby's model-table entry selects one of two model kinds, and the game
draws each kind in its own pass.

| Term | Meaning |
|---|---|
| animated model | The keyframe-animated moby model. Its geometry is a vertex stream per keyframe; the draw takes keyframe A, blends it toward keyframe B by the cull record's blend byte when that is nonzero, and applies the keyframe's faceblock patch list where the game's model carries one. The animation moves vertices per keyframe; there is no bone hierarchy. Drawn by the game's regular moby pass (`hook_draw_regular`). Selector: on Spyro the Dragon the entry's word 0, the animation count, is non-negative (`s1_model_is_animated`); on Spyro 2 and 3 the entry is a KSEG0 pointer (`s23_model_is_animated`). |
| simple model, simple moby, second model kind | The moby model kind without keyframes (`SimpleModel` in the Spyro the Dragon decomp); a simple moby is a moby whose model is one. On Spyro the Dragon the entry's animation count is negative and the shaded pass draws the moby from its own `Moby*` list. On Spyro 2 and 3 the entry is a KUSEG pointer; the cull writes the moby into a second queue (stride 0x10, at draw scratch +0x1C00, at most 63 records) that the game's static draw reads, and openpete decodes that queue in `moby_game.simple_pass` and emits it through the `moby_simple_` calls (`moby_simple_record`, `moby_simple_project_record`, `moby_simple_tri`, declared in `games/common/renderer/moby.h`). A Spyro 2 or 3 record whose far-distance half has bit 15 set is placed in screen space (`moby_simple_screen_origin`). The tile census counts these triangles as `moby.simple.tris` and `moby.screen.tris` ([Tile census](tile-census.md)). |
