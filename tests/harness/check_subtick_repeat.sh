#!/usr/bin/env bash
# check_subtick_repeat.sh - a second sub-tick re-extract inside one canonical
# epoch draws exactly what a single re-extract at its phase draws.
#
# A live run at a present rate above the tick rate re-extracts the scene
# several times per canonical epoch; a headless capture re-extracts it once.
# Any state a re-extract keeps for the later presents of the same epoch (a
# cache of built geometry, a verdict taken on the first present) therefore
# reaches a live run's picture and never a single-leg capture's. This check
# reproduces the live sequence headless.
#
# Legs per row, all at the same feed key, native-only:
#   repeat   OPENPETE_INTERMEDIATE_PRE_ALPHA=0.25 OPENPETE_INTERMEDIATE_ALPHA=0.5:
#            an uncaptured re-extract at 0.25, then the captured one at 0.5
#   single   OPENPETE_INTERMEDIATE_ALPHA=0.5: the captured re-extract alone
#   control  OPENPETE_INTERMEDIATE_ALPHA=0.25: the first phase alone
#
# Pass criterion, per row: repeat and single are pixel-identical (0 px
# differ), and the repeat leg's log carries no `[hold-probe` line (a build
# that replays geometry captured earlier in the epoch prints one per replay
# under OPENPETE_HOLD_PROBE=1).
#
# Engagement (VACUOUS, exit 2): every leg's re-extracts bracketed (the exit
# summaries name 1 bracketed capture and, on the repeat leg, 1 bracketed
# pre-alpha re-extract; a binary without OPENPETE_INTERMEDIATE_PRE_ALPHA
# prints no pre-alpha summary), and control differs from single by at least
# one pixel, so the scene moves between the two phases and a picture held
# from the first re-extract is visible in the second.
#
# Manifest: games/<slug>/corpus/subtick-repeat-corpus.manifest, one row per
# pose:  <level> | <freecam pose> | [lod] | [feed key]
#   lod       original (default) or extended
#   feed key  the captured present's feed key; default the freecam corpus's
#             canonical start + 50 (pg_cap_settings)
#
# Usage: tests/harness/check_subtick_repeat.sh [--game SLUG] [--build-dir=DIR] [filter]
#   OPENPETE_BIN overrides the binary (pg_resolve).
#   SUBTICK_REPEAT_OUT sets the output directory (default
#   ${TMPDIR:-/tmp}/openpete-check-subtick-repeat/<slug>).
# Exit: 0 pass, 1 fail, 2 vacuous, 64 skip (no binary, disc or Vulkan loader).

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 64

ARGS=() FILTER=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) export PG_BUILD_DIR="${a#--build-dir=}" ;;
        *) ARGS+=("$a") ;;
    esac
done
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
GAME="$(pg_parse_game ${ARGS[@]+"${ARGS[@]}"})" || exit 64
pg_resolve "$GAME" || exit 64
pg_cap_settings
for a in ${ARGS[@]+"${ARGS[@]}"}; do
    case "$a" in --game|--game=*|"$GAME") ;; *) FILTER="$a" ;; esac
done

MANIFEST="$(pg_corpus_manifest "$GAME" subtick-repeat-corpus.manifest)"
pg_manifest "$MANIFEST" pipe
[[ -x "$PG_BIN" ]] || { echo "[subtick-repeat] SKIP: no binary $PG_BIN"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "[subtick-repeat] SKIP: no disc $PG_DISC"; exit 64; }
# grep -q exits at the first match and SIGPIPEs ldconfig, which pipefail
# would report as a failure: match on the captured listing instead.
if [[ "$(ldconfig -p 2>/dev/null)" != *libvulkan.so.1* ]]; then
    echo "[subtick-repeat] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi
# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env

OUT="${SUBTICK_REPEAT_OUT:-${TMPDIR:-/tmp}/openpete-check-subtick-repeat/$GAME}"
mkdir -p "$OUT"
PG_CAPTURE_TIMEOUT="${SUBTICK_REPEAT_TIMEOUT:-600}"
KEY_DEFAULT=$((PG_FREECAM_START + 50))

# One leg. args: slug leg lod level pose key env...
run_leg() {
    local slug="$1" leg="$2" lod="$3" level="$4" pose="$5" key="$6"; shift 6
    local png="$OUT/${slug}__${leg}__${key}-vk.png" log="$OUT/${slug}__${leg}.log"
    local -a PG_SKIP_ARGS=(); pg_skip_args "$level"
    PG_CAPTURE_ENV=(OPENPETE_HOLD_PROBE=1 "$@")
    pg_capture "$log" "$png" -- --renderer vk --mute 1 --no-card --aspect "$PG_CAP_ASPECT" \
        --lod "$lod" "${PG_SKIP_ARGS[@]}" --freecam-pose "$pose" \
        --screenshot-native-only --screenshot-intermediate-on-frame "$key" \
        --screenshot-output "$OUT/${slug}__${leg}__{n}.png" --frame-limit $((key + 10))
}

# Count of the summary's bracketed and unbracketed legs. args: log what
summary() {
    grep -a -o "[0-9]* $2: [0-9]* bracketed, [0-9]* unbracketed" "$1" | tail -1
}

# Pixels that differ between two PNGs, and their bounding box.
pxdiff() {
    python3 - "$1" "$2" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert("RGBA"))
b = np.asarray(Image.open(sys.argv[2]).convert("RGBA"))
if a.shape != b.shape:
    print("-1 shape %s vs %s" % (a.shape, b.shape)); sys.exit(0)
d = (a != b).any(axis=2)
n = int(d.sum())
if n:
    ys, xs = np.nonzero(d)
    print("%dpx bbox=(%d,%d)-(%d,%d)" % (n, xs.min(), ys.min(), xs.max(), ys.max()))
else:
    print("0px")
PY
}

fail=0 vac=0 ran=0
mapfile -t ROWS < "$MANIFEST"
for row in "${ROWS[@]}"; do
    IFS='|' read -r level pose lod key <<<"$row"
    level="$(echo "$level" | tr -d ' \t')"
    [[ -z "$level" || "${level:0:1}" == "#" ]] && continue
    pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    lod="$(echo "${lod:-}" | tr -d ' \t')"; lod="${lod:-original}"
    key="$(echo "${key:-}" | tr -d ' \t')"; key="${key:-$KEY_DEFAULT}"
    slug="$(pg_pose_slug "$level" "$pose")-$lod"
    [[ -n "$FILTER" && "$slug" != *"$FILTER"* ]] && continue
    ran=$((ran + 1))
    rc=0
    run_leg "$slug" repeat "$lod" "$level" "$pose" "$key" \
        OPENPETE_INTERMEDIATE_PRE_ALPHA=0.25 OPENPETE_INTERMEDIATE_ALPHA=0.5 || rc=1
    run_leg "$slug" single "$lod" "$level" "$pose" "$key" OPENPETE_INTERMEDIATE_ALPHA=0.5 || rc=1
    run_leg "$slug" control "$lod" "$level" "$pose" "$key" OPENPETE_INTERMEDIATE_ALPHA=0.25 || rc=1
    if [[ $rc -ne 0 ]]; then
        echo "[$slug] FAIL: a leg did not complete (logs in $OUT)"; fail=1; continue; fi
    rep="$OUT/${slug}__repeat__${key}-vk.png"
    sgl="$OUT/${slug}__single__${key}-vk.png"
    ctl="$OUT/${slug}__control__${key}-vk.png"
    v_eng=""
    for leg in repeat single control; do
        s="$(summary "$OUT/${slug}__${leg}.log" "intermediate screenshot leg(s)")"
        [[ "$s" == "1 intermediate screenshot leg(s): 1 bracketed, 0 unbracketed" ]] ||
            v_eng+=" $leg capture not bracketed (${s:-no summary});"
    done
    s="$(summary "$OUT/${slug}__repeat.log" "uncaptured pre-alpha re-extract(s)")"
    [[ "$s" == "1 uncaptured pre-alpha re-extract(s): 1 bracketed, 0 unbracketed" ]] ||
        v_eng+=" repeat pre-alpha re-extract absent or unbracketed (${s:-no summary});"
    d_ctl="$(pxdiff "$ctl" "$sgl")"
    [[ "${d_ctl%%px*}" -gt 0 ]] || v_eng+=" control equals single (${d_ctl});"
    d_rep="$(pxdiff "$rep" "$sgl")"
    holds="$(grep -a -c '\[hold-probe' "$OUT/${slug}__repeat.log")"
    line="[$slug] level=$level lod=$lod key=$key repeat-vs-single=${d_rep} hold-replays=${holds} control-vs-single=${d_ctl}"
    if [[ -n "$v_eng" ]]; then
        echo "$line VACUOUS:$v_eng"; vac=1
    elif [[ "$d_rep" != "0px" || "$holds" != "0" ]]; then
        echo "$line FAIL"; fail=1
    else
        echo "$line PASS"
    fi
done

if [[ $ran -eq 0 ]]; then echo "[subtick-repeat] $GAME: no rows matched '${FILTER}'"; exit 2; fi
if [[ $fail -ne 0 ]]; then echo "[subtick-repeat] $GAME: FAIL"; exit 1; fi
if [[ $vac -ne 0 ]]; then echo "[subtick-repeat] $GAME: VACUOUS"; exit 2; fi
echo "[subtick-repeat] $GAME: PASS ($ran row(s))"
exit 0
