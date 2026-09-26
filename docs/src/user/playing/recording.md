# Recording inputs and screenshots

## Input recordings (bk2)

OpenPete records and replays pad input as BizHawk `.bk2` movies (the
Octoshock player 1 pad: buttons and both analog sticks). A replay from the
same start produces the same game, [tick](../help/glossary.md#tick) for tick, which makes a movie a
reproducible record of a run.

### Recording

```
openpete-spyro1 --bk2-record
```

`--bk2-record [PATH]` <!-- id: flag:--bk2-record home --> writes the live pad
input to PATH. With no PATH it writes `<timestamp>.bk2` (for example
`20260925-143000.bk2`) in the bk2 folder
([File locations](../help/file-locations.md)). Recording stops when the
program exits.

Beside the movie it writes `<movie>.args`, the launch flags of the recorded
run minus the `--bk2` flags, so a replay starts the same way. Settings that
came from `openpete.toml` are not in it.

Combined with `--bk2 OLD.bk2`, recording replays the old movie and then keeps
recording live input after its end, extending it into the new file.

### Replaying

```
openpete-spyro1 --bk2 movie.bk2
```

`--bk2 PATH` <!-- id: knob:bk2 --> replays the movie into the pad. A
`movie.bk2.args` file beside it is read as one more settings layer, above
`openpete.toml` and environment variables and below the flags you type. At
the movie's end live input takes over;
`--bk2-stop-on-end` <!-- id: flag:--bk2-stop-on-end --> exits instead.

A movie records the level-load delay it was made under. A replay under a
different delay refuses to start and names the `--load-delay`
<!-- id: knob:load_delay --> value to pass.

!!! warning "What a replay needs"
    The same disc image, the same set of game-changing mods, and a start
    that matches the recording (the `.args` file covers the flags). The free
    camera cannot be turned on while a movie replays or records. Cheats, the
    level warp and the teleport change the game; a movie recorded with them
    replays correctly only with the same actions at the same ticks.

## Screenshots

++f12++ <!-- id: key:host.screenshot home --> saves a PNG of the picture on screen
as `openpete-<timestamp>.png` (for example `openpete-20260925-143000.png`,
with `-2`, `-3` and so on appended for more shots in the same second) in the
screenshots folder ([File locations](../help/file-locations.md)).

The image is the native renderer's output: widescreen, mods and the in-game
overlays it draws are in it. The dev overlay panel is not.

## Video

The released packages are built without video recording. The `--record`
flags in `--help` apply only to builds made from source with video
encoding enabled.
