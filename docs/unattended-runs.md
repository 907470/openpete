# Unattended runs

How to drive the built binary without a human at the keyboard: the flags a
headless run needs, the gates that decide correctness claims, and the exit
codes that let a script read the verdict.

Building is covered in [`building.md`](building.md).

## One loop

```bash
cmake --build --preset linux-release

./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
    --headless \
    --frame-limit 60 \
    --max-runtime-seconds 30 \
    --screenshot-on-frame 30 --screenshot-output /tmp/shot.png \
    --log-format json 2> /tmp/log.json
echo "exit=$?"

file /tmp/shot.png
jq '.' /tmp/log.json | head -20
```

## The flags a headless run needs

| Flag | Effect |
|---|---|
| `--disc PATH` | Disc image (BIN/CUE). |
| `--headless` | No on-screen window (offscreen video driver). |
| `--frame-limit N` | Exit after N VBLs. A tick is two VBLs. |
| `--max-runtime-seconds N` | Wall-clock SIGTERM after N seconds. |
| `--skip-to-level L` | Boot into level L — numeric id (10 = Artisans, 63 = Gnasty) or `lower_snake_case` name — instead of the attract demo. An unknown name prints the list. |
| `--skip-to-cutscene C` | Play story cutscene C. Spyro 1: `intro` (the opening, from the title through New Game; refused with `--skip-to-level`), `ending` (from level 63), `ending_loot` (from level 64; the credits return to Artisans), `credits` / `credits_100` (alias `full_credits`; the short and the 100% roll, from `--skip-to-level`'s target, else Artisans; the roll returns to that level). Spyro 2: `intro`, `intro_arrival`, `summer_forest`, `crush_intro`, `crush_defeated`, `autumn_plains`, `gulp_intro`, `gulp_defeated`, `winter_tundra`, `ripto_intro`, `ending`, each played through the Dragon Shores (level 65) theater replay; playback returns to Dragon Shores. Spyro 3 has no targets. The run loads the source level itself; a `--skip-to-level` to another level refuses the cutscene. No value or an unknown name prints the game's list and exits 3. |
| `--skip-to-area L:A` | Spyro 3: boot into level L (numeric id or name) and enter its sub-area A through the game's own sub-area transition, as the level's area-0 portal does. A is an index from 1; the game holds no area names. Replaces `--skip-to-level`'s level. No value or an invalid target prints each level's valid indices (17 levels, 38 sub-areas) and exits 3; Spyro 1 and 2 refuse the flag. |
| `--bk2 PATH` | Replay a BizHawk movie (Octoshock P1 pad) as input. The reproducible input source. |
| `--screenshot-on-frame N[,N2...]` | Save an image at each listed VBL. `--screenshot-on-tick` keys on canonical draw-ticks instead, which hit the same sim moment run-to-run; the visual gates use the tick form. |
| `--screenshot-output PATH` | Path template; `{n}` expands to the sim frame, `{p}` to the present index. A `.png` extension writes PNG, otherwise BMP. |
| `--log-format json\|plain` | Structured stderr for `jq` parsing. |
| `--log-level SPEC` | e.g. `cd:debug,*:info`; also `OPENPETE_LOG=...`. |
| `--mods ID[,ID...]` | Override `mods/enabled.txt` for one run; `--mods ""` is a vanilla run. |

`--help` carries the full set, including the display, pacing and renderer
flags this table leaves out. `--config-report` prints which config layer won
for every setting.

Every screenshot flag has a windowed/headless split: in a windowed pure-vk
run the `-vk.png` is a readback of the presented image (sub-tick
interpolation, mods and overlays included); headless runs recompose the
canonical scene instead and emit no intermediate presents, so anything
sub-tick is invisible to a headless capture.

## The gates

| Gate | Script | Proves |
|---|---|---|
| Oracle | `scripts/check_psx_oracle.sh` | Gameplay state matches the PSX reference capture, tick for tick. |
| Presentation golden | `scripts/check_presentation_golden.sh` | The rendered screenshot corpus is unchanged, or intentionally re-blessed with `--bless`. |
| Mods | `scripts/check_mods.sh` | The mod PoC ladder, including per-mod `modtest.toml` scenarios (ctest `mod_poc_ladder`, ~10 min). |

Oracle internals are in [`duckstation-oracle.md`](duckstation-oracle.md).

### Skip targets

`scripts/check_skip_targets.sh [--game SLUG] [--build-dir=DIR] [--filter=NAME]`
(default game `spyro-1`) runs every row of `games/<game>/skip-targets.manifest`
headless: 5 `--skip-to-cutscene` rows for Spyro 1, 11 for Spyro 2, 38
`--skip-to-area` rows for Spyro 3. A row is `name | args | frames | expect`:
`args` the skip flags, `frames` the run's `--frame-limit` in VBLs, `expect` the
tokens the skip's release line (`[skip-to-level] level playable`) must carry,
values the skip module reads back from RAM at the release edge. A row passes
when the run exits 0, prints the release line with every token, and prints no
fail-safe line (`[skip-to-level] fail-safe release (no playable edge)`). The
fail-safe deadline is 4000 VBLs after the skip's first check, or
`OPENPETE_SKIP_FAILSAFE_VBL` when set positive. Negative control: the first row
runs again with `OPENPETE_SKIP_FAILSAFE_VBL=1` and must fail; a pass there
fails the harness. `SKIP_TARGETS_JOBS` (default 2) caps concurrent game
processes; logs land in `<build-dir>/skip-targets/<game>/`. Exit 0 PASS, 1 FAIL,
64 SKIP (binary, disc or manifest missing). ctest runs it as
`skip_targets_spyro-1`, `skip_targets_spyro-2` and `skip_targets_spyro-3`,
labels `slow` and `skip-targets` (`ctest -L skip-targets`).

## Exit codes

Defined in `core/include/openpete.h`.

| Code | Meaning |
|---|---|
| 0 | Clean termination (frame limit, SDL quit). |
| 1 | Panic, crash, or watchdog timeout. |
| 2 | State divergence against the golden — the oracle gate's verdict. |
| 3 | Setup error: unknown flag, disc not found. |

## Artifacts, not interaction

An unattended run has no debugger attached and no one to press a button, so
every piece of state that matters has to reach the filesystem as a readable
artifact: a log line, a JSON record, a PNG, a `.bk2`, a RAM dump
(`--ram-dump-on-frame`), or a savestate (`--state-save-on-frame`). Configure
the dumps before the run, then read them after it exits.

Screenshots answer what the render looks like. They do not settle whether it
is correct; that is the presentation golden's job, and
`scripts/center_crop_diff.py A.png B.png` turns a pair of captures into a
pixel-identity number rather than an impression.
