# Golden fleet

`scripts/golden-fleet.py` runs the presentation golden
(`tests/harness/check_presentation_golden.sh`) on a set of hosts reached
over ssh. The work is split into units, one per (game, tier) pair: the
presentation tier of spyro-1, spyro-2 and spyro-3, and the dragons tier of
spyro-1. Each unit is captured on one host inside the pinned capture image
(`tests/harness/linux-capture.Dockerfile`, run through
`scripts/golden-capture-container.sh`) and decided on that host by the same
harness with `--verdict-only`, so a fleet verdict is the harness's verdict
over the same image, binaries and disc bytes. A run never blesses and never
promotes.

## Host file

Path: `$OPENPETE_FLEET_HOSTS` when set, else
`${XDG_CONFIG_HOME:-~/.config}/openpete/fleet-hosts.toml`.
The main checkout is resolved through `git rev-parse --git-common-dir`, so
every worktree reads the same file. The file is gitignored and belongs to
one machine: it names that machine's hosts and their measurements.

One table per host:

```toml
[hosts.local]
transport = "local"
fleet_root = "~/.cache/openpete-fleet"
slots = 4
speed = 1.00
calib_s = 270
calibrated = "2026-01-01 governor=performance cpu=AMD Ryzen 9 7950X"
enabled = true
default = true
build = true
nice = 19

[hosts.box2]
transport = "ssh"
ssh = "user@box2"
engine = "podman"
slots = 2
speed = 0.45
calib_s = 600
calibrated = "2026-01-01 governor=performance cpu=Intel Core i5-8400"
enabled = true
default = true
build = true
```

| Key | Type | Meaning |
|---|---|---|
| `transport` | string | Required. `"local"` is the coordinator's own machine (at most one host); `"ssh"` a remote host. |
| `ssh` | string | `user@host`. Required for `"ssh"`, refused for `"local"`. |
| `fleet_root` | string | Directory holding every fleet file on the host; `~` resolves on the host. Default `~/.cache/openpete-fleet`. |
| `engine` | string | `"podman"` or `"docker"`. Default: the first of the two found on the host. |
| `slots` | integer | `CORPUS_JOBS` for a unit on the host (at least 1). Written by `add`. |
| `speed` | float | Throughput relative to the local host (local is 1.00). Written by `add` and `calibrate`. Scales leg timeouts, deadlines and placement. |
| `calib_s` | integer | Wall time in seconds of the calibration unit at `slots`. Written by `add` and `calibrate`; the local host's is `t_local` for every remote host's `speed`. |
| `calibrated` | string | `YYYY-MM-DD governor=G cpu=MODEL`, written by `add` and `calibrate`. A host without `governor=` here, or whose current governor differs, is dropped by preflight. |
| `enabled` | boolean | `false`: the host is never used. Default `true`. |
| `default` | boolean | `true`: the host joins every run that names no `--hosts`. Default `true`. |
| `build` | boolean | The host may build the engine for a run. Default `true`. |
| `nice` | integer | CPU priority of units on the host. Default 19 on the local host, 0 on a remote host when absent; `add local` writes 19. |

Only `add` and `calibrate` write the file. Each replaces the named host's
table and keeps every other line. The parser accepts strings, integers,
floats and booleans; any other table, key or type exits 1 naming the line
or key.

`slots`, `speed` and `calib_s` are measured, not chosen. `add` measures a
host with the machinery of `run` (preflight without the calibration
requirement, the host's `clean`, image shipping, the build on the local
host, the positive sync set into `runs/<run-id>/tree` and a detached
supervisor per leg under the host golden lock). Each leg is the spyro-2
presentation unit (capture, then `--verdict-only`) at one slots value, with
`LP_NUM_THREADS = ceil(cores / slots)` and `MemAvailable` sampled once
a second on the host. A leg whose report compares fewer than every blessed
row, or lists a missing tile, ends the `add` with exit 1 after printing the
tails of the leg's logs.

The sweep runs slots 1, 2, 4, ... up to the host's core count (on the local
host, up to `MemAvailable / 2 GiB`) and stops at the first step that
improves wall time by under 10% or whose leg's minimum `MemAvailable` falls
under 2 GiB. `slots` is the smallest sweep value past which wall time
improves by under 10%, lowered while the leg's minimum `MemAvailable` is
under 2 GiB; `calib_s` is the leg's wall time at that value. For a remote
host, `speed` is `t_local / t_host`, where `t_local` is the local host's
`calib_s`. When the local host has none, the same `add` runs a local leg at
the local `slots` beside the remote sweep and writes the local `calib_s`
too. `add local` records speed 1.00. `add` prints its wall time.

A local leg counts only with the local machine idle: the 1-minute load
average at most `max(1, 0.1 x cores)` before the leg (waited for up to
600 s) and, 90 s after it, at most that bound plus a quarter of the load at
the leg's end. A loaded leg is discarded and re-run; the third loaded leg
at one slots value ends the `add` with exit 64.

`--dry-run` preflights, prints the build host and every planned leg, and
writes nothing. `--timings FILE` applies the rules to recorded legs, ships
no image and runs nothing; `"local"` is optional, and without it `t_local`
is the local host's `calib_s`:

```json
{"cores": 8,
 "sweep": [{"slots": 1, "wall_s": 1500, "min_memavail_kib": 9000000}],
 "local": [{"slots": 4, "wall_s": 270, "load_before": 0.4,
            "load_end": 22.0, "load_after": 5.1}]}
```

`calibrate` runs one leg at each host's recorded `slots` (`--hosts`, else
every enabled host; the local one under the idle rule) through the same
machinery, in parallel across hosts, and rewrites `speed`, `calib_s` and
`calibrated`. It records the current governor, so it clears a governor
mismatch that preflight reports. A leg whose minimum `MemAvailable` falls
under 2 GiB prints that `add` re-measures `slots`; `calibrate` never
changes `slots`.

## Commands

Run from the checkout (or a worktree) whose working copy is to be gated.

```sh
# Record the local host (measured sweep, takes the local golden lock).
scripts/golden-fleet.py add local
# Record a remote host (measured sweep on it; a local leg too when the local
# host has no calib_s); --dry-run prints every planned leg and writes nothing.
scripts/golden-fleet.py add box2 --ssh user@box2 --engine podman
# Apply the rules to legs measured elsewhere.
scripts/golden-fleet.py add box2 --ssh user@box2 --timings box2-legs.json
# Re-measure speed at the recorded slots on every enabled host.
scripts/golden-fleet.py calibrate
scripts/golden-fleet.py calibrate --hosts box2
# Reachability, engine, :fleet image Id against the local image, image.id,
# golden lock and its holder, governor, calibration, free space, fleet_root size.
scripts/golden-fleet.py status
scripts/golden-fleet.py status --hosts box2
# Drop reasons per host; exit 0 with a usable host, 64 with none.
scripts/golden-fleet.py preflight --need-gib 20
# Ship the capture image to hosts whose :fleet Id differs.
scripts/golden-fleet.py ship-image --hosts box2
```

`run` gates the working copy. Without `--game` or `--all` the unit is
spyro-1; without `--tier` or `--all-tiers` the tier is `presentation`.

```sh
scripts/golden-fleet.py run --wait                          # spyro-1 presentation
scripts/golden-fleet.py run --game spyro-3 --wait
scripts/golden-fleet.py run --all --all-tiers --wait        # every unit: 4
scripts/golden-fleet.py run --all --hosts local,box2 --wait
scripts/golden-fleet.py run --all --no-local --wait         # keep the dev box free
scripts/golden-fleet.py run --game spyro-2 --pull all --wait
```

| Flag | Meaning |
|---|---|
| `--game G` / `--all` | One game (`spyro-1`, `spyro-2`, `spyro-3`) or every game. |
| `--tier T` / `--all-tiers` | `presentation` or `dragons` (spyro-1 only), or both. |
| `--hosts a,b` | These hosts instead of every enabled host with `default = true`. |
| `--no-local` | Leave the local host out. Exclusive with `--hosts`. |
| `--pull divergent\|all` | `divergent` (default): the report files, divergent tiles and identity pairs; `all`: the whole candidate directory. |
| `--wait` | Follow the detached coordinator and exit with its code. |

`run` detaches the coordinator (`setsid`) and prints its log and done
marker, `<local fleet_root>/runs/<run-id>/coordinator.log` and `done`.
Without `--wait` it exits 0 at once; the run's code is the `rc=N` first
line of `done`.

```sh
scripts/golden-fleet.py clean                     # stale files on every enabled host
scripts/golden-fleet.py clean --hosts box2
scripts/golden-fleet.py clean --tree "$PWD"       # this coordinator's trees of one checkout
scripts/golden-fleet.py clean --all --hosts box2  # every fleet file on box2
```

## What a run does

1. Hosts. The selected hosts are preflighted: a host is dropped when it
   is disabled, unreachable, has no container engine, is uncalibrated,
   runs another governor than the calibrated one, lacks free space (the
   sync set, the build tree and the largest candidate set, plus the image
   when it must ship), or its golden lock is held. The local host's slots
   are lowered to fit 2 GiB of `MemAvailable` per slot. The run refuses
   (64) when the local capture image was built from a recipe other than
   the tree's `tests/harness/linux-capture.Dockerfile`. The coordinator
   takes the local golden lock, runs `clean` on each host and ships the image.
2. Image shipping. The image is identified by its Id under the tag
   `openpete-linux-capture:fleet`. When a host's `:fleet` Id differs from
   the local image's, `<engine> save` is streamed into
   `ssh host <engine> load`, the loaded Id is tagged `:fleet` and
   `<fleet_root>/image.id` records it. No label is added, since a label
   changes the image's Id.
3. Build once. The fastest host with `build = true` builds the engine
   inside the image (`golden-capture-container.sh build`, into
   `build/linux-release-container`): in the checkout when that is the local
   host, else in its snapshot, seeded from its previous tree. The binaries'
   SHA-256 go to `binaries.sha256`; a remote build tree is relayed through
   the coordinator to the other hosts and checked against those hashes.
   Every unit then runs `gate --no-build`, which verifies the binaries with
   `sha256sum -c` before capturing.
4. Sync. The positive set is copied to `<fleet_root>/runs/<run-id>/tree`
   with `rsync --link-dest` from `<fleet_root>/trees/<key>`: git's tracked
   and non-ignored untracked files (submodules included),
   `games/*/generated`, each game's manifest `globals.toml` and
   `symbols.toml`, each run game's disc cue and bin,
   `third_party/psxrecomp/build`, and the build tree minus `home/` and the
   disc-hash memo. While it reads the checkout the coordinator holds
   `build/linux-release-container.build-lock` and runs a
   `golden-fleet-sync` marker process, which marks the checkout as being
   read; do not build while it runs. Each run game's
   capture fields are written with `scripts/presentation-blessed.py
   capture-fields` (a snapshot has no `.git`; the capture record reads them
   through `OPENPETE_CAPTURE_FIELDS`).
5. Torn-sync recheck. Every copy is compared again with
   `rsync -n --checksum` over the same lists; any change refuses the run as
   torn. The synced disc cue and bin are hashed on every host against the
   blessed manifest headers.
6. Placement. Units are placed longest first, one per free host, from the
   unit-time ledger `fleet-unit-times.tsv` beside the reference store (a
   unit without a record is scaled from 60 min for the spyro-1
   presentation unit by candidate size). Each unit runs under a detached
   supervisor that holds the host's golden lock, with `CORPUS_JOBS` =
   slots, `PRESENTATION_LEG_TIMEOUT` = 300 / speed x 1.5 and a deadline of
   expected time / speed x 2. A lost, dead or overdue attempt is killed
   (the supervisor's session by PID and start time, its containers by the
   label `org.openpete.fleet=<run-id>`) and re-queued, at most 2 attempts;
   a host unreachable for 10 min is dropped. A re-queued attempt on the same
   host resumes at row level (Row resume, below).
7. Canary. With two or more capture hosts, every host first captures one
   bk2 and one freecam level of the first unit's game. A canary tile whose
   verdict digest differs between hosts fails the run.
8. Verdict. The capturing host runs the harness's capture, then
   `check_presentation_golden.sh --verdict-only <candidate> --game G` in
   the image. The coordinator prints the report's census,
   `census captured=C imported=I (R rows) compared=K of N`, and the summary
   repeats it beside each unit's verdict.
9. Pull. `verdict-report.txt`, `capture-record.txt`,
   `capture-device.txt`, `credits-verdict.txt` (presentation tier), the
   verdict logs, the divergent tiles and both members of each identity pair
   over baseline (under `identity/`) are pulled into the cleared local
   candidate directory: `build/presentation-candidate`,
   `build/presentation-candidate-<game>`, `build/dragon-candidate`, or
   `$PRESENTATION_GOLDEN_OUT` (with a `<game>-<tier>` subdirectory when the
   run has several units). A unit fails when the report's `blessed.tsv` or
   identity-baseline hash differs from the local file, its `compared`
   census differs from the local row count, or its exit differs from the
   supervisor's.
10. Heat diffs. The pulled divergent tiles are diffed locally against the
    store with `tests/harness/presentation-corpus-diff.py --expect-subset`
    into `<candidate>/diff`.
11. Teardown. `runs/<run-id>/` is deleted on every host on every exit
    path; the local run directory keeps `coordinator.log`, `done` and
    `active.lock`. A host unreachable at teardown keeps its directory for
    `clean`.

## Row resume

A unit's or canary's supervisor sets `OPENPETE_RESUME_RUN=<run-id>`
(passed into the image by `scripts/golden-capture-container.sh`). The
harness (`scripts/corpus-resume.sh`, sourced by
`check_presentation_golden.sh`, `presentation-corpus.sh` and
`freecam-corpus.sh`) then stamps every manifest row it finishes: when the
last of a row's legs (each LOD mode times canon, inter and ident) has
written every tile it was asked for, `cand/resume/<kind>.<key>.stamp` is
written atomically:

```
run: <run id>
tree: <sha256 of the capture-fields file: head, source tree, recomp input, generated tree>
binary: <sha256 of the capture binary>
params: kind=bk2 game=spyro-1 tier=presentation affine=default aspect=16:9 lods=original extended samples=5
row: <the row's manifest line, verbatim>
tile: <sha256> <tile name>          one per tile, identity tiles included
device: <the leg's Vulkan device line>
```

`kind` is `bk2`, `freecam` or `credits`; a passed credits leg is the row
`credits-interp` whose one tile is `credits-verdict.txt`. A row interrupted
mid-capture has no stamp.

A re-queued attempt on the same host runs in the same `u-<job>/` directory.
Before capturing, the harness moves the previous `cand/` aside and imports
a stamp's row only when its run id, tree, binary and params equal the
attempt's, its row line is a line of the current manifest that the run's
filter selects, its key is that row's key, and every listed tile is present
with its sha256. An imported row's tiles move into a fresh `cand/` (its
device lines into `capture-device.txt`); everything else of the previous
attempt is deleted. The capture then skips the imported rows, and the
strict diff covers imported and captured tiles alike.
`cand/resume/imported.tsv` lists the imported rows; the census line
(`captured=C imported=I imported-rows=R compared=K golden=N`) and
`verdict-report.txt` (`captured:`, `imported:`, `imported-rows:`) count
them apart, so an import never stands in for a missing row. A new run id
imports nothing: its run directory is new and every stamp names its run.
A re-queued attempt on another host captures every row. Calibration legs
never resume, since their wall time is the measurement. `--bless` neither
reads nor writes stamps.

`tests/harness/check_corpus_resume.sh` checks the stamp and import rules on
a synthetic candidate directory (no binary or image): a finished, a
partial and a tampered row, another run id, binary, tree, LOD set,
manifest line and filter, an interrupted import, concurrent legs, and
resume off.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | PASS: every unit passed. |
| 1 | FAIL: a unit diverged or failed, a pull or report check refused, the canary differed, or the run failed (torn sync, build, transfer). |
| 64 | SKIP: no usable host, no host file, no local engine or image, a stale image recipe, a missing disc, or a unit whose capture skipped. |
| 2 | Usage error (argparse, `clean --all --tree`, `add <name>` without `--ssh`, a malformed `OPENPETE_FLEET_POKE`). |

A run's code is the worst unit's in the order 1 > 64 > 0: one SKIP unit
makes the run non-PASS. The summary prints each unit's verdict beside its
host, attempt and wall time.

## Files on each host

```
<fleet_root>/
  golden.lock            flock: one presentation golden per host; holds the holder record
  image.id               Id of the :fleet image the fleet shipped
  image.id.history       earlier shipped Ids, removed by clean
  trees/<key>/           link-dest base: the last snapshot of one checkout
  trees/<key>.src        coordinator, checkout path, last-run time
  trees/<key>.lock
  runs/<run-id>/
    coordinator.log      local host only
    done                 local host only: rc=N, run=, reason=
    active.lock          held shared by the coordinator and supervisors
    ship/                supervise.sh, fields-<game>.txt, binaries.sha256
    tree/                the snapshot units run in
    u-<job>/             attempt-N.pid, attempt-N.done, attempt-N.memavail,
                         capture.log, verdict.log, store/, cand/
                         (cand/resume/: row stamps, imported.tsv)
                         (calibration legs: u-calib-<host>-s<slots>-<n>/)
    helpers.tsv          dbus-daemon helpers started during a job
```

`<key>` is the first 16 hex digits of the SHA-256 of the coordinator's
hostname and the checkout's absolute path.

`clean` works per host under its golden lock (a busy host is skipped) and
removes: `runs/` directories no process holds and with no done marker
newer than 24 h (signalling the `dbus-daemon` helpers their supervisors
recorded, when PID and start time still match); exited fleet-labelled
containers; `trees/<key>` this coordinator wrote whose checkout path is
gone or unused for 14 days; shipped images other than the current `:fleet`
one and named by no other tag. `--tree PATH` removes only this
coordinator's trees of PATH. `--all` removes `runs/`, `trees/`, the lock
and image records, every fleet-labelled container and the `:fleet` tag
(the image itself only when no other tag names it), and `fleet_root` when
nothing else is in it.

## Promote stays local

Pulled tiles are adjudication material. `presentation-corpus-promote.sh`
refuses a candidate whose `capture-record.txt` names another host. Started on
the host, the script enters the capture image itself. Re-capture
each adjudicated row on the dev box and promote from that capture:

```sh
tests/harness/check_presentation_golden.sh --game spyro-2 --filter <bk2>
tests/harness/presentation-corpus-promote.sh build/presentation-candidate-spyro-2 <tile>... --game spyro-2
```

A full re-bless (`--bless`) also runs on the dev box.

## Environment

| Variable | Effect |
|---|---|
| `OPENPETE_FLEET_HOSTS` | Host file path. |
| `OPENPETE_CAPTURE_IMAGE` | Local image the coordinator ships (default `openpete-linux-capture`). |
| `OPENPETE_GOLDEN_REF_DIR`, `XDG_DATA_HOME` | Local reference store; the unit-time ledger lies beside it. |
| `PRESENTATION_GOLDEN_OUT` | Local pull target instead of `build/*-candidate*`. |
| `LP_NUM_THREADS` | Pins llvmpipe threads per capture on every host; default `ceil(cores / slots)` per host. |
| `OPENPETE_FLEET_AFTER_SYNC` | Test only: a shell command run in the checkout between the syncs and the recheck (negative control for the torn-sync recheck). |
| `OPENPETE_FLEET_POKE` | Test only: `<host>:<job\|unit\|canary>:<tile substring>` flips one pixel of the first matching candidate tile on that host before its verdict (negative control for the verdict and canary). |
| `OPENPETE_FLEET_DEADLINE_S` | Test only: seconds; replaces the deadline of every unit's and canary's first attempt, so the attempt is killed and re-queued (row resume check). |
| `OPENPETE_FLEET_CANARY_ONLY` | Test only: `1` runs only the canaries, one per host even on one host, pulls each canary's tiles, report, record, device and credits files into `<candidate directory>/canary-<host>`, and exits with the worst canary's code. |

The supervisor sets `CORPUS_JOBS`, `PRESENTATION_LEG_TIMEOUT`,
`PRESENTATION_TIER`, `OPENPETE_FLEET_ROOT`, `OPENPETE_GOLDEN_LOCK_HELD`,
`OPENPETE_CAPTURE_LABEL`, `OPENPETE_CAPTURE_MOUNT_AS`,
`OPENPETE_CAPTURE_FIELDS`, `OPENPETE_CAPTURE_BINARIES` and
`OPENPETE_RESUME_RUN` itself; a value
set by hand does not reach a unit.

## Troubleshooting

| Message | Cause and action |
|---|---|
| `SKIP: no host file ...` | Create it with `scripts/golden-fleet.py add local`. |
| `dropped: not calibrated (no governor= in calibrated); run add` | Run `add` for the host. |
| `dropped: governor X, calibrated at Y` | Restore the governor, or run `calibrate --hosts <host>` to recalibrate under the new one. |
| `dropped: logind kills an ssh session's processes at logout (KillUserProcesses=yes) and lingering is off ...` | Run `loginctl enable-linger` on the host (SteamOS ships `KillUserProcesses=yes`). With lingering on, each supervisor runs as a transient unit of the user's systemd manager (`systemd-run --user`), outside the ssh session; without it, under `setsid`. |
| `dropped: golden lock held: <holder>` | Another golden runs on that host; the holder record names it. |
| `dropped: free space ... need ... (image ships)` | Free space under `fleet_root`, or run `clean`. |
| `local image ... was built from recipe ..., the tree's is ...` | Rebuild it: `scripts/golden-capture-container.sh image`. |
| `no local host with recorded slots in the host file; run add local first` | `add` of a remote host needs the local host's slots. |
| `local: 3 loaded legs at slots N; the machine is not idle` | The local machine was busy through three legs; rerun when it is idle. |
| `leg calib-... compared N of M blessed rows, K missing; not a full unit` | A capture leg timed out or failed; the `add` printed the tails of the leg's `capture.log` and `verdict.log` before deleting the run directory. |
| `torn sync: <host> snapshot differs from ...` | The checkout changed during the sync; rerun when no edit or build is in flight. |
| `synced <file> hashes to ..., blessed header disc-... is ...` | The disc image differs from the one the golden was blessed on. |
| `canary: REFUSED: N of M tile(s) differ between ...` | The hosts render differently; take the named host out (`enabled = false`) until the difference is explained. |
| `REFUSED: blessed-sha256 ... differs from the local blessed.tsv` | The host decided against another manifest; the checkout changed after the sync. |
| `<dir> kept for clean (host unreachable)` | Run `clean --hosts <host>` once it is reachable. |
