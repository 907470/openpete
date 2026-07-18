#!/usr/bin/env bash
#
# check_presentation_golden.sh — the presentation-golden CTest gate.
#
# Captures the native -vk presentation corpus at the current (default) depth
# encoding and diffs it against the committed golden. A clean tree is pixel-
# identical (deterministic under llvmpipe), so ANY divergence is a
# presentation regression the gameplay oracle is blind to. This is the strict
# pixel-identity leg (Mode B); an intended visual change is re-blessed with
# scripts/presentation-corpus-promote.sh after reference-first adjudication.
#
# Usage:
#   scripts/check_presentation_golden.sh [--build-dir=DIR] [--filter=SUBSTR] [--bless]
#     --bless  (re)capture and promote --all instead of gating (first-bless / re-bless)
#
# Exit: 0 = matches golden, 1 = divergence, 64 = SKIP (prereqs missing).
#
# See notes/plans/openpete-presentation-golden.md.
set -u
cd "$(dirname "$0")/.."

FILTER="cliff-town-vista"
BLESS=0
BUILD_ARG=""
for a in "$@"; do case "$a" in
  --bless) BLESS=1;;
  --filter=*) FILTER="${a#--filter=}";;
  --build-dir=*) BUILD_ARG="$a";;
esac; done

OUT="$(mktemp -d -t presentation-golden.XXXXXX)"
trap 'rm -rf "$OUT"' EXIT

# Capture at the default depth encoding (OPENPETE_DEPTH_AFFINE off = golden basis).
scripts/presentation-corpus.sh off "$OUT" "$FILTER" $BUILD_ARG
rc=$?
if [[ $rc -eq 64 ]]; then echo "[presentation-golden] SKIP (capture prereqs missing)"; exit 64; fi
if [[ $rc -ne 0 ]]; then echo "[presentation-golden] FAIL: capture incomplete"; exit 1; fi

if [[ $BLESS -eq 1 ]]; then
  scripts/presentation-corpus-promote.sh "$OUT" --all
  echo "[presentation-golden] blessed — review 'git diff --cached --stat' and commit"
  exit 0
fi

python3 scripts/presentation-corpus-diff.py "$OUT" --diff-out "$OUT/diff"
drc=$?
if [[ $drc -eq 64 ]]; then echo "[presentation-golden] SKIP (diff prereqs missing)"; exit 64; fi
if [[ $drc -ne 0 ]]; then
  echo "[presentation-golden] FAIL: candidate diverged from golden."
  echo "  If this is an INTENDED visual change, adjudicate reference-first then re-bless:"
  echo "    scripts/check_presentation_golden.sh --bless"
  exit 1
fi
echo "[presentation-golden] PASS: corpus matches golden"
exit 0
