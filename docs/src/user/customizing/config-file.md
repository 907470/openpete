# Config file

Settings live in `openpete.toml`, a [TOML](https://toml.io) file. Every
setting in it is also a command-line flag; a flag typed at launch wins over
the file.

## Where the files are

OpenPete reads two files, both named `openpete.toml`:

- the **shipped file**, next to the program;
- the **per-user file**, in your config directory.

=== "Windows"

    ```
    <install folder>\openpete.toml          shipped file
    %APPDATA%\openpete\openpete.toml        per-user file
    ```

=== "Linux AppImage"

    ```
    (inside the AppImage, read-only)        shipped file
    ~/.config/openpete/openpete.toml        per-user file
    ```

    `$XDG_CONFIG_HOME/openpete/openpete.toml` when `XDG_CONFIG_HOME` is set.

=== "Linux tar.xz"

    ```
    <extracted folder>/openpete.toml        shipped file
    ~/.config/openpete/openpete.toml        per-user file
    ```

    `$XDG_CONFIG_HOME/openpete/openpete.toml` when `XDG_CONFIG_HOME` is set.

Where one setting appears in both files at the same scope, the per-user
file wins. `openpete-spyro1 --config-report` prints both paths, which of them
exist, and which one the menus save to.

### Which file the menus save to

The [settings menu](../help/glossary.md#settings-menu) (**SETTINGS** row) and the [dev overlay](../help/glossary.md#dev-overlay) (**Save settings**)
write into one file, the save target:

| Layout | Default on | Save target |
| --- | --- | --- |
| portable | Windows <!-- id: directive:portable --> | the shipped file, next to the program |
| per-user | Linux | the per-user file |

`portable = true` or `portable = false`<!-- id: directive:portable home --> at the top of either file selects
the layout on any platform. A per-user file's directive beats the shipped
file's. When the program's folder is not writable (under `Program Files`,
or the read-only AppImage), the per-user layout is used and the log says
so. The same choice decides where memory cards, [savestates](../help/glossary.md#savestate) and logs go
([File locations](../help/file-locations.md)).

!!! warning "Windows upgrades replace the shipped file"
    In the portable layout the menus save into the `openpete.toml` next to
    `openpete-spyro1.exe`. A release zip contains an `openpete.toml`, so
    extracting a new release over the folder replaces the file and the
    settings saved in it. Copy the `[saved]` block out before upgrading, or
    write your own settings into `%APPDATA%\openpete\openpete.toml`, which no
    release touches.

## The shipped file

Each package ships this `openpete.toml`:

```toml
[game.spyro-1]
fullscreen = true
mouse_sensitivity = 1.0

[game.spyro-1.keys.host]
devui = "M"
freecam = "F"
interp_toggle = "I"
lod_toggle = "L"
menu = "escape"
quit = "none"
look_axis_x = "pad:rightx,mouse:x"
look_axis_y = "pad:-righty,mouse:-y"
```

It changes these built-in defaults for Spyro 1:

- `fullscreen = true`<!-- id: knob:fullscreen @spyro-1 --> (built-in: `0`<!-- id: knob:fullscreen -->,
  windowed);
- ++escape++<!-- id: key:host.menu @spyro-1 --> opens the settings menu (built-in:
  ++o++<!-- id: key:host.menu -->);
- ++escape++ no longer quits (built-in `quit` binding: ++escape++<!-- id: key:host.quit -->);
- ++m++<!-- id: key:host.devui @spyro-1 --> opens the dev overlay,
  ++i++<!-- id: key:host.interp_toggle @spyro-1 --> toggles [frame interpolation](../help/glossary.md#frame-interpolation) and
  ++l++<!-- id: key:host.lod_toggle @spyro-1 --> toggles [extended detail](../help/glossary.md#extended-detail) (built-in:
  unbound).

`freecam`, `mouse_sensitivity` and the two look axes restate the built-in
values so they can be edited in place.

## Writing settings

A setting's key is its flag name without the leading dashes, with `-`
written as `_`: `--fog-scale` is `fog_scale`. The
[config key reference](https://openpete.com/docs/user/reference/config-keys.html) lists every key.

```toml
aspect = "21:9"
fov    = 90

[game.spyro-1]
lod   = "extended"
music = 0

[disc.SCUS-94228]
vertex_precision = "integer"
```

A key the program does not recognise, a value of the wrong type, or an
enum value outside its list stops the launch with a message naming the
file, the section and the key (with a suggestion for a near miss). The
message goes to the console and to `openpete.log`.

### Sections

A setting applies to the section it is written in:

| Section | Applies to |
| --- | --- |
| (top of the file) | every game |
| `[game.spyro-1]` | one game |
| `[disc.SCUS-94228]` | one disc release, by serial |
| `[disc.<sha256>]` | one disc image, by the SHA-256 of its `.bin`; any unique prefix of 8 or more characters works |
| `[saved]` | written by the menus (below) |

### Flags and variables without a key

Two sub-tables carry what has no key of its own:

```toml
[args]                          # keys are flags, as typed
"--rewind" = "30"
[env]                           # keys are OPENPETE_* variables
OPENPETE_VIBRATION = "0"
```

`[args]`, `[env]` and `[keys.*]` belong to the section they are written
under: `[game.spyro-1.args]` and `[disc.SCUS-94228.env]` work the same way.
In `[args]`, `true` passes a flag that takes no value and `false` withholds
it.

### Saved settings

The menus write into a `[saved]` block at the end of the save target,
below the line

```
# >>> openpete managed settings - written by the in-game menus <<<
```

Everything above that line is kept byte for byte, comments included.
Anything written below it by hand is replaced on the next save. If the
per-user file has a `[saved]` block while the save target is the shipped
file, the per-user block outranks every later save and the log warns.

## Which setting wins

Lowest first:

```
global  <  [game.ID]  <  [disc.SERIAL]  <  [disc.SHA]  <  [saved]
  <  environment variables  <  a movie's .bk2.args file  <  flags you type
```

- A more specific section beats a less specific one, whatever spelling
  either uses.
- Inside one section, a bare key loses to `[env]`, which loses to `[args]`.
- At the same scope, the per-user file beats the shipped file.

The dev overlay marks a row whose saved value a launch-time flag or
variable overrides as **pinned**.

## Commands

| Command | Effect |
| --- | --- |
| `--config-report`<!-- id: flag:--config-report home --> | print every setting, the layer that won it and what it beat, the files read, the save target and every data directory; then exit |
| `--config PATH`<!-- id: flag:--config --> | read only this file (it is also the save target) |
| `--no-config`<!-- id: flag:--no-config home --> | read no config file |
| `--keys-report`<!-- id: flag:--keys-report --> | print every key binding and the section that set it; then exit |

The environment variable `OPENPETE_CONFIG` names a file the same way as
`--config`; the value `none` reads no file.

`openpete.opts` and `openpete.env`, used by earlier releases, are no longer
read. A launch that finds one next to the program prints a line saying so;
their lines belong in `[args]` and `[env]`.
