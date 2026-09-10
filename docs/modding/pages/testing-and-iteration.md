# Iteration and testing

!!! warning "Flags and log output are not a stable interface"

    CLI flags and log-line formats change between releases, without a
    deprecation period and without a version to pin. The mod API is
    versioned and append-only; none of what is on this page is. Check
    `--help` and `--keys-report` against the build in use rather than
    assuming a flag or a key binding named here still exists, and expect
    anything that parses log lines or screen-scrapes `openpete-exit:` to
    need updating.

## Basic iteration

Edit `src/`, `shaders/`, `assets/` or `config.toml`,
start the game.

Compile every mod with a `src/` directory without booting the game:

```
openpete-spyro1 --compile-mods [--mods-dir mods]
```

Exit status is nonzero if any mod failed to compile.

## Logging

`api->log(self, OP_MOD_LOG_INFO, "fmt", ...)` writes one line tagged
`mod:<id>`. Levels are `OP_MOD_LOG_DEBUG`, `_INFO`, `_WARN`, `_ERROR`.

Log lines go to stderr and are mirrored to a file:

| Layout | Path | Default on |
|---|---|---|
| Portable | `logs\openpete.log` next to the executable | Windows |
| Per-user | `$XDG_STATE_HOME/openpete/logs/openpete.log`, default `~/.local/state/openpete/logs/openpete.log` | Linux |

`portable` in `openpete.toml` sets the layout explicitly. A portable
install whose directory is not writable falls back to the per-user one
and logs a warning naming the directory it could not write.
`--config-report` prints the resolved directories.

`--log-file PATH` redirects the mirror, `--no-log-file` disables it,
`--log-level SPEC` filters by subsystem (`--log-level mod:debug,*:info`).

A distribution Windows build links as a GUI-subsystem application, so
double-clicking it opens no console and its stderr has nowhere to go.
Launched from a terminal, or with its output redirected, it attaches to
the parent console and binds stdout and stderr to whatever the standard
handles point at, so the log streams live. Page it with `more`:

```
openpete-spyro1.exe --mods your_mod 2>&1 | more
```

The log file is written either way.

The compiler writes two logs the engine does not mirror:
`mods/<id>/.build/compile.log` for a C compile failure,
`mods/<id>/.build/shaders/<name>.log` for a shader one. A mod that fails
to compile is disabled and the game starts without it, so a mod that
produces no output at all is diagnosed from these two files.

## Keys

| Key | Effect |
|---|---|
| ++tab++ | Hold to run unlocked (fast-forward). |
| ++f7++ | Pause and resume the simulation. |
| ++f8++ | Advance one tick while paused. |
| ++f++ | Engage and disengage the debug free camera. |
| ++m++ | Open the dev overlay. Its Mods panel toggles mods, edits `[[config]]` rows and shows `ui_status` lines. |

`--keys-report` prints the live map, including rebindings.

## Flags for scripted runs

`--help` is the authority for the build in use. These are the flags a
mod's test run is built from.

| Flag | Effect |
|---|---|
| `--mods ID[,ID...]` | Run exactly these mods, overriding `enabled.txt`. `--mods ""` runs none: the vanilla baseline. |
| `--headless` | No window. Pass with `--frame-limit`, `--max-runtime-seconds` or `--bk2-stop-on-end`; without one of those the run never ends. |
| `--frame-limit N` | Exit after N rendered frames. |
| `--max-runtime-seconds N` | SIGTERM after N seconds of wall clock. Terminates a run that hangs before reaching its frame limit. |
| `--mute 1` | Mute the audio device. |
| `--skip-to-level L` | Boot into level L, by numeric id (`11`) or name (`stone_hill`). An unknown name prints the list. |
| `--unlock-frames LIST` | Fast-forward inside frame ranges, e.g. `0-1756`. Windowed runs only: a headless run is already unlocked. |
| `--screenshot-on-frame N[,N...]` | Capture at each listed frame. |
| `--screenshot-on-tick N[,N...]` | Capture at each listed canonical tick. The frame-to-tick mapping varies with host timing; tick-keyed captures hit the same simulation moment run to run. |
| `--screenshot-output PATH` | Path template. `{n}` expands to the frame, `{p}` to the present index. |
| `--screenshot-native-only` | Write the native `-vk` image only. |
| `--freecam-pose STR` | Engage the free camera at a fixed pose once the level is live. |
| `--bk2 PATH` | Replay a recorded input movie. |
| `--bk2-record [PATH]` | Record live pad input to a movie. |
| `--bk2-stop-on-end` | Exit after the last recorded frame instead of holding the last input. |
| `--log-file PATH` | Mirror the log to PATH. |
| `--devui-open 1` | Open the dev overlay with every mod node expanded as soon as it initializes, so an unattended windowed run reaches a mod's UI section body. |

Each capture writes two files: `<out>-vk.png` is the native render,
`<out>-og.png` the PsyCross reference. `--screenshot-native-only` skips
the second. A headless run recomposes the canonical scene; a windowed
native run reads back the presented frame, including sub-tick
interpolation and overlays.

The final line of a run goes to stdout, not to the log file:

```
openpete-exit: reason=frame-limit frames=1760 ms=8311 code=0
```

`frames` is the last `openpete_frame` and `code` the process exit status.
`reason` is the terminating condition: `frame-limit`,
`max-runtime-seconds`, `bk2-end`, `user-quit`, `window-closed`,
`game-returned`, or `screenshot-failed` when a requested capture could not
be written.

## Recipes

### Screenshot at the start of a level

At frame 1756 the level is live, the fly-in has finished and the HUD has
not yet appeared.

```
openpete-spyro1 --headless \
  --mods your_mod \
  --skip-to-level 11 \
  --screenshot-on-frame 1756 \
  --screenshot-output shot.png \
  --frame-limit 1760 \
  --mute 1
```

Run again with `--mods ""` for the stock image to diff against.

### Screenshot from a fixed camera pose

Applies to a change gameplay does not read: a shader, a texture pack, a
model. The pose fixes the camera, so two runs frame the scene identically.

Run windowed, press ++f++, move the camera, quit. The free camera logs
its pose on every change; the last line is the pose at quit:

```
grep 'freecam: pos=' ~/.local/state/openpete/logs/openpete.log | tail -1
# freecam: pos=(12480, -8032, 1216) yaw=1024 pitch=-256
```

`--freecam-pose` takes that string verbatim:

```
openpete-spyro1 --headless \
  --mods your_mod \
  --skip-to-level 11 \
  --freecam-pose "pos=(12480, -8032, 1216) yaw=1024 pitch=-256" \
  --screenshot-on-frame 1800 \
  --screenshot-output pose.png \
  --frame-limit 1804 \
  --mute 1
```

The free camera writes the occlusion group in guest RAM to force every
sector drawn. That is a gameplay-state write, so a pose capture is valid
for comparing images and invalid for comparing gameplay.

### Recorded input replay

Applies to a change gameplay does read. A `.bk2` movie holds one pad state
per frame, so a replay reproduces the run tick for tick.

Record. Play to the state under test, then quit:

```
openpete-spyro1 --skip-to-level 11 --bk2-record test.bk2
```

Replay with the mod loaded:

```
openpete-spyro1 --headless \
  --mods your_mod \
  --bk2 test.bk2 \
  --bk2-stop-on-end \
  --mute 1
```

`--bk2-record` also writes a `test.bk2.args` sidecar holding the launch
flags. `--bk2` reads it, so `--skip-to-level` does not need repeating.

To find the frame to capture, pause with ++f7++, step with ++f8++ to the
target state, and quit. The exit line reports the frame:

```
openpete-spyro1 --bk2 test.bk2 | grep openpete-exit
# openpete-exit: reason=user-quit frames=2314 ms=41022 code=0
```

`--screenshot-on-frame` takes a comma list, so one run can sample several
candidate frames.

Replay limits:

- A movie holds PSX pad state only. A mod's `[[binding]]` rows are not
  recorded and not replayed.
- The free camera writes guest RAM. Using it while recording desyncs the
  replay.
- `--bk2-record` truncates an existing file without warning.
