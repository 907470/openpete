#!/usr/bin/env bash
# check_terrain_anim_census.sh - per-sector motion census of the terrain
# environment animation at extended LOD (Spyro 2 and Spyro 3).
#
# The game's wave and colour walkers rewrite a sector's HP banks only while
# the world draw bins it HP (live byte bit 0); extended LOD draws far sectors
# from their HP banks, which the native decode animates from the walkers'
# closed form. Each row boots headless with the env-anim census
# (OPENPETE_TERRAIN_ANIMCENSUS) at a freecam pose and reads the last full
# census window:
#   - every listed sector the native pass drew changed on each list it is
#     named by (vertex bank for the wave lists, colour words for the colour
#     list) at least once in the window;
#   - for every sector the game animated, the walker model equalled guest
#     RAM on every compared tick (the RAM-driven path stays as it was).
# A window with no drawn listed sector or no model comparison is VACUOUS and
# fails the row (engagement). The window is 96 ticks, longer than one
# animation period (about 171 VBLs).
#
# Rows: two poses where the game leaves far lava dormant (S2 magma_cone,
# S2 crushs_dungeon first pose, S3 fireworks_factory) and one control whose
# listed sectors the game animates throughout (S2 crushs_dungeon second
# pose).
#
# Usage: tests/harness/check_terrain_anim_census.sh [--build-dir=DIR]
#        [--out=DIR] [--only=SUBSTR]
#   OPENPETE_BIN selects another binary (the pre-change build, for the
#   negative leg); OPENPETE_TERRAIN_ENVMODEL=0 disables the model in the
#   binary under test.
# Exit 0 = every row PASS, 1 = a row failed or produced no census, 64 = SKIP
# (binary, disc or Vulkan loader missing).
set -uo pipefail

export OPENPETE_VIBRATION=0   # unattended runs (scripts/fences/rumble_guard.py)

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT=""
ONLY=""
for a in "$@"; do
  case "$a" in
    --build-dir=*) BUILD_DIR="${a#*=}" ;;
    --out=*)       OUT="${a#*=}" ;;
    --only=*)      ONLY="${a#*=}" ;;
    *) echo "check_terrain_anim_census: unknown argument '$a'" >&2; exit 64 ;;
  esac
done
[[ -n "$OUT" ]] || OUT="$BUILD_DIR/terrain-anim-census"

WINDOW=96          # census ticks per window
FRAME_LIMIT=2900   # VBLs: the skip-to-level load ends near VBL 1300, so the
                   # last full window lies past it with the pose held
LEG_TIMEOUT="${CENSUS_LEG_TIMEOUT:-600}"

# game | level | pose | role
ROWS=(
  "spyro-2|magma_cone|pos=(93407, 88746, 41508) yaw=-1999 pitch=236|dormant"
  "spyro-2|crushs_dungeon|pos=(-1501, 29835, 47171) yaw=17 pitch=563|dormant"
  "spyro-2|crushs_dungeon|pos=(3080, 30039, 31217) yaw=29 pitch=548|control"
  "spyro-3|fireworks_factory|pos=(-18130, 83228, 13192) yaw=-78 pitch=119|dormant"
)

# The loader list is read whole first: under pipefail, grep -q closing the
# pipe early fails ldconfig with SIGPIPE and reads as a missing loader.
if ! grep -q 'libvulkan\.so\.1' <<<"$(ldconfig -p 2>/dev/null)"; then
  echo "[anim-census] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi
# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env

mkdir -p "$OUT"
rm -f "${OUT:?}"/row*.log "${OUT:?}"/row*.png "${OUT:?}"/row*.census "${OUT:?}"/row*.rc "${OUT:?}/summary.txt"

# run_row N: one headless census run; writes rowN.log and rowN.census (the
# last full window's lines).
run_row() {
  local n="$1" game level pose role
  IFS='|' read -r game level pose role <<<"${ROWS[$n]}"
  PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$game" || return 64
  [[ -x "$PG_BIN" ]]  || { echo "SKIP: no binary at $PG_BIN" >"$OUT/row$n.log"; return 64; }
  [[ -f "$PG_DISC" ]] || { echo "SKIP: no disc at $PG_DISC" >"$OUT/row$n.log"; return 64; }
  local shot="$OUT/row${n}_$((FRAME_LIMIT - 10))-vk.png"
  PG_CAPTURE_ENV=(OPENPETE_HEADLESS_NATIVE=1 "OPENPETE_TERRAIN_ANIMCENSUS=$WINDOW")
  PG_CAPTURE_TIMEOUT="$LEG_TIMEOUT"
  pg_capture "$OUT/row$n.log" "$shot" -- --renderer vk --mute 1 --aspect 16:9 \
    --lod extended --skip-to-level "$level" --freecam-pose "$pose" \
    --screenshot-native-only --screenshot-on-frame "$((FRAME_LIMIT - 10))" \
    --screenshot-output "$OUT/row${n}_{n}.png" --frame-limit "$FRAME_LIMIT"
  local rc=$?
  local w
  w="$(grep -o '^\[animcensus\] w=[0-9]* verdict=' "$OUT/row$n.log" | tail -1 | sed 's/.*w=\([0-9]*\).*/\1/')"
  [[ -n "$w" ]] && grep "^\[animcensus\] w=$w " "$OUT/row$n.log" >"$OUT/row$n.census"
  return $rc
}

rows=()
for n in "${!ROWS[@]}"; do
  [[ -n "$ONLY" && "${ROWS[$n]}" != *"$ONLY"* ]] && continue
  rows+=("$n")
done
(( ${#rows[@]} )) || { echo "[anim-census] FAIL: no row matches '$ONLY'"; exit 1; }
# Two game processes at once; each leg's status lands in rowN.rc.
active=0
for n in "${rows[@]}"; do
  ( run_row "$n"; echo $? >"$OUT/row$n.rc" ) &
  active=$((active + 1))
  if (( active >= 2 )); then wait -n; active=$((active - 1)); fi
done
wait

fail=0
summary="$OUT/summary.txt"
printf '%-8s %-18s %-8s %-8s %s\n' game level role verdict summary >"$summary"
for n in "${rows[@]}"; do
  IFS='|' read -r game level pose role <<<"${ROWS[$n]}"
  rc="$(cat "$OUT/row$n.rc" 2>/dev/null || echo 1)"
  if [[ "$rc" == 64 ]]; then echo "[anim-census] SKIP: $(head -1 "$OUT/row$n.log")"; exit 64; fi
  f="$OUT/row$n.census"
  if [[ ! -s "$f" ]]; then
    printf '%-8s %-18s %-8s %-8s %s\n' "$game" "$level" "$role" NOCENSUS \
      "rc=$rc, no census window in $OUT/row$n.log" >>"$summary"
    fail=1; continue
  fi
  sum="$(grep ' verdict=' "$f")"
  v="$(sed 's/.*verdict=\([A-Z]*\).*/\1/' <<<"$sum")"
  [[ "$v" == PASS && "$rc" == 0 ]] || fail=1
  printf '%-8s %-18s %-8s %-8s %s rc=%s\n' "$game" "$level" "$role" "$v" "${sum#*verdict=$v }" "$rc" >>"$summary"
  grep -v ' verdict=' "$f" | sed 's/^\[animcensus\] w=[0-9]* /    /' >>"$summary"
done
cat "$summary"
if (( fail )); then echo "[anim-census] FAIL"; exit 1; fi
echo "[anim-census] PASS"
exit 0
