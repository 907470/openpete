# Unattended runs

How to drive the built binary from a script: the flags a
headless run needs, the gates that decide correctness claims, and the exit
codes that let a script read the verdict.

Building is covered in [`building.md`](building.md).

## One loop

```bash
cmake --build --preset linux-release

./build/linux-release/openpete \
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
| `--skip-to-level L` | Boot into level L - numeric id (10 = Artisans, 63 = Gnasty) or `lower_snake_case` name - instead of the attract demo. An unknown name prints the list. |
| `--skip-to-cutscene C` | Play story cutscene C. Spyro 1: `intro` (the opening, from the title through New Game; refused with `--skip-to-level`), `ending` (from level 63), `ending_loot` (from level 64; the credits return to Artisans), `credits` / `credits_100` (alias `full_credits`; the short and the 100% roll, from `--skip-to-level`'s target, else Artisans; the roll returns to that level). Spyro 2: `intro`, `intro_arrival`, `summer_forest`, `crush_intro`, `crush_defeated`, `autumn_plains`, `gulp_intro`, `gulp_defeated`, `winter_tundra`, `ripto_intro`, `ending`, each played through the Dragon Shores (level 65) theater replay; playback returns to Dragon Shores. Spyro 2 level cutscenes: `LEVEL_intro` (the first-visit arrival, started through the homeworld portal's own transition call) and `LEVEL_outro` (the completed level's departure, started by the level's completion stores and its exit call) for LEVEL `idol_springs`, `colossus`, `hurricos`, `aquaria_towers`, `sunny_beach`, `skelos_badlands`, `crystal_glacier`, `breeze_harbor`, `zephyr`, `scorch`, `shady_oasis`, `magma_cone`, `fracture_hills`, `mystic_marsh`, `cloud_temples`, `robotica_farms`, `metropolis`. Spyro 3: the 19 story cutscenes, named by the game's loading captions, each started by the game's WarpToLevel call as its trigger makes it: `an_evil_plot_unfolds` from the title through New Game (refused with `--skip-to-level`), `a_powerful_villain_emerges` and `a_desperate_rescue_begins` through its chain; `bianca_strikes_back`, `the_second_warning`, `no_hard_feelings` (level 10); `spike_is_born`, `hunters_tussle`, `byrd_james_byrd` (20); `a_monster_to_end_all_monsters`, `an_apology_and_lunch`, `a_duplicitous_larcenous_ursine` (30); `the_escape` (37); `the_dancing_bear` (40); `deja_vu` and its chain `a_familiar_face`, `billy_in_the_wall`, `one_less_noble_warrior` (47); `the_end` (50). Levels 37 and 47 open a speech before gameplay; the cutscene starts once it is dismissed. The run loads the source level itself; a `--skip-to-level` to another level refuses the cutscene. No value or an unknown name prints the game's list and exits 3. |
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
| Oracle | `tests/harness/check_psx_oracle.sh` | Gameplay state matches the PSX reference capture, tick for tick. |
| Presentation golden | `tests/harness/check_presentation_golden.sh` | The rendered screenshot corpus is unchanged, or intentionally re-blessed with `--bless`. |
| Mods | `tests/harness/check_mods.sh` | The mod PoC ladder, including per-mod `modtest.toml` scenarios (ctest `mod_poc_ladder`, ~10 min). |

Oracle internals are in [`duckstation-oracle.md`](duckstation-oracle.md).
The presentation golden runs across several machines through
[`golden-fleet.md`](golden-fleet.md).

### Skip targets

`tests/harness/check_skip_targets.sh [--game SLUG] [--build-dir=DIR] [--filter=NAME]`
(default game `spyro-1`) runs every row of `games/<game>/corpus/skip-targets.manifest`
headless: 5 `--skip-to-cutscene` rows for Spyro 1, 11 for Spyro 2, 38
`--skip-to-area` and 19 `--skip-to-cutscene` rows for Spyro 3. A row is `name | args | frames | expect`:
`args` the skip flags, `frames` the run's `--frame-limit` in VBLs, `expect` the
tokens the skip's release line (`[skip-to-level] level playable`) must carry,
values the skip module reads back from RAM at the release edge. Log tokens
test the whole log instead, `~` standing for a space: `log:TEXT` requires a
line containing TEXT, `log:A>>B` a line containing A before the first line
containing B, `!log:TEXT` no line containing TEXT. A row passes
when the run exits 0, prints the release line with every token met, and prints no
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
| 2 | State divergence against the golden - the oracle gate's verdict. |
| 3 | Setup error: unknown flag, disc not found. |

## Artifacts, not interaction

An unattended run has no debugger attached and no one to press a button, so
every piece of state that matters has to reach the filesystem as a readable
artifact: a log line, a JSON record, a PNG, a `.bk2`, a RAM dump
(`--ram-dump-on-frame`), or a savestate (`--state-save-on-frame`). Configure
the dumps before the run, then read them after it exits.

Screenshots answer what the render looks like. They do not settle whether it
is correct; that is the presentation golden's job, and
`tests/harness/center_crop_diff.py A.png B.png` turns a pair of captures into a
pixel-identity number rather than an impression.
