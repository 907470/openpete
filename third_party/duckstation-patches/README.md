# DuckStation psxtrace patch series

The openpete project's own changes to DuckStation: the `duckstation-regtest`
psxtrace capture module and its recorders, which write the reference trace
the oracle gate (`tests/harness/check_psx_oracle.sh`) replays and the
per-record GTE hash sidecar (`<trace>.gte`, patch 0028) it compares, plus a
headless EGL build fix and an SPU final-mix WAV dump. Each file is
`git format-patch` output, one commit per file, applied in the order
`series` lists.

The submodule `third_party/duckstation` pins upstream
`https://github.com/stenzek/duckstation.git` at `e4c55ad0283f`, the commit
the series is based on. DuckStation is licensed CC BY-NC-ND 4.0, which
forbids distributing a modified DuckStation, so the project distributes the
patches and applies them locally.

`scripts/setup_duckstation.sh` applies the series onto the pin with `git am`
under a fixed committer identity and author-date committer dates, with
system and global git configuration, hooks and signing excluded, so every
machine produces the same commit ids, then builds `duckstation-regtest`.
`applied-head.txt` records the resulting head commit; DuckStation stamps it
into the binary and every trace header as `scm_hash`, and the oracle gate
refuses a golden whose `scm_hash` differs from it.

Patches that change what a capture records, by number:

| patch | change |
| --- | --- |
| 0029 | every sidecar tick is the exact instruction tick (the CPU tick of the instruction); `<trace>.vblpos` (`-psxtrace-vblpos SPANS`, `-psxtrace-vblpos-skip LIST`: each VBLANK interrupt taken outside a waited VSync, with EPC and the first game-code call target and its ordinal after the handler) and `<trace>.spuq` (`-psxtrace-spuq KIND:HEX`: SpuSetKey, SpuGetKeyStatus, SpuGetAllKeysStatus and envelope- or pitch-setting SpuSetVoiceAttr calls with their arguments); two core hooks, one before each instruction and one at an exception. Layouts: `docs/src/developer/duckstation-oracle.md` |
| 0030 | the NTSC line alternates between 3413 and 3412 GPU ticks (3412.5 on average, 227.5 colour-subcarrier cycles) and the PAL line is 3405 ticks, so a VBL is 566120.83 CPU ticks instead of 566203.78: every sidecar tick column, the `.loads` issue VBLs, the `.vsync` `v0` HBLANK counts and the CD-timing state of a load window move |
| 0032 | the `OPENPETE_SPU_DUMP` final-mix dump is written on the SPU's final-mix output path (before, the dump held a header and no frames) and shutdown closes it, writing the RIFF and data sizes |
| 0033 | `-psxtrace-spuq spuwrite:HEX`: a `.spuq` row per SpuWrite call with source, size and the head of every sounding voice |
| 0034 | `-pad-script PATH`: port 1 presses from steps that wait on guest RAM; `quit` ends the run |

Apply (from the openpete checkout root; `--no-build` skips the build):

```
scripts/setup_duckstation.sh
```

The `git am` the script runs, from `third_party/duckstation` after
`git checkout --detach e4c55ad0283ff26a76991a759d8db13d6969b9ec`:

```
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
GIT_COMMITTER_NAME='openpete psxtrace series' \
GIT_COMMITTER_EMAIL='psxtrace-series@openpete.invalid' \
  git -c core.hooksPath=/dev/null -c core.autocrlf=false \
      -c i18n.commitEncoding=UTF-8 \
  am --committer-date-is-author-date --no-3way --no-message-id \
     --no-keep-cr --no-scissors --utf8 --no-gpg-sign --whitespace=nowarn \
  $(sed 's|^|../duckstation-patches/|' ../duckstation-patches/series)
```

A change to a patch, to `series` or to the pin changes the head:
regenerate the series with `git format-patch --zero-commit --no-signature -N`
from the pin, run `scripts/setup_duckstation.sh --record` to rewrite
`applied-head.txt`, and recapture every game's oracle golden
(`rm tests/golden/<game>/psxtrace-demo-20k.bin*`, then run the gate).
