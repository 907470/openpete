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
fork as a submodule at `third_party/duckstation`, tracking branch
`openpete-psxtrace` of `github.com/tyscorp/duckstation`. That branch
carries two commits on top of upstream `stenzek/duckstation`:

1. **build: include the EGL context header unconditionally for
   surfaceless EGL.** Upstream only includes it when X11 or Wayland
   is enabled, but the EGL context creation is called unconditionally
   on Linux. Breaks our headless config.
2. **regtest: add -psxtrace flag for openpete oracle capture.** Adds
   a new `psxtrace_capture.{cpp,h}` module to `duckstation-regtest`
   and four CLI flags (`-psxtrace PATH`, `-psxtrace-anchor-pc HEX`,
   `-psxtrace-anchor-hit N`, `-psxtrace-frames N`). The module
   mirrors `manifest/gameplay_state_regions.toml`,
   `core/include/openpete_fnv1a64.h`, and `tools/psxtrace_format.py`
   so the emitted v2 `.psxtrace` is byte-compatible with openpete's
   `--psxtrace-verify`.
   The region table is a hand-written copy: `tools/check_ds_region_mirror.py`
   (ctest `ds_region_mirror`) diffs it against the manifest row by row,
   and `scripts/capture_psx_oracle_ds.sh` refuses to capture while they
   differ.

`duckstation-regtest` is the headless regression-test target that
already ships with software audio (no audio output), software
GPU, and non-persistent memory cards — ideal substrate for the oracle.

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
