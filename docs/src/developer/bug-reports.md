# Bug reports

A bug report is one zip a reader with this repository debugs from without
the tester's machine. The `bug_report` key action (default ++f9++, every
build) or `OPENPETE_BUGREPORT_AT`
writes it under the data directory's `bugreports/`; a hang-watchdog trip
or a CdSync spin-guard exit writes one through the exit flush. The User
side is [Reporting a bug](../user/help/reporting-a-bug.md). This page
covers reading one, replaying it, and the variables that drive the writer
from a harness.

## Bundle members

`manifest.json` (schema 1) lists every member with its size and SHA-256,
and the identity fields: build version and commit, exe SHA-256, module
SHA-256 and march, codegen and ROM stamps, disc name and SHA-256, game,
argv, the anchor and press records (frame, VBL, feed key, park site), the
taint counts, the thumbnail list, the sub-tick ring's counts, the note
state and the redaction counts with their findings. Its `records` field
names the performance records the writing build records: the pace record
types (`pace.types`), whether GPU_FRAME records were on
(`pace.gpu_frames`), and the coarse ring's member, version, row count and
row span (`pace_coarse`). A bundle without `records` was written before
these records existed; the reader still reads its pace ring.

| Member | Content |
| --- | --- |
| `anchor.state`, `card<N>.mcd`, `card<N>.mcd.modsave` | the recorder's anchor: a savestate image 30 to 60 s before the press, with the card images and mod save records it held |
| `press.state`, `press-card<N>.mcd`, `press-card<N>.mcd.modsave` | the state at the press; absent in a fatal-exit bundle |
| `segment.bk2` | the input-log rows from the anchor's consume point to the press |
| `hashes.csv` | one `frame,vbl,fnv1a64,<region>...` row per tick from the anchor, the `--gameplay-hash-log` row |
| `host-trace.bin`, `pace.oppl`, `input-events.csv`, `hang/rings.bin` | the host-loop trace, the pace ring (last 30 s), the raw host input ring, the hang watchdog's dispatch rings |
| `screen.png`, `thumbs/NNN.png` | the presented image at the press; the thumbnail ring, oldest first |
| `pace-coarse.bin` | the coarse session ring: one row per second since the pace ring was armed (rows merge pairwise and the span doubles when 8192 rows fill), folded from every pace record: VBLs and the not-live ones, presents, repeats, sheds, slips, the longest present interval, the TICK_SPLIT sums, the EAGER_PLAN slots, scenes, cuts and highest level, the GPU_FRAME sum and maximum (layout in `core/include/openpete_pace_log.h`, `OP_PACE_CR_*`) |
| `subtick.bin` | the sub-tick present ring: the last 1024 re-extracted presents (2.13 s at 480 Hz) with kind, alpha, the tick stamps they lerp between and the scene ring slot, and per drawn object record its pairing key, class, pairing verdict, the snapshot pair's prev and curr positions, angles and roll stamps, and the screen centroid and world position it was drawn at (layout in `core/include/openpete_bugrec_subtick.h`) |
| `audio-feed.txt` | the audio device feed at the press: mute and device state, its counters (the exit summary's `spu:` and `feed:` lines and a `feed: now:` state line), and its event ring of the last 2048 events (layout under [Audio feed record](#audio-feed-record)) |
| `reports.txt`, `pace-census.txt`, `summary.txt` | the subsystem reports at the press and at the screenshot; their pace lines; the state summary |
| `config.txt`, `keymap.txt`, `config-files/`, `env.txt`, `argv.txt` | `--config-report`, `--keys-report`, the raw config tiers, the `OPENPETE_*` environment, the command line |
| `device.txt`, `devices.txt` | Vulkan device, displays, system; the input devices |
| `log.txt`, `log-prev.txt`, `hang/`, `crash/` | the log tails of this run and the previous one, the hang dumps and minidump the previous run's log names |
| `mods.txt`, `mods/<id>/` | every installed mod with per-file SHA-256; every enabled mod's files, its loaded object and its effective config rows |
| `module.txt`, `module/<file>` | the recomp module's linkage, stamps and march; the module itself when it is a file |
| `notes.txt` | the tester's note, when one was saved |

Text members are redacted: every form of the home directory becomes `~`,
the user name `<user>`, the host name `<host>`; the state files' card
paths are rewritten to `card1.mcd` and `card2.mcd`. Guest bytes, modules
and mod objects are never rewritten; a hit in them is a finding listed in
the manifest and logged as `bugreport: finding:`. A mod config row marked
`secret = true`, and every key the mod's schema does not declare, is
written as a comment naming the key.

## Performance records

The pace ring (`pace.oppl`, 30 s) carries three record types beside the
pacing records, and `pace-coarse.bin` folds every record into rows that
cover the whole session.

| Record | When | Content |
| --- | --- | --- |
| `TICK_SPLIT` (15) | after each VBL whose fiber run raised a present (a drawing VBL) | microseconds since the previous one: sim (every fiber run), extract (re-extracts, as in PRESENT_SPAN), terrain (the canonical terrain build and scene feed, inside sim), moby (the canonical moby native pass after the game's draw, inside sim), snap in sub-tick re-extracts (inside extract) and in the canonical feed (inside terrain and moby), render (scene render and submit on the presenting thread), present (the present thread's staged present: acquire, blit, queue present); the counts of extracts, renders, presents and snap resolves; the drawing VBL's own fiber run |
| `EAGER_PLAN` (16) | each span eager production plans (one per canonical tick, windowed only) | the `OPENPETE_EAGER_DBG` plan line: slots, budget, per-scene tail cost, afford, the span's level and the standing level, the previous span's cut, lag, non-drawing VBL cost, tick start lateness, planned scenes and the planned grid period |
| `GPU_FRAME` (17) | each rendered frame's harvested GPU timestamp zones, under `OPENPETE_BUGREPORT_GPU=1` | first zone start to last zone end, the depth-0 zone sum, the zone count and the RHI frame token |

Headless runs write TICK_SPLIT records with the sim, terrain, moby and
canonical snap slots (and only when the native renderer is up, which
headless needs a `--screenshot-on-frame` for); eager production, and with
it EAGER_PLAN and the extract, render and present slots, runs only in a
window. `OPENPETE_BUGREPORT_GPU` is off by default: its GPU cost on a weak
GPU is unmeasured. Measure it on the Windows exe with two windowed
`--pace-log` runs of the same bk2, with and without the variable, and
compare the present-interval p99 and the fence block times
(`scripts/pacing/pace.py`).

Sizes: a TICK_SPLIT and an EAGER_PLAN record are 64 B each per tick, 3.8
KB/s together (115 KB in a 30 s ring); GPU_FRAME is 64 B per rendered frame
(115 KB per 30 s at 60 Hz, 920 KB at 480 Hz). They share the existing 8 MiB
ring (131072 records), which then holds 30 s at about 2600 records per
second. The coarse ring is 1 MiB in memory (8192 rows of 128 B) and
writes only its used rows: 450 KB raw for an hour.

## Audio feed record

PsyCross's device feed (`third_party/PsyCross/src/audio/PsyX_SPUFeed.cpp`)
records every timeline event, the events `OPENPETE_AUDIO_FEED_LOG` writes,
into a ring of the last 2048 (`PSYX_SPUFEED_EVENTS`) whether or not the log
is open: `tick` (a sample every 250 ms of device time; the feed log's name,
not a sim tick), `phase`, `hold`, `release`, `prime`, `UNDERRUN`, `drain`
(data ran out while held), `OVERFLOW`, `overflow-end`, `lag-drop`,
`step-drop`, `stretch`, `stretch-end` and `slip` (the host's slip count
rose).

Threads: the event is recorded by the thread that raises it, under the
feed's mutex, which that thread already holds: the audio consumer thread
(`audio` role) for samples, primes, underruns, drains, drops, holds,
releases, stretch edges and slips; the sim thread (the producer) for
overflows and phase changes. OpenAL's mixer thread never touches the feed.
The press reads the ring on the sim thread without a lock: each 112 B slot
carries a sequence word (odd while written, even naming its event), and a
slot rewritten during the copy is skipped and counted
(`events_skipped`). Recording stores 14 words and reads the clock; it
allocates and prints nothing. The counters are read under the feed's
mutex (the exit summary's call), except at a fatal exit off the sim's
thread, where the file says `counters=not read`.

Sizing: over 51 feed logs of earlier audio work (15 to 218 s each),
non-sample events run at 1 to 10 per minute in steady play; the worst 60 s
window held 464 (an overflow starting and ending at each push under an
injected stall) and the worst 5 s window 159. With the 4 samples per
second the ring covers 8.5 min of steady play, 2.9 min at the worst 60 s
rate and 57 s at the worst 5 s rate sustained. The ring is 229 KiB of
memory; a full ring is about 195 KB of text (95 B per row).

`audio-feed.txt` (version 1): `key=value` header lines (`version`, `mute`,
`device` openal, synthetic or none, `snapshot_wall_ns`,
`snapshot_present_ns`, `snapshot_vbl`, `counters`, `ring`, `events_total`,
`events_held`, `events_skipped`, and `note` lines stating a mute or a
missing device); the counter lines; then one row per event, oldest first:

| Column | Content |
| --- | --- |
| `t_s` | seconds before the snapshot (`SDL_GetTicksNS`, the events' clock) |
| `vbl` | the host's VBL count the feed last received (`openpete_vbl_device`) |
| `consumed` | frames the device had consumed: the device clock |
| `event`, `phase` | the event name; the host's phase (`boot`, `skip`, `unlocked`, `paused`, `live`, `headless`) |
| `flags` | `h` held, `p` data playing, `s` time stretch engaged; `-` when not |
| `ratio` | input frames per output frame; the reader shows it in cents |
| `speed`, `drift` | the measured producer speed against real time; producer-time frames per device frame |
| `depth_s`, `st_rate`, `fifo` | the smoothed queued depth in output frames; the stretch rate; FIFO frames |
| `underruns`, `slips`, `behind` | underruns so far; the host's slip periods; sim periods the producer is behind real time |
| `pushed` | frames the producer had pushed |
| `value` | `lag-drop`, `step-drop`: output frames dropped; `slip`: slip periods added; `prime`: the depth target; `OVERFLOW`, `overflow-end`: frames dropped on overflow so far; `hold`, `release`: frames discarded so far; else 0 |

A muted run (`--mute 1` or the overlay) still runs the feed on the
silenced SPU output; `mute=1` and a `note` line say so. A run whose audio
device did not open holds only `phase` events; `device=none` and a `note`
line say so.

## Reading a bundle: `tools/bugreport.py`

The reader treats a bundle as untrusted input: members are read by name
with size bounds; absolute names, `..` components, drive letters, NUL
bytes, duplicates and symlinks are refused. It uses the standard library
only and runs under `python3 -I`.

```
python3 -I tools/bugreport.py info BUNDLE [--log-lines N] [--members] [--archive DIR]
python3 -I tools/bugreport.py extract BUNDLE DIR
python3 -I tools/bugreport.py replay BUNDLE [--exe PATH] [--disc CUE] [--archive DIR]
                                     [--work DIR] [--timeout S] [--dry-run] [-- EXTRA...]
python3 -I tools/bugreport.py subtick BUNDLE [--class C] [--key K] [--last N] [--max-keys N]
                                      [--tol-px PX] [--tol-world W]
python3 -I tools/bugreport.py audio BUNDLE [--kind K,...] [--last N] [--no-ticks]
python3 -I tools/bugreport.py perf BUNDLE [--rows]
python3 -I tools/bugreport.py corpus PATH... [--tsv OUT] [--quiet]
python3 -I tools/bugreport.py diff-hashes A B
```

- `info` prints the summary a reader starts from: build, exe SHA-256,
  game, disc, stamps, mods, press and anchor counters, the state summary,
  taint counts, members and the hash check, redaction findings, a pace
  summary over the last 30 s of present records, the last `--log-lines`
  (default 20) WARN and ERROR log lines, and the replay route with its
  command.
- `extract` writes every member under DIR and verifies each against the
  manifest.
- `replay` runs the route `info` names and prints the run's
  `bug-report-replay:` verdict line. The run's output goes to
  `<work>/replay.log`; `--work` defaults to `bugreport-replay/<bundle stem>`
  under the working directory. `--disc` defaults to
  `games/<game>/<game>/build/<manifest disc>`. `--timeout` defaults to
  1800 s. `--dry-run` prints the route and the command only. Arguments
  after `--` go to the exe.
- `subtick` reads `subtick.bin` (BUNDLE may be the extracted member):
  per object class the records drawn over the held presents and how many
  were drawn on intermediate presents; with `--class` or `--key`, each
  selected record's last `--last` draws (present, kind, path, alpha,
  verdict, keyframe verdict, the pair's phase, screen centroid, drawn
  world position, the position the pair lerps to). It flags MULTI, a
  record drawn at screen positions more than `--tol-px` (default 2) apart
  within one present, and OFFLERP, a paired record drawn more than
  `--tol-world` (default 2) world units from both its lerp and its curr
  position; exit 2 when either flag fires. `info` prints the ring's
  one-line summary.
- `audio` reads `audio-feed.txt` (BUNDLE may be the extracted member):
  the header and its notes, the counter lines, and the events with the
  ratio in cents; `--kind` lists the named events only, `--no-ticks`
  leaves out the samples, `--last` keeps the last N. Exit 3 (VACUOUS)
  when the feed played data and the ring holds no event. `info` prints a
  one-line summary: mute, device, events by kind, underruns, primes,
  stretch engages, producer speed and pitch p99.
- `perf` reads the performance records (BUNDLE may be an extracted
  `pace.oppl`): the manifest's `records` field; over the pace ring the
  present rate over the window and over its last 10 s, the present
  interval p50, p99 and maximum, the sim tick p50 and p90 (the drawing
  VBL's fiber run: TICK_SPLIT's, else the longer run of each VBL pair),
  scenes and sheds per canonical present; the TICK_SPLIT p50 per slot;
  the EAGER_PLAN means of slots, afford and planned scenes, level p50 and
  maximum, the share of cut spans and the budget p50; the GPU_FRAME p50
  and p99; and the coarse rows' per-row present rate p10, p50 and p90 with
  the first and last row's. `--rows` prints the coarse rows as TSV. `info`
  prints a one-line count of the records.
- `corpus` takes directories (every `bug-*.zip` inside) and bundle paths
  and writes one TSV row per bundle: name, error, build, commit, game,
  level, trigger, platform, GPU, driver, refresh, present mode,
  fullscreen, `--render-fps`, eager, swapchain extent, enabled mods, the
  note's first 80 characters, the records it holds, the `perf`
  rollups, and the audio feed's device, mute, event count, underruns,
  stretch engages, producer speed and pitch p99. A bundle without a record kind leaves its columns empty; a
  bundle that does not open gets a row with the reason. The summary
  counts bundles per record kind, gives the median rollups per game and
  GPU, and lists the five bundles whose last 10 s present rate is
  furthest below their target (`--render-fps` when below the refresh,
  else the refresh). Bundles stay local: a received bundle holds the
  tester's savestates, so the corpus is a directory outside the
  repository.
- `diff-hashes` names the first divergent tick and the regions that
  differ between two region-hash CSVs; A or B may be a bundle, whose
  `hashes.csv` is read.

| Exit | `bugreport.py` |
| --- | --- |
| 0 | verified; IDENTICAL |
| 1 | a member fails its manifest hash or size |
| 2 | DIVERGED |
| 3 | VACUOUS: no rows compared, no verdict line, or the replay timed out |
| 64 | refused: malformed bundle, bad arguments, no anchor, disc not found, replay refused |

## Replay routes

`bugreport.py replay` chooses the route.

**Primary**: the release archive holds the artifact the tester ran. The
reader looks in `<archive>/<build_version>-<build_commit>/` first, then
every directory of the archive, for an `EXE-SHA256SUMS` line whose hash
equals the manifest's `exe_sha256` (the executable inside the artifact,
which is what the writer hashes); `info` names the artifact, its kind and
the exe path inside it. `replay` unpacks the artifact into
`<work>/artifact/` and refuses it (exit 64) when the unpacked exe's
SHA-256 is not the manifest's: a tarball is untarred and its exe runs; an
AppImage is extracted with its own `--appimage-extract` (no FUSE) and
`squashfs-root/AppRun` runs; a Windows zip is unzipped and the command is
printed, not run (exit 64), since the Windows exe is driven on the Windows
host. The archive is `--archive`, else `OPENPETE_RELEASE_ARCHIVE_DIR`, else
`${XDG_DATA_HOME:-~/.local/share}/openpete/release-archive`.

**Fallback**: no archived artifact matches, or `--exe` was given. The
command runs `--bug-report-replay` on `--exe`, default
`build/linux-release/openpete`. The route line says whether that exe's
SHA-256 equals the tester's.

Either route runs the exe with `--game <game> --headless --input-lock
--vibration 0 --mute 1 --disc <cue> --bug-report-replay <bundle>`, the
bundle's card images extracted to `<work>/cards/` as `--card1`/`--card2`
(`--no-card` when the bundle holds none), `OPENPETE_VIBRATION=0`, and
`OPENPETE_MODULE_MARCH` set to the manifest's `module_march`.

## `--bug-report-replay PATH`

A debug flag (absent from `--help` in the `*-launcher` builds). PATH is a
zip or an extracted directory. `--disc` is required: the tester's disc
path is not in the bundle.

1. Every member is verified against `manifest.json` and copied to the
   scratch directory, `<bundle without .zip>.replay/`
   (`OPENPETE_BUGREPORT_REPLAY_DIR` names another).
2. The command line is rebuilt: the tester's argv without its output,
   route and path flags, `~` remapped into the scratch directory; then the
   replay's flags (headless, the scratch cards, the bundle's `mods/` as the
   mods root, `anchor.state` as the `--resume-handoff` file, `segment.bk2`
   as the `--bk2` movie at the anchor's consume point); then the
   developer's own flags. The environment takes `env.txt`'s `OPENPETE_*`
   variables except the harness triggers below, `OPENPETE_HANDOFF_AT`,
   `OPENPETE_FRAME_LIMIT`, `OPENPETE_CONFIG`, `OPENPETE_LOG_FILE` and the
   mouse injection variables; the bundle's `config-files/` become the two
   config tiers (`--no-config` when it holds none). Every other
   `OPENPETE_*` variable of the developer's environment is unset, except
   the `OPENPETE_BUGREPORT_REPLAY_*` ones and those
   `OPENPETE_BUGREPORT_REPLAY_ENV` names (below).
   The launch scripts, `--skip-to-level`, `--skip-to-area`,
   `--skip-to-cutscene`, `--player-pose` (and `OPENPETE_PLAYER_POSE` in
   `env.txt`) and `--cheats`, write guest RAM once at a point of the
   session they wait for and keep their progress in host state the anchor
   does not hold. They pass only when the anchor is the session's first
   (manifest `anchor.seq` 1): it is taken at the first admissible yield
   after fast boot, before any of them runs, and a guest state load drops
   every anchor. From a later anchor they are dropped. A manifest without
   `anchor.seq` is decided by `anchor.frame` 1 (label `launch-inferred`).
   The `bug-report-replay: launch flags kept|dropped: <reason>: <flags>`
   line names the decision. `--load-delay` acts on every tick and always
   passes.
3. When this exe's SHA-256 equals the manifest's, the bundle's
   `module/<file>` is passed as `--recomp-module` (label `primary-route`):
   the tester's module, compiled for the tester's CPU, is loaded as is.
4. Admission: the game must match; the ROM stamps must match (when either
   side has none, the disc SHA-256 must); the anchor's state identity must
   recompute from the manifest's codegen and ROM stamps under this build's
   state ABI. Labels: `cross-disc` (ROM stamps equal, disc SHA-256
   differs), `cross-module` (codegen stamp differs), `cross-build` (build
   version differs), `cross-regions` (the region tables do not share every
   name; totals are then not compared).
5. The anchor loads at its park site on an empty CD queue, waiting up to
   9000 park waits (`OPENPETE_BUGREPORT_REPLAY_SITE_WAIT`), with the movie
   held until the load commits and the anchor's card images restored at
   the load. Each tick's region-hash row is compared with `hashes.csv`; at
   the press frame the state is captured and compared with `press.state`
   section by section. A fatal-exit bundle (manifest `press` null, trigger
   `hang` or `cdsync-spin`) holds no `press.state`: it replays to the last
   row of `hashes.csv`, compares hash rows only, and the verdict line
   carries `sections=skipped fatal=<trigger>`.

The verdict line on stderr:

```
bug-report-replay: IDENTICAL rows=N sections=equal labels=L taint=T [vram-differs=...]
bug-report-replay: IDENTICAL rows=N sections=skipped fatal=hang labels=L taint=T
bug-report-replay: DIVERGED tick=F region=R section=S rows=N [fatal=...] labels=L taint=T
bug-report-replay: VACUOUS rows=0 (the run ended at frame F, press frame P) labels=L taint=T
bug-report-replay: REFUSED <reason> labels=L
```

| Exit | `--bug-report-replay` |
| --- | --- |
| 0 | IDENTICAL |
| 2 | DIVERGED or VACUOUS |
| 64 | refused |

A run that ends before the press frame (for a fatal bundle, `last hash
row`) after comparing rows reports DIVERGED in the VACUOUS form. A DIVERGED line is followed by a
`sections differing:` line naming every press-state section that differs. VRAM is not
gameplay state: a VRAM difference is printed (`vram-differs=` on
IDENTICAL, a `vram differs (not counted)` line on DIVERGED) and does not
make the verdict DIVERGED. `taint` counts the events since the anchor that
a replay cannot reproduce.

`tests/harness/check_bugreport_replay.sh` (ctest `bugreport_replay`)
records two Spyro 2 bundles headless under `OPENPETE_BUGREPORT_AT`, one
with `--skip-to-level`, and requires IDENTICAL replays of both, DIVERGED
with the launch flags dropped when the skip bundle's `anchor.seq` is set to
2, an `OPENPETE_BUGREPORT_REPLAY_ENV` instrument's output under the replay,
and the refusal of `OPENPETE_PLAYER_POSE` there.

## Release archive

`scripts/package-dist.sh` (outside a container) and
`scripts/release-container.sh` (on the host, after the container exits)
copy every package they produce, tagged or not, into

```
${XDG_DATA_HOME:-~/.local/share}/openpete/release-archive/<version>-<commit9>/
    <artifact>...
    SHA256SUMS        "<hex>  <artifact>": each artifact's own SHA-256
    EXE-SHA256SUMS    "<hex>  <artifact>:<path inside>": the executable inside it
```

`OPENPETE_RELEASE_ARCHIVE_DIR` names another root, for the scripts and
for `bugreport.py`. The writer records the SHA-256 of the running binary
(`/proc/self/exe`), which inside an AppImage is the inner binary, so
`exe_sha256` is the `EXE-SHA256SUMS` value and `bugreport.py` matches on
that file alone; an artifact with no `EXE-SHA256SUMS` line is not found.

## Harness variables

| Variable | Effect |
| --- | --- |
| `OPENPETE_BUGREPORT_AT=N` | press at feed key N |
| `OPENPETE_BUGREPORT_NOTE` | `0`: never ask for the note; `1`: ask on an `OPENPETE_BUGREPORT_AT` press too. Unset: ask on a key press only |
| `OPENPETE_BUGREC_ANCHOR_AT=K1,K2,...` | take an extra anchor at each feed key, beside the 30 s cadence |
| `OPENPETE_BUGREC_DUMP_AT=N`, `OPENPETE_BUGREC_DUMP_DIR=D` | at feed key N, write the recorder's contents to D: `anchor-<i>.state`, `.card<N>.mcr`, `.modsave`, `.seg.bk2` and `.txt` per anchor, `hashes.csv`, `hosttrace.bin`, `pace.bin`, `input-events.csv`, `devices.txt`, `recorder.txt`. The first without the second logs an error and dumps nothing |
| `OPENPETE_BUGREPORT_THUMBS=0` | turn the thumbnail ring off (default `1`): 20 thumbnails in a 320x180 box, one per 250 ms of present clock |
| `OPENPETE_BUGREPORT_SUBTICK=0` | turn the sub-tick present ring off (default `1`); the bundle then holds no `subtick.bin` |
| `OPENPETE_BUGREPORT_GPU=1` | arm the GPU timestamp zones and record a GPU_FRAME per rendered frame (default `0`); read at device creation. `OPENPETE_GPU_TIMERS=1` arms the same zones and also prints the `[gpu-pass]` line |
| `OPENPETE_PACE_RING=0` | no pace ring and no coarse ring; the bundle then holds neither `pace.oppl` nor `pace-coarse.bin`, and no TICK_SPLIT, EAGER_PLAN or GPU_FRAME record is made unless `--pace-log` is open |
| `OPENPETE_BUGREPORT_REPLAY_DIR`, `OPENPETE_BUGREPORT_REPLAY_SITE_WAIT`, `OPENPETE_BUGREPORT_REPLAY_WINDOWED` | the replay's scratch directory, park-wait cap (above), and a set value runs the replay in a window instead of headless |
| `OPENPETE_BUGREPORT_REPLAY_ENV=A,B=V,...` | instrument variables the replay keeps: `NAME` keeps the developer's value, `NAME=VALUE` sets one; set after `env.txt`, each printed as `env kept`. Refused (exit 64): a name outside `OPENPETE_*`, a harness trigger the replay drops, an unset name, and a variable whose consumer writes guest RAM, feeds the pad or the CD timing, or loads mods (`OPENPETE_PLAYER_POSE`, `OPENPETE_FREECAM*`, `OPENPETE_CAMLOCK*`, `OPENPETE_CUSTOM_WARP`, `OPENPETE_SKIP_*`, `OPENPETE_LOAD_DELAY`, `OPENPETE_INPUT_*`, `OPENPETE_SYNTH_PADS`, `OPENPETE_MOUSE*`, `OPENPETE_DEVUI_INJECT_MOUSE`, `OPENPETE_REWIND_*`, `OPENPETE_MODS`, `OPENPETE_MODS_DIR`, `OPENPETE_MOD_CHANNEL_MASK`, `OPENPETE_MOD_HOT_RELOAD`, the `*_POKE` fault injectors, `OPENPETE_MODSAVE_FAULT_*`). A kept variable not on that list is unverified as neutral; the row and press-state compares report it if it is not |

A replay drops `OPENPETE_BUGREPORT_AT`, `OPENPETE_BUGREC_ANCHOR_AT`,
`OPENPETE_BUGREC_DUMP_AT` and `OPENPETE_BUGREC_DUMP_DIR` from the
tester's environment.

Feed keys count yields on Spyro the Dragon and VBLs on Spyro 2 and 3, as
`--frame-limit` does ([Unattended runs](unattended-runs.md)).
