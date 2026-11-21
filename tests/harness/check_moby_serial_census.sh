#!/usr/bin/env bash
# check_moby_serial_census.sh - Spyro 1 sub-tick moby pairs that span two
# allocations of one slot.
#
# Spyro 1's free (func_80052568) moves the allocation cursor down to the slot
# it frees, so a moby freed in a tick hands its slot to the next moby the tick
# allocates. The interp record keeps the slot address as its key; the
# allocator hook (func_800524C4, renderer/moby_serial.h) stamps each
# allocation with a serial carried in the record, and a pair whose two states
# carry different serials (moby_pair_reborn) is a dead moby and a newborn. The
# defect is a present that lerps such a pair at an alpha in (0,1): the dead
# moby slides into the newborn's place. The fix holds the pair as a class
# change (tick N-1's moby until alpha 1).
#
# Instrument: OPENPETE_MOBY_SERIAL_CENSUS=1 prints one line per (slot, tick)
# verdict on a reused slot,
#   [moby-serial-census] f=<vbl> m=<moby> cls=<class> verdict=lerp|held a=<alpha>
# and the totals at exit. Each row runs headless with
# OPENPETE_HEADLESS_REEXTRACT=1 (one sub-tick re-extract per VBL at alpha
# 0.5) and counts the lines whose VBL lies in the row's window.
#
# Rows (name, input, window, frame limit): dream-weavers-particle (class 0x4D
# sparks, three reuses), fairy-kiss (class 0x106, one), the attract demo
# (class 0x26, four in its first loop).
#
# Pass per row: no lerp line in the window and at least one line (engaged).
#
# Usage: tests/harness/check_moby_serial_census.sh [--out DIR] [--filter SUBSTR]
#   OPENPETE_BIN selects another binary (the pre-fix build, for the negative leg).
# Exit: 0 = PASS, 2 = FAIL (a lerped reuse, a row with no reuse, or a failed
#       run), 64 = SKIP (binary or disc missing).
set -uo pipefail
export OPENPETE_VIBRATION=0   # unattended runs (scripts/fences/rumble_guard.py)

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh
# shellcheck source=scripts/lib/sandbox.sh
source scripts/lib/sandbox.sh

OUT="build/moby-serial-census" FILTER=""
while (( $# )); do
  case "$1" in
    --out) OUT="$2"; shift 2;;
    --out=*) OUT="${1#--out=}"; shift;;
    --filter) FILTER="$2"; shift 2;;
    --filter=*) FILTER="${1#--filter=}"; shift;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 64;;
    *) echo "check_moby_serial_census: unknown argument '$1'" >&2; exit 64;;
  esac
done
pg_resolve spyro-1 || exit 64
[[ -x "$PG_BIN" ]] || { echo "[moby-serial-census] SKIP: no binary at $PG_BIN"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "[moby-serial-census] SKIP: no disc at $PG_DISC"; exit 64; }
mkdir -p "$OUT"

# name|bk2 (empty = attract demo)|window lo|window hi|frame limit
ROWS=(
  "dream-weavers-particle|dream-weavers-particle|1749|1776|1800"
  "fairy-kiss|fairy-kiss|2060|2065|2100"
  "attract|  |18684|19293|19400"
)

# Run one row and keep its log. args: name bk2 frame-limit
run_row() {
  local name="$1" bk2="$2" flim="$3"
  local log="$OUT/$name.log"
  rm -f "$log" "$log.rc"
  local -a in_args=()
  if [[ -n "$bk2" ]]; then
    in_args=(--bk2 "$PG_BK2_DIR/$bk2.bk2")
    local a; a="$(cat "$PG_BK2_DIR/$bk2.bk2.args" 2>/dev/null || true)"
    [[ -n "$a" ]] && { read -ra extra <<<"$a"; in_args+=("${extra[@]}"); }
  fi
  op_own_run env OPENPETE_MOBY_SERIAL_CENSUS=1 OPENPETE_HEADLESS_REEXTRACT=1 \
      timeout -k 10 1500 "$PG_BIN" --headless --no-config --disc "$PG_DISC" --disc-cache 0 \
      --renderer vk --mute 1 --aspect 4:3 --lod original --no-card "${PG_RUN_ARGS[@]}" \
      "${in_args[@]}" --frame-limit "$flim" > "$log" 2>&1
  echo "$?" > "$log.rc"
}

for r in "${ROWS[@]}"; do
  IFS='|' read -r name bk2 _lo _hi flim <<<"$r"
  [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
  bk2="${bk2// /}"
  while (( $(jobs -rp | wc -l) >= 2 )); do wait -n; done
  run_row "$name" "$bk2" "$flim" &
done
wait

fail=0 rows=0
for r in "${ROWS[@]}"; do
  IFS='|' read -r name _bk2 lo hi _flim <<<"$r"
  [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
  rows=$((rows + 1))
  log="$OUT/$name.log"
  rc="$(cat "$log.rc" 2>/dev/null || echo none)"
  if [[ ! -f "$log" || "$rc" != 0 ]]; then
    echo "[moby-serial-census] $name: FAIL run rc=$rc ($log)"; fail=1; continue
  fi
  read -r held lerp < <(awk -v lo="$lo" -v hi="$hi" '
    /^\[moby-serial-census\] f=/ {
      split($2, f, "="); v = f[2] + 0
      if (v >= lo && v <= hi) { if ($5 == "verdict=lerp") l++; else h++ }
    }
    END { printf "%d %d\n", h, l }' "$log")
  tot="$(grep '^\[moby-serial-census\] lerped=' "$log" | tail -1)"
  verdict=PASS
  if (( lerp > 0 )); then verdict=FAIL; fail=1
  elif (( held == 0 )); then verdict="FAIL (VACUOUS: no reuse in the window)"; fail=1; fi
  echo "[moby-serial-census] $name window $lo-$hi: reuses=$((held + lerp)) lerped=$lerp held=$held; run ${tot#\[moby-serial-census\] }: $verdict"
done
(( rows )) || { echo "[moby-serial-census] FAIL: no row matched '$FILTER'"; exit 2; }
if (( fail )); then echo "[moby-serial-census] verdict: FAIL"; exit 2; fi
echo "[moby-serial-census] verdict: PASS"
exit 0
