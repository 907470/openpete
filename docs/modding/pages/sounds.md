# Sounds

`assets/sfx/<name>.wav` replaces a sound effect. The substitution happens
when the emulated SPU voice's PCM is handed to the mixer. The game's sound
tables, the sample bank in SPU RAM, the voice's start address and pitch,
and the tick on which the game is told the sound has ended are untouched,
so a sound mod never moves gameplay state.

## Glossary

| Term | Meaning |
|---|---|
| voice | One SPU playback channel. A voice is mono, and the game pans it. |
| slot | A sound-table entry the game plays by name, such as `gemPickup`. |
| definition | A slot's binding to one sample at one base pitch. The same slot resolves to different definitions in different levels. |
| hash | The content identity of the original sample. A sample shipped in several levels hashes the same, so one file replaces it everywhere. |
| tag | The `-p<pitch>` suffix, which pins a file to one definition instead of to every use of the sample. |
| ring out | What a replacement longer than the original does: keeps playing on a separate channel after the game frees the voice. |

## Dump, edit, drop in

```
openpete-spyro1 --dump-sfx-pack dump/
```

reads every sample from the disc's WAD (every level, the cutscenes, the
intro, the title screen, dragon speech) and writes each distinct one as a
16-bit mono WAV plus an `index.txt` census listing each sample's stock
duration. Add `--dump-sfx-wads <path>` for a custom level's WAD.

Files are named `<label>-<hash>-p<pitch>.wav`:

- `label` is the sound-table slot name (`gemPickup`, `spyroCharge`,
  `sound_0x3d` for slots the decompilation has not named), `def<n>` for
  samples no slot points at, or `speech<n>` for dragon speech. The
  matcher ignores it.
- `hash` is the content hash of the original sample. It is the identity:
  a sample shipped in several levels hashes the same, so one file
  replaces it everywhere.
- `p<pitch>` is the base pitch the game plays the sample at, in SPU units
  (`0x1000` = 44.1 kHz). The WAV is written at the matching rate.

Edit the file, keep the name, and place it in `<mod>/assets/sfx/`. Any
sample rate and bit depth (16, 24, float) is accepted. Stereo is
downmixed to mono with a warning: the SPU voice is mono and the game pans
it.

## Name forms

| Form | Matches | The WAV is |
|---|---|---|
| `<label>-<hash>-p<pitch>.wav` | That sample, at that base pitch | The sound at base pitch `pitch`, at the file's own rate |
| `<label>-<hash>.wav` | That sample under every definition | The raw sample at the decoder's 44.1 kHz; the game pitches it |
| `<slot>.wav` | Whatever the current level maps to that slot | The sound at that definition's base pitch, at the file's own rate |

The tagged form stays correct for other definitions that share the
sample: the engine folds the tag into the playback rate, so a definition
that plays the sample transposed plays the file transposed the same way.

The symbolic form replaces a role: `gemPickup.wav` is the gem sound in
every level, including a custom level that shipped its own recording.
The hash forms replace a recording: a re-recorded sound in a custom level
has a new hash and is left alone until a file names it.

Precedence when several files match: exact tag, then untagged hash, then
a tag for another definition, then symbolic name. A code registration
through `api->sfx_replace` outranks a symbolic file. Between mods the one
listed later in `enabled.txt` wins. Each collision is logged as
`<winner> overrides "<file>" (was <loser>)`.

## What the game still decides

- **Duration.** The game is told the sound ended when the original sample
  would have ended. A longer replacement keeps ringing on a separate
  channel after the game frees the voice (silenced by a deliberate stop,
  a level load, mute, and pause). A shorter file leaves silence.
- **Pitch and variance.** Per-play pitch, random variance, and pitch
  cycling apply to the replacement as to the original. A `-flat` suffix
  before `.wav` (`gemPickup-flat.wav`, `<label>-<hash>-p0400-flat.wav`)
  pins the file to its own rate for the whole play. Audible only; the
  game's pitch bookkeeping is untouched.
- **Volume and pan.**
- **Looping.** A sample the game loops keeps looping. A replacement with
  a `smpl` loop chunk loops that window; without one the whole file
  loops.

## Iteration

Sources are rescanned at every level load: edit, then re-enter the level.
`OPENPETE_SFXPACK_LOG=1` logs every sample keyed that no file covers
(hash, address, base pitch, size). `--sfxpack-dir DIR` points the base
pack at a directory outside any mod; mod roots always join it.

The `[sfxpack]` line at exit counts `keyed`, `resolved`, `replaced`, and
`rings_out` (replacements that outlast their stock sample).
`OPENPETE_SFXPACK_VERIFY=1` compares each keyed file's PCM against the
original decoded from SPU RAM and reports `verify_ok` and `verify_bad`
counts; with an unmodified dump loaded, `verify_bad` is 0.

## A mod's own sounds

`assets/sfx/own/<name>.wav` holds sounds played host-side with
`api->sfx_play(self, "<name>", gain)`. That directory is not scanned for
replacements.

Example: `declarative-example` (one symbolic file among its assets, no
code).
