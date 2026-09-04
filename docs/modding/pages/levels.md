# Custom levels

!!! warning "Custom overlays are WIP"

    Custom levels that ship their own code overlay are a work in progress. The engine
    will load them into game memory, but they are not currently recompiled and executed.

A `[[level]]` block plus two files under `assets/levels/` makes a
playable custom level with no C. The engine claims the level slot,
declares the WAD table-of-contents entry, serves the files when the game
streams the level, and restores everything when the mod is disabled.

``` toml
[[level]]
name  = "Grim Garden"      # menu name and sort key; also the in-game title
files = "grim_garden"      # stem: assets/levels/grim_garden.WAD + .OVL
donor = "dark_hollow"      # level whose code the hack was authored against:
                           # lower_snake name or numeric id 10..64
replace = true             # optional: serve over the donor's own level
music = "grim_garden.wav"  # optional: the level's own song, from assets/music/
title = "GRIM GARDEN"      # optional: in-game title when it differs from name
```

- `<stem>.WAD` is the level's data segment and can be any size.
  `<stem>.OVL` is the donor's code overlay and must keep the donor's
  stock size. Omit the overlay to run the stock one. Extension case does
  not matter.
- Up to 4 `[[level]]` blocks per mod.
- Custom levels from every mod currently share level slot
  ID 65, so per-level progress (gems, dragons) is shared between them.
  `replace = true` takes over the donor's slot instead, and the donor's
  homeworld portal preview then shows the custom level's own skybox,
  parsed from the `.WAD` file.
- Without `music`, the level plays the donor's track. With it, the engine
  plays the WAV whenever the level starts its music and sizes the game's
  loop bookkeeping to the file's length. Format: see
  [Music](assets.md#music).
- `name` appears in the inventory rows, on the entry title card, and in
  the homeworld portal's floating letters (in replace mode, in place of
  the donor's name). Names are filtered to `A-Z`, `0-9`, space, and
  `! , ? . '`. The portal letters render `A-Z`, `0-9`, and `.`; other
  characters render as an apostrophe there. Set `title` when the in-game
  string should differ from the menu name.

## In play

The M overlay's Mods section shows one row per level, with a Warp
button, a switch between own-slot and replace-donor mode, and a tooltip
naming the level's Custom World number and donor. Levels from all mods
sort alphabetically, five per world.
