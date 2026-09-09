# DuckStation as the openpete PSX oracle

The oracle is the gate that decides whether a change lands: a
DuckStation capture of Spyro 1 running on a real Sony SCPH1001 BIOS is the
project's proxy for hardware, and openpete must reproduce its
gameplay-state hashes tick for tick. This document covers both halves — the
capture side (the DuckStation fork) and the verify side
(`scripts/check_psx_oracle.sh`).

## Why DuckStation

DuckStation replaced PCSX-Redux as the capture emulator for two reasons.

1. Game compatibility. PCSX-Redux's GPU emulation stops on Spyro 1's
   cyclorama renderer (`func_8004EBA8`, about 120 VBLs in): the GPU
   thread stalls on a `DR_ENV` linked-list packet it cannot decode, VSync
   IRQs stop, and the capture wedges. DuckStation runs the same code
   headless at several thousand VBLs per second.
2. BIOS. DuckStation runs Sony SCPH1001; the PCSX-Redux setup ran OpenBIOS,
   an open-source reimplementation with its own bug-compatibility
   differences. The north star is byte-identity with hardware, so
   the retail BIOS is the correct reference.

## The fork

DuckStation is a submodule at `third_party/duckstation`, url
`https://github.com/tyscorp/duckstation.git`, branch `openpete-psxtrace`,
pinned in `.gitmodules`. The branch carries two commits over upstream
`stenzek/duckstation`:

1. Include the EGL context header unconditionally. Upstream includes it
   only under X11 or Wayland, but the EGL context creation is called
   unconditionally on Linux, which breaks a surfaceless headless build.
2. Add the psxtrace capture module to `duckstation-regtest`:
   `src/duckstation-regtest/psxtrace_capture.{cpp,h}` plus five CLI flags
   — `-psxtrace PATH`, `-psxtrace-anchor-pc HEX`
   (default `0x8001ED5C`, Spyro 1 GamestateDraw), `-psxtrace-anchor-hit N`
   (default 1), `-psxtrace-frames N` (0 = bound by `-frames`), and
   `-dump-ram-at-anchor PATH` (2 MB main-RAM dump on the first armed
   hit).

`duckstation-regtest` is upstream's headless regression-test target. Its
default settings are already what the oracle needs: software GPU renderer,
no audio output, and a non-persistent memory card.

### Capture mechanism

The capture module sets an execute breakpoint at the anchor PC.
Each hit reads main RAM for every region
— including the indirect-pointer dereference and per-element masking that
openpete's `gameplay_hash_compute_ex()` (`core/runtime/run.c:529`) performs
— hashes it with FNV-1a-64, and appends one `FRMR` record to the v2
`.psxtrace` file. There is no interpreter slowdown and no scripting layer.

The format constants in `psxtrace_capture.cpp` mirror
`tools/psxtrace_format.py`, and the hash mirrors
`core/include/openpete_fnv1a64.h`; a drift in either produces a golden
openpete can never match.

### Region-table mirror

`kRegions` in `psxtrace_capture.cpp` is a hand-written copy of
`manifest/gameplay_state_regions.toml`. `tools/check_ds_region_mirror.py`
diffs the two row by row (kind, start, length, aux, element size, name,
mask offsets, per-element flag), exits 0 identical, 1 on any difference,
64 when the submodule is not checked out. It runs as the ctest
`ds_region_mirror` (`CMakeLists.txt:382`) and
`scripts/capture_psx_oracle_ds.sh` refuses to capture while the tables
differ.

The `cd_load_transient` and `gameplay_neutral` flags are openpete-side gate
policy, not inputs to the hash; they are not mirrored and not compared.

## Setup

```sh
scripts/setup_duckstation.sh
```

Idempotent, and every step is skipped when already complete. It initialises
the submodule, downloads the upstream prebuilt-deps tarball
(`deps-linux-x64.tar.xz`, release pinned in the script, SHA256 verified),
copies `.tools/pcsx-redux/scph1001.bin` to
`~/.local/share/duckstation/bios/`, and configures plus builds
`duckstation-regtest` into `third_party/duckstation/build-regtest`
(OpenGL on, Vulkan/Wayland/X11 off, Release, Ninja).

DuckStation requires clang-19 system-wide; the script pins `CC=clang-19
CXX=clang++-19`. Ubuntu 22.04 ships clang-14, so clang-19 comes from
`apt.llvm.org`.

## Capturing a golden

```sh
scripts/capture_psx_oracle_ds.sh \
    --out tests/golden/spyro-1/psxtrace-demo-20k.bin --frames 20000
```

`--frames` is the number of records (anchor hits) to capture. DuckStation's
own `-frames` counts presentation frames, so the script passes
`1000 + 4 × --frames` as the runtime cap: the pre-title splash logos run
four VBLs per tick, while the attract demo itself is 1:1.
`--anchor-pc` and `--hit` override the anchor; `--max-runtime-frames`
overrides the cap.

The script verifies the region mirror, runs the capture with software GL
(`LIBGL_ALWAYS_SOFTWARE=1`, `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe`),
asserts the trace exists, and prints `tools.psxtrace_format info` on it.

Re-blessing the shipped golden is only legal for an intentional
gameplay-behaviour change, after reviewing the diff. A golden is
never blind-overwritten to make a red gate go green.

## The gate

```sh
scripts/check_psx_oracle.sh [trace-path] [-- <extra openpete flags>]
```

Defaults: trace `tests/golden/spyro-1/psxtrace-demo-20k.bin`, binary
`build/linux-release/games/spyro-1/openpete-spyro1`, disc `$OPENPETE_DISC`
or `~/code/spyro/spyro-1/build/SCUS-94228.cue`. The shipped golden holds
20000 records over 33 regions, covering four-plus level loads and
gameplay, and replays in roughly two and a half minutes.

The script runs openpete `--headless --no-config --run-recomp 0x80012204
--mute 1 --psxtrace-verify <trace> --frame-limit <N>`. `--no-config` keeps
the verdict independent of any `openpete.toml` beside the exe or in the
runner's home directory. `--mute 1` sits before the pass-through flags so a
forwarded `-- --mute 0` still wins. `OPENPETE_VIBRATION=0` is exported for
the run (fenced by `scripts/fences/rumble_guard.py`).

Anything after `--` is forwarded verbatim to openpete. This is how a
presentation feature is proven gameplay-neutral: run openpete with the
feature flag against the stock golden, and a PASS means the flag perturbed
no gameplay-state region relative to hardware.

```sh
scripts/check_psx_oracle.sh -- --no-lod
scripts/check_psx_oracle.sh -- --aspect 16:9
scripts/check_psx_oracle.sh custom.bin -- --fov 90 --no-lod
```

A flag that takes a value needs the `--` separator, otherwise its value is
read as the trace path.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | every non-tolerated region matched on every record |
| 2 | divergence — `OPENPETE_EXIT_DIVERGENCE` (`core/include/openpete.h:22`) |
| 64 | trace, binary, or disc missing; ctest `SKIP_RETURN_CODE` |
| other | build or runtime error |

The ctest name is `psx_oracle_match`, labels `slow;oracle`, timeout 600 s
(`CMakeLists.txt`). On divergence openpete names the first divergent
record and region, and logs a per-region divergence count over all records
compared, so a stable region (persistent state difference) is
distinguishable from a flickering one.

### Environment knobs

- `OPENPETE_BIN`, `OPENPETE_DISC` — binary and disc overrides.
- `OPENPETE_FRAME_LIMIT` — passed as `--frame-limit`; default 200000. This
  is a safety belt only, since `psxtrace-eof` ends the run first: the
  20k-record demo reaches EOF at about 37000 openpete frames (VBLs, roughly
  two per tick). Setting `OPENPETE_FRAME_LIMIT=8000` cuts the run to a
  roughly 20-second smoke pass covering about 4000 ticks, which reaches no
  level load and therefore clears nothing.
- `PSXTRACE_KEEP_GOING=1` — passes `--psxtrace-keep-going`, comparing every
  record instead of stopping at the first divergence. Used to census which
  regions diverge and for how long.
- `OPENPETE_PSXTRACE_HEAL_WINDOW` — CD-load transient tolerance in ticks;
  see below.

### Sidecar artifacts

If `<trace>.seed.ram`, `<trace>.seed.cpu`, or `<trace>.bk2` exist beside
the trace, the script passes them as `--seed-ram`, `--seed-cpu`, and
`--bk2`, aligning openpete's start state and controller input to the
oracle's. Those artifacts come from the PCSX-Redux capture path
(`scripts/capture_psx_oracle.sh`, a two-stage Lua capture). The shipped
DuckStation golden has none: it is captured from power-on and openpete
replays the same boot.

## CD-load transients

Three regions in `manifest/gameplay_state_regions.toml` carry
`cd_load_transient = true`: `g_LevelMobys_pool`, `g_DynMobys_pool`, and
`g_Sparx_moby`. All three are refreshed in place by synchronous
`psyq_CdRead`. openpete drains a read instantly while the oracle is
hardware-timed, so the pools legitimately differ for the duration of a
level or streaming load and then re-converge.

The verifier (`core/runtime/psxtrace_verify.c`) tracks each transient
region's divergence burst independently. A burst that re-converges is
absorbed and logged. A burst still open after `heal_window` ticks, or still
open when the trace is fully consumed (`openpete_psxtrace_finalize`), is a
hard failure. `heal_window` defaults to 600 ticks; the largest legitimate
load burst measured in the 20k demo is about 310 ticks.
`OPENPETE_PSXTRACE_HEAL_WINDOW` overrides it.

Any region without the flag is a hard failure on its first differing
record. This carve-out is the only divergence the gate accepts, and it
disappears once CD timing is modelled.

`g_Particles_pool` carries `gameplay_neutral = true` instead: its
divergence is logged and never fails, on the strength of a 20k keep-going
32:9 run in which it was the sole diverging region while `psyq_rand_seed`,
`g_Spyro`, `g_Camera`, `g_Sparx`, and `g_MobyAllocPtr` stayed bit-identical
to hardware for all 20000 ticks. Those five regions stay tracked as the
tripwire for that audit.

## Known artefacts

- Mesa GL TLS-destructor SIGABRT on shutdown. On WSL2 with llvmpipe,
  DuckStation's software-renderer path aborts inside `swrast_dri.so` during
  pthread thread-local-storage cleanup, after the regtest has logged
  `Exiting with success.` and the trace file has been finalised.
  `capture_psx_oracle_ds.sh` runs the capture under a wrapper `sh` that
  swallows the abort, and sets `ulimit -c 0`. It does not reproduce on
  systems with a hardware GL stack.
- Screenshots are black in the headless software path. PSX render
  output reaches VRAM, but the GL present step on llvmpipe does not capture
  it. The oracle hashes RAM, not VRAM, so this does not affect it; it would
  need fixing before regtest could drive visual regression tests.

- The reference frame is 83 CPU ticks longer than the hardware frame.
  The reference counts 3413 GPU ticks per NTSC line (measured);
  the hardware line is 227.5
  colour-subcarrier cycles, 3412.5 GPU ticks, which is what
  `core/include/openpete_time.h` and encode (2250000/37609 Hz). The
  reference therefore runs 59.81734 Hz against 59.82611 Hz, 0.0147 % slow,
  one frame of wall-clock drift every 113 s. Guest state is unaffected: the
  guest counts VBLs, guest-timed models count VBLs, and the gate samples at
  anchor PCs. A model that converts cycles to VBL fractions against a
  DuckStation golden uses the reference's 566,204 CPU ticks per VBL; against
  hardware, 566,121.

## Related

- `notes/plans/openpete-psx-oracle-ci.md` — gate design, the transient
  policy, and the CI wiring.
- `manifest/gameplay_state_regions.toml` — the region contract itself, with
  the address source and audit note for every region.
