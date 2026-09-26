# Music and sound packs

WAV files placed in `assets/music/` and `assets/sfx/` play in place of the
game's music and sound effects. The replacement happens at playback: the
game's sound tables, pitches and timing are unchanged, so a pack never
changes gameplay. The disc is still required; level data and every sound
without a replacement come from it.

Both folders live in your data directory:

=== "Windows"

    `<install folder>\assets\music\` and `<install folder>\assets\sfx\`

=== "Linux"

    `~/.local/share/openpete/assets/music/` and `.../assets/sfx/`
    (under `$XDG_DATA_HOME/openpete/` when `XDG_DATA_HOME` is set)

A pack published as a mod installs as a mod instead ([Mods](mods.md)).
For both kinds, an enabled mod's file beats yours, and yours beats a file
next to the program; among mods, the later one in load order wins.

## Music

`assets/music/track_NN.wav` replaces music track NN, a two-digit index
from `00` to `47` in Spyro 1. The Modding site's
[Music](https://openpete.com/docs/modding/music.html) page lists which
track plays where.

A `manifest.txt` in the same folder maps a track to any file name, one
`<index> <path>` pair per line, `#` starting a comment. A manifest entry is
tried before `track_NN.wav`.

```
# index  path relative to assets/music/
0  artisans_home.wav
2  stone_hill.wav
```

| Requirement | Value |
| --- | --- |
| Format | PCM WAV, 16-bit integer samples, mono or stereo |
| Sample rate | 11025, 22050, 32000, 44100 or 48000 Hz |

Convert other files with, for example,
`ffmpeg -i in.flac -ac 2 -ar 44100 -sample_fmt s16 out.wav`. A file in
another format is skipped with a warning in `openpete.log`, and that track
plays from the disc. A file is looked up each time its track starts, so a
file added while the game runs plays the next time the track starts.

The game still decides when a track loops. A replacement shorter than the
original is followed by silence until the loop point; a longer one is cut
there.

## Sound effects

`assets/sfx/<name>.wav` replaces a sound effect. Names are computed from the
original sounds, so start from a dump:

1. Write every sound on the disc into a folder:
   ```
   openpete-spyro1 --dump-sfx-pack dumps
   ```
   <!-- id: flag:--dump-sfx-pack -->
   It reads the most recently played disc in your [library](../help/glossary.md#library), or the image
   named by `--disc`. The folder receives one WAV per distinct sound and an
   `index.txt` listing each sound's original duration. The program exits
   when done; no window opens.
2. Edit or re-record the WAVs, keeping each file name.
3. Copy the edited files into `assets/sfx/`.

Any sample rate, bit depth (16-bit, 24-bit, float) and channel count is
accepted; stereo is mixed down to mono, because the console plays each
sound on a mono channel. A replacement longer than the original keeps
playing after the game has moved on. The folder is read again at every
level load.

`--sfxpack-dir DIR`<!-- id: flag:--sfxpack-dir --> (config key
`sfxpack_dir`, variable `OPENPETE_SFXPACK_DIR`) replaces the folder next to
the program and your own folder with one folder; mods still apply on top.

The name forms (one sound everywhere, one sound at one pitch, or one role
such as the gem pickup in every level) are documented on the Modding
site: [Sounds](https://openpete.com/docs/modding/sounds.html).
