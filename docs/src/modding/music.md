# Music

`assets/music/track_NN.wav` replaces music track NN, where NN is the
zero-padded index into the game's music table (`track_20.wav` is the
title theme). Spyro 1 only: Spyro 2 and 3 select music by disc position
and channel and have no track index, so they have no music override.
Nothing here reaches guest RAM: the substitution happens on
the host side, so a music-only mod is non-mutative.

When several enabled mods ship the same file, the mod listed later in
`enabled.txt` wins. Mod roots outrank the base asset directory and the
player's own data directory.

A `manifest.txt` in the same directory maps other paths to indices, one
`<index> <relative path>` pair per line; a later line for the same index
wins. Both forms work in a mod's `assets/music/` and in the base
directories, and within one directory the manifest mapping is tried
first.

The WAV is PCM 16-bit, mono or stereo, at 11025, 22050, 32000, 44100, or
48000 Hz. It loads whole into RAM.

## Spyro 1 track indices

The index is the entry in the game's `m_MusicTable`:

| File | Plays in |
|---|---|
| `track_00.wav` | Artisans |
| `track_01.wav` | Wild Flight |
| `track_02.wav` | Stone Hill |
| `track_03.wav` | Dry Canyon |
| `track_04.wav` | Haunted Towers |
| `track_05.wav` | Gnorc Cove |
| `track_06.wav` | Sunny Flight |
| `track_07.wav` | Terrace Village |
| `track_08.wav` | Doctor Shemp |
| `track_09.wav` | Wizard Peak |
| `track_10.wav` | Icy Flight |
| `track_11.wav` | Night Flight |
| `track_12.wav` | Dark Passage |
| `track_13.wav` | Ice Cavern |
| `track_14.wav` | Lofty Castle |
| `track_15.wav` | Beast Makers |
| `track_16.wav` | Blowhard |
| `track_17.wav` | Peace Keepers |
| `track_18.wav` | Crystal Flight |
| `track_19.wav` | Gnasty's Loot |
| `track_20.wav` | Title screen |
| `track_21.wav` | Misty Bog |
| `track_22.wav` | Alpine Ridge |
| `track_23.wav` | Metalhead |
| `track_24.wav` | Twilight Harbor |
| `track_25.wav` | Gnasty Gnorc |
| `track_26.wav` | Town Square |
| `track_27.wav` | Cliff Town |
| `track_28.wav` | Jacques |
| `track_29.wav` | Tree Tops |
| `track_30.wav` | Unused |
| `track_31.wav` | Magic Crafters |
| `track_32.wav` | Dark Hollow |
| `track_33.wav` | Toasty, Gnorc Gnexus |
| `track_34.wav` | Dream Weavers |
| `track_35.wav` | Unused |
| `track_36.wav` to `track_47.wav` | Alternate level tracks (`track_46.wav` is also High Caves) |

A level's own track loops. Once it has played through and the level's
idle timer has elapsed, the game swaps in one of three alternate tracks
drawn at random from the 36 to 47 range, and repeats on the same timer.
Replacing a level's track alone leaves the alternates stock.

A custom level names its own song with the `[[level]]` `music` key, see
[Custom levels](levels.md).
