# GTE side-by-side trace workflow

Verifies psxrecomp's GTE math against an independent implementation by
running both on identical pre-state for every COP2 op in a frame window
and CSV-dumping the post-state.

## In-tree reference (PsyCross): the fast path

PsyCross is already linked into `openpete-spyro1`, so this needs no setup
beyond an env var.

```sh
rm -f /tmp/openpete-gte.csv
OPENPETE_GTE_TRACE=/tmp/openpete-gte.csv \
OPENPETE_GTE_TRACE_LO=1100 OPENPETE_GTE_TRACE_HI=1111 \
OPENPETE_GTE_TRACE_OPS='01,06,30,2D,2E' \
./build/linux-release/games/spyro-1/openpete-spyro1 --headless \
    --disc <spyro>/spyro-1/build/SCUS-94228.cue \
    --run-recomp 0x80012204 --frame-limit 1115 \
    --fast-boot-until-frame 800
```

Output is one row per engine per logged op, alternating `rec`,`psy`. To
diff:

```sh
awk -F, 'NR==1 || $2=="rec"' /tmp/openpete-gte.csv > /tmp/rec.csv
awk -F, 'NR==1 || $2=="psy"' /tmp/openpete-gte.csv > /tmp/psy.csv
diff <(awk -F, '{$2="";print}' /tmp/rec.csv) \
     <(awk -F, '{$2="";print}' /tmp/psy.csv)
```

Or use `scripts/gte_diff.py` if you keep one around. As of the GTE rewrite
in commit 9467e0e, every op pair on Spyro 1's titlescreen cutscene matches
bit-for-bit — this is the canary that catches future GTE regressions.

Op codes: `01`=RTPS, `06`=NCLIP, `0C`=OP, `10`=DPCS, `11`=INTPL, `12`=MVMVA,
`13`=NCDS, `14`=CDP, `16`=NCDT, `1B`=NCCS, `1C`=CC, `1E`=NCS, `20`=NCT,
`28`=SQR, `29`=DPCL, `2A`=DPCT, `2D`=AVSZ3, `2E`=AVSZ4, `30`=RTPT, `3D`=GPF,
`3E`=GPL, `3F`=NCCT.

## Third-party reference (pcsx-redux): the slow path

Useful when PsyCross and psxrecomp **agree** on a divergence — i.e., both
might be wrong vs the spec / real hardware. pcsx-redux's GTE was derived
from Nocash's spec independently and is a strong tiebreaker.

### Build pcsx-redux on Linux (one-time)

The clone in `.tools/pcsx-redux/` (gitignored) is ready. Build needs apt
deps:

```sh
sudo apt-get install -y build-essential clang g++ pkg-config \
    libcapstone-dev libfreetype-dev libavcodec-dev libavformat-dev \
    libavutil-dev libcurl4-openssl-dev libglfw3-dev libswresample-dev \
    libuv1-dev zlib1g-dev g++-mipsel-linux-gnu cpp-mipsel-linux-gnu \
    binutils-mipsel-linux-gnu
cd .tools/pcsx-redux && make BUILD=Release -j$(nproc)
```

(If sudo isn't available, the dockermake.sh path produces an AppImage:
`./dockermake.sh appimage` — but Docker on WSL2 needs Docker Desktop with
WSL integration enabled.)

### Use the Lua dumper

`scripts/pcsx-redux-gte-dump.lua` is a Lua script that, when pasted into
pcsx-redux's Lua console, breakpoints on every cop2-instruction PC you
list in its CONFIG block and writes a CSV in the same column layout as the
in-tree dumper. So the same diff recipe works.

The catch: pcsx-redux's Lua API has no per-COP2-op event hook, so you have
to enumerate the cop2 PCs manually:

```sh
mipsel-linux-gnu-objdump -d games/spyro-1/spyro-1/build/PSX.EXE \
    | awk '$3 ~ /^cop2/ {print "    0x"$1","}' \
    > /tmp/cop2-pcs.txt
```

Paste the contents into `cop2_pcs = { ... }` in the Lua script. Restrict to
the PCs inside the function(s) you're investigating (e.g. `r_pete` for the
titlescreen cutscene) so the breakpoint count stays manageable.
