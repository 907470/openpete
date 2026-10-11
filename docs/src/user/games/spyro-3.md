# Spyro: Year of the Dragon

No OpenPete release package includes Spyro 3 yet; the release packages
contain only `openpete-spyro1`. A build from source runs it with
`openpete` on Linux, the one program that runs every game and picks the
game from the disc, and with `openpete-spyro3.exe` on Windows. Its config
sections
are `[game.spyro-3]` and `[disc.SCUS-94467]`
([Config file](../customizing/config-file.md)).

Flags, keys, menus and the config file work as on Spyro the Dragon; this
page lists what differs.

## Disc

OpenPete plays the NTSC-U release, serial SCUS-94467, dumped to a
`.bin`/`.cue` pair with one MODE2/2352 data track of 253933 sectors
([Dumping your disc](../getting-started/dumping-your-disc.md)). The image
OpenPete is built and tested against is version 1.0 of the disc, with these
hashes of the `.bin`:

| Hash | Value |
| --- | --- |
| SHA-256 | `a313c9b1306a08e950edddc152280a443796e68052c8e0454b5d76d69354d157` |
| SHA-1 | `9df489e2420988494691c9193fe74dec4b1dbfcd` |
| MD5 | `edd092fecba64c7a79d4412d3b38eba1` |

Other versions of the disc with the same serial are untested. The
[library](../help/glossary.md#library) lists a Spyro 3 image under its file
name. Drag the image onto the window, or pass it on the command line, as
for Spyro 1 ([First run](../getting-started/first-run.md)):

```
openpete --no-card "Spyro - Year of the Dragon (v1.0).cue"
```

`--no-card` is required for now; see [Memory cards](#memory-cards).

## Memory cards

!!! warning "Current limitation: no memory card"
    Spyro 3 cannot use a memory card yet. With a card inserted, which is
    the default, the game hangs at the title screen and the program exits
    with status 2 after its hang check. Start it with
    `--no-card`<!-- id: flag:--no-card -->, or set it once in
    `openpete.toml`:

    ```toml
    [game.spyro-3]
    no_card = true
    ```

    Without a card the game cannot save.

## Levels

`--skip-to-level L`<!-- id: flag:--skip-to-level --> starts in level L,
given by name or by number, as on Spyro 1
([Practice tools](../playing/practice-tools.md#starting-in-a-level)).

| Homeworld | Levels |
| --- | --- |
| Sunrise Spring | `sunrise_spring` (10), `sunny_villa` (11), `cloud_spires` (12), `molten_crater` (13), `seashell_shore` (14), `mushroom_speedway` (15), `sheilas_alp` (16), `buzzs_dungeon` (17), `crawdad_farm` (18) |
| Midday Gardens | `midday_gardens` (20), `icy_peak` (21), `enchanted_towers` (22), `spooky_swamp` (23), `bamboo_terrace` (24), `country_speedway` (25), `sgt_byrds_base` (26), `spikes_arena` (27), `spider_town` (28) |
| Evening Lake | `evening_lake` (30), `frozen_altars` (31), `lost_fleet` (32), `fireworks_factory` (33), `charmed_ridge` (34), `honey_speedway` (35), `bentleys_outpost` (36), `scorchs_pit` (37), `starfish_reef` (38) |
| Midnight Mountain | `midnight_mountain` (40), `crystal_islands` (41), `desert_ruins` (42), `haunted_tomb` (43), `dino_mines` (44), `harbor_speedway` (45), `agent_9s_lab` (46), `sorceresss_lair` (47), `bugbot_factory` (48), `super_bonus_round` (50) |

The [dev overlay](../help/glossary.md#dev-overlay)'s **Level warp** moves
to any of these levels during play
([Dev overlay](../playing/dev-overlay.md#level-warp)).

### Sub-areas

`--skip-to-area L:A`<!-- id: flag:--skip-to-area --> (config key
`skip_to_area`) loads level L, by number or name, and enters its sub-area A
through the game's own sub-area transition, as the level's portal does. A
counts from 1. With no value, or an invalid target, it prints this list. It
replaces `--skip-to-level`'s level.

| Level | Sub-areas |
| --- | --- |
| `sunny_villa` (11), `cloud_spires` (12), `molten_crater` (13) | 1, 2 |
| `seashell_shore` (14) | 1, 2, 3 |
| `icy_peak` (21), `enchanted_towers` (22), `spooky_swamp` (23), `bamboo_terrace` (24) | 1, 2 |
| `frozen_altars` (31), `lost_fleet` (32), `charmed_ridge` (34) | 1, 2 |
| `fireworks_factory` (33) | 1, 2, 3 |
| `crystal_islands` (41), `desert_ruins` (42), `haunted_tomb` (43) | 1, 2 |
| `dino_mines` (44), `super_bonus_round` (50) | 1, 2, 3 |

```
openpete --game spyro-3 --no-card --skip-to-area seashell_shore:2
```

## Cutscenes

`--skip-to-cutscene C`<!-- id: flag:--skip-to-cutscene --> plays cutscene
C, named by the game's loading caption. Each starts from the level whose
event plays it in the game, and a cutscene that the game follows with
another plays on as in the game. With no value, or an unknown name, it
prints the list.

| Name | Starts from |
| --- | --- |
| `an_evil_plot_unfolds`, `a_powerful_villain_emerges`, `a_desperate_rescue_begins` | the title screen, through New Game; cannot be combined with `--skip-to-level` |
| `bianca_strikes_back`, `the_second_warning`, `no_hard_feelings` | level 10, `sunrise_spring` |
| `spike_is_born`, `hunters_tussle`, `byrd_james_byrd` | level 20, `midday_gardens` |
| `a_monster_to_end_all_monsters`, `an_apology_and_lunch`, `a_duplicitous_larcenous_ursine` | level 30, `evening_lake` |
| `the_escape` | level 37, `scorchs_pit` |
| `the_dancing_bear` | level 40, `midnight_mountain` |
| `deja_vu`, `a_familiar_face`, `billy_in_the_wall`, `one_less_noble_warrior` | level 47, `sorceresss_lair` |
| `the_end` | level 50, `super_bonus_round` |

## Cheats

Spyro 3 has no cheats in OpenPete: `--cheats`<!-- id: flag:--cheats -->
with any value stops the launch with exit status 3.

## Mods

Mods load on Spyro 3 ([Mods](../customizing/mods.md)); a code mod is
compiled for Spyro 3 when its manifest selects the game. Not available on
Spyro 3:

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
- Every level load waits 66 [VBLs](../help/glossary.md#vbl) by default
  (`--load-delay`<!-- id: flag:--load-delay --> default
  `66`<!-- id: knob:load_delay -->). During a load the game checksums its
  own code, a step it advances only while a disc read is pending; with
  reads that complete at once the step never finishes and the game stays
  on the loading screen. 66 is the lowest value at which loads were
  measured to complete. The
  value is recorded in an input recording's header, and a replay with
  another value is refused
  ([Recording](../playing/recording.md#replaying)).
- The default renderer is the native Vulkan renderer, `vk`, as on every
  game.
