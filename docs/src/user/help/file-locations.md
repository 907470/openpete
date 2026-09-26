# File locations

`openpete-spyro1 --config-report`<!-- id: flag:--config-report --> prints
every location below as this install resolves it, then exits.

## What OpenPete writes

| Folder or file | Holds |
| --- | --- |
| `openpete.toml` | your settings ([Config file](../customizing/config-file.md)) |
| `gamecontrollerdb.txt` | your controller mappings ([Controllers](../customizing/controllers.md)) |
| `cards/` | memory cards `card1.mcr` and `card2.mcr`: the game's own saves |
| `states/` | [savestate](glossary.md#savestate) slots |
| `logs/` | `openpete.log` for the current run, `openpete.log.1` for the one before |
| `library/` | your ingested disc image and the game code compiled from it |
| `mods/` | installed mods ([Mods](../customizing/mods.md)) |
| `assets/` | your texture, music and sound packs |
| `bk2/` | [input recordings](glossary.md#input-recording) |
| `screenshots/` | PNGs saved with ++f12++ |

## Where it goes

There are two layouts. Portable keeps everything in the program's folder.
Per-user uses the platform's per-user folders. `portable = true` or
`portable = false`<!-- id: directive:portable --> at the top of `openpete.toml` chooses; when the
program's folder is not writable, per-user is used and the log says so.

=== "Windows"

    Portable is the default. Everything is in the install folder, next to
    `openpete-spyro1.exe`:

    ```
    openpete.toml  gamecontrollerdb.txt
    cards\  states\  logs\  library\  mods\  assets\  bk2\  screenshots\
    ```

    Two per-user files are also read in this layout, and nothing writes
    them: `%APPDATA%\openpete\openpete.toml` and
    `%APPDATA%\openpete\gamecontrollerdb.txt`.

    With `portable = false`, or an install folder that is not writable
    (under `Program Files`):

    ```
    %APPDATA%\openpete\                     openpete.toml, gamecontrollerdb.txt
    %LOCALAPPDATA%\openpete\cards\  states\  logs\
    %LOCALAPPDATA%\openpete\library\  mods\  assets\  bk2\  screenshots\
    ```

=== "Linux AppImage"

    Per-user, always: the AppImage is read-only.

    ```
    ~/.config/openpete/         openpete.toml, gamecontrollerdb.txt
    ~/.local/state/openpete/    cards/  states/  logs/
    ~/.local/share/openpete/    library/  mods/  assets/  bk2/  screenshots/
    ```

    `$XDG_CONFIG_HOME`, `$XDG_STATE_HOME` and `$XDG_DATA_HOME` replace the
    three bases when set.

=== "Linux tar.xz"

    Per-user is the default, with the same folders as the AppImage. A
    `library/`, `mods/`, `assets/`, `bk2/` or `screenshots/` folder that
    already exists next to the program, and is writable, is used in place
    of the per-user one.

    With `portable = true` in the `openpete.toml` next to the program,
    everything is kept in the extracted folder, as on Windows.

## What the program folder holds

A release package holds, next to the program: the shipped `openpete.toml`
and `gamecontrollerdb.txt`; `recomp/`, the compiler and recompiler that
build the game code on [first run](glossary.md#first-run-compile); and `sdk/`, the mod SDK with its
documentation and examples, in packages built with it.

## Other files

- `hang-trace.bin`: written into the folder the program was started from
  when the game stops responding and the program exits
  ([Reporting a bug](reporting-a-bug.md)).
- `--card-dir PATH`<!-- id: flag:--card-dir -->,
  `--card1 PATH`<!-- id: flag:--card1 --> and
  `--card2 PATH`<!-- id: flag:--card2 --> move the memory cards;
  `--mods-dir`, `--texpack-dir` and `--sfxpack-dir` move the mods and packs;
  `--log-file PATH`<!-- id: flag:--log-file --> moves the log and
  `--no-log-file`<!-- id: flag:--no-log-file --> writes none.
