# Custom levels

!!! warning "Custom overlays are WIP"

    Custom levels that ship their own code overlay are a work in progress. The engine
    will load them into game memory, but they are not currently recompiled and executed.

A `[[level]]` block plus two files under `assets/levels/` makes a
playable custom level with no C. Custom levels run on Spyro 1; on Spyro 2
and 3 the rows are refused with one log line and the rest of the mod
loads. The engine claims the level slot,
declares the WAD table-of-contents entry, and serves the files when the
game streams the level.

Disabling the mod gives back what the engine can give back. A level's
target totals are reconciled every tick, so they return to their stock
values as soon as the level stops being live. The slot's data-length and
name declarations are not: a level on the shared slot leaves those in
place until another custom level claims the slot or the game restarts.

## Glossary

| Term | Meaning |
|---|---|
| donor | The stock level whose code the custom level was authored against, named by `lower_snake` name or by id 10 to 64. |
| stem | The `files` value: the shared basename of `<stem>.WAD` and `<stem>.OVL` under `assets/levels/`. |
| slot | The level id the game itself sees. Custom levels share slot 65, and with it per-level progress, unless `replace` takes the donor's slot. |
| WAD | A level's data segment: its geometry, textures, mobys and skybox. |
| code overlay | A level's `.OVL` half, the per-level code the game loads into a fixed buffer, which is why it must keep the donor's stock size. |

``` toml
[[level]]
name  = "Grim Garden"      # menu name and sort key; also the in-game title
files = "grim_garden"      # stem: assets/levels/grim_garden.WAD + .OVL
donor = "dark_hollow"      # level whose code the hack was authored against:
                           # lower_snake name or numeric id 10..64
replace = true             # optional: serve over the donor's own level
music = "grim_garden.wav"  # optional: the level's own song, from assets/music/
title = "GRIM GARDEN"      # optional: in-game title when it differs from name
gems    = 400              # optional: target totals for the inventory rows
dragons = 4                #           and the HUD's all-gems test
eggs    = 1                # optional: replace-mode, first three worlds only
```

- `<stem>.WAD` is the level's data segment.
  `<stem>.OVL` is the donor's code overlay and must keep the donor's
  stock size. Omit the overlay to run the stock one. Extension case does
  not matter.
- A mod declares as many `[[level]]` blocks as it has levels. The game
  exposes one claimable level slot, so they time-share it: one custom
  level is enterable at a time.
- Custom levels from every mod currently share level slot
  ID 65, so per-level progress (gems, dragons) is shared between them.
  The collected count is shared but each level's declared target is its
  own, so 400 gems collected in one own-slot level and then a warp to
  another shows those 400 against the second level's target, and can
  complete its all-gems test on entry.
  `replace = true` takes over the donor's slot instead, which gives the
  level the donor's own progress row and avoids that; the donor's
  homeworld portal preview then shows the custom level's own skybox,
  parsed from the `.WAD` file.
- `gems`, `dragons` and `eggs` declare the level's target totals - the
  numbers the inventory rows show and the HUD's all-gems test compares
  against. They are optional and independent: a key you omit leaves that
  table on the stock row, which for an own-slot level means zero. Ranges
  are `0..255`, `0..32767` and `0..255`; a value outside its range is
  refused by name rather than wrapped.
- `eggs` only applies to a `replace = true` level whose donor is in the
  first three worlds. The game's egg table has one row per level for
  those worlds and none at all for the shared slot, so the engine drops
  an `eggs` declaration it cannot place and says so in the log. The rest
  of the block still applies. The refusal is not a formality: the shared
  slot's index falls outside the table and lands in the memory-card save
  header, so a level on the shared slot can never show an egg count.
- The drop happens when the level is registered, which reads `replace`
  from this file. Switching a live level to replace-donor mode from the
  Mods panel does NOT bring a dropped `eggs` back - the value is already
  gone for that session. Set `replace = true` here and restart to make an
  egg count apply.
- Without `music`, the level plays the donor's track. With it, the engine
  plays the WAV whenever the level starts its music and sizes the game's
  loop bookkeeping to the file's length. Format: see
  [Music](music.md).
- `name` appears in the inventory rows, on the entry title card, and in
  the homeworld portal's floating letters (in replace mode, in place of
  the donor's name). Names are filtered to `A-Z`, `0-9`, space, and
  `! , ? . '`. The portal letters render `A-Z`, `0-9`, and `.`; other
  characters render as an apostrophe there. Set `title` when the in-game
  string should differ from the menu name.

## In play

The M overlay's Mods tab shows one row per level, with a Warp
button, a switch between own-slot and replace-donor mode, and a tooltip
naming the level's Custom World number and donor. Levels from all mods
sort alphabetically, five per world.
