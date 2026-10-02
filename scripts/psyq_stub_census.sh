#!/usr/bin/env bash
#
# psyq_stub_census.sh - which untranslated PSYQ symbols a game actually calls.
#
# `generated/psyq_stubs.c` emits an abort stub for every PSYQ symbol the
# recompiled code references; a thunk in the shared PSYQ-to-PsyCross layer
# overrides the ones that are implemented, and what survives as a weak
# definition in the linked executable is the untranslated surface. That list
# is static and includes symbols the game may never reach. This script
# separates the two: it boots the game once per level (--skip-to-level), runs
# a few seconds of live gameplay under OPENPETE_PSYQ_STUB_CONTINUE=1 - which
# logs each distinct stub once and returns instead of aborting - and reports
# every stub that was hit, with the levels that hit it, alongside the stubs
# nothing reached.
#
# A continued stub call does nothing, so the guest state after the first hit
# is not a state to draw conclusions from. This is a census of what to
# implement, not a test.
#
# Usage:
#   scripts/psyq_stub_census.sh <game> [--frames N] [--out DIR] [--level-filter SUB]
#     game          spyro-2 | spyro-3 | spyro-1
#     --frames N    VBLs per run (default 1200; the level is live from ~850)
#     --out DIR     per-level logs and the summary (default build/psyq-census/<game>)
#     --level-filter SUB   only levels whose name contains SUB
#
# Exit: 0 = census produced, 64 = SKIP (binary or disc missing), 1 = a run failed.
set -u

# Unattended launcher: nobody is holding the pad (scripts/fences/rumble_guard.py).
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/.."
# shellcheck source=scripts/corpus-jobs.sh
source scripts/corpus-jobs.sh

GAME="${1:-}"
[[ -z "$GAME" ]] && { echo "usage: $0 <game> [--frames N] [--out DIR] [--level-filter SUB]" >&2; exit 64; }
shift

FRAMES=1200
OUTDIR=""
FILTER=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --frames)       FRAMES="$2"; shift 2;;
    --out)          OUTDIR="$2"; shift 2;;
    --level-filter) FILTER="$2"; shift 2;;
    *) echo "psyq-census: unknown argument '$1'" >&2; exit 64;;
  esac
done

TAG="${GAME//-/}"
BIN="build/linux-release/games/$GAME/openpete-$TAG"
DISC="$(ls games/"$GAME"/"$GAME"/build/*.cue 2>/dev/null | head -1)"
OUTDIR="${OUTDIR:-build/psyq-census/$GAME}"

[[ -x "$BIN" ]]   || { echo "[psyq-census] SKIP: no binary at $BIN"; exit 64; }
[[ -n "$DISC" ]]  || { echo "[psyq-census] SKIP: no disc under games/$GAME/$GAME/build/"; exit 64; }

# Stale logs would be counted as this run's evidence.
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

# The untranslated surface as linked: weak psyq_* definitions no thunk
# overrode. `nm` prints 'W' for a surviving weak stub and 'T' for one a
# strong thunk replaced.
nm --defined-only "$BIN" | awk '$2=="W" && $3 ~ /^psyq_/ {print $3}' | sort -u \
  > "$OUTDIR/untranslated.txt"

# Levels straight from the game's own resolver ("  name (number)"), so the
# script needs no per-game table.
mapfile -t LEVELS < <("$BIN" --skip-to-level 2>&1 | awk '/^ +[a-z0-9_]+ +\([0-9]+\)$/ {print $1}')
[[ -n "$FILTER" ]] && mapfile -t LEVELS < <(printf '%s\n' "${LEVELS[@]}" | grep -- "$FILTER")
(( ${#LEVELS[@]} )) || { echo "[psyq-census] SKIP: $GAME lists no levels"; exit 64; }

run_level() {
  local level="$1" log="$OUTDIR/$1.log"
  OPENPETE_PSYQ_STUB_CONTINUE=1 "$BIN" \
      --headless --no-card --mute 1 \
      --skip-to-level "$level" --frame-limit "$FRAMES" \
      --disc "$DISC" --disc-cache 0 > "$log" 2>&1
  local rc=$?
  if (( rc != 0 )); then echo "[psyq-census] $level: exit $rc (see $log)" >&2; return 1; fi
  if ! grep -q 'loader took level' "$log"; then
    echo "[psyq-census] $level: never entered the level (see $log)" >&2; return 1; fi
  printf '%-20s %s\n' "$level" \
      "$(grep -c '^psyq-missing:' "$log") hit(s)"
}

echo "[psyq-census] $GAME: ${#LEVELS[@]} level(s), $FRAMES VBLs each, ${CORPUS_JOBS} at a time"
fail=0
for lvl in "${LEVELS[@]}"; do cj_run run_level "$lvl"; done
cj_wait || fail=1

SUMMARY="$OUTDIR/summary.txt"
{
  echo "psyq stub census - $GAME, ${#LEVELS[@]} level(s) x $FRAMES VBLs"
  echo
  echo "untranslated surface as linked: $(wc -l < "$OUTDIR/untranslated.txt") symbol(s)"
  echo
  echo "REACHED (symbol: levels that called it)"
  # Each hit line is "psyq-missing: <name> @ <addr> (ra=...)"; the log's
  # basename is the level.
  grep -H '^psyq-missing:' "$OUTDIR"/*.log 2>/dev/null \
    | sed 's|^.*/\([^/]*\)\.log:psyq-missing: \([^ ]*\) .*|\2 \1|' \
    | sort | awk '{ if ($1 != cur) { if (cur) print "  " cur ": " lv; cur=$1; lv=$2 }
                    else lv = lv " " $2 }
                  END { if (cur) print "  " cur ": " lv }'
  echo
  echo "NOT REACHED"
  comm -23 "$OUTDIR/untranslated.txt" \
       <(grep -h '^psyq-missing:' "$OUTDIR"/*.log 2>/dev/null | awk '{print $2}' | sort -u) \
    | sed 's/^/  /'
} > "$SUMMARY"

cat "$SUMMARY"
echo "[psyq-census] logs + summary: $OUTDIR"
exit $fail
