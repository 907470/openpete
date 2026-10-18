# DuckStation as the openpete PSX oracle

The oracle is the gate that decides whether a change lands: a
DuckStation capture of Spyro 1 running on a real Sony SCPH1001 BIOS is the
project's proxy for hardware, and openpete must reproduce its
gameplay-state hashes tick for tick. This document covers both halves - the
capture side (DuckStation with the psxtrace patch series) and the verify side
(`tests/harness/check_psx_oracle.sh`).

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

## The patch series

DuckStation is a submodule at `third_party/duckstation` that pins upstream
`https://github.com/stenzek/duckstation.git` at `e4c55ad0283f`. The
project's changes are a patch series in `third_party/duckstation-patches/`
(`git format-patch` files, applied in the order `series` lists), because
DuckStation's licence, CC BY-NC-ND 4.0, forbids distributing a modified
DuckStation, and a published fork is such a distribution. The series:

1. Makes the EGL context available on every Linux build, which a
   surfaceless headless build needs.
2. Adds the psxtrace capture module to `duckstation-regtest` (source files
   the series creates) plus its CLI flags - `-psxtrace PATH`,
   `-psxtrace-anchor-pc HEX`
   (default `0x8001ED5C`, Spyro 1 GamestateDraw), `-psxtrace-anchor-hit N`
   (default 1), `-psxtrace-frames N` (0 = bound by `-frames`), and
   `-dump-ram-at-anchor PATH` (2 MB main-RAM dump on the first armed
   hit) - and, in later patches, the region table file, the recorders
   described below, `-card1 none`, and an `OPENPETE_SPU_DUMP` final-mix
   WAV dump.

`scripts/setup_duckstation.sh` applies the series onto the pin with
`git am` on a detached HEAD. The committer name and email are constants,
each committer date is the patch's author date, and system and global git
configuration, hooks and signing are excluded, so every machine produces
the same commit ids from the same pin and patch files. The resulting head
is recorded in `third_party/duckstation-patches/applied-head.txt`; the
script refuses when the head it produces differs from the record, which
means the pin or a patch changed without the record. DuckStation's build
stamps the checkout's HEAD commit into the binary, so the build hash every
trace header carries is the recorded head. After setup, `git status` in
the openpete checkout lists `third_party/duckstation` as modified (its
HEAD is the applied head, not the upstream pin); that path is never
staged.

openpete text outside the patch series names DuckStation as a tool (its
binary, the flags openpete passes, the recorders the series adds, the data
they record) and never by its internals: no upstream source path or line,
no upstream symbol, no account of how it computes a value. A behaviour of
the reference is stated as a measurement with its instrument.
`scripts/fences/provenance_census.py` refuses a staged line (pre-commit)
or tracked line (the `openpete_provenance_census` build target) carrying a
listed internal name, or an emulator's name beside an upstream source form,
outside its allow-list rows.

`duckstation-regtest` is upstream's headless regression-test target. Its
default settings are already what the oracle needs: software GPU renderer,
no audio output, and a non-persistent memory card.

### Capture mechanism

The capture module sets an execute breakpoint at the anchor PC. Each hit
reads main RAM for every region - including the indirect-pointer
dereference and per-element masking that openpete's
`gameplay_hash_compute_ex()` (`core/runtime/run.c:529`) performs - hashes
it with FNV-1a-64, and appends one `FRMR` record to the v2
`.psxtrace` file. There is no scripting layer. While a breakpoint is armed
the CPU runs on the interpreter for the whole capture, which the GTE hash
and VBLANK position recorders rely on; the capture script refuses a run in
which either recorded nothing.

The format constants in the capture module mirror
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
driver's abort vblanks, are absent. `-psxtrace-card-active HEX` writes
`<trace>.card` (`psxtrace-card 1`, columns `idx rec vbl off`): one row
per card transfer start, a write breakpoint on the card driver's
transfer-active byte (`0x8000755A` on the SCPH-1001 kernel) catching the
nonzero store the read, write and info handlers make as the select byte
goes out, with its CPU-tick offset from the VBLANK. Every record's `pad1`
holds the anchor hit's CPU-tick offset from the VBLANK its `pad2` counts
(header flag bit 5); a trace without the flag keeps `0xFFFFFFFF` there,
which no reader uses. `-psxtrace-watch-word HEX` rows
(`psxtrace-watch 2`) end with the clock, so a store's place among the
other recorded events of its record is readable.

### Exact tick, VBLANK positions and libspu calls

Every tick column (`.vsync` `entry_tick` and `ret_tick`, `.loads`
`issue_tick` and `done_tick`, `.cdsync` `tick`, `.watch` `tick`,
`.vblpos` and `.spuq` `tick`) is the CPU tick of the instruction the
recorder stopped at. Before patch 0029 the recorders read a counter that
advances only at the emulator's scheduled events, so every call between
two events shared one value (Spyro 1 `.vsync` rows 795898-795901: a wait's
return and three later queries at 2315778078); patch 0029 adds the ticks
the CPU has run since, and the same rows read 2315787079, 2315787088,
2315787208 and 2315789070. A tick divided by 768 is an SPU sample.

`--vblpos` (capture script; DuckStation `-psxtrace-vblpos SPANS
-psxtrace-vblpos-skip LIST`) writes `<trace>.vblpos`: one row per VBLANK
interrupt the CPU takes while no waited VSync call (mode >= 0) is pending
on the VSync recorder's stack, so it needs `--vsync-pc`. Columns:

| column | meaning |
| --- | --- |
| `idx` | row number |
| `rec` | record being accumulated (records written so far, the `.vsync` convention) |
| `vbl` | VBLANK count |
| `via` | `irq`: the exception dispatch found VBLANK pending; `nested`: VBLANK was raised while another interrupt's handler ran and that handler served it before its `rfe` |
| `epc` | EPC of the interrupt (the branch when it hit a delay slot); for `nested`, the enclosing interrupt's |
| `entry` | entry address of the innermost active call at the EPC (a shadow call stack: push at each `jal`/`jalr` target, pop when the return address executes) |
| `insns` | instructions executed outside exception handlers from that entry to the EPC, callees included |
| `t` | the first call target entered after the handler's `rfe` whose address and call site are game code and which the self-entry list does not name |
| `t_rec` | record being accumulated when `t` was entered |
| `k` | `t`'s ordinal in `t_rec`: executions at pc == `t` since that record's anchor, outside exception handlers, each preceded by a game-code instruction |
| `tick` | exact tick of the dispatch (for `nested`, of the enclosing dispatch) |
| `rt` | the main-RAM word (KSEG0, word-aligned) of the first game-code load, between the `rfe` and `t` and in `t_rec`, of a word the row's VSync callback stored (below); `-` when none (patch 0046; version 1 sidecars have neither column) |
| `rk` | `rt`'s ordinal: game-code loads of that word outside exception handlers since `t_rec`'s anchor, the load included |

Game code is RAM at or above `0x10000` outside the library spans:
`tools/psxtrace_selfentry.py library GAME` lists, from
`manifest/psyq.toml` and the linker map, each thunked PsyQ symbol's span
up to the next map symbol, so a translated library function (the libgte
closure, and on Spyro 2 and 3 every symbol `--thunked-only` left out)
counts as game code, as its generated body carries the per-entry gate.
`tools/psxtrace_selfentry.py scan GAME` lists the generated functions
whose body contains `goto block_<self>;` (Spyro 1 14, Spyro 2 13, Spyro 3
60, `<overlay> 0x<addr>` per line): the reference counts that loop-back at
pc == `t` and openpete's gate does not, so such a function is never `t`;
the recorder skips a listed address whichever overlay is loaded. The
anchor breakpoint restarts the counts before the anchor instruction
executes, so the anchor's own entry counts in the new record. Rows of
interrupts taken before one `t` is reached share it; a row whose `t` is
never reached prints `-` for `t`, `t_rec` and `k`. A row carries `rt`
when, before its call target, game code outside exception handlers loads
a main-RAM word whose last store was made by game code inside the
outermost exception that opened the row or a later row-opening one (the
game's VSync callback: its VBL counters): the interrupt's work is
observable at that load. The row is due at `rt`'s `rk`-th load or `t`'s
`k`-th entry, whichever a host reaches first; `t` is the fallback for a
host whose path skips the load. Any other store (outside exception handlers, by kernel or library
code, or inside an exception that opened no row) clears the word's mark;
library and kernel state is left out because openpete's thunks keep it
their own way. The trailer
`# rows= resolved= unresolved= vbl_irqs= nested= in_wait=
self_entry_skipped= shadow_dropped= records= read_resolved=` (rows with
an `rt`) accounts for every VBLANK
after arming (`vbl_irqs = rows + in_wait`, and equals the VBLANK span of
the capture); the capture refuses `vbl_irqs=0`. The recorder runs on the
interpreter through two hooks the patch adds to the CPU core: one called
before each instruction and one called when an exception is raised, with
CAUSE and EPC.

Reading a row: the interrupt took effect for the game's code at the k-th
entry of `t` in record `t_rec`; a host without a cycle clock that counts
its own entries of `t` at the per-entry gate delivers the VBLANK there.
Spyro 1, record 1555: `83 1555 4092 nested 0x8005DCF8 0x8005DBC4 21
0x80058CC0 1555 1 ...` (VBL 4092 raised during a DMA interrupt taken
inside a `VSync(-1)` query; delivered at the first entry of
`0x80058CC0`).

`--spuq KIND:HEX` (repeatable; DuckStation `-psxtrace-spuq`) writes
`<trace>.spuq`: one row per libspu call that reads or moves voice
envelope state, at the entry KIND names (`setkey` SpuSetKey,
`keystatus` SpuGetKeyStatus, `allkeys` SpuGetAllKeysStatus, `voiceattr`
SpuSetVoiceAttr; addresses from `psyq.toml`). A `voiceattr` call is
recorded only when its mask has a pitch field (PITCH, NOTE, SAMPLE_NOTE)
or an ADSR field (AMODE..SL, ADSR1, ADSR2), and the trailer counts the
rest as `voiceattr_skipped`. Columns: `idx rec vbl kind pc ra tick`, then
per kind `on voices` (setkey), `voice` (keystatus), `buf` (allkeys), or
`voice mask pitch note sample_note a_mode s_mode r_mode ar dr sr rr sl
adsr1 adsr2` (voiceattr; a field outside the mask is whatever the
caller's struct held). The capture refuses a run with zero rows.

KIND `spuwrite` (SpuWrite, a main-RAM to SPU-RAM transfer) adds `src size
n` and then `n` voice heads `v:start:cur:counter`, one per voice whose
envelope is not off, read after the SPU has generated every sample due at
the call's tick: the voice index, its start address register and the
address of the ADPCM block it decodes (both in bytes), and its 4.12
position counter (bits 12-16 the sample in the block's 28, bits 4-11 the
interpolation fraction). A streaming ring refill is placed against the
head of the voice that plays the ring. The header and the trailer name
the kind only when it is armed.

`--spureg KIND:HEX` (repeatable; DuckStation `-psxtrace-spureg`) writes
`<trace>.spureg`: one row per execution of the libspu instruction at HEX
that accesses an SPU register, `kon` a store to the key-on register
(0x1F801D88 or 0x1F801D8A), `pitch` a store to a voice's sample rate
(voice base + 4), `envx` a load of a voice's envelope level (voice base +
0xC); the addresses are read off each game's libspu code and listed in
its `oracle.conf`. Columns: `idx rec vbl spuq kind pc addr value tick`,
where `spuq` is the `idx` of the last `.spuq` row written (the call the
access belongs to), `addr` the effective address from the instruction's
base register and offset, `value` the stored halfword (`-` for a load)
and `tick` the exact tick before the instruction executes. The capture
refuses a run in which a named kind recorded no access.

Two captures of one recipe from builds that differ only in these
recorders compare with `python3 -m tools.psxtrace_format compare A B`
(first differing record and region; the build hash and flags are listed,
not compared) and `python3 -m tools.psxtrace_format sidecar-compare A B`
(every common sidecar equal line for line outside the header's `build=`
and the tick columns; each tick in B at least A's, and B's ticks never
decrease from one row to the next where A's did not; exit 0 only when
every sidecar passes).

## The reference's schedule

A golden verified with `.vsync`, `.loads` and `.srand` siblings beside it
runs openpete under the reference's timing (`core/diag/psxtrace_schedule.c`,
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
- A present card's transfer steps byte by byte (`psx_bios.c`, the
  mcHandler states and the fast track) at the SIO0 cadence measured on
  the reference. Free-running, every step runs in the VBL body. When the
  `.card` start of the step's VBLANK and the next record's `pad1` put
  that record's anchor inside the transfer, the steps run to the anchor
  tick before the record is compared and the rest, with the completion's
  events, after it.
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

`--psxtrace-verify` refuses a VBL-stamped golden (every DuckStation-fork
capture) when `.vsync`, `.loads` or `.srand` does not load beside it,
naming the golden and each missing sidecar; unscheduled, the replay
diverges at the first load. `OPENPETE_PSXTRACE_UNSCHEDULED=1` verifies
such a golden unscheduled on purpose. `check_psx_oracle.sh` exits 64 when
any sidecar its recipe's capture flags record is absent, and
`check_dist_first_boot.sh` exits 64 when one of the three is absent.

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

### Clock replay

`OPENPETE_PSXTRACE_CLOCK_REPLAY=1` (set by the gate under
`OPENPETE_ORACLE_CLOCK=1` with `OPENPETE_ORACLE_GTE_STRICT=1`, refused from
the caller's environment) replays the two clock positions the shipped
engine does not model, from `.vblpos`, `.spuq` and `.spureg`:

- In-work VBLANKs. The per-entry gate (`psx_override_dispatch`) counts the
  entries of each `t` the current record's rows name since the record's
  anchor; at the `k`-th entry the row's VBL runs through the VBL body
  before the entered function's body. Hits inside a host-initiated guest
  call (interrupt callbacks, BIOS event handlers, the hang watchdog's spin
  tick, host features that call a guest body) are not counted, and a hook's
  re-dispatch of the body the guest called is not counted again. A row
  whose VBL a query or poll already charged counts as covered; a row whose
  entry openpete does not reach is charged at the next wait that covers
  its VBL, or before the next anchor's hash, and counts as late. When
  rows carry `rt`, the guest's load functions (`read_word`, `read_half`,
  `read_byte`, `lwl`, `lwr`) are wrapped at the first counted gate hit:
  each load of a word an `rt` names, outside a host-initiated call and
  outside a VBL delivery, is counted since the record's anchor, and at the
  `rk`-th the row's VBL runs before the load reads memory, every register
  preserved; a row whose load is not reached stays due at its entry.
  Without `rt` rows, and off the clock row, the load functions are not
  wrapped.
- The SPU clock. The envelope is charged to the reference's exact clock
  divided by 768 at every waited row's return, answered query, answered
  CdSync poll and libspu call matched to a `.spuq` row (same record, kind,
  call site and arguments), instead of 735 samples per waited VBL. The
  clock seeds at the first such event and again after every load-window
  skip; inside a load window each VBL charges the reference's period
  (527722650/715909 samples). Inside a matched call the clock moves on to
  the reference's register access before the state it touches changes: a
  key-on to the KON store of each half, a pitch to the voice's sample-rate
  store, a key-status read to the voice's ENVX load. SpuGetKeyStatus and
  SpuGetAllKeysStatus answer libspu's own expression over the software key
  flag and the envelope level (ENVX): keyed `ENVX == 0 ? 3 : 1`, else
  `ENVX != 0 ? 2 : 0`.

The summary line `clock: vblpos delivered= covered= late= ...; spuq
charged= unmatched= unmade= ...; spureg charged= of ...; vblpos read
rows= loads counted= delivered=` is the engagement (the last three: rows
with an `rt`, loads of their words counted, rows delivered at a load); the gate reports a run with no VBL delivered, no libspu call
or no register access charged as VACUOUS (exit 3).
`scripts/check_psx_oracle_all.sh` runs a `clock` row per game after the
`stock` rows; the stock row's GTE mismatch count and `vbl deltas` measure
the in-tick timing the shipped engine leaves unreplayed. Under
`OPENPETE_SCHED_LOG` each delivery (`vblpos`, `vblpos-late`) and each
libspu match (`spuq`, `spuq-unmatched`) and register access (`spureg`) is
logged.

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

### GTE hash row

Every capture writes `<trace>.gte` (`-psxtrace-gte`, passed by
`capture_psx_oracle_ds.sh` unconditionally): row k folds the GTE
instructions executed after record k's anchor hit up to record k+1's, so
the row aligns on the record, not on a VBL count (openpete reaches some
records one VBL behind the reference). Per instruction the fold takes
the instruction word and the 64 registers' read view after it, FNV-1a-64
over 32-bit words. The read view is the value `mfc2`/`cfc2` returns,
computed from the stored words by the same function on both sides
(`gte_read_view` in `core/diag/psxtrace_verify.c`, and its counterpart in
the recorder), because the two engines store register words differently:
openpete's register arrays hold a game's raw `mtc2`/`ctc2` word until the
next command rewrites them (`core/psx/gte.c`). The read view, after
psx-spx "GTE Registers": data 1, 3, 5, 8-11 and control 4, 12, 20, 26, 27, 29, 30 sign-extended; data 7, 16-19
zero-extended; data 15 reads data 14; data 28/29 read ORGB from IR1-3;
data 31 the leading-bit count of data 30; FLAG bits 30-12 with bit 31
recomputed; data 23 (RES1) the stored word. A register enters the fold
as 0 until the game has changed its read view (observed at instruction
boundaries, BIOS-ROM instructions excluded), so reset residue never
reaches the hash; each row carries that written bitmap and a hash of the
unwritten registers. Columns: `rec ops hash written residue vbl`; the
trailer `# rows= ops= rom_ops= records=`. The capture refuses a run that
hashed zero instructions.

The gate exports `OPENPETE_GTE_HASH=1` whenever the golden has the
sidecar; openpete then folds every `gte_execute` call by the same rule
and the verifier compares row k when it consumes record k+1, skipping an
interval a load window or a re-alignment cut. The summary line
`gte: records= mismatches= ops_hashed= ref_ops= skipped= mism_ops=
mism_hash= mism_written= first_mismatch=` follows the schedule summary;
the gate exits 3 (VACUOUS) when it is missing or `ops_hashed` is 0, and
a mismatch fails the gate only under `OPENPETE_ORACLE_GTE_STRICT=1`.
openpete knobs: `OPENPETE_GTE_HASH_OUT=PATH` writes openpete's own rows
in the sidecar's format (two runs compare with `python3 -m
tools.psxtrace_format gte A B`); `OPENPETE_GTE_HASH_POKE=REC:REG:XOR`
corrupts one register of the first instruction of interval REC in the
fold only, the negative control, which must fail at REC.

Diagnosis of a mismatching record: `capture_psx_oracle_ds.sh --out
<scratch> --frames <rec+2> --gte-dump REC-REC` writes the reference's
instructions of that record to `<out>.gtedump`, and
`OPENPETE_GTE_HASH_DUMP=REC-REC:PATH` writes openpete's in the same
532-byte rows (`{rec, pc, instr, written, pre[64], post[64]}`, read
view); `python3 -m tools.psxtrace_format gtedump-diff REF OP` prints the
first differing instruction register by register.

### Build provenance

A golden is reproducible only from the head recorded in
`third_party/duckstation-patches/applied-head.txt`.
`scripts/capture_psx_oracle_ds.sh` refuses when the binary's build stamp
differs from the DuckStation checkout's HEAD, when that HEAD is not the
recorded head, or when the DuckStation worktree is dirty (the stamp cannot
see dirtiness: `git describe --dirty` finds no tag in the checkout). The
gate refuses a golden whose build hash differs from the recorded head
unless `OPENPETE_ORACLE_ALLOW_FORK_DRIFT=<reason>` names why.

A change to a patch, to `series` or to the pin changes the head. Its
change set regenerates the series (`git format-patch --zero-commit
--no-signature -N <pin>..`), runs `scripts/setup_duckstation.sh --record`
to rewrite `applied-head.txt`, and the three games' goldens are deleted
(`rm tests/golden/<game>/psxtrace-demo-20k.bin*`) and recaptured by the
next gate run of each game.

## Setup

```sh
scripts/setup_duckstation.sh
```

Idempotent. It initialises the submodule; applies the patch series onto
the pin, or leaves a checkout already at the recorded head as it is; resets
a checkout at another head to the pin and re-applies, refusing instead when
the checkout has changes to tracked files or commits on top of the
recorded head; prints the head; downloads the upstream prebuilt-deps
tarball (`deps-linux-x64.tar.xz`, release pinned in the script, SHA256
verified) when absent; copies `.tools/pcsx-redux/scph1001.bin` to
`~/.local/share/duckstation/bios/`; configures
`third_party/duckstation/build-regtest` when unconfigured (OpenGL on,
Vulkan/Wayland/X11 off, Release, Ninja); builds `duckstation-regtest` on
every run (a no-op when nothing changed); and refuses when the binary's
build stamp is not the head. `--record` writes the head to
`applied-head.txt` instead of refusing on a mismatch; `--no-build` stops
after the series.

DuckStation requires clang-19 system-wide; the script pins `CC=clang-19
CXX=clang++-19`. Ubuntu 22.04 ships clang-14, so clang-19 comes from
`apt.llvm.org`.

## Capturing a golden

A golden is not committed. It is a function of the disc, the applied DuckStation
head, the region manifest and the recipe, and two captures from the
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

Options on the command line override the recipe's; `--recipe PATH` (with
`--game`) sources another recipe of the game in place of `oracle.conf`.
Spyro 2 and 3 anchor
at the first call of each main-loop iteration, record every sidecar
(Spyro 2 without the raw-register floor: its own code never touches the
controller's registers), skip libcd's own `VSync(-1)` polls, and boot
with `--no-card` on both sides; every PC comes from the game's `psyq.toml`.
`games/<game>/oracle-card.conf` is the card-present recipe of Spyro 2 and
3: the same recorders with the fork's formatted card in slot 1 and slot 2
empty, the card exported to `tests/golden/<game>/card-v6-blank.mcd`;
`tests/harness/check_psx_oracle.sh --game <game> --recipe
games/<game>/oracle-card.conf` runs openpete on a scratch copy of it. Without
`--game`, `--out`, `--disc` and `--manifest` (or `OPENPETE_DISC`,
`OPENPETE_REGIONS_MANIFEST`) name the inputs directly.

`--frames` is the number of records (anchor hits) to capture. DuckStation's
own `-frames` counts presentation frames, so the script passes
`1000 + 4 × --frames` as the runtime cap: the pre-title splash logos run
four VBLs per tick, while the attract demo itself is 1:1.
`--anchor-pc` and `--hit` override the anchor; `--max-runtime-frames`
overrides the cap.

The script checks build provenance, generates the region file, runs the
capture with GL on llvmpipe (`scripts/lib/swrender.sh`:
`LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe`), asserts the trace exists and
carries the build hash, deletes a RAM-carrying trace under `tests/golden/`,
and prints `tools.psxtrace_format info` on it.

### Driven captures

A reference that must reach a state past the menus (a cutscene after
starting a new game) cannot replay a per-VBL input log recorded under
openpete's load timing: under the drive's load times the presses land in
another state. `--pad-script PATH` (DuckStation `-pad-script`) drives the
port 1 controller from steps that wait on guest RAM, advanced once per
VBL (DuckStation's frame counter, the `vbl` column of every sidecar)
before the VBL's emulation:

| step | effect |
| --- | --- |
| `wait N` | release every button for N VBLs |
| `tap BUTTONS HOLD` | hold BUTTONS for HOLD VBLs |
| `mash BUTTONS HOLD PERIOD until COND` | hold BUTTONS for the first HOLD VBLs of every PERIOD until COND holds |
| `until COND` | release every button until COND holds |
| `quit` | shut the system down |

BUTTONS is a `+`-joined list of `cross circle square triangle start
select up down left right l1 r1 l2 r2`; COND is `W:HEX OP VALUE`, a byte
(`b`), halfword (`h`) or word (`w`) read of the guest address HEX
compared `eq` or `ne` to VALUE. Each completed step is logged with its
VBL; the script keeps the log, filtered of the per-breakpoint lines, as
`<out>.log` and refuses a run that ended before the script's last step.
`--spu-dump` writes the SPU final mix (post voice volume, reverb and main
volume; 44100 Hz stereo s16) to `<out>.wav` through `OPENPETE_SPU_DUMP`
and refuses an empty dump.

`games/spyro-2/oracle-cutscene-intro.conf` captures the Spyro 2 intro
cutscene into `tests/golden/spyro-2/psxtrace-cutscene-intro.bin`: the
attract recipe's recorders, the consume-keyed movie
`games/spyro-2/bk2/s2-cutscene-intro.bk2` on both sides (three Cross
presses that start a new game, the reference one VBL early), the watched
word 0x80068C50 (the speech ring's chunk index: -1 at the stream read
issue, the chunk at the ring refill), `.spuq` `spuwrite` rows and the
final-mix dump, over 1247 records (14 past the start of the load that
follows the cutscene). The stock run diverges at record 216, after the
load into the cutscene takes 272 VBLANKs inside work; the clock row
(`OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1`) matches every
region over every record:

```sh
scripts/capture_psx_oracle_ds.sh --game spyro-2 --recipe games/spyro-2/oracle-cutscene-intro.conf
OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1 \
  tests/harness/check_psx_oracle.sh --game spyro-2 --recipe games/spyro-2/oracle-cutscene-intro.conf
```

Re-blessing a golden (deleting it so the gate recaptures it under a new
DuckStation head, manifest or recipe) is only legal for an intentional
gameplay-behaviour change, after reviewing the diff. A golden is
never blind-overwritten to make a red gate go green.

## The gate

```sh
tests/harness/check_psx_oracle.sh [--game SLUG] [trace-path] [-- <extra openpete flags>]
```

```sh
tests/harness/check_psx_oracle.sh --game spyro-3
```

`--game` (default `spyro-1`) sources `games/SLUG/oracle.conf` for the
golden path, the disc (`OPENPETE_DISC` overrides), the binary
(`build/linux-release/openpete`,
`OPENPETE_BIN` overrides) and the flags the run adds before the caller's
(Spyro 3: `--no-card`). A missing default golden is captured first, which
takes about fifteen minutes for 20000 records; the script exits 64 when
DuckStation is not built. An explicit trace path is never captured.
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

`tests/harness/check_subtick_ram_guard.sh` is the sub-tick RAM-write row
(ctest `psx_oracle_subtick_ram_guard`, labels `slow;oracle;subtick`): the
windowed row over the full golden on a private Xvfb display with
`--render-fps 60`, and `OPENPETE_SUBTICK_RAM_GUARD=1` mapping guest RAM
read-only for every present the sub-tick view brackets. The guard logs
`GUEST RAM WRITTEN` and lets the store through, so a store that writes the
canonical value back passes the oracle; the row fails on any such line, and
is VACUOUS (exit 3) without a nonzero `sub-tick: N re-extract(s) presented`
count and a `RAM guard: armed_presents=N` exit row with N above zero. It
runs for a diff that touches the sub-tick path (`ctest -L subtick`). Its
negative control is `OPENPETE_SUBTICK_FAULT_STORE=1`: every bracketed
present rewrites one guest word (default RAM offset `0x10000`, or the address the
value names) with the value already there; the oracle passes and the row
fails.

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
the run (fenced by `scripts/fences/rumble_guard.py`). A recipe without a
card fixture boots on two freshly formatted cards in a scratch directory
(`scripts/lib/scratch_cards.sh` exports `OPENPETE_CARD_DIR`), never the
per-user cards, so a save left there cannot change the verdict.

Anything after `--` is forwarded verbatim to openpete. This is how a
feature with a game-side producer is proven gameplay-neutral: run openpete
with the feature flag against the stock golden, and a PASS means the flag
perturbed no gameplay-state region relative to hardware.

```sh
tests/harness/check_psx_oracle.sh -- --no-lod
tests/harness/check_psx_oracle.sh -- --renderer psycross
tests/harness/check_psx_oracle.sh custom.bin -- --no-lod
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
| 2 | divergence - `OPENPETE_EXIT_DIVERGENCE` (`core/include/openpete.h:22`) |
| 64 | trace, binary, or disc missing; ctest `SKIP_RETURN_CODE` |
| other | build or runtime error |

The ctest name is `psx_oracle_match`, labels `slow;oracle`, timeout 600 s
(`CMakeLists.txt`). On divergence openpete names the first divergent
record and region, and logs a per-region divergence count over all records
compared, so a stable region (persistent state difference) is
distinguishable from a flickering one.

### Environment knobs

- `OPENPETE_BIN`, `OPENPETE_DISC` - binary and disc overrides.
- `OPENPETE_FRAME_LIMIT` - passed as `--frame-limit`; default 200000. This
  is a safety belt only, since `psxtrace-eof` ends the run first: the
  20k-record demo reaches EOF at about 37000 openpete frames (VBLs, roughly
  two per tick). Setting `OPENPETE_FRAME_LIMIT=8000` cuts the run to a
  roughly 20-second smoke pass covering about 4000 ticks, which reaches no
  level load and therefore clears nothing.
- `PSXTRACE_KEEP_GOING=1` - passes `--psxtrace-keep-going`, comparing every
  record instead of stopping at the first divergence. Used to census which
  regions diverge and for how long.
- `OPENPETE_PSXTRACE_HEAL_WINDOW` - CD-load transient tolerance in ticks;
  see below.
- `OPENPETE_PSXTRACE_POKE=ADDR:BYTE:FRAME` - negative control. The verifier
  writes one byte into guest RAM at the anchor of that frame, before the
  hashes are taken. A run under it must fail at that frame in the region
  holding the address; a PASS means the gate is not looking at that byte.
  For example `OPENPETE_PSXTRACE_POKE=0x8007582C:7:2000` with
  `OPENPETE_FRAME_LIMIT=8000` fails at tick 2000 in `g_SpyroLifeCount`.
- `OPENPETE_ORACLE_LOG` - keep the run's full console log at this path
  (default: a temp file the script deletes). The script reads the exit-time
  `music: summary:` line from it.
- `OPENPETE_PSXTRACE_VBL_POLICY=exact|log` - how a per-record VBL-delta
  mismatch is judged (default `log`). Every record carries the reference's
  VBLANK count at the hit (header flag bit 4, in `pad2`); openpete stamps
  its own `openpete_vbl` at the same hit, and the verifier compares the
  deltas between consecutive records (absolutes differ by the boot
  offset). `exact` hard-fails the first mismatch (reason `vbl-delta`,
  both deltas named); `log` counts them. The exit summary prints both
  sides' delta histograms (bin `>=16` collects the rest). `exact` refuses
  a trace without stamps.
- `OPENPETE_ANCHOR_VBL_LOG=PATH` - one `hit vbl` line per anchor hit,
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
says so), openpete with `--halt-at-pc`/`--halt-hit` under the game's
golden (`--psxtrace-verify` with `--psxtrace-keep-going`, when the golden
its `oracle.conf` names exists; `OPENPETE_RAM_DIFF_NO_SCHEDULE=1` runs
unscheduled), so openpete's hit N is the reference's hit N at any hit,
loads included, and
`tools/ram_diff.py` sorts every differing range into a class from
`games/<game>/manifest/ram_diff_expect.toml`, in this order: the exact
words openpete's PSYQ mirrors write (`populated`; a difference here is a
defective mirror and fails), the kernel area, the PSYQ library's own
section extents from the decomp link map (`psyq_internal`, ratcheted per
anchor against the counts recorded in the file), the BIOS's stack frames,
bytes of a read's destination that differ while the read is in flight
(`load_in_flight`: the golden's `.loads` rows issued before the hit and
completed at or after it, the reference's own timing; without a golden,
the CdRead lines in openpete's log; any such byte means the anchor sits
inside a load and the run does not pass, and under the schedule openpete
lands sectors at the reference's interrupts, so the class is empty
whenever the landing is exact), and named ranges
with a reason and an owner. Anything else is unexpected and fails; the
union of expected differing bytes is capped at 25% of RAM. The anchor must
be a dispatch-table entry whose entry block is not a loop head, since the
module counts executions of the instruction and openpete counts calls.
Each game's file names a `first` anchor, the entry of the function issuing
its first CdRead, where no read has been issued on either side; Spyro 1
also names its `loop` anchor, GamestateDraw, where hit N is the same
manifest state on both sides by the oracle and the whole-RAM residue is
what the tool reports. A range leaves the expectation file only by a fix. The `psyq_internal`
ratchet applies at the hit its row was measured at; a run at another hit
prints the measurement without it, so any hit is a diagnostic.

A savestate load is refused while the run is under the schedule: the
thunk layer's position in the schedule's rows is host state no state file
carries, and a load would replay from a RAM image the schedule has moved
past. No golden is verified through a savestate round-trip.

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
end. The Spyro 1 manifest's head comment lists the rows. Charging the
reference's load VBLs on openpete's side would let the main-image flags
be removed.

The verifier (`core/diag/psxtrace_verify.c`) tracks each transient
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

- The NTSC line is 227.5 colour-subcarrier cycles, 3412.5 GPU ticks, the
  period `core/include/openpete_time.h` encodes (2250000/37609 Hz). Patch
  0030 sets the reference's NTSC lines to alternate 3413 and 3412 GPU
  ticks and its PAL line to 3405. The reference frame is then 897487.5
  GPU ticks on average, 566120.83 CPU ticks at a 53693175 Hz GPU clock
  (psx-spx's video clock; the 15 x colour-subcarrier clock, 53693181.8 Hz,
  gives 566120.76); the `.vsync` waited rows of a 2000-record Spyro 1
  capture read 566120.81 by least squares (566203.76 without the patch). A
  model that converts reference CPU ticks to VBL fractions uses this one
  period against a golden and against hardware.

## Related

- `games/spyro-1/manifest/gameplay_state_regions.toml` - the region contract itself, with
  the address source and audit note for every region.
