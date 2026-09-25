#!/usr/bin/env bash
#
# freecam-corpus.sh — capture the native -vk debug-freecam presentation corpus.
#
# A second corpus alongside the bk2 set (presentation-corpus.sh): a set of
# (level, freecam pose) tuples, each a place a rendering bug was once chased.
# For every manifest row, boot headless under software Vulkan, --skip-to-level
# into the level, jump the debug freecam to the pose (which freezes Spyro + the
# camera so the view holds still), and capture two legs on the native-only path:
#   canonical    a pinned VBL frame                  -> <slug>__<lod>canon__<f>-vk.png
#   intermediate deterministic alpha=0.5 at +50      -> <slug>__<lod>inter__<f>-vk.png
#                (fails the row unless the engine reports the leg bracketed)
#   identity     alpha=1 re-extract on the canon frame -> <slug>__<lod>ident__<f>-vk.png
#                (not a golden tile; presentation-ident-check.py diffs it against canon)
# at 16:9, at every LOD mode in CAP_LODS. The freecam freezes the camera but NOT
# world animation, so the canonical leg is pinned to a fixed frame (deterministic
# run-to-run); the intermediate leg then exercises the moving terrain / mobys at
# that pose (the toasty platform is a freecam case).
#
# A row is `<level>[@<variant>] | <pose> | [start] | [launch]`. The level is
# everything before the first `@`; the whole field keys the tiles, so a variant
# row never collides with a plain row at the same pose. `launch` holds tokens
# applied to every leg of that row only (KEY=VALUE with an upper-case key is an
# environment entry, anything else a command-line argument), so the row
# measures that mode's own pixels. Rows without the field launch as before.
#
# Shares the diff/promote/golden infrastructure with the bk2 corpus: it writes
# PNGs into the same output directory, so check_presentation_golden.sh runs both
# capture scripts into one dir and diffs the union.
#
# Usage:
#   scripts/freecam-corpus.sh <off|on> <outdir> [level-filter] [--game SLUG]
#     off|on        OPENPETE_DEPTH_AFFINE for the run (default off = HEAD behavior)
#     outdir        where the -vk PNGs are written (created)
#     level-filter  substring; only rows whose level contains it are run (default: all)
#     --game        game slug (default spyro-1); selects binary, disc and
#                   manifest through scripts/presentation-game.sh
#
# Exit: 0 = all captures produced (or a manifest with no rows = nothing to
#           do),
#       1 = a capture is missing, an intermediate leg was unbracketed
#           (hard failure), or the manifest is missing (the message names
#           the path),
#       64 = SKIP (binary / disc / Vulkan loader missing).
#
# See notes/plans/openpete-presentation-golden.md.
set -u

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/.."
# shellcheck source=scripts/corpus-jobs.sh
source scripts/corpus-jobs.sh
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

AFFINE="${1:-off}"
OUTDIR="${2:-}"
FILTER="${3:-}"

CAP_ASPECT="${OPENPETE_CAP_ASPECT:-16:9}"
CAP_LODS="${OPENPETE_CAP_LODS:-original extended}"
# Pinned post-load frame for the canonical leg (+HALF for the intermediate).
# --skip-to-level is playable well before this (loads ~f1700); a row may override.
START_DEFAULT="${OPENPETE_FREECAM_START:-1900}"

BUILD_DIR="build/linux-release"
for a in "$@"; do case "$a" in --build-dir=*) BUILD_DIR="${a#--build-dir=}";; esac; done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"
MANIFEST="$PG_FREECAM_MANIFEST"

if [[ -z "$OUTDIR" ]]; then echo "usage: $0 <off|on|default> <outdir> [level-filter]" >&2; exit 64; fi
case "$AFFINE" in
  off|0)        AFFINE_ENV=0;;
  on|1)         AFFINE_ENV=1;;
  default|def)  AFFINE_ENV=default;;
  *) echo "freecam-corpus: first arg must be off|on|default (got '$AFFINE')" >&2; exit 64;; esac

pg_manifest "$MANIFEST" pipe

if [[ ! -x "$BIN" ]]; then echo "[freecam-corpus] SKIP: no binary at $BIN"; exit 64; fi
if [[ ! -f "$DISC" ]]; then echo "[freecam-corpus] SKIP: no disc at $DISC"; exit 64; fi
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
  echo "[freecam-corpus] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi

LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"
if [[ "$AFFINE_ENV" != "default" ]]; then export OPENPETE_DEPTH_AFFINE="$AFFINE_ENV"; else unset OPENPETE_DEPTH_AFFINE; fi

mkdir -p "$OUTDIR"
pg_clear_crash_logs "$OUTDIR"
echo "[freecam-corpus] game=$GAME bin=$BIN affine=$AFFINE_ENV aspect=$CAP_ASPECT lods=[$CAP_LODS] filter='$FILTER' -> $OUTDIR"

lod_tag() { [[ "$1" == "extended" ]] && echo "lodext__" || echo ""; }

# A stable, unique, human-scannable slug per (level, pose): the level name plus a
# short hash of the pose, so two poses in the same level don't collide and the
# filename survives re-ordering the manifest.
pose_slug() {  # level pose
  local h; h="$(printf '%s' "$2" | md5sum | cut -c1-6)"
  echo "${1}-${h}"
}

run_leg() {  # slug frame_limit leg flag target lodmode level pose launch
  local slug="$1" flim="$2" leg="$3" flag="$4" target="$5" lod="$6" level="$7" pose="$8" launch="${9:-}"
  local -a ROW_ENV=() ROW_ARGS=(); pg_split_launch "$launch"
  local tag; tag="$(lod_tag "$lod")"
  local out="$OUTDIR/${slug}__${tag}${leg}__{n}.png"
  local log; log="$(mktemp)"
  local -a leg_env=(); [[ "$leg" == ident ]] && leg_env=(OPENPETE_INTERMEDIATE_ALPHA=1)
  env "${leg_env[@]}" "${ROW_ENV[@]}" timeout 300 "$BIN" --headless --disc "$DISC" --disc-cache 0 --renderer vk \
    --mute 1 --aspect "$CAP_ASPECT" --lod "$lod" "${PG_RUN_ARGS[@]}" \
    --skip-to-level "$level" --freecam-pose "$pose" \
    --screenshot-native-only "$flag" "$target" --screenshot-output "$out" \
    --frame-limit "$flim" --max-runtime-seconds 280 "${ROW_ARGS[@]}" >"$log" 2>&1
  local rc=$?
  if [[ ! -f "$OUTDIR/${slug}__${tag}${leg}__${target}-vk.png" ]]; then
    echo "  MISSING ${slug}__${tag}${leg}__${target}-vk.png (rc=$rc)"
    pg_keep_crash_log "$log" "$OUTDIR" "${slug}__${tag}${leg}" "$rc"
    rm -f "$log"; return 1
  fi
  # The intermediate leg is only evidence when the sub-tick re-extract actually
  # installed an interpolation bracket. Unbracketed, the re-extract composes the
  # canonical pose and writes it under the sub-tick filename, so the image is a
  # copy of the canonical leg and every diff over it is vacuous. The engine's
  # exit summary counts the legs it bracketed; an absent summary (a run killed
  # before shutdown) proves nothing either, and is refused the same way.
  if [[ "$leg" == inter ]]; then
    local sum; sum="$(grep -o 'intermediate screenshot leg(s): [0-9]* bracketed, [0-9]* unbracketed' "$log" | tail -1)"
    local unbr="${sum##*, }"; unbr="${unbr%% *}"
    if [[ -z "$sum" || "$unbr" != 0 ]]; then
      echo "  VACUOUS ${slug}__${tag}${leg}__${target}-vk.png: no bracketed sub-tick re-extract (${sum:-no exit summary})"
      rm -f "$log"; return 1
    fi
  fi
  rm -f "$log"; return 0
}

fail=0 ran=0
while IFS='|' read -r name pose start launch; do
  name="$(echo "$name" | tr -d ' \t')"
  [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
  level="${name%%@*}"
  # Trim surrounding whitespace from the pose but keep its internal spaces.
  pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
  start="$(echo "$start" | tr -d ' \t')"; start="${start:-$START_DEFAULT}"
  launch="$(echo "${launch:-}" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
  [[ -z "$pose" ]] && { echo "  SKIP row: no pose for level '$level'"; continue; }
  [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
  local_istart=$((start + 50))
  flim=$((local_istart + 51))
  slug="$(pose_slug "$name" "$pose")"
  ran=$((ran+1))
  echo "[$slug] level=$level start=$start pose='$pose'${launch:+  launch=$launch}"
  for lod in $CAP_LODS; do
    cj_run run_leg "$slug" "$flim" canon --screenshot-on-frame "$start" "$lod" "$level" "$pose" "$launch"
    cj_run run_leg "$slug" "$flim" inter --screenshot-intermediate-on-frame "$local_istart" "$lod" "$level" "$pose" "$launch"
    cj_run run_leg "$slug" "$flim" ident --screenshot-intermediate-on-frame "$start" "$lod" "$level" "$pose" "$launch"
  done
done < "$MANIFEST"
cj_wait || fail=1

if [[ $ran -eq 0 ]]; then echo "[freecam-corpus] $GAME: no rows matched '$FILTER' — 0 rows"; exit 0; fi
if [[ $fail -ne 0 ]]; then echo "[freecam-corpus] FAIL: missing captures"; exit 1; fi
echo "[freecam-corpus] OK: $(ls "$OUTDIR"/*-vk.png 2>/dev/null | wc -l) images in $OUTDIR across $ran pose(s)"
exit 0
