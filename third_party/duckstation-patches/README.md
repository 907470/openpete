# DuckStation psxtrace patch series

The openpete project's own changes to DuckStation: the `duckstation-regtest`
psxtrace capture module and its recorders, which write the reference trace
the oracle gate (`tests/harness/check_psx_oracle.sh`) replays, plus a
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
