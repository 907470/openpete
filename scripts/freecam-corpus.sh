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
# at 16:9, at every LOD mode in CAP_LODS. The freecam freezes the camera but NOT
# world animation, so the canonical leg is pinned to a fixed frame (deterministic
# run-to-run); the intermediate leg then exercises the moving terrain / mobys at
# that pose (the toasty platform is a freecam case).
#
# Shares the diff/promote/golden infrastructure with the bk2 corpus: it writes
# PNGs into the same output directory, so check_presentation_golden.sh runs both
# capture scripts into one dir and diffs the union.
#
# Usage:
#   scripts/freecam-corpus.sh <off|on> <outdir> [level-filter]
#     off|on        OPENPETE_DEPTH_AFFINE for the run (default off = HEAD behavior)
#     outdir        where the -vk PNGs are written (created)
#     level-filter  substring; only rows whose level contains it are run (default: all)
#
# Exit: 0 = all captures produced (or no manifest = nothing to do),
#       1 = a capture is missing (hard failure),
#       64 = SKIP (binary / disc / Vulkan loader missing).
#
# See notes/plans/openpete-presentation-golden.md.
set -u
cd "$(dirname "$0")/.."

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

BIN="${OPENPETE_BIN:-$BUILD_DIR/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
MANIFEST="scripts/freecam-corpus.manifest"

if [[ -z "$OUTDIR" ]]; then echo "usage: $0 <off|on|default> <outdir> [level-filter]" >&2; exit 64; fi
case "$AFFINE" in
  off|0)        AFFINE_ENV=0;;
  on|1)         AFFINE_ENV=1;;
  default|def)  AFFINE_ENV=default;;
  *) echo "freecam-corpus: first arg must be off|on|default (got '$AFFINE')" >&2; exit 64;; esac

if [[ ! -x "$BIN" ]]; then echo "[freecam-corpus] SKIP: no binary at $BIN"; exit 64; fi
if [[ ! -f "$DISC" ]]; then echo "[freecam-corpus] SKIP: no disc at $DISC"; exit 64; fi
# An absent (or comment-only) manifest is not a failure — the freecam corpus is
# additive and may not be populated yet. Nothing to capture = success.
if [[ ! -f "$MANIFEST" ]]; then echo "[freecam-corpus] no manifest ($MANIFEST) — nothing to capture"; exit 0; fi
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
  echo "[freecam-corpus] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi

LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"
if [[ "$AFFINE_ENV" != "default" ]]; then export OPENPETE_DEPTH_AFFINE="$AFFINE_ENV"; else unset OPENPETE_DEPTH_AFFINE; fi

mkdir -p "$OUTDIR"
echo "[freecam-corpus] bin=$BIN affine=$AFFINE_ENV aspect=$CAP_ASPECT lods=[$CAP_LODS] filter='$FILTER' -> $OUTDIR"

lod_tag() { [[ "$1" == "extended" ]] && echo "lodext__" || echo ""; }

# A stable, unique, human-scannable slug per (level, pose): the level name plus a
# short hash of the pose, so two poses in the same level don't collide and the
# filename survives re-ordering the manifest.
pose_slug() {  # level pose
  local h; h="$(printf '%s' "$2" | md5sum | cut -c1-6)"
  echo "${1}-${h}"
}

run_leg() {  # slug frame_limit leg flag target lodmode level pose
  local slug="$1" flim="$2" leg="$3" flag="$4" target="$5" lod="$6" level="$7" pose="$8"
  local tag; tag="$(lod_tag "$lod")"
  local out="$OUTDIR/${slug}__${tag}${leg}__{n}.png"
  local log; log="$(mktemp)"
  timeout 300 "$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 --renderer vk \
    --aspect "$CAP_ASPECT" --lod "$lod" --skip-to-level "$level" --freecam-pose "$pose" \
    --screenshot-native-only "$flag" "$target" --screenshot-output "$out" \
    --frame-limit "$flim" --max-runtime-seconds 280 >"$log" 2>&1
  local rc=$?
  if [[ ! -f "$OUTDIR/${slug}__${tag}${leg}__${target}-vk.png" ]]; then
    echo "  MISSING ${slug}__${tag}${leg}__${target}-vk.png (rc=$rc)"
    echo "  --- last log lines ---"; tail -6 "$log" | sed 's/^/  /'
    rm -f "$log"; return 1
  fi
  rm -f "$log"; return 0
}

fail=0 ran=0
while IFS='|' read -r level pose start; do
  level="$(echo "$level" | tr -d ' \t')"
  [[ -z "$level" || "${level:0:1}" == "#" ]] && continue
  # Trim surrounding whitespace from the pose but keep its internal spaces.
  pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
  start="$(echo "$start" | tr -d ' \t')"; start="${start:-$START_DEFAULT}"
  [[ -z "$pose" ]] && { echo "  SKIP row: no pose for level '$level'"; continue; }
  [[ -n "$FILTER" && "$level" != *"$FILTER"* ]] && continue
  local_istart=$((start + 50))
  flim=$((local_istart + 51))
  slug="$(pose_slug "$level" "$pose")"
  ran=$((ran+1))
  echo "[$slug] level=$level start=$start pose='$pose'"
  for lod in $CAP_LODS; do
    run_leg "$slug" "$flim" canon --screenshot-on-frame "$start" "$lod" "$level" "$pose" || fail=1
    run_leg "$slug" "$flim" inter --screenshot-intermediate-on-frame "$local_istart" "$lod" "$level" "$pose" || fail=1
  done
done < "$MANIFEST"

if [[ $ran -eq 0 ]]; then echo "[freecam-corpus] no rows matched '$FILTER' — nothing to capture"; exit 0; fi
if [[ $fail -ne 0 ]]; then echo "[freecam-corpus] FAIL: missing captures"; exit 1; fi
echo "[freecam-corpus] OK: $(ls "$OUTDIR"/*-vk.png 2>/dev/null | wc -l) images in $OUTDIR across $ran pose(s)"
exit 0
