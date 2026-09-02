# Sounds

`assets/sfx/` is the sound-effect overlay: drop a WAV in, the game plays
it in place of the original sample. No code, no SPU format, no size
budget. It works like the texture pack: the engine names every sound for
you, you edit or re-record the file, and the file name is its identity.

The substitution happens at the last possible point, when the emulated
SPU voice's PCM is handed to the mixer. The game's own sound tables, the
sample bank in SPU RAM, the voice's start address and pitch, and the
moment the game is told the sound has finished are all untouched, so a
sound mod cannot move a single byte of gameplay state.

## Dump → edit → drop in

```
openpete-spyro1 --disc SCUS-94228.cue --dump-sfx-pack dump/
```

reads every sample straight from the disc's WAD — every level, the
cutscenes, the intro, the titlescreen, dragon speech — and writes each
distinct one as a 16-bit mono WAV plus an `index.txt` census. No boot,
no level walking; a few seconds and roughly 40 MB. Custom levels are the
same layout: pass their Data files after the directory
(`--dump-sfx-wads mods/<id>/assets/levels/<stem>.wad`).

Files are named `<label>-<hash>-p<pitch>.wav`:

- `label` is the SoundTable slot name (`gemPickup`, `spyroCharge`,
  `sound_0x3d` for the ones the decomp has not identified) or `def<n>`
  for samples no slot points at, `speech<n>` for dragon speech. It is
  only there for you; the engine ignores it.
- `hash` is the content hash of the original sample. It is the
  identity: the same chime shipped in several levels hashes the same, so
  one file replaces it everywhere.
- `p<pitch>` is the base pitch the game plays that sample at (SPU units,
  `0x1000` = 44.1 kHz). The WAV is written at the matching rate, so it
  sounds as in-game at base pitch in any player.

Edit the file in any editor, keep the name, drop it in
`<mod>/assets/sfx/`. Any sample rate, bit depth (16/24/float) and
stereo are accepted; stereo is downmixed to mono with a warning, since
the SPU voice is mono and the game pans it.

## The three name forms

| Form | Matches | Meaning of the WAV |
|---|---|---|
| `<label>-<hash>-p<pitch>.wav` | that sample, at that base pitch | "at its own rate this is the sound at base pitch `pitch`" |
| `<label>-<hash>.wav` | that sample under every def | raw sample at the decoder's 44.1 kHz; the game pitches it |
| `<slotname>.wav` | whatever the current level maps to that role | "at its own rate this is the sound at this def's base pitch" |

The tagged form is what the dump writes and what you get by editing a
dumped file. It stays correct for other defs that share the sample: the
engine folds the tag into the playback rate, so a def that plays the
sample transposed still plays your file transposed the same way.

The symbolic form replaces a *role*: `gemPickup.wav` is the gem sound in
every level, including a custom level that shipped its own recording.
The tagged and untagged forms replace a *recording*: a custom level
that re-recorded a sound gets a new hash and is left alone until you
dump it and name a file for it. Pick the form you mean.

When more than one file matches, an exact tag wins over the untagged
form, which wins over a tag for another def, which wins over a symbolic
name; between two mods the later one in `enabled.txt` wins, the same
rule as every other asset namespace. The loader logs each collision as
`<winner> overrides "<file>" (was <loser>)`.

## What the game still decides

- **Duration.** The game is told a sound has ended when the original
  sample would have ended, and that moment is gameplay state — it does
  not move. A longer replacement still plays to its own end: when the
  game frees the voice at the stock end, the tail keeps ringing on a
  separate channel (it is silenced by a deliberate stop, a level load,
  mute and pause). A shorter file leaves silence. `index.txt` lists
  every sample's stock duration in milliseconds.
- **Pitch and variance.** The game's per-play pitch, its random
  variance and its pitch cycling all apply to your file as they did to
  the original. Add `-flat` before `.wav` (`gemPickup-flat.wav`,
  `<label>-<hash>-p0400-flat.wav`) to pin the file to its own rate for
  the whole play instead: no variance, no cycling, no slides. Audible
  only; the game's own pitch bookkeeping is untouched.
- **Volume and pan** are the game's.
- **Looping.** A sample the game loops keeps looping. A replacement with
  a `smpl` loop chunk (what samplers and DAWs write) loops that window;
  without one the whole file loops.

## Iteration

Sources are rescanned at every level load, so the loop is edit → re-enter
the level. Set `OPENPETE_SFXPACK_LOG=1` to have the engine name every
sample it keyed that no file covers (hash, address, base pitch, size).
`--sfxpack-dir DIR` points the base pack at a directory of your own;
mods' `assets/sfx/` always join it.

The `[sfxpack]` line at exit counts `keyed`, `resolved`, `replaced` and
`rings_out` (replacements that outlast their stock sample).

`OPENPETE_SFXPACK_VERIFY=1` is the round-trip check: with an unmodified
dump loaded, every key-on compares the file's PCM against the original
decoded live from SPU RAM and reports `verify_ok`/`verify_bad` counts at
exit. It is how the dump and the loader are held to "bit-identical".

## Your own one-shots

`assets/sfx/own/<name>.wav` is a mod's private sounds, played host-side
with `api->sfx_play(self, "<name>", gain)`. That directory is not
scanned for replacements.

Example: `mods/sfx-single-sound` (one symbolic file, no code).
