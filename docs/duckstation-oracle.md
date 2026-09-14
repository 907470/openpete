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

### Region table

The module carries no region table. `-psxtrace-regions PATH` names a
line-per-region text file that `tools/gen_state_regions.py --out-flat`
writes from the game's manifest (kind, start, length, aux, element size,
per-element flag, mask offsets, name; header line
`psxtrace-regions <version> <fields>`), and the module refuses an
unreadable, empty or malformed file, another format version or field
count, a name over 63 bytes, or more than 128 rows. The same generator
run writes the C header openpete compiles, so both sides hash one
reduction of the manifest. The capture script generates the file into
scratch immediately before each capture.

Two hashes travel in the trace header (flag bits 2 and 3, each a block
after the names blob): the FNV-1a-64 of the regions file, which the
generator also emits as `OPENPETE_STATE_REGIONS_TABLE_HASH`, and the git
hash of the capturing DuckStation build. `openpete_psxtrace_open` refuses
a golden with no table hash, one whose table hash differs from the
registered table ("manifest differs from capture": an edited
start/length/mask under a stable name would otherwise fail at record 0
in that row, indistinguishable from a game divergence), and one whose
region names or order differ from the registered table.

The `cd_load_transient` and `gameplay_neutral` flags are openpete-side gate
policy, not inputs to the hash; they are not in the file.

### Fork provenance

A golden is reproducible only from the fork commit the submodule pins.
`scripts/capture_psx_oracle_ds.sh` refuses when the binary's build stamp
differs from the fork checkout's HEAD, when that HEAD is not the staged
submodule pointer, or when the fork worktree is dirty (the stamp cannot
see dirtiness: `git describe --dirty` finds no tag in the fork). The gate
refuses a golden whose build hash differs from the pinned commit unless
`OPENPETE_ORACLE_ALLOW_FORK_DRIFT=<reason>` names why.

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

The script checks fork provenance, generates the region file, runs the
capture with software GL (`LIBGL_ALWAYS_SOFTWARE=1`,
`MESA_LOADER_DRIVER_OVERRIDE=llvmpipe`), asserts the trace exists and
carries the build hash, deletes a RAM-carrying trace under `tests/golden/`,
and prints `tools.psxtrace_format info` on it.

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

The script runs openpete `--headless --no-config
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
- `OPENPETE_PSXTRACE_POKE=ADDR:BYTE:FRAME` — negative control. The verifier
  writes one byte into guest RAM at the anchor of that frame, before the
  hashes are taken. A run under it must fail at that frame in the region
  holding the address; a PASS means the gate is not looking at that byte.
  For example `OPENPETE_PSXTRACE_POKE=0x8007582C:7:2000` with
  `OPENPETE_FRAME_LIMIT=8000` fails at tick 2000 in `g_SpyroLifeCount`.
- `OPENPETE_ORACLE_LOG` — keep the run's full console log at this path
  (default: a temp file the script deletes). The script reads the exit-time
  `music: summary:` line from it.

### Sidecar artifacts

If `<trace>.seed.ram`, `<trace>.seed.cpu`, or `<trace>.bk2` exist beside
the trace, the script passes them as `--seed-ram`, `--seed-cpu`, and
`--bk2`, aligning openpete's start state and controller input to the
oracle's. Those artifacts come from the PCSX-Redux capture path
(`scripts/capture_psx_oracle.sh`, a two-stage Lua capture). The shipped
DuckStation golden has none: it is captured from power-on and openpete
replays the same boot.

## Checking the gate is not vacuous

Three checks, each cheap, establish that a PASS is a comparison and not an
accident. The trace's table hash equals the registered table's (the
verifier refuses otherwise). A fresh 5-record capture at the default anchor must verify PASS,
and one captured with `-psxtrace-anchor-hit 2` must fail at record 0 (one
tick of offset changes every live region's hash). A poke run
(`OPENPETE_PSXTRACE_POKE`) must fail at the poked tick in the poked
region. The golden's first records are byte-equal to a fresh capture, and
the 2 MB dump `-dump-ram-at-anchor` writes at the armed hit hashes to the
values the same hit's record holds, so the dump and the trace describe one
instant.

## Whole-RAM diff at an anchor

`scripts/check_ram_diff.sh <game> [--anchor first|loop|HEX] [--hit N]
[--ref-hit M] [--keep DIR]` compares the reference's 2 MB RAM with
openpete's at a translated function's entry: the module runs with
`-dump-ram-at-anchor` (and `-card1 none` when the game's expectation file
says so), openpete with `--halt-at-pc`/`--halt-hit`, and
`tools/ram_diff.py` sorts every differing range into a class from
`games/<game>/manifest/ram_diff_expect.toml`, in this order: the exact
words openpete's PSYQ mirrors write (`populated`; a difference here is a
defective mirror and fails), the kernel area, the PSYQ library's own
section extents from the decomp link map (`psyq_internal`, ratcheted per
anchor against the counts recorded in the file), the BIOS's stack frames,
data openpete landed synchronously that hardware still had in flight
(computed from the CdRead lines in openpete's log; any such byte means the
anchor sits inside a load and the run does not pass), and named ranges
with a reason and an owner. Anything else is unexpected and fails; the
union of expected differing bytes is capped at 25% of RAM. The anchor must
be a dispatch-table entry whose entry block is not a loop head, since the
module counts executions of the instruction and openpete counts calls.
Each game's file names a `first` anchor, the entry of the function issuing
its first CdRead, where no read has been issued on either side; Spyro 1
also names its `loop` anchor, GamestateDraw, where hit N is the same
manifest state on both sides by the oracle and the whole-RAM residue is
what the tool reports. A range leaves the expectation file only by a fix.

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
