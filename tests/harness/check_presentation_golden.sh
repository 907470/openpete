#!/usr/bin/env bash
#
# check_presentation_golden.sh - the presentation-golden CTest gate.
#
# Captures the native -vk presentation corpus at the current (default) depth
# encoding and compares each tile's verdict digest (decoded RGB payload, mode
# and size) against its row of the tier's committed blessed.tsv. The reference
# images are not in the tree: a passing run reads none, and a divergent row's
# reference is read from the local store or the archive for its heat diff
# (scripts/presentation-blessed.py). A clean tree is pixel-identical
# (deterministic under llvmpipe), so ANY divergence is a presentation
# regression the gameplay oracle is blind to. This is the strict
# pixel-identity leg (Mode B); an intended visual change is re-blessed with
# tests/harness/presentation-corpus-promote.sh after reference-first adjudication.
#
# Usage:
#   tests/harness/check_presentation_golden.sh [--build-dir=DIR] [--filter=SUBSTR]
#                                        [--freecam-filter=SUBSTR] [--bless]
#                                        [--game SLUG]
#   tests/harness/check_presentation_golden.sh --verdict-only DIR [--game SLUG]
#   tests/harness/check_presentation_golden.sh --materialize-ref [--game SLUG]
#                                        [--tier T] [--at REV] <tile>...
#     --filter          bk2-name substring (default all bk2s)
#     --freecam-filter  freecam level substring (default all poses)
#                       Either filter scopes the run to part of the corpus: the
#                       golden tiles it does not capture are then reported as
#                       out of scope instead of missing.
#     --bless           (re)capture, promote --all (rewrite the changed rows of
#                       blessed.tsv, file the tiles into the store) and
#                       re-baseline the identity leg, instead of gating
#     --game            game slug (default spyro-1); binary, disc, bk2 dir,
#                       manifests and golden all derive from it
#     --environment-accept=REASON
#                       with --bless: admit a capture environment that departs
#                       from the manifest header (scripts/presentation-blessed.py,
#                       Environment); the header records REASON
#     --verdict-only DIR
#                       capture nothing: decide the verdict of the candidate
#                       directory DIR (a whole-corpus capture, taken elsewhere)
#                       with the post-capture steps a capturing run takes over
#                       its own candidates: the row census, the identity leg,
#                       the credits leg's recorded result and the strict diff
#                       (never --expect-subset). Takes no --bless, --filter or
#                       --freecam-filter. DIR must lie under this tree or the
#                       store's parent directory, the paths the capture image
#                       mounts; outside the image the run enters it through
#                       the wrapper's `run`, which builds no engine.
#
# The verdict is defined over the pinned capture image
# (tests/harness/linux-capture.Dockerfile). Run outside it, the script
# re-runs itself inside the image through scripts/golden-capture-container.sh,
# which builds the engine into build/linux-release-container with the image's
# toolchain (any --build-dir= is replaced by that directory) and holds a lock
# on it; this process stays the host-side parent for the whole run. Without a container
# engine or a current image the run reports SKIP (64) and prints the manifest
# header's environment beside this machine's. Inside the image every run prints
# the same comparison for its capture; a bless or promote whose environment
# departs from the header is refused.
#
# The candidate images are KEPT (build/presentation-candidate by default, or
# PRESENTATION_GOLDEN_OUT) so a selective promote costs nothing: adjudicate the
# diff, then run presentation-corpus-promote.sh against that directory. Only a
# whole-corpus re-bless needs --bless. PRESENTATION_GOLDEN_KEEP=0 discards them.
# Before capturing, the run writes capture-record.txt into the candidate
# directory (the working copy's tree over the build's source roots and the
# recomp identity), from which a promote takes the blessed revision. A tree
# without .git (a copy of a checkout) cannot measure those fields: the run
# reads them from the file OPENPETE_CAPTURE_FIELDS names (`key: value` lines,
# as `scripts/presentation-blessed.py capture-fields` writes them in the
# checkout), and a copy without that variable fails before capturing.
#
# A third leg, tests/harness/check_credits_interp.sh, runs after the corpus capture:
# sub-tick interpolation of the credits-roll letters (settled-line pose,
# fly-in position/depth/rotation, key stability), a case the corpus bk2s do
# not reach. It runs whether or not the diff passes, so one run reports every
# verdict; it is not gated by --filter. The leg is a capture: its exit code
# and output lines are recorded in credits-verdict.txt in the candidate
# directory (first line `rc=N`), and the verdict reads its result from that
# file, so --verdict-only decides a candidate captured elsewhere the same way.
# On a tier that runs the leg, an absent or unreadable file is a failure.
#
# Every verdict writes verdict-report.txt into the candidate directory: the
# verdict and exit code, the row census, the sha256 of the blessed.tsv and
# identity baseline read, the credits, identity and diff exit codes, the
# divergent, missing and extra tiles and the identity pairs over baseline
# (both members). The identity and diff output is kept beside it in
# verdict-ident.log and verdict-diff.log.
#
# PRESENTATION_TIER selects the corpus tier (scripts/presentation-game.sh):
# its manifest, golden, identity baseline and default candidate directory. A
# tier without a freecam manifest runs no freecam leg, and the credits leg
# belongs to the presentation tier only. tests/harness/check_dragon_golden.sh
# runs the `dragons` tier through this script.
#
# Every run prints its row counts: images captured, the captured tiles that
# name a blessed.tsv row, and the manifest's row count. A run that captured
# nothing compared nothing, so it is refused as VACUOUS rather than reported as
# a pass; a run in which no captured tile names a row is refused by the diff.
#
# Exit: 0 = matches golden, the identity leg is within baseline and the credits
#       leg passes; 1 = divergence, identity exceed, credits failure (or no
#       credits-verdict.txt on the presentation tier), a vacuous (zero-row)
#       run, a --verdict-only DIR that is absent or not mounted in the
#       image, a missing corpus manifest (either one; the message names the
#       path), or a blessed.tsv absent or with zero rows;
#       64 = SKIP (prereqs missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

# --materialize-ref [--game G] [--tier T] [--at REV] <tile>...: rebuild the
# blessed revision of the named rows in a throwaway worktree and re-capture
# their tiles (scripts/presentation-materialize.py). Manual; refused under ctest.
if [[ "${1:-}" == "--materialize-ref" ]]; then
  shift; exec python3 scripts/presentation-materialize.py "$@"
fi

FILTER=""           # bk2-name substring; default = the whole fanned-out corpus
FREECAM_FILTER=""   # level substring for the freecam corpus; default = all poses
BLESS=0
BUILD_ARG=""
ENV_ACCEPT=""
VERDICT_ONLY=""     # candidate directory decided without a capture
next_verdict=0
for a in "$@"; do
  if (( next_verdict )); then VERDICT_ONLY="$a"; next_verdict=0; continue; fi
  case "$a" in
  --bless) BLESS=1;;
  --filter=*) FILTER="${a#--filter=}";;
  --freecam-filter=*) FREECAM_FILTER="${a#--freecam-filter=}";;
  --build-dir=*) BUILD_ARG="$a";;
  --environment-accept=*) ENV_ACCEPT="${a#--environment-accept=}";;
  --verdict-only) next_verdict=1;;
  --verdict-only=*) VERDICT_ONLY="${a#--verdict-only=}";;
esac; done
if (( next_verdict )) || [[ "$*" == *--verdict-only* && -z "$VERDICT_ONLY" ]]; then
  echo "[presentation-golden] FAIL: --verdict-only takes a candidate directory"; exit 1
fi
if [[ -n "$VERDICT_ONLY" && ( $BLESS -eq 1 || -n "$FILTER" || -n "$FREECAM_FILTER" ) ]]; then
  echo "[presentation-golden] FAIL: --verdict-only takes no --bless, --filter or --freecam-filter"; exit 1
fi
GAME="$(pg_parse_game "$@")" || exit 64
BUILD_DIR="build/linux-release"
[[ -n "$BUILD_ARG" ]] && BUILD_DIR="${BUILD_ARG#--build-dir=}"
PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$GAME" || exit 64
if ! pg_in_capture_image; then
  if [[ -n "$VERDICT_ONLY" ]]; then
    # The container sees this tree and the store's parent directory only.
    vdir="$(realpath -m -- "$VERDICT_ONLY")"
    store="${OPENPETE_GOLDEN_REF_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/openpete/golden-ref}"
    sparent="$(dirname "$(realpath -m -- "$store")")"
    if [[ "$vdir/" != "$PWD/"* && "$vdir/" != "$sparent/"* ]]; then
      echo "[presentation-golden] FAIL: --verdict-only $VERDICT_ONLY lies outside $PWD and $sparent, which the capture image mounts"
      exit 1
    fi
    pg_enter_capture_image --no-build tests/harness/check_presentation_golden.sh "$@"
  else
    pg_enter_capture_image tests/harness/check_presentation_golden.sh "$@"
  fi
  rc=$?
  if [[ $rc -eq 64 ]]; then
    echo "[presentation-golden] SKIP: no verdict outside the capture image; environment for information:"
    python3 scripts/presentation-blessed.py environment "$GAME" --tier "$PG_TIER" --build-dir "$BUILD_DIR"
  fi
  exit $rc
fi
GAME_ARG="--game=$GAME"
TIER_LABEL="" TIER_ENV=""
[[ "$PG_TIER" != "presentation" ]] && TIER_LABEL=" $PG_TIER" TIER_ENV="PRESENTATION_TIER=$PG_TIER "
pg_manifest_load "$PG_CORPUS_MANIFEST" pipe
[[ -n "$PG_FREECAM_MANIFEST" ]] && pg_manifest_load "$PG_FREECAM_MANIFEST" pipe
pg_manifest_dry_run_exit

# The candidate images OUTLIVE the run, in a stable per-build path. A capture is
# ~24 minutes; discarding it on exit meant that adjudicating a divergence - or
# promoting a single tile - cost a full re-capture, and the temp dir was already
# gone by the time the diff had been read. One fixed directory (rather than a
# fresh mktemp each run) keeps exactly the last run without accumulating.
# PRESENTATION_GOLDEN_OUT overrides; PRESENTATION_GOLDEN_KEEP=0 restores the old
# discard-on-exit behaviour.
# Per game and tier (PG_CAND_DIR), so a run for one game or tier cannot wipe
# another's kept candidates.
CREDITS_NAME="credits-verdict.txt"
REPORT_NAME="verdict-report.txt"

# ---- post-capture steps ------------------------------------------------------
#
# One implementation for a capturing run over its own OUT and --verdict-only
# over a candidate directory captured elsewhere: vr_census, then vr_verdict.

# Row census. `captured` counts the images in OUT; `compared` counts the
# tiles that name a row of blessed.tsv and `golden` the manifest's rows (the
# identity leg has no row, so it is excluded from both). A capture that
# produced nothing proves nothing about the renderer, so it is refused here;
# a capture no row matches is refused by the diff. Sets captured, compared,
# goldrows.
captured=0 compared=0 goldrows=0 irc="not run" crc="not run" drc="not run"
vr_census() {
  local f census
  captured=0
  for f in "$OUT"/*-vk.png; do [[ -e "$f" ]] && captured=$((captured+1)); done
  census="$(python3 scripts/presentation-blessed.py census "$GAME" --tier "$PG_TIER" --candidate "$OUT")" \
    || { echo "[presentation-golden] FAIL: $PG_BLESSED unreadable (see above)"; vr_finish 1; }
  goldrows="${census#rows=}"; goldrows="${goldrows%% *}"
  compared="${census##*matched=}"
  echo "[presentation-golden] $GAME$TIER_LABEL rows: captured=$captured compared=$compared golden=$goldrows"
  if [[ $captured -eq 0 ]]; then
    echo "[presentation-golden] VACUOUS: $GAME captured 0 rows - nothing was rendered,"
    echo "  so nothing was verified. Populate $PG_CORPUS_MANIFEST"
    [[ -n "$PG_FREECAM_MANIFEST" ]] && echo "  (or $PG_FREECAM_MANIFEST)"
    echo "  and bless rows into $PG_BLESSED."
    vr_finish 1
  fi
  return 0
}

# Record the credits leg's exit code and output lines in OUT. args: rc log
vr_record_credits() {
  { echo "rc=$1"; cat "$2"; } >"$OUT/$CREDITS_NAME.tmp" && mv -f "$OUT/$CREDITS_NAME.tmp" "$OUT/$CREDITS_NAME"
}

# The credits leg's recorded result: crc = its exit code, 64 on a tier
# without the leg, 1 when the file is absent or its first line is not
# `rc=N`.
vr_read_credits() {
  crc=64
  [[ "$PG_TIER" == "presentation" ]] || return 0
  local first
  if [[ ! -f "$OUT/$CREDITS_NAME" ]]; then
    echo "[presentation-golden] FAIL: $OUT/$CREDITS_NAME absent; the credits leg's result was not recorded"
    crc=1; return 0
  fi
  first="$(head -n 1 "$OUT/$CREDITS_NAME")"
  if [[ ! "$first" =~ ^rc=([0-9]+)$ ]]; then
    echo "[presentation-golden] FAIL: $OUT/$CREDITS_NAME: first line '$first' is not rc=N"
    crc=1; return 0
  fi
  crc="${BASH_REMATCH[1]}"
  if [[ -n "$VERDICT_ONLY" ]]; then
    echo "[presentation-golden] credits leg recorded rc=$crc:"
    tail -n +2 "$OUT/$CREDITS_NAME" | sed 's/^/  /'
  fi
}

# Write OUT/verdict-report.txt and exit with rc. The lists are read from the
# identity and diff logs of this run.
vr_finish() {
  local rc="$1" verdict=FAIL
  [[ $rc -eq 0 ]] && verdict=PASS
  [[ $rc -eq 64 ]] && verdict=SKIP
  {
    echo "verdict: $verdict"
    echo "exit: $rc"
    echo "game: $GAME"
    echo "tier: $PG_TIER"
    echo "mode: $([[ -n "$VERDICT_ONLY" ]] && echo verdict-only || echo capture)"
    echo "candidate: $OUT"
    echo "captured: $captured"
    echo "compared: $compared"
    echo "manifest-rows: $goldrows"
    echo "blessed-sha256: $(sha256sum "$PG_BLESSED" 2>/dev/null | cut -d' ' -f1) $PG_BLESSED"
    echo "ident-baseline-sha256: $(sha256sum "$PG_IDENT_BASELINE" 2>/dev/null | cut -d' ' -f1) $PG_IDENT_BASELINE"
    echo "credits-rc: $crc"
    echo "identity-rc: $irc"
    echo "diff-rc: $drc"
    [[ -f "$OUT/verdict-diff.log" ]] && awk '
      $1 == "DIVERGE" { print "divergent: " $2 " " substr($0, index($0, $3)) }
      $1 == "MISSING" { print "missing: " $2 }
      $1 == "EXTRA"   { print "extra: " $2 }' "$OUT/verdict-diff.log"
    [[ -f "$OUT/verdict-ident.log" ]] && awk '
      $1 == "EXCEED" || $1 == "NOCANON" {
        canon = $2; sub(/__ident__/, "__canon__", canon)
        print "identity-" tolower($1) ": " $2 " " canon " " $3 " " $4 }' "$OUT/verdict-ident.log"
  } >"$OUT/$REPORT_NAME"
  echo "[presentation-golden] report: $OUT/$REPORT_NAME"
  exit "$rc"
}

# Identity leg, the credits leg's recorded result and the strict diff over
# OUT, then the verdict; exits. The identity leg and the diff both run
# whatever the other reports, so one run reports every verdict. Their
# previous outputs in OUT (heat diffs, logs, report) are deleted first.
vr_verdict() {
  rm -rf "$OUT/diff"
  rm -f "$OUT/verdict-ident.log" "$OUT/verdict-diff.log" "$OUT/$REPORT_NAME"
  # Identity leg: the alpha-0 re-extract against the canon tile of the same
  # capture (no golden).
  python3 tests/harness/presentation-ident-check.py "$OUT" --game "$GAME" --baseline "$PG_IDENT_BASELINE" \
    | tee "$OUT/verdict-ident.log"
  irc=${PIPESTATUS[0]}
  vr_read_credits
  # A filtered run captures part of the corpus on purpose, so the golden
  # tiles it did not capture are out of scope rather than missing. An
  # unfiltered run and --verdict-only keep the strict shape check: every
  # golden tile must be captured.
  local -a subset=()
  [[ -n "$FILTER" || -n "$FREECAM_FILTER" ]] && subset=(--expect-subset)
  python3 tests/harness/presentation-corpus-diff.py "$OUT" --game "$GAME" --manifest "$PG_BLESSED" --diff-out "$OUT/diff" "${subset[@]}" \
    | tee "$OUT/verdict-diff.log"
  drc=${PIPESTATUS[0]}
  if [[ $drc -eq 64 ]]; then echo "[presentation-golden] SKIP (diff prereqs missing)"; vr_finish 64; fi
  if [[ $crc -ne 0 && $crc -ne 64 ]]; then
    echo "[presentation-golden] FAIL: credits interpolation leg (see the credits-interp lines above)."
    [[ $drc -eq 0 ]] && vr_finish 1
  fi
  if [[ $irc -ne 0 && $irc -ne 64 ]]; then
    echo "[presentation-golden] FAIL: identity leg exceeds its baseline (see EXCEED lines above)."
    echo "  A raised count is a sub-tick spine regression. Re-baseline only for a"
    echo "  deliberate change: tests/harness/presentation-ident-check.py $OUT --bless --baseline $PG_IDENT_BASELINE"
    [[ $drc -eq 0 ]] && vr_finish 1
  fi
  if [[ $drc -ne 0 ]]; then
    echo "[presentation-golden] FAIL: candidate diverged from golden."
    echo "  Candidate images kept in: $OUT"
    echo "  If this is an INTENDED visual change, adjudicate reference-first, then"
    echo "  promote from the images ALREADY CAPTURED above - no re-capture needed:"
    echo "    ${TIER_ENV}tests/harness/presentation-corpus-promote.sh $OUT --game $GAME <tile-or-substring>...   # selective"
    echo "    ${TIER_ENV}tests/harness/presentation-corpus-promote.sh $OUT --game $GAME --all                    # whole corpus"
    vr_finish 1
  fi
  [[ $crc -eq 64 && "$PG_TIER" == "presentation" ]] && echo "[presentation-golden] credits leg SKIPPED (see the credits-interp line above)"
  echo "[presentation-golden] PASS: $GAME$TIER_LABEL corpus matches golden ($compared rows compared)"
  vr_finish 0
}

if [[ -n "$VERDICT_ONLY" ]]; then
  OUT="$VERDICT_ONLY"
  if [[ ! -d "$OUT" ]]; then
    echo "[presentation-golden] FAIL: --verdict-only: no candidate directory $OUT"; exit 1
  fi
  vr_census
  python3 scripts/presentation-blessed.py environment "$GAME" --tier "$PG_TIER" --candidate "$OUT"
  vr_verdict
fi

OUT="${PRESENTATION_GOLDEN_OUT:-$PG_CAND_DIR}"
rm -rf "$OUT"; mkdir -p "$OUT"
if [[ "${PRESENTATION_GOLDEN_KEEP:-1}" == "0" ]]; then trap 'rm -rf "$OUT"' EXIT; fi
FIELDS_ARG=()
if [[ -n "${OPENPETE_CAPTURE_FIELDS:-}" ]]; then
  FIELDS_ARG=(--fields "$OPENPETE_CAPTURE_FIELDS")
elif [[ ! -e .git ]]; then
  echo "[presentation-golden] FAIL: $PWD has no .git, so the capture record's tree fields cannot be measured here; set OPENPETE_CAPTURE_FIELDS to a file written by scripts/presentation-blessed.py capture-fields in the checkout"
  exit 1
fi
python3 scripts/presentation-blessed.py capture-record "$GAME" --tier "$PG_TIER" "$OUT" --build-dir "$BUILD_DIR" "${FIELDS_ARG[@]}" \
  || { echo "[presentation-golden] FAIL: capture record not written"; exit 1; }

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

vr_census

# A refused promote stops the bless before the identity baseline is rewritten,
# so the baseline is never re-blessed against a manifest the promote did not
# write.
ACCEPT_ARG=()
[[ -n "$ENV_ACCEPT" ]] && ACCEPT_ARG=("--environment-accept=$ENV_ACCEPT")
python3 scripts/presentation-blessed.py environment "$GAME" --tier "$PG_TIER" --candidate "$OUT"
if [[ $BLESS -eq 1 ]]; then
  PRESENTATION_TIER="$PG_TIER" tests/harness/presentation-corpus-promote.sh "$OUT" --all "$GAME_ARG" "${ACCEPT_ARG[@]}" \
    || { echo "[presentation-golden] FAIL: promote refused; identity baseline left as is"; exit 1; }
  python3 tests/harness/presentation-ident-check.py "$OUT" --bless --game "$GAME" --baseline "$PG_IDENT_BASELINE" "${ACCEPT_ARG[@]}" || exit 1
  git add -- "$PG_IDENT_BASELINE"
  echo "[presentation-golden] blessed - review 'git diff --cached --stat' and commit"
  exit 0
fi

# Credits leg (no golden: the checks are relations between one run's own
# canonical and intermediate captures). Presentation tier only. Its result is
# recorded in OUT and read back by vr_verdict.
if [[ "$PG_TIER" == "presentation" ]]; then
  credits_log="$(mktemp)"
  PG_BUILD_DIR="$BUILD_DIR" tests/harness/check_credits_interp.sh "$GAME_ARG" | tee "$credits_log"
  vr_record_credits "${PIPESTATUS[0]}" "$credits_log"
  rm -f "$credits_log"
fi
vr_verdict
