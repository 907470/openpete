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
| `--headless` | No on-screen window and no display server. A run PsyCross rasterizes for takes its GL context from SDL's offscreen driver (surfaceless EGL; needs libEGL) on Linux and from the platform driver on a hidden window on Windows; any other run uses SDL's dummy driver. A set `SDL_VIDEO_DRIVER` or `SDL_VIDEODRIVER` wins. |
| `--frame-limit N` | Exit once the feed key reaches N: yields on Spyro 1, VBLs on Spyro 2 and 3. A tick is two VBLs. |
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
| Mods | `tests/harness/check_mods.sh` | The mod PoC ladder, including per-mod `modtest.toml` scenarios (ctest `mod_ladder_games`, `mod_ladder_matrix` and `mod_ladder_timeline`, one `--section` each; ~19 min together). |

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

### Loop decision trace

`tests/harness/check_loop_trace.sh [--game SLUG] [--build-dir=DIR] [--row NAME]...
[--windowed] [--bless] [--ref-bin=PATH]` runs each row of
`games/<game>/corpus/loop-trace.manifest` with `OPENPETE_LOOP_TRACE` set and
checks that the host loop made the same decision at every iteration as the
blessed build. A row is `row | kind | source | launch | covers`: `kind` is
`boot`, `bk2` (the movie replayed to its end), `setup-ftl` or `setup-pace`
(a session that fails setup and exits 3), or `windowed`; `covers` lists the
engagement terms, a count from `tests/harness/looptrace.py counts` that must
be non-zero (`speculated`, `rollbacks`), equal a value (`headers=2`), or a
log line (`log:TEXT`, `~` for a space). A row that misses a term is VACUOUS.
The verdict is the row's line in `tests/golden/<game>/loop-trace-baseline.tsv`:
exit code, record count and the sha256 of the compared fields of every
record, equal exactly when `looptrace.py compare` reports two traces
identical. Off the `--record-cfr` virtual clock the wall-clock fields are
masked, so headless traces of one build are identical run to run. Traces are
not committed; on a DIVERGED row, `--ref-bin=PATH` reruns it with a build of
the baseline's revision and prints the first differing record, and
`looptrace.py dump` prints the records around it. `--bless` rewrites the
rows the run made, for an intended change to a loop decision. The
`windowed` rows (t3: `--record-cfr` at 4 presents per tick, 32-56 min;
t3b: the same with `OPENPETE_PACE_INJECT_PRESENT_US` and
`OPENPETE_PACE_LATE_BUDGET_US`, so strict, late, probe and shed admits all
occur) need `xvfb-run` and run only with `--windowed`. Outputs land in
`<build-dir>/loop-trace/<game>/<row>/`. Exit 0 PASS, 1 FAIL, 64 SKIP. ctest
runs the headless rows as `loop_trace_spyro-1`, `loop_trace_spyro-2` and
`loop_trace_spyro-3`, labels `slow`, `determinism` and `loop-trace`
(`ctest -L loop-trace`).

## Exit codes

Defined in `core/include/openpete.h`.

| Code | Meaning |
|---|---|
| 0 | Clean termination (frame limit, SDL quit). |
| 1 | Panic, crash, or watchdog timeout. |
| 2 | State divergence against the golden - the oracle gate's verdict. |
| 3 | Setup error: unknown flag, disc not found, a disc this game cannot boot (another region's release), or under `OPENPETE_DISC_BUILD_REFUSE=1` any disc build that is not supported. |
| 64 | The card guard refused a card path in the per-user card directory (`OPENPETE_CARD_GUARD`); a gate harness's SKIP. |
| 66 | The sandbox guard refused a write path outside the `OPENPETE_SANDBOX` root (`core/include/openpete_paths.h`). |

## Disc builds

At boot, before the game's code is attached, the disc's SYSTEM.CNF serial
and the SHA-256 of the boot executable it names are classified against the
known builds (`core/psx/disc_build.c`) and logged as a `disc` line:
`build of PATH: supported|recognised|unknown|unreadable (serial, region,
boot, exe sha256)`. A disc whose boot executable is not the one the game
boots is refused (exit 3, `OP-E-DISC-BUILD`). Another build of a supported
serial logs a warning, holds a toast for 30 s, and boots. A gate or corpus
run sets `OPENPETE_DISC_BUILD_REFUSE=1` to refuse every build that is not
supported, so a run is never measured on another build.
`tests/unit/disc_build_test` covers the classifier; with
`OPENPETE_DISC_BUILD_REAL=1` it also classifies the three checkouts' discs.

## The sandbox

A run writes into the per-user directories by default: it rotates
`openpete.log`, writes the hang watchdog's `hang-trace.bin` and
`hang-ram.bin`, puts a bug-report bundle among the player's own, saves the
settings menu's changes into `openpete.toml`, fills the Vulkan pipeline
cache, and reads the player's config. A scripted run sets
`OPENPETE_SANDBOX=<root>`, under which `core/config/paths.c` resolves the
config, state and data directories to `<root>/config/openpete`,
`<root>/state/openpete` and `<root>/data/openpete` on every platform: logs
and hang files in `state/openpete/logs`, cards, savestates and the pipeline
cache in `state/openpete`, bundles, screenshots, recordings, the library
and modules in `data/openpete`, the settings file and the overlay's
`openpete-devui.ini` in `config/openpete`. The portable layout and the
adoption of directories beside the executable are off, and the default
screenshot, `--bk2-record` and hang-file paths never fall back to the
working directory.

The game refuses to start, exit 66, when the root, a state or data leaf it
resolves, or an output path named by `OPENPETE_LOG_FILE`,
`OPENPETE_HANG_TRACE`, `OPENPETE_HANG_RAM`, `OPENPETE_CARD_DIR`,
`OPENPETE_CONFIG` or an output flag (`--log-file`, `--card1`, `--config`,
`--bk2-record`, `--screenshot-output` and the rest
`openpete_paths_sandbox_guard` lists) lies in a per-user openpete
directory (as XDG or `%APPDATA%`/`%LOCALAPPDATA%` names it and, outside
Windows, under the account's passwd home) or in a leaf the portable layout
writes beside the executable. The guard runs before anything is written,
and only when the variable was set by the caller; a bug-report replay sets
its own root, `<scratch>/sandbox`.

Harnesses take the root from `scripts/lib/sandbox.sh` (sourced; also
through `scratch_cards.sh`, `presentation-game.sh`, `game.sh` and
`corpus-jobs.sh`) or `hostenv.sandbox()` in Python: a fresh
`$TMPDIR/openpete-harness-sandbox/<pid>.<random>` removed at exit (another
parent with `OPENPETE_SANDBOX_PARENT`), its `state/openpete/cache` a link to
one shared pipeline cache, `~/.cache/openpete-harness/cache`
(`OPENPETE_SANDBOX_CACHE`), so runs stay warm. `op_sandbox_run_root` and
`hostenv.sandbox_run_root()` make a fresh root per run; `pg_capture` uses
one for every run. `scripts/lib/wslenv.sh` and `hostenv.child_env` forward
the root to a Windows exe as a Windows path; a root on a Windows drive
gets a plain cache directory instead of the link.
`scripts/fences/sandbox_guard.py` (in the `openpete_launch_guards` build
target) refuses a script that launches the game without the library or the
variable; a script that needs the player's real state or a fresh-user
boot carries `sandbox-guard: intentional <reason>`, one that never starts
the game `sandbox-guard: no-launch`.

`tests/harness/check_sandbox.sh` proves it: it lists the per-user
directories (path, mtime, size) before and after runs that write a log,
the pipeline cache, a savestate, a player screenshot, a settings save, a
default `--bk2-record`, a bug bundle, the hang watchdog's files and a
replay, fails on a change, and requires each file under the run's root.
Its controls: a root inside a per-user directory exits 66 with nothing
written, and a run without a root changes the listing.

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
