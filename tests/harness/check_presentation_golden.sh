#!/usr/bin/env bash
#
# check_presentation_golden.sh — the presentation-golden CTest gate.
#
# Captures the native -vk presentation corpus at the current (default) depth
# encoding and diffs it against the committed golden. A clean tree is pixel-
# identical (deterministic under llvmpipe), so ANY divergence is a
# presentation regression the gameplay oracle is blind to. This is the strict
# pixel-identity leg (Mode B); an intended visual change is re-blessed with
# tests/harness/presentation-corpus-promote.sh after reference-first adjudication.
#
# Usage:
#   tests/harness/check_presentation_golden.sh [--build-dir=DIR] [--filter=SUBSTR]
#                                        [--freecam-filter=SUBSTR] [--bless]
#                                        [--game SLUG]
#     --filter          bk2-name substring (default all bk2s)
#     --freecam-filter  freecam level substring (default all poses)
#                       Either filter scopes the run to part of the corpus: the
#                       golden tiles it does not capture are then reported as
#                       out of scope instead of missing.
#     --bless           (re)capture and promote --all instead of gating
#     --first-run       for a generated golden (never committed): when the
#                       golden holds no tile, copy the capture into it, write
#                       the identity baseline only if none exists (a committed
#                       baseline stays authoritative and is checked), and
#                       report `verdict: BLESSED`; a golden with tiles gates
#                       as without the flag
#     --game            game slug (default spyro-1); binary, disc, bk2 dir,
#                       manifests and golden all derive from it
#
# The candidate images are KEPT (build/presentation-candidate by default, or
# PRESENTATION_GOLDEN_OUT) so a selective promote costs nothing: adjudicate the
# diff, then run presentation-corpus-promote.sh against that directory. Only a
# whole-corpus re-bless needs --bless. PRESENTATION_GOLDEN_KEEP=0 discards them.
#
# A third leg, tests/harness/check_credits_interp.sh, runs after the corpus capture:
# sub-tick interpolation of the credits-roll letters (settled-line pose,
# fly-in position/depth/rotation, key stability), a case the corpus bk2s do
# not reach. It runs whether or not the diff passes, so one run reports every
# verdict; it is not gated by --filter.
#
# PRESENTATION_TIER selects the corpus tier (scripts/presentation-game.sh):
# its manifest, golden, identity baseline and default candidate directory. A
# tier without a freecam manifest runs no freecam leg, and the credits leg
# belongs to the presentation tier only. tests/harness/check_dragon_golden.sh
# runs the `dragons` tier through this script.
#
# Every run prints its row counts: images captured, and of those the ones the
# golden also holds. A run that captured nothing compared nothing, so it is
# refused as VACUOUS rather than reported as a pass.
#
# Exit: 0 = matches golden, the identity leg is within baseline and the credits
#       leg passes; 1 = divergence, identity exceed, credits failure, a
#       vacuous (zero-row) run, or a missing corpus manifest (either one; the
#       message names the path); 64 = SKIP (prereqs missing).
#
# See notes/plans/openpete-presentation-golden.md.
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

FILTER=""           # bk2-name substring; default = the whole fanned-out corpus
FREECAM_FILTER=""   # level substring for the freecam corpus; default = all poses
BLESS=0
FIRST_RUN=0
BUILD_ARG=""
for a in "$@"; do case "$a" in
  --bless) BLESS=1;;
  --first-run) FIRST_RUN=1;;
  --filter=*) FILTER="${a#--filter=}";;
  --freecam-filter=*) FREECAM_FILTER="${a#--freecam-filter=}";;
  --build-dir=*) BUILD_ARG="$a";;
esac; done
GAME="$(pg_parse_game "$@")" || exit 64
BUILD_DIR="build/linux-release"
[[ -n "$BUILD_ARG" ]] && BUILD_DIR="${BUILD_ARG#--build-dir=}"
PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$GAME" || exit 64
GAME_ARG="--game=$GAME"
TIER_LABEL="" TIER_ENV=""
[[ "$PG_TIER" != "presentation" ]] && TIER_LABEL=" $PG_TIER" TIER_ENV="PRESENTATION_TIER=$PG_TIER "
pg_manifest_load "$PG_CORPUS_MANIFEST" pipe
[[ -n "$PG_FREECAM_MANIFEST" ]] && pg_manifest_load "$PG_FREECAM_MANIFEST" pipe
pg_manifest_dry_run_exit

# The candidate images OUTLIVE the run, in a stable per-build path. A capture is
# ~24 minutes; discarding it on exit meant that adjudicating a divergence — or
# promoting a single tile — cost a full re-capture, and the temp dir was already
# gone by the time the diff had been read. One fixed directory (rather than a
# fresh mktemp each run) keeps exactly the last run without accumulating.
# PRESENTATION_GOLDEN_OUT overrides; PRESENTATION_GOLDEN_KEEP=0 restores the old
# discard-on-exit behaviour.
# Per game and tier (PG_CAND_DIR), so a run for one game or tier cannot wipe
# another's kept candidates.
OUT="${PRESENTATION_GOLDEN_OUT:-$PG_CAND_DIR}"
rm -rf "$OUT"; mkdir -p "$OUT"
if [[ "${PRESENTATION_GOLDEN_KEEP:-1}" == "0" ]]; then trap 'rm -rf "$OUT"' EXIT; fi

# Capture at the compiled DEFAULT depth encoding (now screen-affine), so the
# golden tracks whatever ships. The depth A/B is a separate manual tool
# (presentation-corpus.sh off = the legacy encoding via the kill switch).
# Both corpora write into the SAME dir so the diff covers their union: the bk2
# set (presentation-corpus.sh) + the debug-freecam poses (freecam-corpus.sh).
tests/harness/presentation-corpus.sh default "$OUT" "$FILTER" $BUILD_ARG "$GAME_ARG"
rc=$?
if [[ $rc -eq 64 ]]; then echo "[presentation-golden] SKIP (capture prereqs missing)"; exit 64; fi
# 65 = the bk2 corpus had no rows for this game. Not a failure on its own: the
# freecam corpus may still carry rows, and the row count below decides.
if [[ $rc -ne 0 && $rc -ne 65 ]]; then echo "[presentation-golden] FAIL: capture incomplete"; exit 1; fi

# The freecam corpus is additive: a manifest with no rows returns 0 (nothing to
# capture), a missing Vulkan loader returns 64 (skip that corpus, keep the bk2
# result), and a missing capture or a missing manifest (1) fails the gate.
if [[ -n "$PG_FREECAM_MANIFEST" ]]; then
  tests/harness/freecam-corpus.sh default "$OUT" "$FREECAM_FILTER" $BUILD_ARG "$GAME_ARG"
  frc=$?
  if [[ $frc -eq 1 ]]; then echo "[presentation-golden] FAIL: freecam capture incomplete"; exit 1; fi
fi

# Row census. `captured` counts the images this run produced; `compared` counts
# the golden tiles among them (the identity leg has no golden twin, so it is
# excluded from both the compare set and the golden count). A capture that
# produced nothing proves nothing about the renderer, so it is refused instead
# of falling through the diff as "golden and candidate are both empty".
captured=0 compared=0 goldrows=0
for f in "$OUT"/*-vk.png; do
  [[ -e "$f" ]] || continue
  captured=$((captured+1))
  [[ "$f" == *__ident__* ]] && continue
  [[ -f "$PG_GOLDEN/$(basename "$f")" ]] && compared=$((compared+1))
done
if [[ -d "$PG_GOLDEN" ]]; then
  for f in "$PG_GOLDEN"/*-vk.png; do [[ -e "$f" ]] && goldrows=$((goldrows+1)); done
fi
echo "[presentation-golden] $GAME$TIER_LABEL rows: captured=$captured compared=$compared golden=$goldrows"
if [[ $captured -eq 0 ]]; then
  echo "[presentation-golden] VACUOUS: $GAME captured 0 rows — nothing was rendered,"
  echo "  so nothing was verified. Populate $PG_CORPUS_MANIFEST"
  [[ -n "$PG_FREECAM_MANIFEST" ]] && echo "  (or $PG_FREECAM_MANIFEST)"
  echo "  and bless a golden into $PG_GOLDEN."
  exit 1
fi

if [[ $BLESS -eq 1 ]]; then
  tests/harness/presentation-corpus-promote.sh "$OUT" --all "$GAME_ARG"
  python3 tests/harness/presentation-ident-check.py "$OUT" --bless --game "$GAME" --baseline "$PG_IDENT_BASELINE"
  echo "[presentation-golden] blessed — review 'git diff --cached --stat' and commit"
  exit 0
fi

# First run of a generated golden: the golden holds no tile, so this run
# compares nothing and seeds it (not a PASS). The identity leg is a
# relation inside this capture, so it still runs against a committed baseline.
if [[ $FIRST_RUN -eq 1 && $goldrows -eq 0 ]]; then
  mkdir -p "$PG_GOLDEN"
  seeded=0
  for f in "$OUT"/*-vk.png; do
    [[ -e "$f" && "$f" != *__ident__* ]] || continue
    cp "$f" "$PG_GOLDEN/"; seeded=$((seeded+1))
  done
  if [[ -f "$PG_IDENT_BASELINE" ]]; then
    python3 tests/harness/presentation-ident-check.py "$OUT" --game "$GAME" --baseline "$PG_IDENT_BASELINE"
    irc=$?
    if [[ $irc -ne 0 && $irc -ne 64 ]]; then
      echo "[presentation-golden] FAIL: identity leg exceeds the committed baseline (see EXCEED lines above)."
      echo "verdict: FAIL (first run, $seeded tiles seeded, identity leg over baseline)"
      exit 1
    fi
  else
    python3 tests/harness/presentation-ident-check.py "$OUT" --bless --game "$GAME" --baseline "$PG_IDENT_BASELINE"
  fi
  echo "verdict: BLESSED (first run, $seeded tiles, nothing compared)"
  exit 0
fi

# Identity leg: the alpha-0 re-extract against the canon tile of the same
# capture (no golden). Runs whether or not the golden diff below passes, so
# one run reports both verdicts.
python3 tests/harness/presentation-ident-check.py "$OUT" --game "$GAME" --baseline "$PG_IDENT_BASELINE"
irc=$?

# Credits leg (no golden: the checks are relations between one run's own
# canonical and intermediate captures). Presentation tier only; 64 reads as
# skipped below.
crc=64
if [[ "$PG_TIER" == "presentation" ]]; then
  tests/harness/check_credits_interp.sh "$GAME_ARG"
  crc=$?
fi

# A filtered run captures part of the corpus on purpose, so the golden tiles it
# did not capture are out of scope rather than missing. An unfiltered run keeps
# the strict shape check: every golden tile must be captured.
SUBSET_ARG=()
[[ -n "$FILTER" || -n "$FREECAM_FILTER" ]] && SUBSET_ARG=(--expect-subset)
python3 tests/harness/presentation-corpus-diff.py "$OUT" --game "$GAME" --golden "$PG_GOLDEN" --diff-out "$OUT/diff" "${SUBSET_ARG[@]}"
drc=$?
if [[ $drc -eq 64 ]]; then echo "[presentation-golden] SKIP (diff prereqs missing)"; exit 64; fi
if [[ $crc -ne 0 && $crc -ne 64 ]]; then
  echo "[presentation-golden] FAIL: credits interpolation leg (see the credits-interp lines above)."
  [[ $drc -eq 0 ]] && exit 1
fi
if [[ $irc -ne 0 && $irc -ne 64 ]]; then
  echo "[presentation-golden] FAIL: identity leg exceeds its baseline (see EXCEED lines above)."
  echo "  A raised count is a sub-tick spine regression. Re-baseline only for a"
  echo "  deliberate change: tests/harness/presentation-ident-check.py $OUT --bless --baseline $PG_IDENT_BASELINE"
  [[ $drc -eq 0 ]] && exit 1
fi
if [[ $drc -ne 0 ]]; then
  echo "[presentation-golden] FAIL: candidate diverged from golden."
  echo "  Candidate images kept in: $OUT"
  echo "  If this is an INTENDED visual change, adjudicate reference-first, then"
  echo "  promote from the images ALREADY CAPTURED above — no re-capture needed:"
  echo "    ${TIER_ENV}tests/harness/presentation-corpus-promote.sh $OUT --game $GAME <tile-or-substring>...   # selective"
  echo "    ${TIER_ENV}tests/harness/presentation-corpus-promote.sh $OUT --game $GAME --all                    # whole corpus"
  exit 1
fi
[[ $crc -eq 64 && "$PG_TIER" == "presentation" ]] && echo "[presentation-golden] credits leg SKIPPED (see the credits-interp line above)"
echo "[presentation-golden] PASS: $GAME$TIER_LABEL corpus matches golden ($compared rows compared)"
exit 0
