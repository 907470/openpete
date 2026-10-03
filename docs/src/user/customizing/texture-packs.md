# Texture packs

A texture pack is a folder of PNG files, each replacing one of the game's
textures: world and character textures, HUD sprites and the title logo. A
replacement can have any resolution, so a larger image is a high-definition
texture. Replacements change only what is drawn; the game itself runs
unchanged.

Replacements show in the default renderer (`--renderer vk`). The PsyCross
reference renderer (`--renderer psycross`) always draws the original
textures.

## Installing a pack

Copy the pack's PNG files into `assets/textures/` in your data directory:

=== "Windows"

    `<install folder>\assets\textures\`

=== "Linux"

    `~/.local/share/openpete/assets/textures/`
    (`$XDG_DATA_HOME/openpete/assets/textures/` when `XDG_DATA_HOME` is set)

A pack published as a mod installs as a mod instead
([Mods](mods.md)). A pack covers what it covers: a texture without a
replacement draws the original. The folder is read when the renderer
starts, so a pack added while the game runs appears on the next start.

### Which file wins

Lowest first:

1. `assets/textures/` next to the program (what a release ships).
2. `assets/textures/` in your data directory.
3. Each enabled mod's `assets/textures/`, later mods in load order over
   earlier ones.

`--texpack-dir DIR`<!-- id: flag:--texpack-dir --> (config key
`texpack_dir`, variable `OPENPETE_TEXPACK_DIR`) replaces the first two
with one folder; mods still apply on top of it.

The log line `[texpack] replacement pack active: N entries` reports how many
replacements were found.

## Making a pack

Replacements are matched by file name, and the name is computed from the
original texture, so the way to start is to dump the originals:

1. Set the variable `OPENPETE_TEXPACK_DUMP_DIR` to an empty folder, launch,
   and play through the areas to cover. Every texture drawn is written
   there once, under its name.
2. Edit or upscale the PNGs at any resolution. Keep each file name.
3. Copy the edited files into `assets/textures/`.

The variable can be set in [`openpete.toml`](config-file.md) on any
platform; remove the line when the dump is done:

```toml
[env]
OPENPETE_TEXPACK_DUMP_DIR = "C:/Games/texture-dump"
```

A name identifies the texture by its content, not by where it sits in
video memory, so one file applies in every level that uses that texture.
The name forms, the fog blend and textures for new models are documented
on the Modding site:
[Textures](../../modding/textures.md).
