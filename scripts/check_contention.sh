#!/usr/bin/env bash
# check_contention.sh — the presentation golden's own legs at full parallelism,
# repeated, as a measurement of capture survival under load.
#
# The corpus capture forks one game process per leg and caps them at
# CORPUS_JOBS (default nproc/2, see scripts/corpus-jobs.sh). Legs have been
# lost mid-run on a loaded machine: the process disappears, its tiles are
# never written, and the only trace is a MISSING row in the diff — a symptom
# that reads like a corpus-shape error rather than like a killed process, and
# that nothing measures on purpose. This harness makes it measurable: it runs
# the golden's own legs at CORPUS_JOBS=nproc, the worst case the corpus ever
# presents to the machine, for ROUNDS rounds, and keeps every round's log.
#
# What counts as a contention failure, per round:
#   - an exit status outside {0, 1}. 0 is a clean golden and 1 is a golden
#     divergence; a divergence is a rendering verdict, not a lost process, so
#     it is reported and does not fail this harness. Everything else is the
#     run dying: 124 (timeout), 137 (SIGKILL, which is what an OOM killer
#     leaves), 143 (SIGTERM), 139 (SIGSEGV), and any other signal-derived
#     status.
#   - a MISSING tile: the golden holds it and the run did not capture it, so
#     the leg that owned it did not finish.
#   - a "capture incomplete" line: a corpus leg returned nonzero, which is the
#     same lost leg caught one step earlier.
#
# Engagement, per round: the golden's own row census must report compared > 0.
# A round that compared nothing exercised no legs and proves nothing about
# contention, so it is refused as VACUOUS rather than counted as survival.
#
# Round line:
#   [contention] game=<g> round=<i>/<n> jobs=<n> exit=<rc> captured=<n>
#                compared=<n> missing=<n> incomplete=<n> verdict=OK|KILLED|VACUOUS
# Verdict line:
#   [contention] game=<g> rounds=<n> jobs=<n> killed=<n> vacuous=<n>
#                verdict=PASS|FAIL
#
# Usage: scripts/check_contention.sh [--game SLUG] [--rounds N] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing corpus manifest (the message
# names the path), 64 SKIP (the golden reported its own prereqs missing).
set -u
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

ROUNDS=3
BUILD_ARG=""
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_ARG="$a"; PG_BUILD_DIR="${a#--build-dir=}";;
        --rounds=*)    ROUNDS="${a#--rounds=}";;
    esac
    [[ "$prev" == "--rounds" ]] && ROUNDS="$a"
    prev="$a"
done
[[ "$ROUNDS" =~ ^[0-9]+$ && "$ROUNDS" -ge 1 ]] ||
    { echo "contention: FAIL (--rounds must be a positive integer, got '$ROUNDS')"; exit 1; }

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
# The golden reads both corpus manifests; loading them here fails a missing one
# before any round starts and reports the rows every round runs.
pg_manifest_load "$PG_CORPUS_MANIFEST" pipe
pg_manifest_load "$PG_FREECAM_MANIFEST" pipe
pg_manifest_dry_run_exit
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "contention: SKIP (binary or disc missing)"; exit 64; }

JOBS="$(nproc)"
OUT="${PG_BUILD_DIR:-build/linux-release}/contention/$GAME"
mkdir -p "$OUT"
# Every round's log goes before the first round, so a log from an earlier
# invocation can never be read as this one's evidence.
rm -f "$OUT/round-"*.log

killed=0 vacuous=0 skipped=0
for ((i = 1; i <= ROUNDS; i++)); do
    log="$OUT/round-$i.log"
    CORPUS_JOBS="$JOBS" scripts/check_presentation_golden.sh \
        --game "$GAME" $BUILD_ARG >"$log" 2>&1
    rc=$?

    captured=0 compared=0
    census="$(grep -m1 -oE 'captured=[0-9]+ compared=[0-9]+' "$log" || true)"
    if [[ -n "$census" ]]; then
        captured="${census#captured=}"; captured="${captured%% *}"
        compared="${census##*compared=}"
    fi
    missing="$(grep -c '^MISSING ' "$log" || true)"
    incomplete="$(grep -c 'capture incomplete' "$log" || true)"

    verdict=OK
    if (( rc == 64 )); then
        verdict=SKIP; skipped=$((skipped + 1))
    elif (( rc != 0 && rc != 1 )) || (( missing > 0 )) || (( incomplete > 0 )); then
        verdict=KILLED; killed=$((killed + 1))
    elif (( compared == 0 )); then
        verdict=VACUOUS; vacuous=$((vacuous + 1))
    fi
    echo "[contention] game=$GAME round=$i/$ROUNDS jobs=$JOBS exit=$rc" \
         "captured=$captured compared=$compared missing=$missing" \
         "incomplete=$incomplete verdict=$verdict  log=$log"
done

if (( skipped == ROUNDS )); then
    echo "[contention] game=$GAME rounds=$ROUNDS: every round reported missing" \
         "prereqs — SKIP"
    exit 64
fi
echo "[contention] game=$GAME rounds=$ROUNDS jobs=$JOBS killed=$killed" \
     "vacuous=$vacuous verdict=$( ((killed || vacuous)) && echo FAIL || echo PASS )"
(( killed || vacuous )) && exit 1
exit 0
