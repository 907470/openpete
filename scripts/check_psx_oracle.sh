#!/usr/bin/env bash
# scripts/check_psx_oracle.sh — drive openpete --psxtrace-verify against a
# PCSX-Redux baseline trace.
#
# Companion to scripts/check_aspect_determinism.sh. Where the aspect harness
# proves "openpete matches itself across 4:3/16:9", this harness proves
# "openpete matches PSX hardware as observed by PCSX-Redux".
#
# Usage:
#   scripts/check_psx_oracle.sh [trace-path]
#
# Defaults:
#   trace-path: tests/golden/spyro-1/psxtrace-titlescreen-baseline.bin
#   binary:     build/linux-release/games/spyro-1/openpete-spyro1
#   disc:       env $OPENPETE_DISC or /home/$USER/code/spyro/spyro-1/build/SCUS-94228.cue
#
# Env knobs:
#   OPENPETE_BIN, OPENPETE_DISC, OPENPETE_FRAME_LIMIT (default trace frames + 5)
#   PSXTRACE_KEEP_GOING=1   -- diff every frame, don't stop on first leak
#
# Exit codes:
#   0   bit-identical to oracle for every traced frame
#   2   divergence (openpete ran fine but RAM mismatched the trace)
#   *   setup/build/runtime error
set -euo pipefail
cd "$(dirname "$0")/.."

TRACE="${1:-tests/golden/spyro-1/psxtrace-titlescreen-baseline.bin}"
BIN="${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-/home/$USER/code/spyro/spyro-1/build/SCUS-94228.cue}"

if [[ ! -f "$TRACE" ]]; then
    echo "[psx-oracle] trace not found: $TRACE" >&2
    echo "[psx-oracle] regenerate with scripts/pcsx-redux-trace.lua, see" >&2
    echo "             docs/ram-diff-vs-pcsxr.md for the workflow."   >&2
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

# Frame limit is a safety belt — psxtrace-eof normally terminates first.
LIMIT="${OPENPETE_FRAME_LIMIT:-2000}"

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
