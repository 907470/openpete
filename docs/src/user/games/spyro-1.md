# Spyro the Dragon

The program is `openpete-spyro1` (`openpete-spyro1.exe` on Windows). Its
config sections are `[game.spyro-1]` and `[disc.SCUS-94228]`
([Config file](../customizing/config-file.md)).

## Disc

OpenPete plays the NTSC-U release, serial SCUS-94228, dumped to a
`.bin`/`.cue` pair with one MODE2/2352 data track of 281270 sectors. The
Redump-verified image has these hashes of the `.bin`:

| Hash | Value |
| --- | --- |
| SHA-256 | `fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37` |
| SHA-1 | `cf3ce6bedeb89dfbc40990336180f3b9b0f40d9f` |
| MD5 | `387185f71cd0043fd75a19521dc75104` |

The same image covers the original, Greatest Hits and Collectors' Edition
pressings. The [library](../help/glossary.md#library) names an image with this SHA-256
"Spyro the Dragon (NTSC-U)"; any other image is listed under its file name.

## Levels

`--skip-to-level L`<!-- id: flag:--skip-to-level --> (config key
`skip_to_level`) starts in level L, given by name or by number. Names are
not case-sensitive and accept `-` for `_`. In a window, the logos, title
and loading screens are [fast-forwarded](../help/glossary.md#fast-forward) behind a black window with the
sound muted, until the level is playable. `--skip-to-level` with no value,
or an unknown name, prints this list.

`--skip-to-area` <!-- id: flag:--skip-to-area --> enters a level's sub-area
in Spyro 3. Spyro the Dragon has no sub-area targets: the flag refuses every
value and the program does not start.

| Homeworld | Levels |
| --- | --- |
| Artisans | `artisans` (10), `stone_hill` (11), `dark_hollow` (12), `town_square` (13), `toasty` (14), `sunny_flight` (15) |
| Peace Keepers | `peace_keepers` (20), `dry_canyon` (21), `cliff_town` (22), `ice_cavern` (23), `doctor_shemp` (24), `night_flight` (25) |
| Magic Crafters | `magic_crafters` (30), `alpine_ridge` (31), `high_caves` (32), `wizard_peak` (33), `blowhard` (34), `crystal_flight` (35) |
| Beast Makers | `beast_makers` (40), `terrace_village` (41), `misty_bog` (42), `tree_tops` (43), `metalhead` (44), `wild_flight` (45) |
| Dream Weavers | `dream_weavers` (50), `dark_passage` (51), `lofty_castle` (52), `haunted_towers` (53), `jacques` (54), `icy_flight` (55) |
| Gnasty's World | `gnorc_gnexus` (60), `gnorc_cove` (61), `twilight_harbor` (62), `gnasty_gnorc` (63), `gnastys_loot` (64) |

`gnastys_world` is accepted for `gnorc_gnexus` (60) and `twilight_harbour`
for `twilight_harbor` (62).

The [dev overlay](../help/glossary.md#dev-overlay)'s **Warp: level** list and **Jump** button move to any of
these levels during play, through the game's own level exit
([Dev overlay](../playing/dev-overlay.md)).

## Cutscenes

`--skip-to-cutscene C`<!-- id: flag:--skip-to-cutscene --> (config key
`skip_to_cutscene`) plays cutscene C through the game's own transition.
With no value, or an unknown name, it prints this list.

| Name | Plays | Starts from |
| --- | --- | --- |
| `intro` | the opening cutscene | the title screen, through New Game; cannot be combined with `--skip-to-level` |
| `ending` | the cutscene after Gnasty Gnorc is defeated | level 63, `gnasty_gnorc` |
| `ending_loot` | the Gnasty's Loot ending; the credits then return to Artisans | level 64, `gnastys_loot` |
| `credits` | the short credits roll | the `--skip-to-level` level, else Artisans; the roll returns to that level |
| `credits_100` | the full credits roll of the 100% ending (also `full_credits`) | as `credits` |

## Cheats

`--cheats A,B`<!-- id: flag:--cheats --> (config key `cheats`) applies the
game's own cheat codes without entering them on the controller:

| Name | Effect | The game's code |
| --- | --- | --- |
| `99_lives` | sets Spyro's extra lives to 99 | "99 LIVES" |
| `unlock_all_levels` | marks every level visited and every homeworld unlocked | "LEVELS OPEN" |

A cheat is applied once per session, on the first [tick](../help/glossary.md#tick) in a level, and
again for each new session started from the title screen. Dying still costs
a life. The dev overlay's **Cheat: 99 lives**
<!-- id: knob:cheat_99_lives --> and **Cheat: unlock all levels**
<!-- id: knob:cheat_unlock_all_levels --> buttons apply a cheat on demand
during play.

!!! note
    Cheats change the game's state, as entering the code does. A run using
    them does not match an unmodified console run.

## Notes

- The default window is 640x448<!-- id: knob:window -->, twice the
  game's drawn area.
- Spyro 1 never reads the L3 button, so a controller's left-stick click
  toggles the [free camera](../help/glossary.md#free-camera) by default (`freecam` binding,
  [Key bindings](../customizing/key-bindings.md)).
- Level loads complete without an added wait by default
  ({{ flag("flag:--load-delay") }}<!-- id: flag:--load-delay --> default
  `{{ default("knob:load_delay", "spyro-1") }}`<!-- id: knob:load_delay --> on Spyro 1).
- The game has 48 music tracks, `track_00.wav` to `track_47.wav`
  ([Music and sound packs](../customizing/music-and-sound-packs.md)).
