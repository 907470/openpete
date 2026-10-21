#!/usr/bin/env bash
# check_terrain_anim_census.sh - per-sector motion census of the terrain
# environment animation (Spyro 1, 2 and 3).
#
# Spyro 2 and 3: the game's wave and colour walkers rewrite a sector's HP
# banks only while the world draw bins it HP (live byte bit 0); extended LOD
# draws far sectors from their HP banks, which the native decode animates
# from the walkers' closed form. Spyro 1: the animation update posts each
# keyframe advance into the sector's pending bytes and the world draw packs
# the pose only into the tiers it bins (inside the camera's occlusion-group
# list and its 4:3 frustum); the native decode packs every pending pose.
# Each row boots headless with the env-anim census
# (OPENPETE_TERRAIN_ANIMCENSUS) at a freecam pose or at the level's spawn
# and reads the last full census window:
#   - every listed sector the native pass drew changed on each list it is
#     named by (vertex bank for the wave lists, colour words for the colour
#     list) at least once in the window; on Spyro 1, every bank whose posted
#     pose changed changed natively (a paused animation posts nothing);
#   - the model equalled guest RAM on every compared tick: the walkers'
#     model on every sector the game animated (Spyro 2 and 3), the pack's
#     model on every bank the draw packed (Spyro 1).
# A window with no drawn listed sector or no model comparison is VACUOUS and
# fails the row (engagement). The window is 96 ticks, longer than one
# animation period (about 171 VBLs).
#
# Rows: poses where the game leaves far lava or colours dormant (S2
# magma_cone, S2 crushs_dungeon first pose, S3 fireworks_factory, S1
# dark_hollow, S1 metalhead and tree_tops spawn) and one control whose
# listed sectors the game animates throughout (S2 crushs_dungeon second
# pose; S1 blowhard inside the moving-platform sectors, whose packs blend
# keyframes by INTPL). Spyro 1 rows run at 4:3, the dormant ones at
# extended and at original LOD.
#
# Usage: tests/harness/check_terrain_anim_census.sh [--build-dir=DIR]
#        [--out=DIR] [--only=SUBSTR]
#   OPENPETE_BIN selects another binary (the pre-change build, for the
#   negative leg); OPENPETE_TERRAIN_ENVMODEL=0 disables the walker model and
#   OPENPETE_TERRAIN_KFMODEL=0 the keyframe pack model in the binary under
#   test.
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

# game | level | pose ('-' = the level's spawn) | role | lod | aspect
ROWS=(
  "spyro-2|magma_cone|pos=(93407, 88746, 41508) yaw=-1999 pitch=236|dormant|extended|16:9"
  "spyro-2|crushs_dungeon|pos=(-1501, 29835, 47171) yaw=17 pitch=563|dormant|extended|16:9"
  "spyro-2|crushs_dungeon|pos=(3080, 30039, 31217) yaw=29 pitch=548|control|extended|16:9"
  "spyro-3|fireworks_factory|pos=(-18130, 83228, 13192) yaw=-78 pitch=119|dormant|extended|16:9"
  "spyro-1|dark_hollow|pos=(15607, 62161, 18376) yaw=3735 pitch=87|dormant|extended|4:3"
  "spyro-1|dark_hollow|pos=(15607, 62161, 18376) yaw=3735 pitch=87|dormant|original|4:3"
  "spyro-1|metalhead|-|dormant|extended|4:3"
  "spyro-1|metalhead|-|dormant|original|4:3"
  "spyro-1|tree_tops|-|dormant|extended|4:3"
  "spyro-1|tree_tops|-|dormant|original|4:3"
  "spyro-1|blowhard|pos=(52656, 88208, 14968) yaw=0 pitch=0|control|extended|4:3"
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
  local n="$1" game level pose role lod aspect
  IFS='|' read -r game level pose role lod aspect <<<"${ROWS[$n]}"
  PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$game" || return 64
  [[ -x "$PG_BIN" ]]  || { echo "SKIP: no binary at $PG_BIN" >"$OUT/row$n.log"; return 64; }
  [[ -f "$PG_DISC" ]] || { echo "SKIP: no disc at $PG_DISC" >"$OUT/row$n.log"; return 64; }
  local shot="$OUT/row${n}_$((FRAME_LIMIT - 10))-vk.png"
  PG_CAPTURE_ENV=(OPENPETE_HEADLESS_NATIVE=1 "OPENPETE_TERRAIN_ANIMCENSUS=$WINDOW")
  PG_CAPTURE_TIMEOUT="$LEG_TIMEOUT"
  local pa=()
  [[ "$pose" != - ]] && pa=(--freecam-pose "$pose")
  pg_capture "$OUT/row$n.log" "$shot" -- --renderer vk --mute 1 --aspect "$aspect" \
    --lod "$lod" --skip-to-level "$level" "${pa[@]}" \
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
printf '%-8s %-18s %-8s %-8s %-8s %s\n' game level role lod verdict summary >"$summary"
for n in "${rows[@]}"; do
  IFS='|' read -r game level pose role lod aspect <<<"${ROWS[$n]}"
  rc="$(cat "$OUT/row$n.rc" 2>/dev/null || echo 1)"
  if [[ "$rc" == 64 ]]; then echo "[anim-census] SKIP: $(head -1 "$OUT/row$n.log")"; exit 64; fi
  f="$OUT/row$n.census"
  if [[ ! -s "$f" ]]; then
    printf '%-8s %-18s %-8s %-8s %-8s %s\n' "$game" "$level" "$role" "$lod" NOCENSUS \
      "rc=$rc, no census window in $OUT/row$n.log" >>"$summary"
    fail=1; continue
  fi
  sum="$(grep ' verdict=' "$f")"
  v="$(sed 's/.*verdict=\([A-Z]*\).*/\1/' <<<"$sum")"
  [[ "$v" == PASS && "$rc" == 0 ]] || fail=1
  printf '%-8s %-18s %-8s %-8s %-8s %s rc=%s\n' "$game" "$level" "$role" "$lod" "$v" "${sum#*verdict=$v }" "$rc" >>"$summary"
  grep -v ' verdict=' "$f" | sed 's/^\[animcensus\] w=[0-9]* /    /' >>"$summary"
done
cat "$summary"
if (( fail )); then echo "[anim-census] FAIL"; exit 1; fi
echo "[anim-census] PASS"
exit 0
