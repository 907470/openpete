# DuckStation psxtrace series

The openpete project's own additions to DuckStation: the
`duckstation-regtest` psxtrace capture module and its recorders, which write
the reference trace the oracle gate (`tests/harness/check_psx_oracle.sh`)
replays and its sidecars, the pad script, movie playback, card import and
export, the motor log, an SPU final-mix WAV dump, a headless EGL build fix,
and NTSC scanlines of alternating length.

The submodule `third_party/duckstation` pins upstream
`https://github.com/stenzek/duckstation.git` at `e4c55ad0283f`. DuckStation
is licensed CC BY-NC-ND 4.0, which forbids distributing a modified
DuckStation, so the project distributes its additions and applies them
locally. The series holds no upstream text:

- `files/<path>`: files of the series, copied to `<path>` in the DuckStation
  tree; each path is absent upstream.
  - `src/core/openpete_hooks.h`: the hook variables the recorders set and
    the core calls (CPU instruction and exception, CD-ROM command, interrupt
    and register read, GTE instruction, root-counter read, controller command
    and motor), `SPU::GetVoiceHead`, `System::GetFrameStartTick`.
  - `src/core/openpete_crtc.h`: NTSC scanline pairs (below).
  - `src/core/openpete_cdrom.inl`, `src/core/openpete_spu.inl`: code that
    reads the CD-ROM's FIFOs and the SPU's voices and final mix, compiled
    inside `cdrom.cpp` and `spu.cpp` by one inserted `#include` each.
  - `src/duckstation-regtest/openpete_regtest.{h,cpp}`: every option of the
    test binary the series adds, and the pad script, movie player, motor
    log, card import and export, behind five calls from `regtest_host.cpp`.
  - `src/duckstation-regtest/{psxtrace,probe}_capture.{h,cpp}`: the
    recorders.
  - `src/duckstation-regtest/openpete.cmake`: adds the sources and `minizip`
    to the test binary.
- `hooks.txt`: the lines inserted into upstream files: hook calls,
  `#include` lines and the scanline overrides. No upstream line is removed
  or changed. Each insertion is located by an anchor: the first 16 hex
  digits of `sha256("openpete-anchor-1\0" + path + "\0" + window)`, where the
  window is the smallest run of upstream lines next to the insertion point
  that holds at least 48 non-blank characters and occurs once in the file.
  A 64-bit digest of a window of 48 or more characters cannot reproduce it;
  only hashing candidate windows of the upstream file finds it.
- `applied-head.txt`: the applied head.

NTSC scanlines: an NTSC line is 227.5 colour-subcarrier cycles, 3412.5 GPU
clocks, so lines alternate between 3413 and 3412 clocks and a VBL is
566120.83 CPU ticks (59.826 Hz); a PAL line is 3405 clocks. Upstream gives
every line 3413 (PAL 3406) clocks. The CRTC's `horizontal_total` field holds
the current line's length; the sites that span several lines (the line
wrap of the tick event, the ticks until the next CRTC event, the beam
position, the ticks until a beam position, the line and field frequencies)
are overridden after upstream computes them. Not carried: the save-state
field (no capture loads a state), the PAL display-window clamp (3406), the
GPU-dump replay's per-frame tick count and the debug window's text.

`scripts/setup_duckstation.sh` (environment `OPENPETE_DS_DIR` selects a
checkout other than `third_party/duckstation`) runs
`scripts/duckstation_hooks.py apply`: the pin's tree with the insertions
made and the files added, written with git's object commands without
filters or local configuration, committed with parent the pin, author and
committer `openpete psxtrace series <psxtrace-series@openpete.invalid>`,
both dates the pin's committer date and the message
`openpete psxtrace series`. That commit id, the applied head, is a function
of the pin and the series only. `applied-head.txt` records it; DuckStation
stamps it into the binary and every trace header as `scm_hash`, and the
oracle gate refuses a golden whose `scm_hash` differs from it.
`scripts/duckstation_hooks.py check` applies twice and asserts the same head
and that every upstream line of every target file is kept in order.

Changing the series: edit a checkout at the applied head (a series file, or
an inserted line in an upstream file), run
`scripts/duckstation_hooks.py derive <checkout> --pin <pin>` (it rewrites
`hooks.txt` and `files/` from the checkout's difference to the pin and
refuses a removed or changed upstream line), then
`scripts/setup_duckstation.sh --record`, and recapture every game's oracle
golden.

Pin bump: `apply` lists every insertion whose anchor no longer matches,
with its file, and refuses. `apply --partial` checks out the new pin with
the matching insertions made; make the listed ones by hand there (their
lines in `hooks.txt` say what goes where), run `derive`, `--record`, and
recapture the goldens.

Recorders and options: `docs/src/developer/duckstation-oracle.md`.
