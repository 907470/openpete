# First run

## Giving OpenPete the disc

With no disc in the library, OpenPete opens a window reading **DRAG YOUR
DISC IMAGE (.CUE / .BIN) HERE**. The disc of any game OpenPete runs is
accepted; the game is read from the disc. Any of these hands it the image:

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
says so. The library holds one disc per game: a disc of a game already in
the library is refused before anything is copied, with a message naming
the library's copy, which is left unchanged.
The library's location is listed under
[File locations](../help/file-locations.md).

The copy runs behind a **FIRST-TIME SETUP** screen with a progress bar.

## The one-time compile

Next, still under **FIRST-TIME SETUP**, OpenPete recompiles the game's code
for your machine, with the line **YOUR DISC IS REBUILT FOR THIS PC. THIS
HAPPENS ONCE.** Measured times: about 26 seconds on a 16-thread desktop, 45 to
90 seconds on a four-core laptop. A disc given on the command line or opened
with the program then starts its game; a disc dragged onto the window or
chosen by browsing returns to the **CHOOSE A GAME** screen below, with the
new disc listed.

After an update that changes the generated code, the compile runs again for
each game in the library: in the background while the **CHOOSE A GAME**
screen shows, or behind its progress screen when that game is chosen first.
A compile that fails is not retried by later starts of the same OpenPete
build; the log file names the step that failed, and choosing the disc on
the **CHOOSE A GAME** screen retries it.

Later runs open the **CHOOSE A GAME** screen, which lists every disc in the
library, of every game, most recently played first, each named with its
game. It shows with a single disc in the library too, so the disc of
another game can be added: ++up++ / ++down++ or the stick select,
++enter++ or ++a++ plays, ++esc++ or ++b++ quits. Dragging a `.bin` or
`.cue` onto this screen, or pressing ++space++ or the pad's ++x++ button
to browse for one, adds that disc to the library, compiles its game's code
and returns to this screen.

A start without `--game` or `--disc` always opens this screen, whatever
the library holds. A launch that names its game with `--game` skips it and
starts that game's most recently played disc; see
[Installing](install.md#opening-a-disc-image-directly). One OpenPete at a
time prepares a game: a second one started while the first is still on
these screens exits and logs that another OpenPete is preparing a game.

## Mods compile on start

A start that finds mod source code it has not compiled yet, or source that
changed since the last compile, shows **PREPARING MODS** with a
**COMPILING MODS** progress line and **CHANGED MOD SOURCES ARE COMPILED FOR
THIS PC.** before the game starts. Mods made only of assets do not compile.
See [Mods](../customizing/mods.md).

## Shaders compile on start

The game's graphics pipelines are compiled for the graphics card and driver
before the game starts, and kept in a cache. A start whose compile takes
longer than 0.4 seconds (the first start after a new graphics driver or a
new release) shows **PREPARING GRAPHICS** with a **COMPILING SHADERS**
progress line and **SHADERS ARE COMPILED FOR THIS PC'S GRAPHICS DRIVER.**
Other starts finish the compile without showing it.

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
