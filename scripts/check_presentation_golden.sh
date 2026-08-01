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
#   scripts/check_presentation_golden.sh [--build-dir=DIR] [--filter=SUBSTR]
#                                        [--freecam-filter=SUBSTR] [--bless]
#     --filter          bk2-name substring (default cliff-town-vista, bring-up scope)
#     --freecam-filter  freecam level substring (default all poses)
#     --bless           (re)capture and promote --all instead of gating
#
# The candidate images are KEPT (build/presentation-candidate by default, or
# PRESENTATION_GOLDEN_OUT) so a selective promote costs nothing: adjudicate the
# diff, then run presentation-corpus-promote.sh against that directory. Only a
# whole-corpus re-bless needs --bless. PRESENTATION_GOLDEN_KEEP=0 discards them.
#
# Exit: 0 = matches golden, 1 = divergence, 64 = SKIP (prereqs missing).
#
# See notes/plans/openpete-presentation-golden.md.
set -u
cd "$(dirname "$0")/.."

FILTER=""           # bk2-name substring; default = the whole fanned-out corpus
FREECAM_FILTER=""   # level substring for the freecam corpus; default = all poses
BLESS=0
BUILD_ARG=""
for a in "$@"; do case "$a" in
  --bless) BLESS=1;;
  --filter=*) FILTER="${a#--filter=}";;
  --freecam-filter=*) FREECAM_FILTER="${a#--freecam-filter=}";;
  --build-dir=*) BUILD_ARG="$a";;
esac; done

# The candidate images OUTLIVE the run, in a stable per-build path. A capture is
# ~24 minutes; discarding it on exit meant that adjudicating a divergence — or
# promoting a single tile — cost a full re-capture, and the temp dir was already
# gone by the time the diff had been read. One fixed directory (rather than a
# fresh mktemp each run) keeps exactly the last run without accumulating.
# PRESENTATION_GOLDEN_OUT overrides; PRESENTATION_GOLDEN_KEEP=0 restores the old
# discard-on-exit behaviour.
OUT="${PRESENTATION_GOLDEN_OUT:-build/presentation-candidate}"
rm -rf "$OUT"; mkdir -p "$OUT"
if [[ "${PRESENTATION_GOLDEN_KEEP:-1}" == "0" ]]; then trap 'rm -rf "$OUT"' EXIT; fi

# Capture at the compiled DEFAULT depth encoding (now screen-affine), so the
# golden tracks whatever ships. The depth A/B is a separate manual tool
# (presentation-corpus.sh off = the legacy encoding via the kill switch).
# Both corpora write into the SAME dir so the diff covers their union: the bk2
# set (presentation-corpus.sh) + the debug-freecam poses (freecam-corpus.sh).
scripts/presentation-corpus.sh default "$OUT" "$FILTER" $BUILD_ARG
rc=$?
if [[ $rc -eq 64 ]]; then echo "[presentation-golden] SKIP (capture prereqs missing)"; exit 64; fi
if [[ $rc -ne 0 ]]; then echo "[presentation-golden] FAIL: capture incomplete"; exit 1; fi

# The freecam corpus is additive: an absent/empty manifest returns 0 (nothing to
# capture), a missing Vulkan loader returns 64 (skip that corpus, keep the bk2
# result), and only a genuine missing capture (1) fails the gate.
scripts/freecam-corpus.sh default "$OUT" "$FREECAM_FILTER" $BUILD_ARG
frc=$?
if [[ $frc -eq 1 ]]; then echo "[presentation-golden] FAIL: freecam capture incomplete"; exit 1; fi

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
  echo "  Candidate images kept in: $OUT"
  echo "  If this is an INTENDED visual change, adjudicate reference-first, then"
  echo "  promote from the images ALREADY CAPTURED above — no re-capture needed:"
  echo "    scripts/presentation-corpus-promote.sh $OUT <tile-or-substring>...   # selective"
  echo "    scripts/presentation-corpus-promote.sh $OUT --all                    # whole corpus"
  exit 1
fi
echo "[presentation-golden] PASS: corpus matches golden"
exit 0
