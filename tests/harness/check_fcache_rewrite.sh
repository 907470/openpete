#!/usr/bin/env bash
# check_fcache_rewrite.sh - the native moby face-decode cache follows an
# in-place rewrite of a faceblock's texture words.
#
# The Tier-1 face cache (games/common/renderer/moby.c, fcache_get) serves a
# decoded faceblock while guest RAM holds the bytes it was decoded from. A
# writer that rewrites a face record in place mid-level (a mod refilling a
# faceblock it owns, a level load into the same address) must get a fresh
# decode.
#
# Instrument: OPENPETE_FCACHE_CHECK=1. Once per faceblock on a canonical
# pass the game rewrites the stream's last textured record's Tiledef in guest
# RAM (UV, CLUT and tpage bits), asks the cache for the faceblock, compares it
# with an uncached decode of the same bytes, restores the words and compares
# again, and prints
#   [fcache-check] vbl=<n> fb=<addr> faces=<n> rewritten=ok|STALE restored=ok|STALE
#                  checked=<n> ok=<n> fail=<n> skip=<n>
#
# Rows: Spyro 1 artisans, Spyro 2 glimmer, Spyro 3 sunrise_spring, headless,
# loaded with --skip-to-level and run to VBL 2400. A native screenshot at VBL
# 2300 keeps the native extraction, and with it the cache, running headless.
#
# Pass per row: at least one faceblock checked (engaged) and no STALE line.
#
# Usage: tests/harness/check_fcache_rewrite.sh [--out DIR] [--filter SUBSTR]
#   OPENPETE_BIN selects another binary (the pre-fix build, for the negative leg).
# Exit: 0 = PASS, 2 = FAIL (a stale decode, a row with nothing checked, or a
#       failed run), 64 = SKIP (binary or disc missing).
set -uo pipefail
export OPENPETE_VIBRATION=0   # unattended runs (scripts/fences/rumble_guard.py)

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh
# shellcheck source=scripts/lib/sandbox.sh
source scripts/lib/sandbox.sh

OUT="build/fcache-rewrite" FILTER=""
while (( $# )); do
  case "$1" in
    --out) OUT="$2"; shift 2;;
    --out=*) OUT="${1#--out=}"; shift;;
    --filter) FILTER="$2"; shift 2;;
    --filter=*) FILTER="${1#--filter=}"; shift;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 64;;
    *) echo "check_fcache_rewrite: unknown argument '$1'" >&2; exit 64;;
  esac
done
mkdir -p "$OUT"

# game|level
ROWS=(
  "spyro-1|artisans"
  "spyro-2|glimmer"
  "spyro-3|sunrise_spring"
)

fail=0 ran=0
for row in "${ROWS[@]}"; do
  IFS='|' read -r game level <<<"$row"
  name="$game-$level"
  [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
  pg_resolve "$game" || exit 64
  [[ -x "$PG_BIN" ]] || { echo "[fcache-rewrite] SKIP: no binary at $PG_BIN"; exit 64; }
  [[ -f "$PG_DISC" ]] || { echo "[fcache-rewrite] SKIP: no disc at $PG_DISC"; exit 64; }
  log="$OUT/$name.log" shot="$OUT/$name.png"
  rm -f "$log" "$shot"
  OPENPETE_SANDBOX="$(op_sandbox_run_root)" OPENPETE_FCACHE_CHECK=1 \
    timeout -k 10 600 "$PG_BIN" --game "$game" --headless --no-config --disc "$PG_DISC" \
      --disc-cache 0 --renderer vk --mute 1 --aspect 4:3 --lod original --no-card \
      "${PG_RUN_ARGS[@]}" --skip-to-level "$level" \
      --screenshot-native-only --screenshot-on-frame 2300 --screenshot-output "$shot" \
      --frame-limit 2400 > "$log" 2>&1
  rc=$?
  [[ -f "$log" ]] || { echo "[fcache-rewrite] $name: FAIL (no log)"; fail=1; continue; }
  ran=$((ran + 1))
  checked=$(grep -c '^\[fcache-check\]' "$log")
  stale=$(grep '^\[fcache-check\]' "$log" | grep -c 'STALE')
  last=$(grep '^\[fcache-check\]' "$log" | tail -1 | grep -o 'checked=.*')
  if (( rc != 0 )); then
    echo "[fcache-rewrite] $name: FAIL (exit $rc; log $log)"; fail=1
  elif (( checked == 0 )); then
    echo "[fcache-rewrite] $name: FAIL (VACUOUS: no faceblock checked; log $log)"; fail=1
  elif (( stale > 0 )); then
    echo "[fcache-rewrite] $name: FAIL ($stale of $checked faceblocks STALE; $last)"; fail=1
  else
    echo "[fcache-rewrite] $name: PASS ($last)"
  fi
done
(( ran > 0 )) || { echo "[fcache-rewrite] FAIL: no row ran"; exit 2; }
(( fail )) && { echo "verdict: FAIL"; exit 2; }
echo "verdict: PASS"
