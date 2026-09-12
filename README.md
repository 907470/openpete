# OpenPete

Cross-platform PC port of *Spyro the Dragon* (PSX, 1998), built on a hybrid
static-recompilation + matching-decomp foundation. Designed to share its core
runtime across multiple PSX titles in the Spyro trilogy as their decomps
mature.

> Status: **Phase 0b (bootstrap)** complete. Phase 1 (recompiler integration)
> in progress. See `notes/plans/openpete-pc-port.md` for the full plan.

## Architecture (one paragraph)

The PSX EXE is statically recompiled to C by `psxrecomp` (vendored under
`third_party/psxrecomp/`). The PSYQ system library calls are intercepted at
the symbol level and serviced by `PsyCross`, which provides a modern
SDL2 + OpenGL + audio backend. As individual functions become available
in C from the matching decomp at `games/spyro-1/spyro-1/`, they are
swapped in via a manifest-driven thunk infrastructure (Phase 2+). Per-game
specifics (entry points, overlay layouts, runtime quirks) live behind a
narrow `core/include/game_extras.h` abstraction; `core/` itself stays
game-agnostic so a future Spyro 2 or 3 port reuses the same runtime.

## Quick start

```bash
# 1. Initialize submodules.
git submodule update --init --recursive

# 2. Validate setup (disc image + submodules + decomp build artifacts).
./scripts/setup.sh

# 3. Build for native Linux. Requires `libopenal-dev` (PsyCross audio) and
#    libav* (the --record video encoder; see "Recording gameplay video").
sudo apt install libopenal-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
cmake --preset linux-release
cmake --build build/linux-release -j

# 4. (Optional) cross-compile to Windows from WSL2. NOTE: --record video
#    capture is compiled out of the Windows cross build (no MinGW libav);
#    the rest of the game is unaffected. See "Recording gameplay video".
sudo apt install gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
cmake --preset windows-mingw
cmake --build build/windows-mingw -j

# 5. Run the developer-loop sanity check.
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --headless --frame-limit 60 --screenshot-on-frame 30 \
    --screenshot-output ./shot.bmp --max-runtime-seconds 30 --log-format json
```

## Config file

Settings live in `openpete.toml`, so you don't retype your preferences
every launch. Two are read: one **next to the executable** (what a build
or a download ships with) and one in your config dir
(`~/.config/openpete/openpete.toml`, or `%APPDATA%\openpete\` on Windows).
Which one the in-game menus save to depends on the layout below.

```toml
# openpete.toml
aspect = "21:9"
window = "1920x1080"
fov    = 90

[game.spyro-1]
disc  = "games/spyro-1/spyro-1/build/SCUS-94228.cue"
lod   = "extended"
music = 0                # the game's own MUSIC slider, 0 (OFF) .. 10; sfx = N likewise

[disc.SCUS-94228]        # just this release
vertex_precision = "integer"
```

Key names are the flag names without the dashes (`--fog-scale` →
`fog_scale`); `--help` lists them all. Two escape hatches for anything
that has no key of its own:

```toml
[args]                    # keys are flags, exactly as you'd type them
"--screenshot-on-tick" = "1200"
[env]                     # keys are OPENPETE_* variables
OPENPETE_TERRAIN_TIMERS = "1"
```

`[args]` and `[env]` are **not global-only** — like every sub-table here
they belong to whichever section they're written under, so per-game and
per-disc overrides work the same way plain keys do:

```toml
[game.spyro-1.args]
"--screenshot-on-tick" = "1200"
[disc.SCUS-94228.env]
OPENPETE_TERRAIN_TIMERS = "1"
```

### Where your files go

On **Windows** an install is portable: everything openpete writes stays in
the game folder, next to the exe.

```
openpete-spyro1.exe
openpete.toml     your settings (what the in-game menus save)
cards/            memory cards — card1.mcr, card2.mcr
states/           savestate slots
library/          ingested discs and their compiled modules
mods/
screenshots/      PNGs the screenshot key writes
```

On **Linux** the first three go to the per-user directories instead
(`~/.config/openpete` for the config, `~/.local/state/openpete` for cards
and states), because that is where a Linux install is expected to put
them. `library/` and `mods/` are always next to the exe on both;
`screenshots/` follows on Linux to `~/.local/share/openpete/screenshots`
unless a `screenshots/` folder already sits next to the exe, in which case
that one is used.

Either platform will do either — say so at the top of `openpete.toml`:

```toml
portable = true    # settings, cards and states in the game folder
portable = false   # settings, cards and states in the per-user directories
```

A portable install whose folder isn't writable — under `Program Files`,
say — falls back to the per-user directories and says so at startup.
`--config-report` always prints the three resolved paths.

### Controls

Every key and pad button is rebindable, under `[keys.pad]` (the PSX
controller) and `[keys.host]` (openpete's own hotkeys) — again in any
section, so a binding can be global or per-game:

```toml
[keys.pad]
cross = ["C", "pad:south"]     # any of these presses it
l2    = "Left Ctrl"

[keys.host]
savestate_save = "F5"
savestate_load = "F9"
quit           = "none"        # stop Escape quitting the game
rewind         = "none"        # unbind it entirely

[game.spyro-1.keys.host]
freecam = "F"                  # just for this game
```

Keyboard bindings are key names as SDL spells them (`"F5"`,
`"Left Shift"`, `"Keypad 5"`) and are **positional** — on AZERTY, `"A"`
is the key where a US layout has A. Pad bindings take a `pad:` prefix
(`"pad:south"`, `"pad:leftshoulder"`, `"pad:-leftx"` to invert an axis).
A list binds several sources to one action; `"none"` or `[]` unbinds.

Escape quits by default; `quit = "none"` turns that off, and Escape still
dismisses an open menu or overlay. The window close button always quits
whatever `quit` is bound to.

`F12` saves a PNG of the frame on screen — the native renderer's own
output, so it carries whatever the window is showing (widescreen, mods,
overlays). It lands in `screenshots/` (see "Where your files go") under a
timestamped name; rebind it as `screenshot` under `[keys.host]`.

`--keys-report` prints every action, what it's bound to, which section
bound it, and the label your keyboard layout gives each key. Development
instruments (the fill-tag inspector, the interpolation and LOD A/B
toggles, the dev overlay) ship **unbound** and show up there as `dev` —
bind one in your own config if you want it.

### On-screen notifications

Changing a setting raises a toast in the corner — the game's own gold
frame and 3D lettering, not a debug overlay. It fades in, holds a few
seconds and fades out; several stack. Mods raise them too.

```toml
osd          = "off"          # or leave them on (the default)
osd_position = "bottom-right" # any of the six corners and edges
osd_timeout  = 5              # seconds
perf_position = "top-left"    # the FPS / pacing block; defaults to top-right
```

The notifications and the performance-stats block share a placement
ledger, so pointing both at one corner is fine: the stats block is
permanent and claims its spot, and the toasts stack clear of it. Both
also step aside for the settings menu (`O`) while it is open, so nothing
is ever hidden behind the panel.

Toasts stay out of `--record` video by default, like the stats block —
`--record-fps-overlay` lets both in when you actually want them filmed.

### Which setting wins

Lowest first:

```
openpete.toml global  <  [game.ID]  <  [disc.SERIAL]  <  [disc.SHA]  <  [saved]
  <  environment variables  <  a movie's .bk2.args sidecar  <  the flags you type
```

Two rules cover everything else. **A more specific section always beats a
less specific one**, however either is written — a `[args]` line in the
global scope never overrides a plain key under `[disc.…]`. And **inside a
section, `[args]` beats `[env]` beats a bare key**, mirroring the way a
typed flag beats an exported variable, so moving a line between the file
and your shell never changes which setting wins. Where two files set the
same thing at the same scope, yours beats the shipped one.

`--config-report` prints the whole resolution — every setting, which layer
won it, and what that layer beat. `--config PATH` uses one file and
nothing else; `--no-config` ignores all of them.

### Saved settings

The in-game menus (`O`, and `M` for the full list) can write what you
changed into a `[saved]` block at the bottom of your config file.
Everything above the managed marker is left exactly as you wrote it,
comments included.

Saved settings beat everything else in the file, but still lose to a flag
you type or a variable you export for that launch — so scripts and movie
replays stay reproducible. When a row is in that position the menu marks
it *pinned*, naming the override.

### Retired: openpete.opts / openpete.env

Neither file is read any more; if one is sitting next to the executable,
openpete prints a line saying it is ignored. Their flags and variables are
written as `[args]` and `[env]` entries in `openpete.toml` instead — see
[Config file](#config-file).

## Recording gameplay video

`--record PATH` captures gameplay to a video file. Capture is **true VFR**:
one encoded frame per game-presented frame, timestamped with the wall-clock
instant it was drawn, so the file plays back at the real game cadence (a
load-induced dip to 24 fps shows up as 24 frames in that wall-second, not a
re-timed 30). Frames are read from the renderer's present FBO at internal
resolution and cropped to the canonical PSX visible region — the recording
is **bars-free** and independent of window size (a 21:9 scene records the
same internal-res frame whether the window is 640 or 1280 wide; window
*height* sets the internal render resolution).

Recordings include **audio** by default: the mixed SPU output is captured via
an OpenAL loopback device (active only while recording — normal playback is
unchanged) and encoded as a second AAC stream. Audio is on by default; pass
`--mute 1` to silence the recording. Use `--no-record-audio` for video only,
or `--record-audio-encoder NAME` to pick a different encoder.

```bash
# Record the attract demo to an .mkv (recommended container).
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
 --record gameplay.mkv

# Widescreen + higher internal resolution, custom quality.
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
 --aspect 21:9 --window 1920x1080 \
    --record gameplay.mkv --record-crf 18
```

Flags: `--record-encoder` (libavcodec encoder, default `libx264`; e.g.
`h264_nvenc` for GPU encode), `--record-crf` (quality, default 23, lower is
better), `--record-gop` (keyframe interval, default 120), `--record-pixfmt`
(default `yuv420p`). `.mkv` and `.mp4` are auto-detected by extension; `.mkv`
is preferred (honest VFR, survives an interrupted run). Ctrl-C finalizes a
playable file. Requires the libav* dev packages from step 4 above; linking
`libx264` makes the resulting binary GPL — use `--record-encoder libopenh264`
(BSD) if that matters for distribution. The MinGW Windows cross build has no
target libav, so `--record` is compiled out there (it warns once and is
otherwise a no-op); a native Windows (MSVC + vcpkg ffmpeg) build can re-enable
it.

> Note: the default `libx264` software encode is comfortable at normal frame
> rates. Capture currently uses a synchronous GPU readback, so at very high
> host frame rates it can add a few percent of overhead; an async path is
> planned (see `notes/plans/openpete-record-video.md`).

### Constant-frame-rate capture

`--record-cfr RATE` swaps true VFR for a fixed cadence: frame N is stamped at
exactly N/RATE, wall-clock never enters the timestamps, and the container
carries the exact NTSC rational (`30000/1001`, `60000/1001`, ...). RATE must be
an integer multiple of the 29.97 Hz sim-tick rate — **29.97, 59.94, 119.88**,
up to 16x — because only a whole number of presents per tick gives the sub-tick
interpolator an even number of intermediates to emit.

The mode also takes over pacing. Presents are scheduled on a clock the pacer
advances itself rather than on wall time, so exactly RATE/29.97 presents are
produced per tick however slowly the machine renders — a software rasterizer
and a fast GPU emit the identical frame sequence. Pair it with `--record-frames
N` to close the capture on an exact frame count (and end the run there): the
clip then covers exactly N/RATE seconds of *game* time, so the same window
captured at 29.97, 59.94 and 119.88 spans the identical moment with 1x, 2x and
4x the frames. Audio is dropped in this mode (there is no wall-clock second for
it to sit in), and `--mute 1` keeps the run itself quiet.

```bash
# The same 1220-tick window at three rates: 1220 / 2440 / 4880 frames,
# 40.708 s each, byte-for-byte the same game moments.
for r in 29.97:1220 59.94:2440 119.88:4880; do
  ./build/linux-release/games/spyro-1/openpete-spyro1 \
      --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
      --aspect 16:9 --window 1280x720 --mute 1 --no-record-audio \
      --record-cfr "${r%%:*}" --record-frames "${r##*:}" \
      --record-crf 22 --record "clip-${r%%:*}.mp4"
done
```

The recorder writes a valid CFR MP4 directly; a `-c copy` remux is still worth
running to add `+faststart` for web delivery.

## Repository layout

See `notes/plans/openpete-pc-port.md` §3. Briefly:

- `core/` — game-agnostic runtime (window, input, audio, dispatcher, GL).
  Must contain zero Spyro-specific addresses or symbols.
- `games/spyro-1/` — Spyro 1 binary entry; manifest, generated thunks,
  per-game `game_extras_impl.c`. `games/spyro-1/spyro-1/` is the matching
  decomp submodule.
- `third_party/` — vendored deps (psxrecomp, libco, GLAD, stb) and
  submodules (PsyCross, SDL, cubeb).
- `tools/` — codegen + image diff scripts.

## Custom music (WAV override)

Drop PCM WAV files into `assets/music/` **next to the executable** and
they play in place of the disc's XA-ADPCM music streams. The build
stages the directory for you, seeded with a template `manifest.txt`
that lists all 48 tracks and their in-game usage (your edits to it are
never overwritten by a rebuild):

```
assets/music/track_00.wav     # NN = in-game track index, zero-padded 00..47
assets/music/track_01.wav
...
assets/music/manifest.txt     # template with the full track table
```

`manifest.txt` maps track indices to arbitrary relative paths, one entry
per line (`#` starts a comment); manifest entries take precedence over
`track_NN.wav`:

```
# index  path-relative-to-assets/music/
0  artisans_home.wav
1  town_square.wav
```

Supported format: PCM int16, mono or stereo, at 11025/22050/32000/44100/
48000 Hz (e.g. `ffmpeg -i in.flac -ac 2 -ar 44100 -sample_fmt s16 out.wav`).
Anything else is rejected with a log warning and that track falls back to
the disc's XA audio. Tracks without an override file also fall back —
partial coverage is fine.

Track looping and the long-idle easter-egg track rotation are driven by
the game itself and keep working; if a WAV is shorter than the original
track's window there is silence until the game loops it, and if longer it
is cut at the loop point.

The disc image is still required — the override only replaces music
playback; level data, SFX, and everything else still come from the disc.

## Custom textures (HD packs)

Drop replacement PNGs into `assets/textures/` **next to the executable**
and the native renderer displays them in place of the original PSX
textures — 3D world/moby textures, HUD sprites, and the title logo
alike. Any resolution works (the image is sampled normalized over the
texel box it replaces, with linear filtering and real per-texel alpha).
Presentation-only by construction: replacements never touch the game's
VRAM or any gameplay-readable byte.

The workflow is **dump → edit → drop in**:

```
OPENPETE_TEXPACK_DUMP_DIR=dumps ./openpete-spyro1 ...   # play; textures dump as they draw
# edit/upscale dumps/<name>.png at any resolution, keep the filename
cp dumps/<name>.png assets/textures/
```

Filenames are the identity — `<content-hash>-<x>x<y>+<w>x<h>-<palette-
hash>-<bpp>bpp.png`, derived from the *uploaded* texture data, so they are
stable across levels, VRAM repacking, and sessions. A file covers every
draw that samples inside its texel box (one tile file serves both terrain
LOD tiers). Partial packs are fine — anything without a replacement draws
the original. `OPENPETE_TEXPACK_DIR=<dir>` overrides the pack location.

Replacement applies in the native (`--renderer vk`) display path, which is the
default for Spyro 1; `--headless` and the PsyCross reference path always render
the originals.

## Custom sounds (sfx packs)

Drop replacement WAVs into `assets/sfx/` **next to the executable** (or a
mod's `assets/sfx/`) and they play in place of the original sound
effects — every level's samples, dragon speech, the intro and titlescreen
alike. Presentation-only by construction: the swap happens when the
emulated SPU voice's PCM reaches the mixer, so the game's sound tables,
sample bank, pitch and the tick a sound ends on are all untouched.

The workflow is **dump → edit → drop in**, and the dump needs no gameplay:

```
./openpete-spyro1 --disc SCUS-94228.cue --dump-sfx-pack dumps/   # every sample, from the WAD
# edit/re-record dumps/<name>.wav in any editor, keep the filename
cp dumps/<name>.wav assets/sfx/
```

Filenames are the identity — `<slot-name>-<content-hash>-p<pitch>.wav` —
so one file covers the same sample wherever the game plays it, and
`<slot-name>.wav` alone replaces that role in every level. Any rate, bit
depth or channel count is accepted (stereo is downmixed; the SPU voice is
mono). A replacement longer than the original rings out on its own
channel after the game has moved on; `-flat` before `.wav` pins a file to
its own rate, ignoring the game's pitch variance and cycling.
`--sfxpack-dir <dir>` overrides the pack location; sources are rescanned
at every level load. See `docs/modding/pages/sounds.md` for the name forms
and the round-trip check.

## Disc requirement

Original *Spyro the Dragon* (NTSC-U, SCUS-94228) BIN/CUE pair, as a single
MODE2/2352 data track of 281270 sectors. The .bin's SHA256 must be
`fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37`; that is
the Redump-verified image, whose SHA1 `cf3ce6bedeb89dfbc40990336180f3b9b0f40d9f`
and MD5 `387185f71cd0043fd75a19521dc75104` can be checked against Redump's
own database entry. The same image covers the Original, Greatest Hits, and
Collectors' Edition pressings. The disc library validates the SHA256 at
ingest.

Development runs use the disc rebuilt from the decomp output,
`games/spyro-1/spyro-1/build/SCUS-94228.cue`, which is the default for every
script and run command; `OPENPETE_DISC` or
`--disc` overrides it.

End-user distribution builds don't use this path at all — they ingest a
disc into a local library on first run (see
`notes/plans/openpete-distribution-local-recomp.md` and the generated
`README.md` in a packaged archive).

## License

Source code is offered under PolyForm Noncommercial 1.0.0 (`LICENSE`). The
Spyro decompilation submodule, PsyCross, psxrecomp, and other vendored deps
retain their respective upstream licenses; see `NOTICE.md`.

This project is not affiliated with or endorsed by Sony, Insomniac Games,
or Activision.
