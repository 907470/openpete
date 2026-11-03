# Spyro 2: Ripto's Rage!

OpenPete runs it with `openpete` (`openpete.exe` on Windows), the one
program that runs every game and picks the game from the disc. Its config sections are
`[game.spyro-2]` and `[disc.SCUS-94425]`
([Config file](../customizing/config-file.md)).

Flags, keys, menus and the config file work as on Spyro the Dragon; this
page lists what differs.

## Disc

OpenPete plays the NTSC-U release, serial SCUS-94425, dumped to a
`.bin`/`.cue` pair with one MODE2/2352 data track of 240379 sectors
([Dumping your disc](../getting-started/dumping-your-disc.md)). The image
OpenPete is built and tested against has these hashes of the `.bin`:

| Hash | Value |
| --- | --- |
| SHA-256 | `b3fd9fdb24b115fee4583dbe3c36ab788fd8679fb4592dbec8736392dec64551` |
| SHA-1 | `b3a28f8c5ec02dfbd1f9bd47adbe1d75186a5d3f` |
| MD5 | `9771d273c498e29aa9543c30ec00aa35` |

The [library](../help/glossary.md#library) names an image with this SHA-256
"Spyro 2: Ripto's Rage! (NTSC-U)"; any other image is listed under its file
name. Drag the image onto the window, or pass it on the command line,
as for Spyro 1 ([First run](../getting-started/first-run.md)):

```
openpete "Spyro 2 - Ripto's Rage.cue"
```

## Memory cards

Spyro 2 creates, saves and loads its save on a memory card as on Spyro 1
([Saves](../playing/saves.md#memory-cards)).

## Levels

`--skip-to-level L`<!-- id: flag:--skip-to-level --> starts in level L,
given by name or by number, as on Spyro 1
([Practice tools](../playing/practice-tools.md#starting-in-a-level)).

| Homeworld | Levels |
| --- | --- |
| Summer Forest | `summer_forest` (10), `glimmer` (11), `idol_springs` (12), `colossus` (13), `hurricos` (21), `aquaria_towers` (22), `sunny_beach` (23), `ocean_speedway` (25), `crushs_dungeon` (26) |
| Autumn Plains | `autumn_plains` (30), `skelos_badlands` (31), `crystal_glacier` (32), `breeze_harbor` (33), `zephyr` (34), `metro_speedway` (35), `scorch` (41), `shady_oasis` (42), `magma_cone` (43), `fracture_hills` (44), `icy_speedway` (45), `gulps_overlook` (46) |
| Winter Tundra | `winter_tundra` (50), `mystic_marsh` (51), `cloud_temples` (52), `canyon_speedway` (55), `robotica_farms` (61), `metropolis` (62), `dragon_shores` (65), `riptos_arena` (66) |

The [dev overlay](../help/glossary.md#dev-overlay)'s **Level warp** moves
to any of these levels during play
([Dev overlay](../playing/dev-overlay.md#level-warp)).

`--skip-to-area`<!-- id: flag:--skip-to-area --> has no Spyro 2 targets:
every value prints "Spyro 2: Ripto's Rage! has no --skip-to-area targets".

## Cutscenes

`--skip-to-cutscene C`<!-- id: flag:--skip-to-cutscene --> plays cutscene
C. With no value, or an unknown name, it prints the list.

| Names | Plays | Starts from |
| --- | --- | --- |
| `intro`, `intro_arrival`, `summer_forest`, `crush_intro`, `crush_defeated`, `autumn_plains`, `gulp_intro`, `gulp_defeated`, `winter_tundra`, `ripto_intro`, `ending` | a story cutscene, through the Dragon Shores theater's replay | level 65, `dragon_shores`; playback returns to Dragon Shores |
| `<level>_intro` | the level's arrival cutscene | the level's homeworld, through its portal, as on a first visit |
| `<level>_outro` | the level's departure cutscene | the level, completed and left |
| `summer_forest_story` | the story cutscene of leaving Glimmer for Summer Forest, then the story step after it: the save selected at the title is autosaved and Summer Forest loads | Glimmer, completed and left through its exit; Glimmer is reached by play (a New Game from the title selects a save) or by `--skip-to-level glimmer` (no save is selected, so nothing is saved) |

`<level>` is one of `idol_springs`, `colossus`, `hurricos`,
`aquaria_towers`, `sunny_beach`, `skelos_badlands`, `crystal_glacier`,
`breeze_harbor`, `zephyr`, `scorch`, `shady_oasis`, `magma_cone`,
`fracture_hills`, `mystic_marsh`, `cloud_temples`, `robotica_farms` or
`metropolis`.

## Cheats

Spyro 2 has no cheats in OpenPete: `--cheats`<!-- id: flag:--cheats -->
with any value stops the launch with exit status 3.

## Mods

Mods load on Spyro 2 ([Mods](../customizing/mods.md)); a code mod is
compiled for Spyro 2 when its manifest selects the game. Not available on
Spyro 2:

- replacement music: the game has no track index
  ([Music](../../modding/music.md));
- custom levels ([Custom levels](../../modding/levels.md));
- mod materials, and mod text drawn on screen.

The Modding section's
[per-game support table](../../modding/code.md#per-game-support) lists
every difference.

## Notes

- The default window is 640x432<!-- id: knob:window -->, twice the game's
  drawn area.
- Level loads complete without an added wait by default
  (`--load-delay`<!-- id: flag:--load-delay --> default
  `0`<!-- id: knob:load_delay -->).
- The default renderer is the native Vulkan renderer, `vk`, as on every
  game.
