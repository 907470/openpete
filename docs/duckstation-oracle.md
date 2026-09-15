# DuckStation as the openpete PSX oracle

The oracle is the gate that decides whether a change lands: a
DuckStation capture of Spyro 1 running on a real Sony SCPH1001 BIOS is the
project's proxy for hardware, and openpete must reproduce its
gameplay-state hashes tick for tick. This document covers both halves — the
capture side (the DuckStation fork) and the verify side
(`scripts/check_psx_oracle.sh`).

## Why DuckStation

DuckStation replaced PCSX-Redux as the capture emulator for two reasons.

1. Game compatibility. PCSX-Redux's GPU emulation stops on Spyro 1's
   cyclorama renderer (`func_8004EBA8`, about 120 VBLs in): the GPU
   thread stalls on a `DR_ENV` linked-list packet it cannot decode, VSync
   IRQs stop, and the capture wedges. DuckStation runs the same code
   headless at several thousand VBLs per second.
2. BIOS. DuckStation runs Sony SCPH1001; the PCSX-Redux setup ran OpenBIOS,
   an open-source reimplementation with its own bug-compatibility
   differences. The north star is byte-identity with hardware, so
   the retail BIOS is the correct reference.

## The fork

DuckStation is a submodule at `third_party/duckstation`, url
`https://github.com/tyscorp/duckstation.git`, branch `openpete-psxtrace`,
pinned in `.gitmodules`. The branch carries two commits over upstream
`stenzek/duckstation`:

1. Include the EGL context header unconditionally. Upstream includes it
   only under X11 or Wayland, but the EGL context creation is called
   unconditionally on Linux, which breaks a surfaceless headless build.
2. Add the psxtrace capture module to `duckstation-regtest`:
   `src/duckstation-regtest/psxtrace_capture.{cpp,h}` plus five CLI flags
   — `-psxtrace PATH`, `-psxtrace-anchor-pc HEX`
   (default `0x8001ED5C`, Spyro 1 GamestateDraw), `-psxtrace-anchor-hit N`
   (default 1), `-psxtrace-frames N` (0 = bound by `-frames`), and
   `-dump-ram-at-anchor PATH` (2 MB main-RAM dump on the first armed
   hit).

`duckstation-regtest` is upstream's headless regression-test target. Its
default settings are already what the oracle needs: software GPU renderer,
no audio output, and a non-persistent memory card.

### Capture mechanism

The capture module sets an execute breakpoint at the anchor PC.
Each hit reads main RAM for every region
— including the indirect-pointer dereference and per-element masking that
openpete's `gameplay_hash_compute_ex()` (`core/runtime/run.c:529`) performs
— hashes it with FNV-1a-64, and appends one `FRMR` record to the v2
`.psxtrace` file. There is no interpreter slowdown and no scripting layer.

The format constants in `psxtrace_capture.cpp` mirror
`tools/psxtrace_format.py`, and the hash mirrors
`core/include/openpete_fnv1a64.h`; a drift in either produces a golden
openpete can never match.

### Region table

The module carries no region table. `-psxtrace-regions PATH` names a
line-per-region text file that `tools/gen_state_regions.py --out-flat`
writes from the game's manifest (kind, start, length, aux, element size,
per-element flag, mask offsets, name; header line
`psxtrace-regions <version> <fields>`), and the module refuses an
unreadable, empty or malformed file, another format version or field
count, a name over 63 bytes, or more than 128 rows. The same generator
run writes the C header openpete compiles, so both sides hash one
reduction of the manifest. The capture script generates the file into
scratch immediately before each capture.

Two hashes travel in the trace header (flag bits 2 and 3, each a block
after the names blob): the FNV-1a-64 of the regions file, which the
generator also emits as `OPENPETE_STATE_REGIONS_TABLE_HASH`, and the git
hash of the capturing DuckStation build. `openpete_psxtrace_open` refuses
a golden with no table hash, one whose table hash differs from the
registered table ("manifest differs from capture": an edited
start/length/mask under a stable name would otherwise fail at record 0
in that row, indistinguishable from a game divergence), and one whose
region names or order differ from the registered table.

The `cd_load_transient` and `gameplay_neutral` flags are openpete-side gate
policy, not inputs to the hash; they are not in the file.

### VSync and srand recorders

`-psxtrace-vsync-pc HEX` (libetc `VSync`, `psyq.toml`) writes
`<trace>.vsync`: one row per waited call (`mode >= 0`) with the VBLANK
count at entry and return and the CPU ticks, followed by the first query
after it with the counter value it returned; queries a game spins on are
not logged. `-psxtrace-srand-pc HEX` writes `<trace>.srand`: every seed
passed to `srand` with its record and VBLANK. Both are stamped with the
record index (records written when the call was made).
`-psxtrace-vsync-query-skip LO:HI` leaves out the queries returning into
`[LO, HI)`: a library's own timeout polls (libcd's `CD_sync`..`CD_flush`
spin on the counter thousands of times per record), which openpete's
thunks never make; the trailer counts them as `query_skipped`.

### Controller, drive and kernel recorders

`-psxtrace-pad-cmds` writes `<trace>.padcmd`: every command byte the
controllers accept, with record,
VBLANK and port; libpad's command sequences (actuator alignment
`43 4D 43 43`, main-mode change `43 44 4D 43 43`) and the vsyncs they
span are read off it. `-psxtrace-cdsync-pc HEX` (libcd `CdSync`) writes
`<trace>.cdsync`: every call with mode, return value, call site and clock,
runs of equal calls as one row with a count. `-psxtrace-cd-irq` writes
`<trace>.cdirq` (controller commands, interrupts with their record, and
the game's own register reads above `-psxtrace-cd-read-pc-min`).
`-psxtrace-sio0-word HEX` writes `<trace>.sio0`: the vblanks the kernel's
SIO0 (controller and memory card) handler served, from a write breakpoint
on its port-flip word (`0x80007264` on the SCPH-1001 kernel), run-length
encoded; a vblank whose interrupt an earlier handler took, and the card
driver's abort vblanks, are absent. `-psxtrace-watch-word HEX` rows
(`psxtrace-watch 2`) end with the clock, so a store's place among the
other recorded events of its record is readable.

The clock in every recorder is DuckStation's global tick counter, which
advances per CPU time slice: events inside one slice share a value, so an
equal clock orders nothing and a lower one is earlier.

## The reference's schedule

A golden verified with `.vsync`, `.loads` and `.srand` siblings beside it
runs openpete under the reference's timing (`core/runtime/psxtrace_schedule.c`,
consumed by the VSync, CdRead and srand thunks). The schedule tracks the
reference VBLANK count openpete has reached (`pos`); each event charges
`target - pos` VBLs as calls of the VBL body (counter, SPU envelope, yield,
VSync callback, pad pump, deferred read completion), one per VBL, as the
interrupt fired on the reference:

- A waited `VSync` charges to its row's return VBLANK. A wait the
  reference returned from at once charges nothing. A call with no row
  left in the current record (the reference's frame ran long enough that
  its wait loop never waited) charges to the record's anchor stamp and
  consumes no row.
- A `CdRead` with a completion in `.loads` completed before the next
  anchor hit charges to the completion VBLANK inside the read; one that
  completed later defers its completion by the difference; one with no
  completion callback (Spyro 1) charges to the reference's next event
  (the next read's issue, the next wait's entry or the next anchor stamp).
- `VSync(-1)` returns the reference's counter: the position minus the
  offset each waited row's following query re-derives.
- `srand` receives the reference's seed in order.
- An interrupt of the reference (a drive response, a data sector, a
  read's completion) recorded in record R at VBLANK N happened between
  anchor hits R and R+1, so it is delivered once the current record is
  past R, or is R and the position has reached N; at the anchor hook of
  hit H (the hash of record H-1) only records before H count. A data
  read's sectors land one by one at their data-ready interrupts, the
  destination keeping its previous bytes until then. A read's completion
  callback also carries its clock: it is delivered before the first
  waited VSync or CdSync row of its record whose clock is not lower, and,
  when the game's witness site (`psyq_game_cd_done_witness_ra`, a poll
  it makes only after seeing the callback) has a row in that record at or
  after the callback's clock, right after the call preceding it.
- `CdSync` polls answer as the reference's poll at the same site and mode
  did, the VBLs to that poll's VBLANK passing first; the kernel's card
  driver steps on the vblanks `.sio0` marks served and libpad's command
  sequence on the vblanks `.padcmd` holds a command for.
- A VBL the reference spent inside a tick's work (a long frame) is charged
  by the next wait, so openpete reaches that record one VBL behind and the
  next one ahead; the verifier's exact VBL policy admits a deficit that
  opens and closes within two records and fails one that grows negative
  or persists. The summary counts the long frames.

A row whose shape differs from what openpete issues (mode, a read's
sectors or destination), rows exhausted before the trace's last record,
openpete ahead of a stamp, or a `.loads` read count short of the sidecar's
is a schedule failure: `hard fail: schedule` with the record. The gate
script refuses a run whose consumption summary is missing or failed.
`OPENPETE_SCHED_LOG=path` writes every consumed row with openpete's hit.

Controls: a waited row's return VBLANK moved by 5 fails at that record
(`vbl-delta`); a row deleted is absorbed (the stamps carry the record's
total), so a missing row is not a detectable fault and the record count
check is the engagement. Two captures are `cmp`-identical including every
sidecar.

What the schedule does not reproduce: the reference's time-sliced level
loader (Spyro 1 decompresses until two VBLs have passed, so its
intermediate state per tick is CPU time), which the `cd_load_transient`
rows of the main image carry; and where inside a tick's work a long
frame's interrupt landed, which leaves the VBL-derived counters one VBL
behind on those records.

### GTE instruction log

`-psxtrace-gte-log PATH -psxtrace-gte-vbl LO:HI` (with `-cpu Interpreter`;
the log records only under the interpreter) writes one CSV row per
GTE instruction the reference executes while its VBLANK count is in
`[LO, HI)`, in the layout openpete's `OPENPETE_GTE_TRACE` writes (engine
`ds`, last column the guest PC). openpete's trace for the same tick
(`OPENPETE_GTE_TRACE_LO/HI` in `openpete_frame`, which is the VBL count
minus the fast-boot anchor the log names) diffs row for row against it;
a record's `pad2` stamp gives the reference window for a hit. A
difference in an instruction's inputs at the same PC is upstream of the
GTE; a difference in its outputs on equal inputs is a GTE defect.

### Fork provenance

A golden is reproducible only from the fork commit the submodule pins.
`scripts/capture_psx_oracle_ds.sh` refuses when the binary's build stamp
differs from the fork checkout's HEAD, when that HEAD is not the staged
submodule pointer, or when the fork worktree is dirty (the stamp cannot
see dirtiness: `git describe --dirty` finds no tag in the fork). The gate
refuses a golden whose build hash differs from the pinned commit unless
`OPENPETE_ORACLE_ALLOW_FORK_DRIFT=<reason>` names why.

## Setup

```sh
scripts/setup_duckstation.sh
```

Idempotent, and every step is skipped when already complete. It initialises
the submodule, downloads the upstream prebuilt-deps tarball
(`deps-linux-x64.tar.xz`, release pinned in the script, SHA256 verified),
copies `.tools/pcsx-redux/scph1001.bin` to
`~/.local/share/duckstation/bios/`, and configures plus builds
`duckstation-regtest` into `third_party/duckstation/build-regtest`
(OpenGL on, Vulkan/Wayland/X11 off, Release, Ninja).

DuckStation requires clang-19 system-wide; the script pins `CC=clang-19
CXX=clang++-19`. Ubuntu 22.04 ships clang-14, so clang-19 comes from
`apt.llvm.org`.

## Capturing a golden

A golden is not committed. It is a function of the disc, the pinned fork
commit, the region manifest and the recipe, and two captures from the
same inputs are byte-identical (trace and every sidecar), so a checkout
generates its own: `tests/golden/*/psxtrace-*` is ignored by git, and the
gate captures a missing golden before verifying against it. Each game's
recipe is `games/<game>/oracle.conf` (sourced as shell: golden path,
record count, disc, manifest, anchor, the recorder options, and the
openpete flags the gate run adds):

```sh
scripts/capture_psx_oracle_ds.sh --game spyro-1
scripts/capture_psx_oracle_ds.sh --game spyro-2
scripts/capture_psx_oracle_ds.sh --game spyro-3
```

Options on the command line override the recipe's. Spyro 2 and 3 anchor
at the first call of each main-loop iteration, record every sidecar
(Spyro 2 without the raw-register floor: its own code never touches the
controller's registers), skip libcd's own `VSync(-1)` polls, and boot
with `--no-card` (Spyro 3's title card probe answers differ with a card
present; openpete's Spyro 2 stalls at its first card wait); every PC comes
from the game's `psyq.toml`. Without
`--game`, `--out`, `--disc` and `--manifest` (or `OPENPETE_DISC`,
`OPENPETE_REGIONS_MANIFEST`) name the inputs directly.

`--frames` is the number of records (anchor hits) to capture. DuckStation's
own `-frames` counts presentation frames, so the script passes
`1000 + 4 × --frames` as the runtime cap: the pre-title splash logos run
four VBLs per tick, while the attract demo itself is 1:1.
`--anchor-pc` and `--hit` override the anchor; `--max-runtime-frames`
overrides the cap.

The script checks fork provenance, generates the region file, runs the
capture with software GL (`LIBGL_ALWAYS_SOFTWARE=1`,
`MESA_LOADER_DRIVER_OVERRIDE=llvmpipe`), asserts the trace exists and
carries the build hash, deletes a RAM-carrying trace under `tests/golden/`,
and prints `tools.psxtrace_format info` on it.

Re-blessing a golden (deleting it so the gate recaptures it under a new
fork pin, manifest or recipe) is only legal for an intentional
gameplay-behaviour change, after reviewing the diff. A golden is
never blind-overwritten to make a red gate go green.

## The gate

```sh
scripts/check_psx_oracle.sh [--game SLUG] [trace-path] [-- <extra openpete flags>]
```

```sh
scripts/check_psx_oracle.sh --game spyro-3
```

`--game` (default `spyro-1`) sources `games/SLUG/oracle.conf` for the
golden path, the disc (`OPENPETE_DISC` overrides), the binary
(`build/linux-release/games/SLUG/openpete-<slug without the dash>`,
`OPENPETE_BIN` overrides) and the flags the run adds before the caller's
(Spyro 3: `--no-card`). A missing default golden is captured first, which
takes about fifteen minutes for 20000 records; the script exits 64 when
the fork binary is not built. An explicit trace path is never captured.
`OPENPETE_ORACLE_WINDOWED=1` is the windowed row (ctest
`psx_oracle_windowed`, Spyro 1): the same golden verified with the native
renderer drawing into a real window under llvmpipe (`VK_ICD_FILENAMES`
set to the lvp ICD), the eager present thread active. Headless never
starts the present thread, native extraction's live path or eager
production, so a write that path could make into a hashed row is
reachable only here. The run paces to the display, so the row carries
`OPENPETE_FRAME_LIMIT=6000` (about 3000 records, the first level load
included, ~95 s); it fails as VACUOUS unless the log carries
`present-thread: started` and a nonzero `scene(s) presented`, and SKIPs
without a display or the lvp ICD. Its controls: `OPENPETE_EAGER=0` trips
the vacuity check; `OPENPETE_PSXTRACE_POKE` fails at the poked record.

`scripts/check_psx_oracle_all.sh [-- flags]` runs every game's gate
concurrently and prints one table (game, verdict, records consumed, first
divergence outside the neutral rows); exit 2 when any game fails, 64 when
none ran. The music-routing bounds (`MODE_RT_CLEARED_MIN`,
`READS_RT_CLEAR_MAX`) come from the recipe and apply to a run that reached
the trace's end; a recipe lacking them fails with the measured values.
Each golden holds 20000 records over the manifest's rows (Spyro 1: 54),
covering four-plus level loads and gameplay, and replays in roughly two
and a half minutes.

The golden must hold `OPENPETE_ORACLE_RECORDS` records (default 20000)
and a PASS requires the run to have consumed every one; a capture cut
short by `-frames` is self-consistent (the module patches the header to
the records written) and is refused. `OPENPETE_PSXTRACE_HEAL_WINDOW` and
`OPENPETE_PSXTRACE_VBL_POLICY` in the caller's environment are refused
unless `OPENPETE_ORACLE_ALLOW_KNOBS=<reason>` is set, since either changes
the verdict.

The script runs openpete `--headless --no-config
--mute 1 --psxtrace-verify <trace> --frame-limit <N>`. `--no-config` keeps
the verdict independent of any `openpete.toml` beside the exe or in the
runner's home directory. `--mute 1` sits before the pass-through flags so a
forwarded `-- --mute 0` still wins. `OPENPETE_VIBRATION=0` is exported for
the run (fenced by `scripts/fences/rumble_guard.py`).

Anything after `--` is forwarded verbatim to openpete. This is how a
feature with a game-side producer is proven gameplay-neutral: run openpete
with the feature flag against the stock golden, and a PASS means the flag
perturbed no gameplay-state region relative to hardware.

```sh
scripts/check_psx_oracle.sh -- --no-lod
scripts/check_psx_oracle.sh -- --renderer psycross
scripts/check_psx_oracle.sh custom.bin -- --no-lod
```

A native-renderer display flag (`--aspect`, `--fov`, `--ssaa`) has no
game-side producer: the headless run hashes the stock gameplay stream
whatever its value and proves nothing about the flag. That class belongs
to the windowed oracle row.

A flag that takes a value needs the `--` separator, otherwise its value is
read as the trace path.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | every non-tolerated region matched on every record |
| 2 | divergence — `OPENPETE_EXIT_DIVERGENCE` (`core/include/openpete.h:22`) |
| 64 | trace, binary, or disc missing; ctest `SKIP_RETURN_CODE` |
| other | build or runtime error |

The ctest name is `psx_oracle_match`, labels `slow;oracle`, timeout 600 s
(`CMakeLists.txt`). On divergence openpete names the first divergent
record and region, and logs a per-region divergence count over all records
compared, so a stable region (persistent state difference) is
distinguishable from a flickering one.

### Environment knobs

- `OPENPETE_BIN`, `OPENPETE_DISC` — binary and disc overrides.
- `OPENPETE_FRAME_LIMIT` — passed as `--frame-limit`; default 200000. This
  is a safety belt only, since `psxtrace-eof` ends the run first: the
  20k-record demo reaches EOF at about 37000 openpete frames (VBLs, roughly
  two per tick). Setting `OPENPETE_FRAME_LIMIT=8000` cuts the run to a
  roughly 20-second smoke pass covering about 4000 ticks, which reaches no
  level load and therefore clears nothing.
- `PSXTRACE_KEEP_GOING=1` — passes `--psxtrace-keep-going`, comparing every
  record instead of stopping at the first divergence. Used to census which
  regions diverge and for how long.
- `OPENPETE_PSXTRACE_HEAL_WINDOW` — CD-load transient tolerance in ticks;
  see below.
- `OPENPETE_PSXTRACE_POKE=ADDR:BYTE:FRAME` — negative control. The verifier
  writes one byte into guest RAM at the anchor of that frame, before the
  hashes are taken. A run under it must fail at that frame in the region
  holding the address; a PASS means the gate is not looking at that byte.
  For example `OPENPETE_PSXTRACE_POKE=0x8007582C:7:2000` with
  `OPENPETE_FRAME_LIMIT=8000` fails at tick 2000 in `g_SpyroLifeCount`.
- `OPENPETE_ORACLE_LOG` — keep the run's full console log at this path
  (default: a temp file the script deletes). The script reads the exit-time
  `music: summary:` line from it.
- `OPENPETE_PSXTRACE_VBL_POLICY=exact|log` — how a per-record VBL-delta
  mismatch is judged (default `log`). Every record carries the reference's
  VBLANK count at the hit (header flag bit 4, in `pad2`); openpete stamps
  its own `openpete_vbl` at the same hit, and the verifier compares the
  deltas between consecutive records (absolutes differ by the boot
  offset). `exact` hard-fails the first mismatch (reason `vbl-delta`,
  both deltas named); `log` counts them. The exit summary prints both
  sides' delta histograms (bin `>=16` collects the rest). `exact` refuses
  a trace without stamps.
- `OPENPETE_ANCHOR_VBL_LOG=PATH` — one `hit vbl` line per anchor hit,
  with or without a trace (`--psxtrace-anchor-pc HEX` arms the counter
  alone). `tools/psxtrace_loads.py vbl TRACE PATH` prints both histograms
  and the first mismatching record.

### Sidecar artifacts

If `<trace>.seed.ram`, `<trace>.seed.cpu`, or `<trace>.bk2` exist beside
the trace, the script passes them as `--seed-ram`, `--seed-cpu`, and
`--bk2`, aligning openpete's start state and controller input to the
oracle's. Those artifacts come from the PCSX-Redux capture path
(`scripts/capture_psx_oracle.sh`, a two-stage Lua capture). The shipped
DuckStation golden has none: it is captured from power-on and openpete
replays the same boot.

### CD read recorder

`capture_psx_oracle_ds.sh --cd-read-pc HEX --cd-read-cb-slot HEX` (the
game's `CdRead` entry from `psyq.toml`, and `recomp.conf`'s
`CD_READ_CB_ADDR`, the guest word libcd's `CdReadCallback` stores the
game's handler in) writes `<trace>.loads`: one line per read with the
record index, VBLANK count, CPU tick and scanline at issue and at
completion, the sector count, destination and mode, and the callback's
intr code. The handler is read from the slot at each issue and
breakpointed, so the handler PC need not be known up front; the header
line names the build. A read the game waits on without reaching the
anchor between issue and completion shows `issue_rec == done_rec`
(synchronous); one polled across hits does not (asynchronous). The script
refuses a capture that recorded zero reads. `--cd-control-pc HEX` logs
every `CdControl` command to `<trace>.cdctl`. openpete's side is its
console log: `CdRead ... hit=H vbl=V`, `read instant`/`load delay: ...
deferred`, `read complete (hit=H vbl=V)`.

`tools/psxtrace_loads.py table LOADS` classifies every reference read;
`align LOADS OPLOG` pairs them with openpete's by index, checks the
(sectors, destination) sequence and prints the issue-hit shift and both
sides' hit and VBL spans per read; `cdctl CDCTL` histograms the commands.

## Checking the gate is not vacuous

The verifier counts, per region, the records on which the oracle hash
differed from the previous record's, and the exit summary names every
region at zero: `zero-change regions justified (constant_ok):` for rows
whose manifest entry carries `constant_ok = true` with its reason (a
progression total the attract demo never advances), and `zero-change
regions unjustified:` for the rest. On a run that reaches EOF the gate
script fails a PASS whose unjustified list is not `none`.

Three further checks, each cheap, establish that a PASS is a comparison and not an
accident. The trace's table hash equals the registered table's (the
verifier refuses otherwise). A fresh 5-record capture at the default anchor must verify PASS,
and one captured with `-psxtrace-anchor-hit 2` must fail at record 0 (one
tick of offset changes every live region's hash). A poke run
(`OPENPETE_PSXTRACE_POKE`) must fail at the poked tick in the poked
region. The golden's first records are byte-equal to a fresh capture, and
the 2 MB dump `-dump-ram-at-anchor` writes at the armed hit hashes to the
values the same hit's record holds, so the dump and the trace describe one
instant.

## Whole-RAM diff at an anchor

`scripts/check_ram_diff.sh <game> [--anchor first|loop|HEX] [--hit N]
[--ref-hit M] [--keep DIR]` compares the reference's 2 MB RAM with
openpete's at a translated function's entry: the module runs with
`-dump-ram-at-anchor` (and `-card1 none` when the game's expectation file
says so), openpete with `--halt-at-pc`/`--halt-hit`, and
`tools/ram_diff.py` sorts every differing range into a class from
`games/<game>/manifest/ram_diff_expect.toml`, in this order: the exact
words openpete's PSYQ mirrors write (`populated`; a difference here is a
defective mirror and fails), the kernel area, the PSYQ library's own
section extents from the decomp link map (`psyq_internal`, ratcheted per
anchor against the counts recorded in the file), the BIOS's stack frames,
data openpete landed synchronously that hardware still had in flight
(computed from the CdRead lines in openpete's log; any such byte means the
anchor sits inside a load and the run does not pass), and named ranges
with a reason and an owner. Anything else is unexpected and fails; the
union of expected differing bytes is capped at 25% of RAM. The anchor must
be a dispatch-table entry whose entry block is not a loop head, since the
module counts executions of the instruction and openpete counts calls.
Each game's file names a `first` anchor, the entry of the function issuing
its first CdRead, where no read has been issued on either side; Spyro 1
also names its `loop` anchor, GamestateDraw, where hit N is the same
manifest state on both sides by the oracle and the whole-RAM residue is
what the tool reports. A range leaves the expectation file only by a fix.

## CD-load transients

Rows in `games/<game>/manifest/gameplay_state_regions.toml` carrying
`cd_load_transient = true` are of two kinds. The pool rows
(`g_LevelMobys_pool`, `g_DynMobys_pool`, `g_Sparx_moby` on Spyro 1) are
CdRead destinations refreshed in place. The main-image rows (load stage
and wait counters, the SPU and model tables, CD bookkeeping, the XA
stream request, environment and cyclorama pointers, the title sequence,
`g_DeltaTime`, the `CdLastCom` mirror byte) hold the state the load code
fills or advances as sectors arrive. openpete lands a read instantly
while the reference lands it over the load's VBLs, so both kinds differ
for the duration of a level or streaming load and re-converge at its
end. The Spyro 1 manifest's head comment lists the rows; the
openpete-per-game-oracle plan (P6) charges the reference's VBLs on
openpete's side and removes the main-image flags.

The verifier (`core/runtime/psxtrace_verify.c`) tracks each transient
region's divergence burst independently. A burst that re-converges is
absorbed and logged. A burst still open after `heal_window` ticks, or still
open when the trace is fully consumed (`openpete_psxtrace_finalize`), is a
hard failure. `heal_window` defaults to 600 ticks; the largest legitimate
load burst measured in the 20k demo is about 310 ticks.
`OPENPETE_PSXTRACE_HEAL_WINDOW` overrides it.

Any region without the flag is a hard failure on its first differing
record.

The Spyro 1 table is derived from the link map
(`tools/derive_state_regions.py`: the main image's sections minus the
`psyq_internal` extents and the `[[expect]]` ranges of
`ram_diff_expect.toml`, named rows carved out, the rest coalesced), and
`tools/check_regions_containment.py OLD.flat NEW.flat` proves a
replacement table hashes every byte the previous one hashed. A residual
inside a row becomes a `mask_offsets` entry with its owner in
`mask_reason`, never a dropped row.

Every tolerated row (`cd_load_transient`, `gameplay_neutral` or
`host_precision`) carries `diverge_baseline = N`, the records it diverged
on over the golden it was measured against; a run that reaches the
trace's end with a higher count fails (`hard fail: ratchet`). The
generator refuses the field on a strict row. `host_precision` marks a
row holding the state of a host device model coarser than hardware (the
SPU envelope at whole-VBL granularity: Spyro 2 `spu_voice_table`, Spyro 3
`g_ActiveSounds`); it is tolerated like `gameplay_neutral` and is a named
residual, not an audited neutral. The verdict line counts the two
tolerance classes apart: `CD-load transients absorbed=` is the
`cd_load_transient` bursts that healed inside the window, `neutral
bursts=` the bursts on neutral and host-precision rows. The summary names
the first divergence in a row that is neither (`first_non_neutral_div=`)
and, on a FAIL, the row and reason (`non-transient`, `unhealed`,
`unhealed-eof`, `ratchet`).

Each manifest carries a `[derivation]` table (the `--named`, `--overlay`
and `min_row` arguments its direct rows were derived with);
`tools/derive_state_regions.py --check MANIFEST` re-derives against the
current `ram_diff_expect.toml` and fails on any extent difference or
dropped fragment (ctest `state_regions_derivation_<game>`, labels
`fence;oracle`), so an expect row deleted by a fix re-derives the manifest
in the same change.

`g_Particles_pool` carries `gameplay_neutral = true` instead: its
divergence is logged and never fails, on the strength of a 20k keep-going
32:9 run in which it was the sole diverging region while `psyq_rand_seed`,
`g_Spyro`, `g_Camera`, `g_Sparx`, and `g_MobyAllocPtr` stayed bit-identical
to hardware for all 20000 ticks. Those five regions stay tracked as the
tripwire for that audit.

## Known artefacts

- Mesa GL TLS-destructor SIGABRT on shutdown. On WSL2 with llvmpipe,
  DuckStation's software-renderer path aborts inside `swrast_dri.so` during
  pthread thread-local-storage cleanup, after the regtest has logged
  `Exiting with success.` and the trace file has been finalised.
  `capture_psx_oracle_ds.sh` runs the capture under a wrapper `sh` that
  swallows the abort, and sets `ulimit -c 0`. It does not reproduce on
  systems with a hardware GL stack.
- Screenshots are black in the headless software path. PSX render
  output reaches VRAM, but the GL present step on llvmpipe does not capture
  it. The oracle hashes RAM, not VRAM, so this does not affect it; it would
  need fixing before regtest could drive visual regression tests.

- The reference frame is 83 CPU ticks longer than the hardware frame.
  The reference counts 3413 GPU ticks per NTSC line (measured);
  the hardware line is 227.5
  colour-subcarrier cycles, 3412.5 GPU ticks, which is what
  `core/include/openpete_time.h` and encode (2250000/37609 Hz). The
  reference therefore runs 59.81734 Hz against 59.82611 Hz, 0.0147 % slow,
  one frame of wall-clock drift every 113 s. Guest state is unaffected: the
  guest counts VBLs, guest-timed models count VBLs, and the gate samples at
  anchor PCs. A model that converts cycles to VBL fractions against a
  DuckStation golden uses the reference's 566,204 CPU ticks per VBL; against
  hardware, 566,121.

## Related

- `notes/plans/openpete-psx-oracle-ci.md` — gate design, the transient
  policy, and the CI wiring.
- `games/spyro-1/manifest/gameplay_state_regions.toml` — the region contract itself, with
  the address source and audit note for every region.
