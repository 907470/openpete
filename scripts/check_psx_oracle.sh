#!/usr/bin/env bash
# scripts/check_psx_oracle.sh — run openpete --psxtrace-verify against a
# PCSX-Redux baseline trace. The trace's per-frame oracle hashes (one per
# gameplay-state region) are value-equality compared against openpete's
# live hashes; first mismatch = first leak.
#
# Usage:
#   scripts/check_psx_oracle.sh [trace-path]
#
# Defaults:
#   trace-path: tests/golden/spyro-1/psxtrace-titlescreen-baseline.bin
#   binary:     build/linux-release/games/spyro-1/openpete-spyro1
#   disc:       env $OPENPETE_DISC or /home/$USER/code/spyro/spyro-1/build/SCUS-94228.cue
#
# Sibling artifacts (auto-detected — produced by pcsx-redux-trace.lua):
#   <trace>.seed.ram   passed to openpete via --seed-ram (RAM at trace t0)
#   <trace>.seed.cpu   passed via --seed-cpu (528B 'CSv1' CPU state at t0)
#   <trace>.bk2        passed via --bk2 so openpete replays the same input
#                      pcsx-redux-bk2-replay.lua drove on the oracle side
#
# Env knobs:
#   OPENPETE_BIN, OPENPETE_DISC
#   OPENPETE_FRAME_LIMIT (default 8000 — safety belt; psxtrace-eof exits first)
#   PSXTRACE_KEEP_GOING=1   diff every frame, don't stop on first mismatch
#
# Exit codes:
#   0   bit-identical hashes for every traced frame
#   2   divergence (openpete ran fine but a hash mismatched)
#   64  prerequisites missing (trace / binary / disc) — ctest treats as SKIP
#   *   build/runtime error
set -euo pipefail
cd "$(dirname "$0")/.."

TRACE="${1:-tests/golden/spyro-1/psxtrace-titlescreen-baseline.bin}"
BIN="${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-/home/$USER/code/spyro/spyro-1/build/SCUS-94228.cue}"

if [[ ! -f "$TRACE" ]]; then
    echo "[psx-oracle] trace not found: $TRACE" >&2
    echo "[psx-oracle] regenerate with scripts/pcsx-redux-trace.lua, see"   >&2
    echo "             docs/ram-diff-vs-pcsxr.md for the workflow."         >&2
    exit 64
fi
if [[ ! -x "$BIN" ]]; then
    echo "[psx-oracle] binary missing: $BIN — run cmake --build --preset linux-release" >&2
    exit 64
fi
if [[ ! -f "$DISC" ]]; then
    echo "[psx-oracle] disc not found: $DISC (override with OPENPETE_DISC=)" >&2
    exit 64
fi

EXTRA=()
[[ "${PSXTRACE_KEEP_GOING:-0}" != "0" ]] && EXTRA+=(--psxtrace-keep-going)

# Auto-detect sibling artifacts so a `${TRACE}.seed.{ram,cpu}` + `${TRACE}.bk2`
# triple cleanly aligns openpete's start to the oracle's.
SEED_RAM="${TRACE}.seed.ram"
SEED_CPU="${TRACE}.seed.cpu"
BK2="${TRACE}.bk2"

[[ -f "$SEED_RAM" ]] && EXTRA+=(--seed-ram "$SEED_RAM") && \
    echo "[psx-oracle] seed RAM: $SEED_RAM"
[[ -f "$SEED_CPU" ]] && EXTRA+=(--seed-cpu "$SEED_CPU") && \
    echo "[psx-oracle] seed CPU: $SEED_CPU"
[[ -f "$BK2"      ]] && EXTRA+=(--bk2 "$BK2") && \
    echo "[psx-oracle] bk2:      $BK2"

LIMIT="${OPENPETE_FRAME_LIMIT:-8000}"

set +e
"$BIN" --headless \
    --disc "$DISC" \
    --run-recomp 0x80012204 \
    --psxtrace-verify "$TRACE" \
    --frame-limit "$LIMIT" \
    "${EXTRA[@]}"
rc=$?
set -e
echo "[psx-oracle] exit code=$rc"
exit "$rc"
