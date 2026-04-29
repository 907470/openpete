# GTE side-by-side trace workflow

Verifies psxrecomp's GTE math against two independent implementations by
running all three on identical pre-state for every COP2 op in a frame
window and CSV-dumping the post-state. Three rows per op:

| `engine` | source |
|----------|--------|
| `rec`    | psxrecomp's GTE (`third_party/psxrecomp/runner/src/gte.cpp`) |
| `psy`    | PsyCross's GTE (`third_party/PsyCross/src/gte/PsyX_GTE.cpp`) |
| `pcsx`   | pcsx-redux's GTE, vendored at `third_party/pcsx_redux_gte/` |

As of commit 9467e0e + the pcsx-redux vendoring, all three are bit-identical
on 32,962 ops captured during Spyro 1's titlescreen cutscene frames
1100-1110. So the GTE is *triply* verified, and the user-reported peak
flicker has to come from somewhere downstream of the GTE math.

## In-tree usage

All three engines are already linked into `openpete-spyro1`, so this needs
no setup beyond an env var.

```sh
# openpete_frame is now anchored at the first DrawOTag, so splash frames
# don't count toward the trace window. Frame numbers below are in the new
# numbering (subtract ~850 from the old fast-boot-relative numbering).
rm -f /tmp/openpete-gte.csv
OPENPETE_GTE_TRACE=/tmp/openpete-gte.csv \
OPENPETE_GTE_TRACE_LO=250 OPENPETE_GTE_TRACE_HI=261 \
OPENPETE_GTE_TRACE_OPS='01,06,30,2D,2E' \
./build/linux-release/games/spyro-1/openpete-spyro1 --headless \
    --disc <spyro>/spyro-1/build/SCUS-94228.cue \
    --run-recomp 0x80012204 --frame-limit 265
```

Output is three rows per logged op, in the order `rec`, `psy`, `pcsx`.
Diff them pairwise:

```sh
awk -F, 'NR==1 || $2=="rec"'  /tmp/openpete-gte.csv > /tmp/rec.csv
awk -F, 'NR==1 || $2=="psy"'  /tmp/openpete-gte.csv > /tmp/psy.csv
awk -F, 'NR==1 || $2=="pcsx"' /tmp/openpete-gte.csv > /tmp/pcsx.csv
diff <(awk -F, '{$2="";print}' /tmp/rec.csv) \
     <(awk -F, '{$2="";print}' /tmp/psy.csv)
diff <(awk -F, '{$2="";print}' /tmp/rec.csv) \
     <(awk -F, '{$2="";print}' /tmp/pcsx.csv)
```

The expected outcome is empty diffs. As of commit 9467e0e + the pcsx-redux
vendoring, every triple on Spyro 1's titlescreen cutscene matches bit-for-
bit — this is the canary that catches future GTE regressions.

Op codes: `01`=RTPS, `06`=NCLIP, `0C`=OP, `10`=DPCS, `11`=INTPL, `12`=MVMVA,
`13`=NCDS, `14`=CDP, `16`=NCDT, `1B`=NCCS, `1C`=CC, `1E`=NCS, `20`=NCT,
`28`=SQR, `29`=DPCL, `2A`=DPCT, `2D`=AVSZ3, `2E`=AVSZ4, `30`=RTPT, `3D`=GPF,
`3E`=GPL, `3F`=NCCT.

## Live pcsx-redux replay (alternative when you need real-game-state)

The vendored pcsx-redux GTE above runs *only* on pre-state captured by
openpete. If you need to validate against pcsx-redux running the actual
disc — e.g. to rule out openpete's MIPS recompiler producing different
*inputs* to the GTE than the original code path would — use the live
emulator path below. (Note: as of openpete commit 9467e0e + the vendored
GTE, all three engines bit-match on captured pre-state, so this is rarely
needed in practice.)

### Build pcsx-redux on Linux (one-time)

The clone in `.tools/pcsx-redux/` (gitignored) is ready. Build needs apt
deps:

```sh
sudo apt-get install -y build-essential clang g++ pkg-config \
    libcapstone-dev libfreetype-dev libavcodec-dev libavformat-dev \
    libavutil-dev libcurl4-openssl-dev libglfw3-dev libswresample-dev \
    libuv1-dev zlib1g-dev g++-mipsel-linux-gnu cpp-mipsel-linux-gnu \
    binutils-mipsel-linux-gnu
cd .tools/pcsx-redux
# FFmpeg 4.4 (Ubuntu 22.04) needs the audio-file reader stubbed; 5+ has the
# new AVChannelLayout / swr_alloc_set_opts2 API. A reader whose every open
# fails is enough.
make openbios            # build OpenBIOS first (used as bootstrap)
make BUILD=Release -j8   # higher -j may OOM-kill cc1plus
cp src/mips/openbios/openbios.bin ./openbios.bin
```

### Run the Lua dumper

`scripts/pcsx-redux-gte-dump.lua` sets one Exec breakpoint per cop2 PC
(populated from r_pete + r_environment in PSX.EXE), counts VSync events
as frames, and emits a CSV at `/tmp/pcsx-redux-gte.csv` in the same
column layout as the in-tree dumper.

Two non-obvious requirements:

  1. **Pass `--debugger`**. Without it breakpoints (Lua or otherwise)
     never fire, and nothing reports it.
  2. **The invoker must return `true`** to keep the breakpoint armed.
     A falsy return deletes the breakpoint,
     so a BP that returns `false` fires exactly once. Returning `nil`
     also deletes (it's coerced to false at the C boundary).

Invocation:

```sh
cd .tools/pcsx-redux
./pcsx-redux --no-ui --debugger --interpreter \
    --bios ./openbios.bin \
    --iso <spyro>/spyro-1/build/SCUS-94228.cue \
    --run --dofile ../../scripts/pcsx-redux-gte-dump.lua
```

To regenerate the cop2 PC list (different game, or you want a wider
range):

```sh
mipsel-linux-gnu-objdump -d games/spyro-1/spyro-1/build/PSX.EXE \
    | awk '$3 ~ /^cop2/ {print "    0x"$1","}' \
    > /tmp/cop2-pcs.txt
```

### Known gap: reaching the cutscene from a fresh boot

OpenBIOS doesn't fully implement the pad-poll commands Spyro 1 issues
(it logs `Unknown command for pad: 43` and ignores them), so scripted
`setOverride(3)` Start presses don't advance past the title screen. A
real Sony BIOS (e.g. SCPH-1001) would handle this — but that path
currently SIGSEGVs pcsx-redux on `--run`, so neither path is end-to-end
clean here without manual intervention.

Workaround until that's fixed: run pcsx-redux interactively (drop
`--no-ui`), navigate manually to the cutscene with the keyboard, save
state at the cutscene entry, then re-run with the Lua script + a
preloaded save state to dump from the cutscene frame range.

Or — the simpler path — sidestep the live-emulation requirement
entirely by vendoring pcsx-redux's GTE into openpete and adding a
third "pcsx" engine to the in-tree trace logger. The GTE math has no
dependencies beyond a stub `g_emulator->m_cpu->m_regs.{CP2D,CP2C}`
register file plus PGXP no-ops. Same diff recipe, no input replay.
