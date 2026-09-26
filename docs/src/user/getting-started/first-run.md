# First run

## Giving OpenPete the disc

With no disc in the library, OpenPete opens a window reading **DRAG YOUR
SPYRO THE DRAGON DISC IMAGE (.CUE / .BIN) HERE**. Any of these hands it the
image:

- Drag the `.bin` or `.cue` onto the window.
- Press ++space++ or the pad's ++a++ button to browse for it.
- Pass the path on the command line or open the image with the program; see
  [Installing](install.md#opening-a-disc-image-directly).

++esc++ or the pad's ++b++ button quits.

Only `.bin`/`.cue` images are ingested: given a `.cue`, OpenPete uses the
`.bin` of the same name beside it, or the only `.bin` in that folder.

## Ingest: the disc goes into the library

OpenPete copies the image into its library and names the entry by the
image's SHA-256. After that the original file is not needed again; the log
says so. Ingesting the same image a second time reuses the existing entry.
The library's location is listed under
[File locations](../help/file-locations.md).

The copy runs behind a **FIRST-TIME SETUP** screen with a progress bar.

## The one-time compile

Next, still under **FIRST-TIME SETUP**, OpenPete recompiles the game's code
for your machine, with the line **YOUR DISC IS REBUILT FOR THIS PC. THIS
HAPPENS ONCE.** Measured times: about 26 seconds on a 16-thread desktop, 45 to
90 seconds on a four-core laptop. The game then starts.

Later runs boot the library entry straight into the game. With more than one
entry in the library, a **CHOOSE A VERSION** screen lists them, most recently
played first: ++up++ / ++down++ or the stick select, ++enter++ or ++a++ plays,
++esc++ or ++b++ quits. Dropping a new image on this screen ingests it.

## Mods compile on start

A start that finds mod source code it has not compiled yet, or source that
changed since the last compile, shows **PREPARING MODS** with a
**COMPILING MODS** progress line and **CHANGED MOD SOURCES ARE COMPILED FOR
THIS PC.** before the game starts. Mods made only of assets do not compile.
See [Mods](../customizing/mods.md).

## Upgrades

Install a new release over the old one, or beside it (see
[Installing](install.md)). The first start of a new release recompiles the
game once more from the library's own copy of the disc: 12 to 50 seconds,
under the same **FIRST-TIME SETUP** screen. The original disc image is not
needed.

What carries over:

| | Across an upgrade |
| --- | --- |
| Settings (`openpete.toml`) | Kept. On Windows, extracting the new zip over the old folder replaces the shipped `openpete.toml` next to the exe; your in-game saved settings live in the same file, so extract into a new folder and copy the file across, or re-save after the upgrade. |
| Memory cards | Kept. |
| Savestates | Refused by a build whose compiled game differs from the one that wrote them; the log names the state as made against a different build. Finish with savestates before upgrading. |
| Library | Kept; each release adds its own compiled build to the entry. |
| `gamecontrollerdb.txt` next to the exe (Windows) | Replaced by the new zip. Keep your own mapping lines elsewhere; see [Controllers](../customizing/controllers.md). |
