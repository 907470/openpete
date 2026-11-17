# Mods

A mod is a folder, or a `.zip` of one, holding a `mod.toml` manifest and any
of:

- `assets/`: replacement textures, music and sounds, and custom levels;
- `src/*.c`: code, compiled on your machine when the game starts; a mod
  with this folder is a [code mod](../help/glossary.md#code-mod).

!!! warning "Code mods run with the game's permissions"
    A mod with a `src/` folder runs inside the game process with the same
    access to your files and network as the game itself. There is no
    sandbox. Install only mods you trust, as you would any other program.
    A mod without a `src/` folder never runs code.

## Installing a mod

Copy the mod's folder, or its `.zip`, into the `mods` folder:

=== "Windows"

    `<install folder>\mods\`

=== "Linux"

    `~/.local/share/openpete/mods/`
    (`$XDG_DATA_HOME/openpete/mods/` when `XDG_DATA_HOME` is set)

    A `mods` folder next to the program is used instead when it already
    exists and is writable (a `.tar.xz` install that predates the per-user
    folders).

`--mods-dir PATH`<!-- id: flag:--mods-dir --> (config key `mods_dir`,
variable `OPENPETE_MODS_DIR`) names another folder. `--config-report`
prints the folder in use. The **Mods** chip on the launcher's
[game page](../getting-started/first-run.md#the-game-page) opens the folder in use,
creating it when it does not exist.

A `.zip` is unpacked into `mods/.unpacked/<id>/` on the next start, where
`<id>` is the zip's file name without `.zip`. The zip must hold `mod.toml`
at its root, or inside one top-level folder. Replacing the zip with a new
version keeps the mod's `data/` folder and the settings changed in the
Mods tab.

## Choosing which mods load

`mods/enabled.txt` lists mod IDs, one per line; `#` starts a comment. The
listed mods load in that order, and a later mod wins where two replace the
same thing. Without `enabled.txt`, every mod in the folder loads, in
alphabetical order.

```
# mods/enabled.txt
hd-textures
my-music
```

`--mods ID,ID`<!-- id: flag:--mods home --> (config key `mods`, variable
`OPENPETE_MODS`) loads exactly the listed mods in that order and ignores
`enabled.txt`. `--mods ""` loads none.

The [dev overlay](../help/glossary.md#dev-overlay)'s **Mods** tab lists the mods, with a checkbox that
turns each one on or off while the game runs. With an `enabled.txt`, the
mods it does not list appear there switched off. A checkbox lasts for the
session; `enabled.txt` is not rewritten.

## Code mods

A mod with `src/` is compiled on the first start after it is installed or
changed, against the SDK in the `sdk` folder next to the program. The
compiler is the one bundled in `recomp/tools/` next to the program; a Linux
install without one uses the system `cc` (the variable `OPENPETE_CC` names
another). The launcher's progress screen shows
**Compiling mods (1/2)** meanwhile, counting the mods compiled and the mods
to compile. An unchanged mod is not compiled again.

A mod that fails to compile or load is switched off and the game starts
without it. The compiler's output is in
`mods/<id>/.build/<game>/compile.log` (`<game>` is `spyro-1`, `spyro-2` or
`spyro-3`),
and `openpete.log` names the mod. A mod that failed to compile is not
compiled again until its sources, the mod SDK or the compiler change;
`--compile-mods` retries it.

`--compile-mods` <!-- id: flag:--compile-mods --> compiles every mod with
`src/` in the mods folder, listed in `enabled.txt` or not, then exits; the
exit status is nonzero when a compile fails.

A package built without the mod SDK has no `sdk` folder. Its log says so,
code mods are not compiled, and mods made only of `mod.toml` and assets
load as usual. A mod that ships a prebuilt library in
`.build/<game>/<id>.dll` (Windows) or `.build/<game>/<id>.so` (Linux) and
no `src/` loads that library as-is when it was built for the running game.

## Mods and Spyro 3's anti-piracy checks

Spyro: Year of the Dragon checks its own program code while it runs, and a
check that finds a changed byte damages saves and levels. When a code mod
changes that code, openpete puts the original bytes back and `openpete.log`
names the mod.

`--scan-guard 0|1`<!-- id: flag:--scan-guard --> (config key `scan_guard`,
variable `OPENPETE_SCAN_GUARD`, default `1`<!-- id: knob:scan_guard -->)
turns this off with `0`: the mod's change stays, the log says once at
start that the guard is off, and mod code runs without the check's cost. A
mod's change can then set off the game's anti-piracy measures. The flag has
no effect on Spyro the Dragon and Spyro 2.

## Mods and saves

Memory-card saves record which mods were active. Loading a save made under
a different set of mods shows a warning until the save is adopted
([Saves](../playing/saves.md)).

## Making mods

The [Modding section](../../modding/index.md) documents mod
authoring: start with
[Getting started](../../modding/getting-started.md);
the manifest, code, assets, custom levels and shaders each have a page. A
package that includes the SDK carries working example mods in
`sdk/examples/`; copy one into the `mods` folder to run it.
