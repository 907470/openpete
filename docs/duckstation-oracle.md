# DuckStation as the openpete PSX oracle

## Why DuckStation

DuckStation supersedes PCSX-Redux as the canonical PSX oracle for the
.psxtrace gate (Phase 2 of `notes/plans/openpete-determinism-enforcement.md`).
Two reasons:

1. **Game compatibility.** PCSX-Redux's GPU emulation chokes on Spyro
   1's cyclorama renderer (`func_8004EBA8`, hits ~120 frames in) — the
   GPU thread freezes once the game emits a `DR_ENV` linked-list packet
   the emulator can't decode, which stops VSync IRQs and wedges the
   capture. DuckStation runs the same code at 6000+ fps headless
   without breaking a sweat.
2. **Authoritative BIOS.** DuckStation runs real Sony SCPH1001; PCSX-
   Redux was running OpenBIOS (an open-source clone with bug-compat
   differences). For the North-Star "every byte matches PSX hardware"
   property, SCPH1001 is the right ground truth. PCSX-Redux+OpenBIOS
   is a useful sanity check but not the final reference.

## Architecture

DuckStation has no Lua scripting layer (and we don't want to add one —
maintenance burden, upstream wouldn't accept). Instead we vendor a
pinned commit + apply a small source patch to `duckstation-regtest`,
the headless regression-test target that already ships with software
audio (no audio output), software GPU, and non-persistent memory
cards.

The patch (`scripts/duckstation-patches/`):

- `psxtrace_capture.{cpp,h}` — new module. Owns the gameplay-state
  region table (mirror of `manifest/gameplay_state_regions.toml`),
  FNV-1a-64 (mirror of `core/include/openpete_fnv1a64.h`), and the
  v2 `.psxtrace` format writer (mirror of `tools/psxtrace_format.py`).
- `01-regtest-psxtrace.patch` — wires CLI flags (`-psxtrace PATH`,
  `-psxtrace-anchor-pc HEX`, `-psxtrace-anchor-hit N`,
  `-psxtrace-frames N`) and adds the new TU
  to the CMake target. Also fixes an unrelated DuckStation build
  bug: the base EGL header (the EGL context header) was only
  conditionally included when X11 or Wayland was on, but the
  surfaceless the EGL context creation call is unconditional.

The capture mechanism is an execute breakpoint at
the anchor PC. On each hit, the callback reads main RAM for each
region (with the same indirect-pointer dereference logic as
`gameplay_hash_compute_ex`), computes hashes, writes one `.psxtrace`
record. No interpreter slowdown, no scripting overhead.

## Setup

```sh
scripts/setup_duckstation.sh
```

Idempotent. Clones DuckStation at the pinned commit, downloads the
upstream prebuilt-deps tarball (SHA256 verified), stages our patch +
new files, copies SCPH1001 BIOS from `.tools/pcsx-redux/`, builds the
regtest target.

Requires `clang-19` system-wide (DuckStation's hard requirement;
Ubuntu 22.04 ships clang-14 only — install from
`http://apt.llvm.org/jammy/llvm-toolchain-jammy-19/`).

## Usage

```sh
scripts/capture_psx_oracle_ds.sh \
    --out tests/golden/spyro-1/psxtrace-titlescreen-baseline.bin \
    --frames 600
```

Captures 600 records from DuckStation in ~5 seconds, writing a v2
`.psxtrace` file consumable by openpete's `--psxtrace-verify` flag.

## Known artefacts

- **Mesa GL TLS-destructor SIGABRT on shutdown.** DuckStation's
  software-renderer path on WSL2 + llvmpipe crashes inside
  `swrast_dri.so` during pthread thread-local-storage cleanup, AFTER
  the regtest has logged "Exiting with success." and our trace file
  has been finalised. Cosmetic — `capture_psx_oracle_ds.sh` swallows
  the abort. Doesn't repro on systems with a real GL stack.
- **Frame screenshots are black** in the headless software path — the
  PSX render output reaches VRAM but the GL present step on llvmpipe
  doesn't capture it correctly. Not a problem for the oracle (we hash
  RAM, not VRAM); revisit if we ever want regtest-driven visual
  regression testing.

## Comparison to PCSX-Redux oracle

`scripts/capture_psx_oracle.sh` (PCSX-Redux + Lua scripting) is
preserved for forensic single-shot RAM dumps. DuckStation should be
the default for streaming `.psxtrace` capture going forward.
