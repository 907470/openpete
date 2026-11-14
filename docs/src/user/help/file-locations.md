# File locations

`openpete --config-report`<!-- id: flag:--config-report --> prints
every location below as this install resolves it, then exits.

## What OpenPete writes

| Folder or file | Holds |
| --- | --- |
| `openpete.toml` | your settings ([Config file](../customizing/config-file.md)) |
| `gamecontrollerdb.txt` | your controller mappings ([Controllers](../customizing/controllers.md)) |
| `cards/` | memory cards `card1.mcr` and `card2.mcr`: the game's own saves |
| `states/` | [savestate](glossary.md#savestate) slots, one folder per game (`states/spyro-1/`) |
| `logs/` | `openpete.log` for the current run, `openpete.log.1` for the one before, up to `openpete.log.15` for the oldest kept; `hang-trace.bin` and `hang-ram.bin` after the game stopped responding; `openpete-crash.dmp` after a crash on Windows |
| `cache/` | the graphics pipeline cache and the game page's pictures, one folder per game; safe to delete, rebuilt on the next start |
| `library/` | your ingested disc image and the game code compiled from it |
| `mods/` | installed mods ([Mods](../customizing/mods.md)) |
| `assets/` | your texture, music and sound packs |
| `bk2/` | [input recordings](glossary.md#input-recording) |
| `screenshots/` | PNGs saved with ++f12++ |
| `bugreports/` | [bug report](glossary.md#bug-report) zips saved with ++f9++ |

## Where it goes

There are two layouts. Portable keeps everything in the program's folder.
Per-user uses the platform's per-user folders. `portable = true` or
`portable = false`<!-- id: directive:portable --> at the top of `openpete.toml` chooses; when the
program's folder is not writable, per-user is used and the log says so.

=== "Windows"

    Portable is the default. Everything is in the install folder, next to
    `openpete.exe`:

    ```
    openpete.toml  gamecontrollerdb.txt
    cards\  states\  logs\  cache\  library\  mods\  assets\  bk2\  screenshots\  bugreports\
    ```

    Two per-user files are also read in this layout, and nothing writes
    them: `%APPDATA%\openpete\openpete.toml` and
    `%APPDATA%\openpete\gamecontrollerdb.txt`.

    With `portable = false`, or an install folder that is not writable
    (under `Program Files`):

    ```
    %APPDATA%\openpete\                     openpete.toml, gamecontrollerdb.txt
    %LOCALAPPDATA%\openpete\cards\  states\  logs\  cache\
    %LOCALAPPDATA%\openpete\library\  mods\  assets\  bk2\  screenshots\  bugreports\
    ```

=== "Linux AppImage"

    Per-user, always: the AppImage is read-only.

    ```
    ~/.config/openpete/         openpete.toml, gamecontrollerdb.txt
    ~/.local/state/openpete/    cards/  states/  logs/  cache/
    ~/.local/share/openpete/    library/  mods/  assets/  bk2/  screenshots/  bugreports/
    ```

    `$XDG_CONFIG_HOME`, `$XDG_STATE_HOME` and `$XDG_DATA_HOME` replace the
    three bases when set.

=== "Linux tar.xz"

    Per-user is the default, with the same folders as the AppImage. A
    `library/`, `mods/`, `assets/`, `bk2/`, `screenshots/` or `bugreports/` folder that
    already exists next to the program, and is writable, is used in place
    of the per-user one.

    With `portable = true` in the `openpete.toml` next to the program,
    everything is kept in the extracted folder, as on Windows.

## What the program folder holds

A release package holds, next to the program: the shipped `openpete.toml`
and `gamecontrollerdb.txt`; `recomp/`, the compiler and recompiler that
build the game code on [first run](glossary.md#first-run-compile); and `sdk/`, the mod SDK with its
documentation and examples, in packages built with it.

## Files from earlier versions

Earlier versions kept every game's savestate slots in `states/`, and
some kept the [dev overlay](../playing/dev-overlay.md)'s window layout in
one file per game (`openpete-devui-spyro-1.ini`) next to the program. The
first time a game runs on this version, it copies these files to their
current names and leaves the originals where they are; nothing is moved
or deleted:

- A slot file in `states/` made by this game is copied into the game's
  folder (`states/spyro-1/slot3.state`). A slot file made by another game
  is copied when that game first runs. A slot the game's folder already
  holds is not overwritten. When the copying is done the game's folder
  gets a `.seeded` file, and later runs copy nothing; delete it to copy
  again.
- A slot file from a version that did not record its game is copied only
  when the game code was compiled from your disc on
  [first run](glossary.md#first-run-compile), which lets the program prove
  the file came from this game. Otherwise it stays in `states/`, the log
  names it, no `.seeded` file is written, and each later run checks it
  again.
- When `openpete-devui.ini` does not exist, the most recently written
  `openpete-devui-<game>.ini` is copied to `openpete-devui.ini`, which every
  game then reads and writes. The other per-game files are not read.

The log lists every file copied and every file left in place, with the
reason.

## Other files

- `openpete-devui.ini`: the [dev overlay](../playing/dev-overlay.md)'s
  window positions, next to the program, one file shared by every game.
- `hang-trace.bin` and `hang-ram.bin`: written into the `logs` folder, beside
  `openpete.log`, when the game stops responding and the program exits
  ([Reporting a bug](reporting-a-bug.md)).
- `--card-dir PATH`<!-- id: flag:--card-dir -->,
  `--card1 PATH`<!-- id: flag:--card1 --> and
  `--card2 PATH`<!-- id: flag:--card2 --> move the memory cards;
  `--mods-dir`, `--texpack-dir` and `--sfxpack-dir` move the mods and packs;
  `--log-file PATH`<!-- id: flag:--log-file --> moves the log and
  {{ flag("flag:--no-log-file") }}<!-- id: flag:--no-log-file --> writes none.
