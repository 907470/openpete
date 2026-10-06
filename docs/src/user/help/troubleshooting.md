# Troubleshooting

Most problems leave a line in `openpete.log`, in the `logs` folder
([File locations](file-locations.md)). Read the last lines before the
problem first.

## By symptom

### Windows shows "Windows protected your PC"

The download is not signed, so SmartScreen warns. Click **More info**, then
**Run anyway**. To check the download first, compare it with the published
`.sha256` checksum.

### First run fails on Windows with a path error

The [first run](glossary.md#first-run-compile) builds the game code inside the install folder, and a deep
folder can exceed the Windows path-length limit. The message reads
"cannot create the module build directory". Move the install to a short
path, such as `C:\Games\OpenPete`.

### The AppImage exits with a FUSE error

The distribution does not install libfuse. Install it, or run the image
without it:

```
./OpenPete-*.AppImage --appimage-extract-and-run
```

### The tar.xz reports "no compiler for the first-boot module build"

A `.tar.xz` without a bundled compiler uses the system `cc`. Install one
(`sudo apt install gcc` or your distribution's equivalent), or name one in
the variable `OPENPETE_CC`.

### The game stopped starting after an edit to `openpete.toml`

An unknown key, a value of the wrong type or a value outside a key's list
stops the launch. The message names the file, the section and the key, and
suggests the nearest key name for a typo. Correct the line, or launch with
{{ flag("flag:--no-config") }}<!-- id: flag:--no-config --> to confirm the file is the
cause ([Config file](../customizing/config-file.md)).

### Escape does not open the settings menu, or quits the game

{{ key("key:host.menu") }}<!-- id: key:host.menu --> opens the
[settings menu](glossary.md#settings-menu) and no key quits
<!-- id: key:host.quit -->, unless a config file rebinds `menu` or `quit`.
{{ flag("flag:--keys-report") }}<!-- id: flag:--keys-report --> shows where each binding
came from ([Key bindings](../customizing/key-bindings.md)).

### Settings chosen in the menu are lost on the next start

- A flag or environment variable given at launch wins over saved settings.
  The [dev overlay](glossary.md#dev-overlay) marks such a row **pinned**.
- In the portable layout the menus save into the `openpete.toml` next to the
  program. If the per-user `openpete.toml` also has a `[saved]` block, that
  block wins, and the log warns about it at startup. Delete the block from
  the per-user file.
- On Windows, extracting a new release over the install folder replaces the
  `openpete.toml` holding the saved settings.

`--config-report` shows which file is the save target and which layer won
each setting.

### Memory card saves or savestates are missing

Switching between the portable and per-user layouts changes where the
`cards` and `states` folders are read; nothing is copied. When a portable
install has no `cards` or `states` folder yet and the per-user one holds
files, the log names the per-user location. Copy the files across, or
switch back
([File locations](file-locations.md)).

### A controller is not recognised, or its buttons are in the wrong places

Add a mapping line for it ([Controllers](../customizing/controllers.md)).
`openpete.log` names, for each connected controller, the mapping used and
its source. Under Steam, Steam's own mappings outrank every file.

### Keys or the mouse do nothing

- {{ flag("flag:--input-lock") }}<!-- id: flag:--input-lock --> ignores every keyboard,
  mouse and controller; `--mouse 0`<!-- id: flag:--mouse --> ignores the
  mouse. Check `--config-report` for either.
- A key your file binds to another action no longer does its default job.
  `--keys-report` lists every binding.

### There is no sound

Check, in `--config-report`, for `mute`, and for `sfx` or `music` set to
`0` ([Audio](../customizing/audio.md)). The game is silent while paused
(++f7++). The game's own sliders are stored in its save.

### A texture pack does not show

- Replacements draw only in the default renderer; `--renderer psycross`
  shows the originals.
- The folder is read when the game starts: restart after adding files.
- The log line `[texpack] replacement pack active` counts the files found;
  if it is absent, the files are in another folder than the one in use
  ([Texture packs](../customizing/texture-packs.md)).

### Replacement music does not play

A WAV in an unsupported format is skipped with a warning in the log, and
the track plays from the disc. The file must be 16-bit PCM, mono or stereo,
at one of the listed rates
([Music and sound packs](../customizing/music-and-sound-packs.md)).

### A mod does not load

- A mod not listed in `mods/enabled.txt` does not load at startup when that
  file exists.
- A [code mod](glossary.md#code-mod) that failed to compile is switched off; its compiler output is
  in `mods/<id>/.build/compile.log`.
- A package without the mod SDK compiles no code mods; the log says
  "packaged without the mod SDK".

See [Mods](../customizing/mods.md).

### Loading a save warns about mods

The save was made with a different set of mods. See
[Saves](../playing/saves.md) for adopting it.

### The game hangs and exits

See [Reporting a bug](reporting-a-bug.md): attach `openpete.log` and the
`hang-trace.bin` written next to where the program was started.

## By error id

This table lists the error ids (`OP-E-...`) that error messages print, each
with its cause and fix. No message prints an id yet.

| Id | Message | Cause | Fix |
| --- | --- | --- | --- |
