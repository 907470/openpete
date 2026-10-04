# GTE

The PSX Geometry Transformation Engine (coprocessor 2) is implemented in
`core/psx/gte.c`, written from the register-level hardware description.
Every recompiled cop2 command, in every game and overlay, calls
`gte_execute(CPUState*, uint32_t cmd)`; the module ABI exports the symbol
(`games/spyro-1/manifest/module_abi_exports.txt`). The diagnostics around
it live in `core/psx/gte_instruments.cpp`, declared in
`core/include/openpete_gte.h`; the per-record hash that compares it with
the reference lives in `core/diag/psxtrace_verify.c`.

## Implementation

### Dispatch and the register view

The register file is `cpu->gte_data[32]` (cop2r0-31) and
`cpu->gte_ctrl[32]` (cop2r32-63); there is no second copy. `gte_execute`
calls `openpete_gte_run(data, ctrl, cmd)`, which takes the two arrays
alone, so a harness drives it without a CPU. The command word is decoded
into a `gte_op_t`: function code bits 0-5, `sf` bit 19 (fraction shift
`sh` = 12 or 0), `lm` bit 10, and for MVMVA `mx` bits 17-18, `v` bits
15-16, `cv` bits 13-14. `gte_dispatch` switches on the function code: the
22 documented commands RTPS 01, NCLIP 06, OP 0C, DPCS 10, INTPL 11, MVMVA
12, NCDS 13, CDP 14, NCDT 16, NCCS 1B, CC 1C, NCS 1E, NCT 20, SQR 28, DCPL
29, DPCT 2A, AVSZ3 2D, AVSZ4 2E, RTPT 30, GPF 3D, GPL 3E, NCCT 3F. Any
other code returns 0 and changes no register, FLAG included. FLAG is
accumulated in `gte_op_t.flag` from zero at command start and stored by
the writeback.

MVMVA with `mx` = 3 uses the matrix `[-R*10h, R*10h, IR0, RT13, RT13,
RT13, RT22, RT22, RT22]` (R = RGBC bits 0-7); `cv` = 3 adds no
translation; `cv` = 2 (FC) takes `gte_mat_vec_fc`: the translation and
first column raise only the IR saturation flag (saturated as `lm` = 0),
and MAC/IR come from the last two columns alone.

### The UNR division

RTPS and RTPT divide H by SZ3 in `gte_divide`. When H >= SZ3*2 the
result is 1FFFFh with FLAG bit 17. Otherwise the divisor is shifted left
until bit 15 is set, a reciprocal seed is read from the 257-entry
`k_unr_table` at `(d - 7FC0h) >> 7`, refined by two Newton-Raphson steps,
and the rounded product `(n*d + 8000h) >> 16` is limited to 1FFFFh
without a flag. H is read unsigned 16-bit for the division.

### MAC overflow checks

MAC1-3 accumulate in a signed 44-bit range. `gte_acc` checks every
addition, not only the final sum: a sum above +7FFFFFFFFFFh raises bit
31-i, below -80000000000h bit 28-i, and the accumulator keeps the 44-bit
wrap, so an overflow undone by a later term leaves a correct value with
the flag set. `gte_set_mac` stores `acc SAR sh` truncated to 32 bits.
MAC0 is computed in 64 bits and `gte_set_mac0` raises bit 16 (above
INT32_MAX) or bit 15 (below INT32_MIN) on the internal value; the screen
coordinates and IR0 derive from the internal value, not the stored word.

### Saturation and flags

| Value | Range | FLAG bit |
|-------|-------|----------|
| IR1-3 | 0..7FFFh under `lm`, else -8000h..7FFFh | 24, 23, 22 |
| IR0 | 0..1000h | 12 |
| SX2, SY2 | -400h..3FFh | 14, 13 |
| SZ3, OTZ | 0..FFFFh | 18 |
| colour R, G, B (MAC/16) | 0..FFh | 21, 20, 19 |
| perspective divide | 1FFFFh | 17 |

RTPS/RTPT saturate IR1-3 to -8000h..7FFFh regardless of `lm`. IR3's flag
is raised from the full 44-bit sum SAR 12 (the value SZ3 is taken from),
not from MAC3 SAR `sh` that IR3 is clamped from; the two differ when
`sf` = 0. The depth-cue step (`gte_depth_cue`, used by DPCS, DPCT, INTPL,
DCPL, NCDx, CDP) saturates its intermediate IR as `lm` = 0. FLAG bit 31
is the OR of bits 30-23 and 18-13; bits 11-0 are never stored.

### The FIFOs

- SXY: `gte_push_sxy` shifts SXY0 <- SXY1 <- SXY2 <- new on every RTPS
  vertex and each RTPT vertex.
- SZ: `gte_push_sz` shifts SZ0 <- SZ1 <- SZ2 <- SZ3 <- new, before the
  division.
- RGB: `gte_push_color` shifts RGB0 <- RGB1 <- RGB2 <- `[MAC1/16,
  MAC2/16, MAC3/16, CODE]`, CODE being RGBC bits 24-31; pushed by every
  colour command (NCx, NCCx, NCDx, CC, CDP, DPCS, DPCT, INTPL, DCPL, GPF,
  GPL).

AVSZ3 sums SZ1-3 and AVSZ4 SZ0-3, multiply by ZSF3/ZSF4 into MAC0, and
store OTZ = MAC0 SAR 12.

### The writeback extension table

The recompiled code reads and writes the register arrays directly, so
after each documented command `gte_writeback` rewrites the file in its
hardware read form:

- sign-extended from 16 bits: data 1, 3, 5 (VZ0-2), 8 (IR0), 9-11
  (IR1-3); control 4 (RT33), 12 (L33), 20 (LB3), 26 (H), 27 (DQA), 29
  (ZSF3), 30 (ZSF4);
- zero-extended from 16 bits: data 7 (OTZ), 16-19 (SZ0-3);
- data 15 (SXYP) = data 14 (SXY2);
- data 28 (IRGB) and 29 (ORGB) = IR1-3, each SAR 7 saturated to 0..1Fh,
  packed 5:5:5;
- data 31 (LZCR) = count of leading bits of data 30 (LZCS) equal to its
  bit 31, 1..32;
- control 31 (FLAG) = bits 30-12 of the command's flags plus the bit-31
  summary.

Data 23 (RES1) and every 32-bit register keep the stored word. The table
`k_sext_data`/`k_sext_ctrl`/`k_zext_data` is the same list the hash's
read view uses (below).

### The canonical-SX FIFO

Each RTPS/RTPT vertex also pushes MAC0 of the X projection (`n*IR1 +
OFX`, the 64-bit internal value before SX2 saturation, 16.16 fixed
point) onto `openpete_canonical_sx0/1/2_q16` (defined in
`core/renderer/widescreen.c`, sx2 the latest). It is written
unconditionally. It is a gameplay channel: the `replace_call` sites in
`games/spyro-1/generated/channel_sites.txt` route the game's particle
and sparkle screen-X tests through `psx_runtime_part_cull_sx1`,
`_sx2`, `_part_cull_4vert` and `psx_runtime_sparkle_canon_sx`
(`games/spyro-1/renderer/render_decouple.c`), which read the FIFO and
decide the 0 < SX < 512 verdict behind the particle drawn-flag store and
the g_Sparkles slot use. The GTE applies no presentation policy
(widescreen and FOV are applied in the native renderer), so the FIFO
holds the projection of the game's own register state under every
presentation flag.

## What the generator owns

The recompiler (`third_party/psxrecomp/recompiler/src/code_generator.cpp`)
emits register moves inline; `gte.c` sees only the command calls.

- `mfc2`/`cfc2` are raw array reads (`rt = cpu->gte_data[rd]`). A value
  read between commands is the stored word, which the writeback left in
  read form unless the game stored to it since.
- `mtc2`/`lwc2` to data 28 (IRGB) stores `v & 7FFFh` and sets IR1-3 to
  the 5-bit fields SHL 7; to data 30 (LZCS) stores the word and sets data
  31 (LZCR) to its leading-bit count. Every other `mtc2`/`ctc2`/`lwc2` is
  a raw store.
- A raw store to data 15 (SXYP) does not push the SXY FIFO, a raw store
  to a 16-bit register reads back unextended, and a store to ORGB, LZCR or
  FLAG reads back as stored, each until the next command's writeback.
  `OPENPETE_GTE_CENSUS` counts each class (below); the reg-15 count over
  the 20k demo of all three games is 0.

## Instruments

All read their environment on the first command; with none armed,
`openpete_gte_instruments_on` drops to 0 and a command pays one branch.

| Env var | Output |
|---------|--------|
| `OPENPETE_GTE_TRACE=PATH` (`_LO`, `_HI`, `_OPS`) | CSV, a `rec` row (`gte.c`) and a `psy` row (PsyCross's `GTE_operator` on the same pre-state) per command in an `openpete_frame` window; see [GTE trace vs reference](gte-trace-vs-reference.md). |
| `OPENPETE_GTE_CENSUS=1` | at exit, stderr `[gte-census]` lines: `fn=XX n=` per function code; `undocumented cmd= first_pc= n=` per undocumented word (first 64); `store <kind> n=` for stores between commands, kinds `sxyp`, `sxy2_without_sxyp`, `unextended_16bit`, `irgb`, `orgb`, `lzcr`, `flag`, `other`; `sxyp_store_before_pc=` (first 16 PCs); `sxyp_stores=`. A store is a register whose entry value differs from the previous command's writeback. |
| `OPENPETE_GTE_RECORD=PATH[:OPS]` | binary entry register files of the listed function codes (hex, default `01,30`), delta-coded: per command the 25-bit command field, a 64-bit mask (two u32, low first) of the registers changed since the previous record, then those words. |
| `OPENPETE_RENDER_PROBE=1` | RTPS/RTPT/MVMVA/other counts and rotation/translation changes, printed and reset by PsyCross per present. |
| `OPENPETE_GTE_PCHIST=N` | `[gtehist] pc= n=` caller-PC histogram of `openpete_frame` N. |
| `OPENPETE_RTPS_BOXLOG=x0,y0,x1,y1,N` | `[rtps-box]` caller PC, SX, SY, SZ of each RTPS/RTPT vertex inside the box on `openpete_frame` N (at most 4000 rows). |
| `OPENPETE_OFX_CENSUS=1` | `[ofx-census]` per caller PC, the OFX range of RTPS/RTPT commands whose OFX is not 256.0. |

`openpete_gte_caller_pc` (defined in `gte.c`) is the guest PC the
generated call site stores before each `gte_execute` call; every
instrument and the hash read it.

## The hash row

The oracle capture writes `<trace>.gte` beside the golden; the recorder,
the read view and the gate wiring are described in
[DuckStation oracle](duckstation-oracle.md#gte-hash-row).
`check_psx_oracle.sh` exports `OPENPETE_GTE_HASH=1` when the sidecar
exists; `gte_execute` then passes the pre- and post-command files to
`openpete_gte_hash_op`, which folds the instruction word
(`cmd | 4A000000h`) and the 64-register read view after it into one
FNV-1a-64 hash per record interval. A register enters the fold as 0 until
its read view first changes. Interval k runs from record k's anchor hit
to record k+1's and is compared when record k+1 is consumed; an interval
cut by a load window or a re-alignment skip is counted as skipped.

The summary line:

```
gte: records=N mismatches=M ops_hashed=O ref_ops=R skipped=S mism_ops=A mism_hash=B mism_written=C first_mismatch=K (ops ref= op= hash ref= op= written ref= op=)
```

- `records`: intervals compared; `skipped`: intervals not compared.
- `ops_hashed` / `ref_ops`: commands folded on each side over the
  compared intervals. `ops_hashed` = 0 makes the gate exit 3 (VACUOUS).
- `mismatches`: intervals where the op count, the hash or the written
  bitmap differs; `mism_ops`, `mism_hash`, `mism_written` count each
  field. An interval with `mism_ops` set has a different number of cop2
  commands on the two sides: the game took a different path, upstream of
  the GTE. An interval with equal op counts and a different hash is a
  GTE defect or a difference in the GTE's inputs.
- `first_mismatch`: the first such record, with both sides' fields.

A mismatch fails the gate only under `OPENPETE_ORACLE_GTE_STRICT=1`,
which is off while the residual classes below are open. Knobs:
`OPENPETE_GTE_HASH_OUT=PATH` writes openpete's rows in the sidecar
format (`python3 -m tools.psxtrace_format gte A [B]` summarises one file
or compares two); `OPENPETE_GTE_HASH_POKE=REC:REG:XOR` corrupts one
post-command register of interval REC's first command in the fold only,
the negative control, which must report `first_mismatch=REC`;
`OPENPETE_GTE_HASH_DUMP=LO-HI:PATH` writes full rows for those records.

Reading a `gtedump-diff`: capture the reference's rows with
`scripts/capture_psx_oracle_ds.sh --out <scratch> --frames <REC+2>
--gte-dump REC-REC`, openpete's with `OPENPETE_GTE_HASH_DUMP=REC-REC:PATH`
on the gate's run line, then run `python3 -m tools.psxtrace_format
gtedump-diff REF OP`. Both files hold 532-byte rows `{rec, pc, instr,
written lo, written hi, pre[64], post[64]}` in read view. The tool
prints the first command whose instruction, pre or post view differs,
then each differing register by name. A difference in `pc` or `instr`
means the two sides issue different command streams; equal `pc` and
`instr` with a differing `pre` row places the cause before the command
(a guest store or an earlier command); equal `pre` with a differing
`post` row is a defect in `gte.c`.

## Verification record

- Canonical-SX FIFO: an A/B against the earlier vendored implementation
  over the Spyro 1 demo compared 60.3 million pushes; 0 differ.
- Fuzz: 10^6 randomized register files per documented command against
  the earlier implementation differ only where that implementation
  departs from the hardware description: FLAG bit 16 (MAC0 positive
  overflow) on RTPS with SZ3 = 0, which `gte.c` raises. That class was
  the cause of every hash mismatch the earlier implementation produced
  with equal inputs. The fuzz harness is not in the tree.
- Census (`OPENPETE_GTE_CENSUS`, 20k demo, all three games): every
  executed function code is a documented one, 11 codes in all (01, 06,
  0C, 10, 11, 12, 1C, 28, 30, 3D, 3E); no undocumented word executes;
  raw reg-15 stores 0.
- Hash row (20k demo): the GTE-defect class (equal op count, different
  hash) is 0 on all three games. Residual mismatching records: Spyro 1
  13, Spyro 2 61, Spyro 3 9. At each first difference the inputs or the
  guest PC differ, never the function of equal inputs.

## Residual classes

**In-work VBL delivery (Spyro 1, six records per 20k run).** The demo
start zeroes `g_LevelTicks` (0x800758C8), which the PadVSync handler
increments. On the reference one or two VBL interrupts land inside a
tick's work, before the next anchor; openpete charges every VBL at the
next waited VSync (`core/diag/psxtrace_schedule.c`), so on these records
the counter is one or two behind at the anchor. The DEMO MODE letters'
`m_Rotation.z` derives from the counter, and the MVMVA at 0x80022E04
reads other sine-table entries. Neither the anchor nor the next wait is
the reference's delivery point; delivering each VBL at its position in
the guest's execution needs a recorder of where each interrupt lands.

**SPU time base (Spyro 1 seven records, Spyro 2 all, Spyro 3 all).** A
sound slot frees when libspu reports key status 3 (keyed on, envelope
zero). openpete's SPU answers from the ADSR phase and advances 735
samples per waited VBL, at the wait, with envelope time standing still
during the tick's work; the reference advances 737.137 samples per VBL
continuously. The envelope ends on the same sample, but the query falls
on the other side of it, so the slot frees one tick apart and the
positional pan routine with its SQR pair (Spyro 1 SoundsUpdate, Spyro 2
func_80051704, Spyro 3 func_8003CB00) runs on one side only. A uniform
rate correction at the wait moves frees earlier wherever the reference's
key-on and query fall late inside the VBL; the end state needs the SPU
advanced at each libspu call by a guest cycle clock.
