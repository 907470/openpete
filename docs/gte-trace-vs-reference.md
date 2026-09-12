# GTE side-by-side trace workflow

Runs three independent GTE implementations on identical pre-state for every
COP2 op inside a VBL window and dumps the post-state to CSV. Divergence
between any pair localizes a GTE math bug to one engine. Three rows per
logged op:

| `engine` | source |
|----------|--------|
| `rec`    | psxrecomp's GTE (`third_party/psxrecomp/runner/src/gte.cpp`) |
| `psy`    | PsyCross's GTE (`third_party/PsyCross/src/gte/PsyX_GTE.cpp`) |
| `pcsx`   | pcsx-redux's GTE, vendored at `third_party/pcsx_redux_gte/` |

A bug shared by two codebases derived from a common ancestor survives a
2-way diff. pcsx-redux's GTE was written from the Nocash spec independently
of PsyCross, so a 3-way bit-identical match is the stronger claim
(`third_party/pcsx_redux_gte/README.md`).

## In-tree usage

All three engines link into `openpete-spyro1` on the `linux-release`
preset, so this needs no setup beyond env vars. On `windows-mingw` the
vendored engine is off: `third_party/pcsx_redux_gte` needs C++23 and
MinGW-GCC 10 caps at C++20, so `CMakeLists.txt` defaults
`OPENPETE_BUILD_PCSX_REDUX_GTE` OFF when cross-compiling and
`core/runtime/pcsxr_gte_stub.c` supplies a `pcsxr_gte_run` that zeroes the
post-state. Every `pcsx` row is then all-zero and the 3-way diff is
meaningless; trace on Linux.

```sh
rm -f /tmp/openpete-gte.csv
OPENPETE_GTE_TRACE=/tmp/openpete-gte.csv \
OPENPETE_GTE_TRACE_LO=250 OPENPETE_GTE_TRACE_HI=261 \
OPENPETE_GTE_TRACE_OPS='01,06,30,2D,2E' \
build/linux-release/games/spyro-1/openpete-spyro1 --headless \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
 --frame-limit 265
```

Knobs, all read once on the first COP2 op and parsed in `gte_trace_init()`
(`third_party/psxrecomp/runner/src/gte.cpp`):

- `OPENPETE_GTE_TRACE` — output CSV path. Unset disables the logger.
- `OPENPETE_GTE_TRACE_LO` / `_HI` — half-open `openpete_frame` window,
  `[LO, HI)`. The logger closes the file on the first op at or past `HI`.
  Defaults are 1100 and 1111; those predate the anchoring of
  `openpete_frame` to the first `psyq_DrawOTag` call
  (`core/include/openpete_fiber.h`), which removed the splash VBLs from the
  count, so always set the window explicitly.
- `OPENPETE_GTE_TRACE_OPS` — comma-separated hex `func` values. Default
  `01,06,30,2D,2E`.

`openpete_frame` is a VBL counter, not a tick counter.

Output is three rows per logged op in the order `rec`, `psy`, `pcsx`. The
multi-vertex ops RTPT, NCDT, NCT and NCCT emit one triple per vertex,
labelled `_v0`, `_v1`, `_v2`. Diff pairwise, with the engine column blanked:

```sh
awk -F, 'NR==1 || $2=="rec"'  /tmp/openpete-gte.csv > /tmp/rec.csv
awk -F, 'NR==1 || $2=="psy"'  /tmp/openpete-gte.csv > /tmp/psy.csv
awk -F, 'NR==1 || $2=="pcsx"' /tmp/openpete-gte.csv > /tmp/pcsx.csv
diff <(awk -F, '{$2="";print}' /tmp/rec.csv) \
     <(awk -F, '{$2="";print}' /tmp/psy.csv)
diff <(awk -F, '{$2="";print}' /tmp/rec.csv) \
     <(awk -F, '{$2="";print}' /tmp/pcsx.csv)
```

Both diffs are empty on a healthy tree. The logged columns are the decoded
CP2D/CP2C register file: the input vector, translation, rotation matrix,
screen offsets, `H`, `DQA`, `DQB`, `MAC0-3`, `IR0-3`, `SZ0-3`, `SXY0-2`,
`RGB0-2`, and `FLAG`.

Op codes: `01`=RTPS, `06`=NCLIP, `0C`=OP, `10`=DPCS, `11`=INTPL, `12`=MVMVA,
`13`=NCDS, `14`=CDP, `16`=NCDT, `1B`=NCCS, `1C`=CC, `1E`=NCS, `20`=NCT,
`28`=SQR, `29`=DPCL, `2A`=DPCT, `2D`=AVSZ3, `2E`=AVSZ4, `30`=RTPT, `3D`=GPF,
`3E`=GPL, `3F`=NCCT. An unrecognized `func` is logged as `UNK`.

## Live pcsx-redux replay

The vendored `pcsx` engine runs only on pre-state captured by openpete, so
it cannot detect openpete's MIPS recompiler feeding the GTE different
different inputs than the original code path would. Ruling that out needs
pcsx-redux running the actual disc. See also
[ram-diff-vs-pcsxr](ram-diff-vs-pcsxr.md), which compares whole-RAM
snapshots for the same class of question.

### Build pcsx-redux on Linux (one-time)

The clone lives at `.tools/pcsx-redux/` (gitignored). Build deps:

```sh
sudo apt-get install -y build-essential clang g++ pkg-config \
    libcapstone-dev libfreetype-dev libavcodec-dev libavformat-dev \
    libavutil-dev libcurl4-openssl-dev libglfw3-dev libswresample-dev \
    libuv1-dev zlib1g-dev g++-mipsel-linux-gnu cpp-mipsel-linux-gnu \
    binutils-mipsel-linux-gnu
cd .tools/pcsx-redux
make openbios            # OpenBIOS bootstrap, built first
make BUILD=Release -j8   # higher -j may OOM-kill cc1plus
cp src/mips/openbios/openbios.bin ./openbios.bin
```

On FFmpeg 4.4 (Ubuntu 22.04) its audio-file reader does not compile: it
uses the `AVChannelLayout` / `swr_alloc_set_opts2` API introduced in FFmpeg
5. Stubbing the reader so that every open fails is enough for this
workflow.

### Run the Lua dumper

`scripts/pcsx-redux-gte-dump-loading-text.lua` sets one Exec breakpoint per
COP2 PC (the list is inline in the script, covering `r_pete` and
`r_environment` in PSX.EXE), counts VSync events as frames, and writes
`/tmp/pcsx-redux-gte-loading-text.csv` in the in-tree dumper's column
layout.

Two non-obvious requirements:

1. Pass `--debugger`. Without it
   breakpoints — Lua or otherwise — silently never fire.
2. The breakpoint invoker must return `true`. A falsy invoker
   return is a delete request, so an invoker returning `false` fires
   exactly once; `nil` is coerced to false at the C boundary and also
   deletes.

```sh
cd .tools/pcsx-redux
./pcsx-redux --no-ui --debugger --interpreter \
    --bios ./openbios.bin \
    --iso ../../games/spyro-1/spyro-1/build/SCUS-94228.cue \
    --run --dofile ../../scripts/pcsx-redux-gte-dump-loading-text.lua
```

Regenerate the COP2 PC list for a different binary or a wider range with:

```sh
mipsel-linux-gnu-objdump -d games/spyro-1/spyro-1/build/PSX.EXE \
    | awk '$3 ~ /^cop2/ {print "    0x"$1","}' \
    > /tmp/cop2-pcs.txt
```

### Known gap: reaching the cutscene from a fresh boot

OpenBIOS does not implement the pad-poll commands Spyro 1 issues — it logs
`Unknown command for pad: 43` and ignores them — so the script's scheduled
`setOverride` Start presses do not advance past the title screen. A real
SCPH-1001 BIOS handles those commands, but that path SIGSEGVs pcsx-redux on
`--run`. Neither path reaches the cutscene unattended.

Workaround: run pcsx-redux interactively (drop `--no-ui`), navigate to the
cutscene by keyboard, save state at the entry, then re-run with the Lua
script and that save state preloaded.
