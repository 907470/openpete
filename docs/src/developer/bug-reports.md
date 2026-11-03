# Bug reports

A bug report is one zip a reader with this repository debugs from without
the tester's machine. The `bug_report` key action (default ++f9++, every
build), the settings menu's **BUG REPORT** row, or `OPENPETE_BUGREPORT_AT`
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
taint counts, the thumbnail list, the note state and the redaction counts
with their findings.

| Member | Content |
| --- | --- |
| `anchor.state`, `card<N>.mcd`, `card<N>.mcd.modsave` | the recorder's anchor: a savestate image 30 to 60 s before the press, with the card images and mod save records it held |
| `press.state`, `press-card<N>.mcd`, `press-card<N>.mcd.modsave` | the state at the press; absent in a fatal-exit bundle |
| `segment.bk2` | the input-log rows from the anchor's consume point to the press |
| `hashes.csv` | one `frame,vbl,fnv1a64,<region>...` row per tick from the anchor, the `--gameplay-hash-log` row |
| `host-trace.bin`, `pace.oppl`, `input-events.csv`, `hang/rings.bin` | the host-loop trace, the pace ring (last 30 s), the raw host input ring, the hang watchdog's dispatch rings |
| `screen.png`, `thumbs/NNN.png` | the presented image at the press; the thumbnail ring, oldest first |
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
every directory of the archive, for a file whose SHA-256 (its
`SHA256SUMS` line, else computed) equals the manifest's `exe_sha256`, or a
tarball or zip holding an `openpete` or `openpete.exe` member of that
SHA-256. A tarball is extracted to `<work>/artifact/` and its exe runs; a
Windows zip prints a note and runs nothing (exit 64), since the Windows
exe is driven on the Windows host. The archive defaults to
`${XDG_DATA_HOME:-~/.local/share}/openpete/release-archive`; `--archive`
names another.

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
   config tiers (`--no-config` when it holds none).
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
   section by section.

The verdict line on stderr:

```
bug-report-replay: IDENTICAL rows=N sections=equal labels=L taint=T [vram-differs=...]
bug-report-replay: DIVERGED tick=F region=R section=S rows=N labels=L taint=T
bug-report-replay: VACUOUS rows=0 (the run ended at frame F, press frame P) labels=L taint=T
bug-report-replay: REFUSED <reason> labels=L
```

| Exit | `--bug-report-replay` |
| --- | --- |
| 0 | IDENTICAL |
| 2 | DIVERGED or VACUOUS |
| 64 | refused |

A run that ends before the press frame after comparing rows reports
DIVERGED in the VACUOUS form. A DIVERGED line is followed by a
`sections differing:` line naming every press-state section that differs. VRAM is not
gameplay state: a VRAM difference is printed (`vram-differs=` on
IDENTICAL, a `vram differs (not counted)` line on DIVERGED) and does not
make the verdict DIVERGED. `taint` counts the events since the anchor that
a replay cannot reproduce. A bundle whose manifest `press` is null (a fatal-exit
bundle) is refused.

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

`OPENPETE_RELEASE_ARCHIVE_DIR` names another root for the scripts;
`bugreport.py` does not read it (pass `--archive`). The writer records the
SHA-256 of the running binary (`/proc/self/exe`), which inside an AppImage
is the inner binary, so `exe_sha256` is the `EXE-SHA256SUMS` value.
`bugreport.py` matches an AppImage by the AppImage file's own SHA-256 and
does not read `EXE-SHA256SUMS`, so a bundle from an AppImage finds its
artifact only through a tarball holding the same `openpete` binary.

## Harness variables

| Variable | Effect |
| --- | --- |
| `OPENPETE_BUGREPORT_AT=N` | press at feed key N |
| `OPENPETE_BUGREPORT_NOTE` | `0`: never ask for the note; `1`: ask on an `OPENPETE_BUGREPORT_AT` press too. Unset: ask on a key or menu press only |
| `OPENPETE_BUGREC_ANCHOR_AT=K1,K2,...` | take an extra anchor at each feed key, beside the 30 s cadence |
| `OPENPETE_BUGREC_DUMP_AT=N`, `OPENPETE_BUGREC_DUMP_DIR=D` | at feed key N, write the recorder's contents to D: `anchor-<i>.state`, `.card<N>.mcr`, `.modsave`, `.seg.bk2` and `.txt` per anchor, `hashes.csv`, `hosttrace.bin`, `pace.bin`, `input-events.csv`, `devices.txt`, `recorder.txt`. The first without the second logs an error and dumps nothing |
| `OPENPETE_BUGREPORT_THUMBS=0` | turn the thumbnail ring off (default `1`): 20 thumbnails in a 320x180 box, one per 250 ms of present clock |
| `OPENPETE_BUGREPORT_REPLAY_DIR`, `OPENPETE_BUGREPORT_REPLAY_SITE_WAIT` | the replay's scratch directory and park-wait cap (above) |

A replay drops `OPENPETE_BUGREPORT_AT`, `OPENPETE_BUGREC_ANCHOR_AT`,
`OPENPETE_BUGREC_DUMP_AT` and `OPENPETE_BUGREC_DUMP_DIR` from the
tester's environment.

Feed keys count yields on Spyro the Dragon and VBLs on Spyro 2 and 3, as
`--frame-limit` does ([Unattended runs](unattended-runs.md)).
