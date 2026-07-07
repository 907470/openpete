#!/usr/bin/env bash
# check_dist_first_boot.sh — the distribution gate (D4; plan
# openpete-distribution-local-recomp).
#
# Proves the SHIPPED artifact works: package the Linux dist archive,
# extract it into a clean directory, --ingest the reference disc, let
# first boot extract+recompile+link the module, then replay the full
# 20k hardware oracle against the dist exe + its self-built module.
# This is the closest thing to "a stranger downloaded the zip and it
# worked" that CI can check.
#
# Exit 0 = PASS; 2 = failure; 64 = prerequisites missing.
set -euo pipefail
cd "$(dirname "$0")/.."

DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
TRACE=tests/golden/spyro-1/psxtrace-demo-20k.bin
[[ -f "$DISC" ]] || { echo "[dist-gate] disc missing: $DISC" >&2; exit 64; }
[[ -f "$TRACE" ]] || { echo "[dist-gate] golden missing: $TRACE" >&2; exit 64; }
[[ -x build/linux-release-shared/games/spyro-1/openpete-spyro1 ]] \
    || { echo "[dist-gate] build linux-release-shared first" >&2; exit 64; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/openpete-dist-gate.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "[dist-gate] packaging..."
scripts/package-dist.sh linux --out "$WORK/dist" >/dev/null

archive=$(ls "$WORK"/dist/*.tar.xz)
echo "[dist-gate] extracting $(basename "$archive") into a clean dir..."
mkdir -p "$WORK/install"
tar xJf "$archive" -C "$WORK/install"
appdir=$(ls -d "$WORK"/install/*/)

echo "[dist-gate] first boot (--ingest)..."
DISC_ABS=$(realpath "$DISC")
TRACE_ABS=$(realpath "$TRACE")
( cd "$appdir" && ./openpete-spyro1 --headless --frame-limit 300 \
      --ingest "$DISC_ABS" --max-runtime-seconds 900 ) \
      > "$WORK/first-boot.log" 2>&1 \
    || { echo "[dist-gate] FAIL: first boot did not complete" >&2;
         tail -20 "$WORK/first-boot.log" >&2; exit 2; }

module=$(ls "$appdir"/library/*/modules/*/spyro1-recomp.so 2>/dev/null | head -1)
[[ -n "$module" ]] || { echo "[dist-gate] FAIL: no module built" >&2; exit 2; }

echo "[dist-gate] full 20k oracle on the dist exe + self-built module..."
( cd "$appdir" && ./openpete-spyro1 --headless \
      --run-recomp 0x80012204 \
      --psxtrace-verify "$TRACE_ABS" \
      --frame-limit 200000 ) > "$WORK/oracle.log" 2>&1 \
    || { echo "[dist-gate] FAIL: oracle divergence or crash" >&2;
         tail -20 "$WORK/oracle.log" >&2; exit 2; }

echo "[dist-gate] PASS — packaged artifact: extract → ingest → self-build → oracle-clean"
