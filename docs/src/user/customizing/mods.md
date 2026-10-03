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
prints the folder in use.

A `.zip` is unpacked into `mods/.unpacked/<id>/` on the next start, where
`<id>` is the zip's file name without `.zip`. The zip must hold `mod.toml`
at its root, or inside one top-level folder.

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

The [dev overlay](../help/glossary.md#dev-overlay)'s **Mods** section lists the mods, with a checkbox that
turns each one on or off while the game runs. With an `enabled.txt`, the
mods it does not list appear there switched off. A checkbox lasts for the
session; `enabled.txt` is not rewritten.

## Code mods

A mod with `src/` is compiled on the first start after it is installed or
changed, against the SDK in the `sdk` folder next to the program. The
compiler is the one bundled in `recomp/tools/` next to the program; a Linux
install without one uses the system `cc` (the variable `OPENPETE_CC` names
another). The splash screen shows
**COMPILING MODS** meanwhile. An unchanged mod is not compiled again.

A mod that fails to compile or load is switched off and the game starts
without it. The compiler's output is in `mods/<id>/.build/compile.log`,
and `openpete.log` names the mod.

A package built without the mod SDK has no `sdk` folder. Its log says so,
code mods are not compiled, and mods made only of `mod.toml` and assets
load as usual. A mod that ships a prebuilt library in
`.build/<id>.dll` (Windows) or `.build/<id>.so` (Linux) and no `src/` loads
that library as-is.

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
