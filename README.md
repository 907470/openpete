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

# 2. Apply local patches to vendored third-party submodules
#    (e.g. PsyCross Linux build fixes — see patches/ for sources).
./tools/apply_third_party_patches.sh

# 3. Validate setup (disc image + submodules + decomp build artifacts).
./scripts/setup.sh

# 4. Build for native Linux. Requires `libopenal-dev` (PsyCross audio) and
#    libav* (the --record video encoder; see "Recording gameplay video").
sudo apt install libopenal-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
cmake --preset linux-release
cmake --build build/linux-release -j

# 5. (Optional) cross-compile to Windows from WSL2.
sudo apt install gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
cmake --preset windows-mingw
cmake --build build/windows-mingw -j

# 6. Run the developer-loop sanity check.
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --headless --frame-limit 60 --screenshot-on-frame 30 \
    --screenshot-output ./shot.bmp --max-runtime-seconds 30 --log-format json
```

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
unchanged) and encoded as a second AAC stream. Pass `--mute off` to actually
hear/record sound (audio is muted by default). Use `--no-record-audio` for
video only, or `--record-audio-encoder NAME` to pick a different encoder.

```bash
# Record the attract demo to an .mkv (recommended container).
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
    --run-recomp 0x80012204 --record gameplay.mkv

# Widescreen + higher internal resolution, custom quality.
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
    --run-recomp 0x80012204 --aspect 21:9 --window 1920x1080 \
    --record gameplay.mkv --record-crf 18
```

Flags: `--record-encoder` (libavcodec encoder, default `libx264`; e.g.
`h264_nvenc` for GPU encode), `--record-crf` (quality, default 23, lower is
better), `--record-gop` (keyframe interval, default 120), `--record-pixfmt`
(default `yuv420p`). `.mkv` and `.mp4` are auto-detected by extension; `.mkv`
is preferred (honest VFR, survives an interrupted run). Ctrl-C finalizes a
playable file. Requires the libav* dev packages from step 4 above; linking
`libx264` makes the resulting binary GPL — use `--record-encoder libopenh264`
(BSD) if that matters for distribution.

> Note: the default `libx264` software encode is comfortable at normal frame
> rates. Capture currently uses a synchronous GPU readback, so at very high
> host frame rates it can add a few percent of overhead; an async path is
> planned (see `notes/plans/openpete-record-video.md`).

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

## Disc requirement

Original *Spyro the Dragon* (NTSC-U, SCUS-94228) BIN/CUE pair. The .bin's
SHA256 must be `95f03abf97c9ff0b2a64888ed7dbbb4b59a7b4363cf188cd0a562b95cfd4809f`.
`scripts/setup.sh` validates this. The default expected location is
`<spyro>/roms/SCUS-94228/SCUS-94228.bin`; override with the
`OPENPETE_DISC` environment variable.

## License

Source code is offered under PolyForm Noncommercial 1.0.0 (`LICENSE`). The
Spyro decompilation submodule, PsyCross, psxrecomp, and other vendored deps
retain their respective upstream licenses; see `NOTICE.md`.

This project is not affiliated with or endorsed by Sony, Insomniac Games,
or Activision.
