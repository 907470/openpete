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
"cannot create the module build directory"
([OP-E-MODULE-DIR](#op-e-module-dir)). Move the install to a short
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
the variable `OPENPETE_CC` ([OP-E-NO-COMPILER](#op-e-no-compiler)).

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
  in `mods/<id>/.build/<game>/compile.log`.
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

Each error message ends with its id (`OP-E-...`) and the address of its
row in this table, for example
`[OP-E-CONFIG: https://openpete.com/docs/user/help/troubleshooting.html#op-e-config]`.
On Windows the message box shows the same id and address. The message
column gives the start of the text, or a box's text.

| Id | Message | Cause | Fix |
| --- | --- | --- | --- |
| <a id="op-e-config"></a>`OP-E-CONFIG` | A setting in openpete.toml (or an OPENPETE_ environment variable) was not accepted. | An unknown key, a value of the wrong type or a value outside a key's list, in `openpete.toml` or in an environment variable whose name starts with `OPENPETE_`. The line before it names the file, section and key. | Correct the line ([edit to `openpete.toml`](#the-game-stopped-starting-after-an-edit-to-openpetetoml)). |
| <a id="op-e-library"></a>`OP-E-LIBRARY` | The game library could not be read. | The `library` folder could not be read or written, or a disc image could not be copied into it. The line before it names the step: `library dir not writable`, `--ingest copy failed`, or `no .bin found`. | Free disk space, check that the folder is writable ([File locations](file-locations.md)), and keep the `.bin` next to its `.cue`. |
| <a id="op-e-ingest"></a>`OP-E-INGEST` | The disc image could not be added to the game library. | The image dropped on the first-run screen could not be copied into the [library](glossary.md#library); the causes are those of OP-E-LIBRARY. | As for [OP-E-LIBRARY](#op-e-library), then drop the image again. |
| <a id="op-e-wrong-disc"></a>`OP-E-WRONG-DISC` | The disc does not match the game chosen to run. | The disc image is another game than the one this program runs, or the game named on the command line differs from the disc's. | Open the right disc image, or start without naming a game. |
| <a id="op-e-no-disc"></a>`OP-E-NO-DISC` | No game disc image was found. | The [library](glossary.md#library) holds no disc and none was named. | Start OpenPete without arguments and drop the disc image on its window ([Installing](../getting-started/install.md)). |
| <a id="op-e-disc-read"></a>`OP-E-DISC-READ` | The game's program could not be read from the disc image. | The disc image is missing, truncated, larger than 2 GiB, or not a raw `.bin` (a 2048-byte-sector `.iso` is refused). The log line names every file it tried. | Make a new image of the disc as `.bin` and `.cue`, and open the `.cue`. |
| <a id="op-e-module"></a>`OP-E-MODULE` | The game code built from the disc could not be loaded or built. | The [first-run compile](glossary.md#first-run-compile) failed, or the compiled game code in the library could not be loaded. A `command failed` line names the failing step. | Check the lines before it for OP-E-NO-COMPILER, OP-E-MODULE-DIR or OP-E-PACKAGE. Otherwise free disk space and start again; if it fails again, [report a bug](reporting-a-bug.md). |
| <a id="op-e-no-compiler"></a>`OP-E-NO-COMPILER` | no compiler for the first-boot module build | The package has no bundled compiler and the system has no `cc`. | Install one (`sudo apt install gcc` or your distribution's equivalent), or name one in the variable `OPENPETE_CC`. |
| <a id="op-e-module-dir"></a>`OP-E-MODULE-DIR` | cannot create the module build directory | The library folder is not writable, or on Windows the install path is too deep for the path-length limit. | Move the install to a short path, such as `C:\\Games\\OpenPete`, and check that the folder is writable. |
| <a id="op-e-package"></a>`OP-E-PACKAGE` | recomp inputs not staged next to the exe, PSXRecomp not found, or cannot parse the manifest | Files of the `recomp` folder next to the program are missing or damaged. | Extract the release again into an empty folder. |
| <a id="op-e-display"></a>`OP-E-DISPLAY` | No display could be opened. | No window system is reachable: no desktop session, or a remote shell without its display variables. | Start OpenPete from the desktop session. |
| <a id="op-e-vulkan"></a>`OP-E-VULKAN` | OpenPete could not start Vulkan graphics. | No graphics device with the required Vulkan support was found, or its driver failed to create one. | Install the latest driver for your graphics card. To run without Vulkan, set `renderer = "psycross"` in `openpete.toml` ([Renderer](../graphics/display.md#renderer)). |
| <a id="op-e-gpu-lost"></a>`OP-E-GPU-LOST` | the graphics device stopped responding; closing | The graphics driver reset the device: a driver fault, an overheating or overclocked card, or a mod shader that ran too long. The line before it names the last GPU pass begun. | Install the latest driver and remove overclocks. If the line names a mod's pass, disable that mod ([Mods](../customizing/mods.md)). |
| <a id="op-e-hang"></a>`OP-E-HANG` | The game stopped responding and was closed. | The game's code made no progress for several seconds. | [Report a bug](reporting-a-bug.md) with `openpete.log` and the `hang-trace.bin` the message names. |
| <a id="op-e-crash"></a>`OP-E-CRASH` | === openpete crash: ... === | The program crashed. The log holds the crash report. | [Report a bug](reporting-a-bug.md) with `openpete.log`; on Windows also attach `openpete-crash.dmp` from the same folder. |
| <a id="op-e-card-open"></a>`OP-E-CARD-OPEN` | card N: ... cannot be opened, could not be read, or no card directory resolves; saves are refused | The memory card file or its folder cannot be read: missing permissions, or another program holds the file. | Close other programs using the file and check the `cards` folder's permissions ([File locations](file-locations.md)). The game runs, but nothing it saves is kept. |
| <a id="op-e-card-bad"></a>`OP-E-CARD-BAD` | card N: ... is not a 128 KiB card image | The card file has the wrong size or an unknown header. It was renamed to `<name>.bad-N` and a new card made in its place. | To use the old saves, convert the renamed file to a raw 128 KiB `.mcr` and put it back under the card's name ([Saves](../playing/saves.md)). |
| <a id="op-e-card-readonly"></a>`OP-E-CARD-READONLY` | card N: ... is a ... image; loaded read-only | The card file is a DexDrive `.gme`, VGS `.mem` or PSP/PS3 `.vmp` image. It loads, and saves to it are refused. | Convert it to a raw 128 KiB `.mcr` with a memory card tool. |
| <a id="op-e-card-write"></a>`OP-E-CARD-WRITE` | card N: the save could not be written | Writing the card file failed: the disk is full, the folder is read-only, or another program holds the file. | Free disk space or close the other program, then save again. The card keeps its contents from before the failed save. |
