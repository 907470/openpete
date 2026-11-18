# Provenance

Where openpete's code comes from, under which terms, and what the
repository holds of the reference emulator the oracle gate captures from.

## Licences

openpete's own files are under the MIT License (`LICENSE`). `NOTICE.md`
states the scope of that licence and lists every third-party component the
repository holds or a package carries, with its licence and upstream.
`packaging/licences/manifest.tsv` maps each component to its licence text;
`scripts/dist_compliance.py licences` copies the texts into every package's
`licences/` directory and refuses a package when a component has no text, a
licence file under `third_party/` belongs to no row, a `third_party/`
directory is covered by no row, or a row names a path that does not exist.
`scripts/dist_compliance.py scan` refuses a package whose binaries carry a
marker of a copyleft library that must not ship (a statically linked FFmpeg
or x264).

The decomp submodules `games/<game>/<game>/` are separate repositories,
each under CC0 1.0.

## The recompiler

`third_party/psxrecomp` is PSXRecomp, licensed under the PolyForm
Noncommercial License 1.0.0 and used with its author's permission. The
packages carry its recompiler as a separate program under `recomp/tools/`,
which recompiles the player's disc at first boot, with its licence as
`licences/PSXRecomp.txt`. The `openpete` executable compiles no PSXRecomp
source or header: the recompiled C is compiled against openpete's own
headers (`core/include/psxrt/`). PSXRecomp's runner
(`third_party/psxrecomp/runner/`) is compiled into no build.

## The oracle reference

The oracle gate (`tests/harness/check_psx_oracle.sh`) compares openpete with
a trace captured from DuckStation, licensed CC BY-NC-ND 4.0, running the
retail SCPH-1001 BIOS ([DuckStation oracle](duckstation-oracle.md)).

- `third_party/duckstation` is a submodule: the repository records the
  upstream commit it pins and holds none of DuckStation's files.
  `scripts/setup_duckstation.sh` initialises the submodule from upstream
  at that commit on a developer's machine.
- `third_party/duckstation-patches/` is openpete's series: whole files of
  its own (`files/`, MIT like the rest of openpete, each with an SPDX
  header), copied into the reference tree at paths upstream lacks, and
  `hooks.txt`, a list of inserted lines, each located by a
  salted hash of a window of upstream text. The series carries no upstream
  context, removed or modified line; an inserted line, once applied, is
  part of an upstream file under DuckStation's licence.
  `scripts/duckstation_hooks.py`, run by
  the setup script, applies it to the pinned commit and builds the test
  binary locally; the applied head is a function of the pin and the applied
  tree.
- No package contains DuckStation or any part of it.

Data the reference produced (the trace goldens under `tests/golden/`, which
are generated and not committed, and the measured tables below) records the
reference's output, not its source.

## Code rebuilt from measurements

These parts of openpete and its PsyCross fork reproduce the reference's
observable behaviour and were written without sight of its source or of the
earlier code that followed it:

| code | measurement | instrument |
| ---- | ----------- | ---------- |
| controller motor response (the input offset, output scale and small-motor rule; the cubic is Pokopom's, below), `core/input/rumble_response.c` | `tests/unit/data/rumble-response/reference.tsv` | `tools/rumble_response_measure.py`, `tools/rumble_probe/` |
| line rasterizer of the unit tests, `tests/unit/psx_gpu_line.c` | `tests/unit/data/gpu-line/line.pixels` | `tools/gpu_blackbox/measure_lines.py` |
| UI sprite texel stepping, `games/common/renderer/psx_uv.c` | `tests/unit/data/gpu-uv/uv.texels` | `tools/gpu_blackbox/measure_uv.py` |
| SPU voice envelope, `third_party/PsyCross/src/audio/PsyX_SPUAL.cpp` | `tests/unit/data/spu-envelope/reference.env` | `tools/spu_envelope_measure.py`, `tools/spu_envelope_probe/` |
| analog stick response, `core/input/stick_response.c` | `tests/unit/data/stick-response/console.runs` | `tools/stick_response_measure.py` |

Each table's header states the method, the reference's applied head and the
command that regenerates it byte for byte; a unit test checks the code
against the table.

Method. The work is split between two parties.

1. The measurer may read the reference's source. It builds the instrument (a
   PS-EXE probe booted in the reference's test binary, or a recorder in the
   patch series), runs it over the whole input domain or a stated sample,
   and hands over the measured table, the interface (signatures, types,
   units, domain), stub files and the test. The hand-over carries no
   formula, constant, identifier or comment from the reference or from the
   earlier code.
2. The writer has read neither the reference nor the earlier code. It reads
   only the hand-over and the public hardware documents (psx-spx) and writes
   the code from them; the required form is a directory outside the
   repository that holds only the hand-over, the writer opening nothing
   outside it. Review feedback to it is limited to failing table rows and
   interface defects.
3. The record of every file the writer opened is checked against the list
   of files it was allowed to read; a forbidden read voids the code and the
   writer's stage is run again.

The code's expression is independent of the reference's; its function is
not, since it is fitted to a measurement of the reference. One hand-over
departs from item 1: the texel stepping hand-over named the earlier code's
function names, state type and line range, with no body or arithmetic.

The large-motor curve in `core/input/rumble_response.c` is the cubic of
Pokopom (https://github.com/KrossX/Pokopom, copyright 2012 KrossX, MIT),
evaluated at an input offset and output scale fitted to the measured table;
the source comment and `NOTICE.md` credit it, and its licence ships as
`licences/Pokopom.txt`.

The rest of PsyCross's SPU (reverb, interpolation, noise, ADPCM) and its XA
decoder follow psx-spx.
