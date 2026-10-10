# Recording gameplay video

`--record PATH` captures gameplay to a video file. The `--record` family
exists only in a build with libav (`OPENPETE_ENABLE_RECORD`, default ON,
`core/CMakeLists.txt:537`); both distribution presets turn it off
(`cmake/presets/linux-release-launcher.cmake:10`,
`cmake/presets/windows-mingw-launcher.cmake:17`), and the MinGW cross build
links libav only when a static FFmpeg exists at `third_party/ffmpeg-mingw/`
(`scripts/setup_mingw_ffmpeg.sh` builds one; see
[Building](building.md#troubleshooting)). Without libav `record.c` is a stub that warns once and
ignores the flag, and `--help` omits the family.

## True VFR (the default)

One encoded frame per present, timestamped in microseconds with the
wall-clock instant it was drawn, so the file plays back at the cadence the
game was presented at: a load-induced dip to 24 presents per second shows
up as 24 encoded frames in that wall-second. Frames are read from the
renderer's present FBO at internal resolution and cropped to the canonical
PSX visible region, so the recording carries no letterbox bars and does
not depend on window width (a 21:9 scene records the same internal-res
image whether the window is 640 or 1280 wide; window height sets the
internal render resolution).

Recordings include audio by default: the mixed SPU output is captured
through an OpenAL loopback device (active only while recording) and
encoded as a second stream (`--record-audio-encoder`, default `aac`).
`--no-record-audio` records video only; `--mute 1` silences the run and the
recording.

```bash
# Record the attract demo to an .mkv (recommended container).
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
    --record gameplay.mkv

# Widescreen, higher internal resolution, custom quality.
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
    --aspect 21:9 --window 1920x1080 \
    --record gameplay.mkv --record-crf 18
```

| Flag | Default | Effect |
|---|---|---|
| `--record-encoder NAME` | `libx264` | libavcodec encoder (`h264_nvenc` for GPU encode, `libopenh264`) |
| `--record-crf N` | 23 | quality for libx264/libx265; lower is better |
| `--record-gop N` | 120 | keyframe interval in encoded frames |
| `--record-pixfmt NAME` | `yuv420p` | encoder pixel format |
| `--record-audio-encoder NAME` | `aac` | libavcodec audio encoder |
| `--no-record-audio` | off | video only |
| `--record-fps-overlay 0\|1` | 0 | burn the FPS overlay and the notifications into the video |

Defaults are the rows' declarations (`def_str`, `default_note`) in
`core/config/knob_registry.c`; `--help` interpolates `def_str`.
`.mkv` and `.mp4` are detected by extension; `.mkv` survives an
interrupted run. Ctrl-C finalizes a playable file. Linking `libx264` makes
the binary GPL; `--record-encoder libopenh264` (BSD) avoids that.

Capture uses a synchronous GPU readback, which costs a few percent at high
present rates.

## Constant-rate capture

`--record-cfr RATE` replaces VFR with a fixed cadence: encoded frame N is
stamped at N/RATE, wall-clock time never enters the timestamps, and the
container carries the exact NTSC rational (time base
`37609/(1125000*k)`, `core/record/record.c:462-470`). RATE must be an
integer multiple k of the tick rate 1125000/37609 Hz (29.913, 59.826,
119.652, up to 16x, `OPENPETE_RECORD_CFR_MAX_MULT`), because only a whole
number of presents per tick gives the sub-tick interpolator the same number
of intermediates on every tick. The parser rounds a value within 0.5 % of a
multiple (30, 60, 120 are accepted and snapped, with a stderr line) and
rejects anything else (`core/config/options.c:1427-1454`).

The mode also takes over pacing: presents are scheduled on a clock the
pacer advances itself, so exactly k presents are produced per tick however
slowly the machine renders; a software rasterizer and a fast GPU emit the
same sequence. `--record-frames N` closes the capture after exactly N
encoded frames and ends the run, a window of N/RATE seconds of game time,
so the same window captured at 1x, 2x and 4x spans the same game moments
with 1x, 2x and 4x the encoded frames. CFR implies `--no-record-audio`.

```bash
# The same 1220-tick window at three rates: 1220 / 2440 / 4880 encoded
# frames, 40.785 s of game time each.
for r in 29.913:1220 59.826:2440 119.652:4880; do
  ./build/linux-release/games/spyro-1/openpete-spyro1 \
      --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
      --aspect 16:9 --window 1280x720 --mute 1 \
      --record-cfr "${r%%:*}" --record-frames "${r##*:}" \
      --record-crf 22 --record "clip-${r%%:*}.mp4"
done
```

The recorder writes a valid CFR MP4; a `-c copy` remux adds `+faststart`
for web delivery.
