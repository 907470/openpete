#!/usr/bin/env bash
#
# portal-ref-check.sh — fast portal-sky fidelity harness.
#
# Scores the native portal-sky rendering against the PSX (-og) reference on a
# small set of portal-specific cases, so portal work can iterate in minutes
# instead of running the whole presentation corpus (1184 images) every turn.
#
# The -og reference is INVARIANT under native render knobs, so it is captured
# once and cached; only the native leg re-runs per iteration. Everything is
# captured at 4:3 because that is the aspect the PsyCross headless path
# produces — comparing a 16:9 native shot against a 4:3 og requires a resize
# that destroys pixel correspondence and silently invalidates the metric.
#
# Output is one row per case. The mask is the pixels where the aperture knob
# changes the image (native as-is vs native with OPENPETE_PORTAL_APERTURE=0);
# over that mask, `baseline` is mean |og - aperture-OFF| and `current` is mean
# |og - aperture-ON|. Lower is closer to the original. A case only means
# something when its mask is non-empty — a 0-px mask means the knob changed
# nothing there.
#
# READ THE COLUMNS CAREFULLY — three ways this misleads:
#   1. `baseline` is the aperture-OFF leg of the SAME build. It is NOT "before
#      your change". Both columns come from whatever binary you just built.
#   2. `verdict` is therefore near-constant BETTER: it only says the aperture
#      beats no-aperture, which stays true no matter what you break. It carries
#      essentially no regression signal.
#   3. The mask keys on PORTAL_APERTURE=0, a weak knob (measured at 20 px where
#      real preview coverage was 362), so a genuine portal change can move zero
#      pixels here. It was blind to the transparent-terrain sky-cover fix.
#
# To detect a REGRESSION you must run it twice — once on the base revision,
# once with your change — and diff the `mask px` and `current` columns yourself.
# That comparison is what catches real movement (it caught the `cull` case going
# 38.8 -> 52.0 on a rejected experiment). Treat a single run as a smoke test.
#
# Usage:
#   scripts/portal-ref-check.sh [case-filter] [-- <extra openpete args>]
#     case-filter   substring; only matching case tags run (default: all)
#
# Env:
#   OPENPETE_PORTAL_*   forwarded to the native leg (that is the point)
#   PORTAL_REF_JOBS     parallel capture jobs (default: nproc/3, min 1)
#   PORTAL_REF_CACHE    reference cache dir (default: build/portal-ref-cache)
#
# Exit: 0 always (this is a measurement tool, not a gate — the presentation
# golden remains the gate; run it before a re-bless).
set -u
cd "$(dirname "$0")/.."

FILTER="${1:-}"
CACHE="${PORTAL_REF_CACHE:-build/portal-ref-cache}"
JOBS="${PORTAL_REF_JOBS:-$(( $(nproc) / 3 ))}"; [[ "$JOBS" -lt 1 ]] && JOBS=1
BIN="build/linux-release/games/spyro-1/openpete-spyro1"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"

[[ -x "$BIN"  ]] || { echo "[portal-ref] SKIP: no binary at $BIN"; exit 0; }
[[ -f "$DISC" ]] || { echo "[portal-ref] SKIP: no disc at $DISC"; exit 0; }
LVP="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
[[ -e "$LVP" ]] && export VK_ICD_FILENAMES="$LVP"
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

# tag | frame | selector           (freecam poses carry their own level)
CASES=$(cat <<'EOF'
dw-poke      | 1900 | --skip-to-level dream_weavers --freecam-pose "pos=(31259, 44935, 13693) yaw=1379 pitch=-66"
bm-gap       | 1900 | --skip-to-level beast_makers --freecam-pose "pos=(120326, 113108, 18243) yaw=-509 pitch=79"
pk-zfight    | 1900 | --skip-to-level peace_keepers --freecam-pose "pos=(73452, 77502, 55889) yaw=2942 pitch=81"
wf-text      | 3400 | --bk2 games/spyro-1/bk2/beast-makers-wild-flight-text.bk2
cyc-portal   | 2000 | --bk2 games/spyro-1/bk2/cyclorama-portal.bk2
cull         | 2000 | --bk2 games/spyro-1/bk2/portal-cyclorama-cull.bk2
ground       | 2000 | --bk2 games/spyro-1/bk2/portal-ground-disocclusion.bk2
tsq-entry    | 2000 | --bk2 games/spyro-1/bk2/town-square-portal-entry.bk2
bm-distant   | 2000 | --bk2 games/spyro-1/bk2/beast-makers-distant-portal.bk2
mc-cyc       | 2000 | --bk2 games/spyro-1/bk2/magic-crafters-portal-cyclorama.bk2
EOF
)

OUT="$(mktemp -d -t portal-ref.XXXXXX)"; trap 'rm -rf "$OUT"' EXIT
mkdir -p "$CACHE"

# leg: <outfile-prefix> <dir> <renderer> <frame> <native-only?> <selector...>
capture() {
  local pref="$1" dir="$2" rend="$3" frame="$4" nat="$5"; shift 5
  local nat_arg=(); [[ "$nat" == "1" ]] && nat_arg=(--screenshot-native-only)
  eval timeout 400 "$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
    --renderer "$rend" --mute 1 --aspect 4:3 "$@" "${nat_arg[@]+"${nat_arg[@]}"}" \
    --screenshot-on-frame "$frame" --screenshot-output "'$dir/${pref}__{n}.png'" \
    --frame-limit $((frame + 51)) --max-runtime-seconds 380 >/dev/null 2>&1
}

pending=0
while IFS='|' read -r tag frame sel; do
  tag="$(echo "$tag" | tr -d ' \t')"; frame="$(echo "$frame" | tr -d ' \t')"
  [[ -z "$tag" || "${tag:0:1}" == "#" ]] && continue
  [[ -n "$FILTER" && "$tag" != *"$FILTER"* ]] && continue
  # og reference: cached across runs (invariant under native knobs)
  if [[ ! -f "$CACHE/${tag}__${frame}-og.png" ]]; then
    echo "[portal-ref] caching og reference: $tag"
    capture "$tag" "$CACHE" psycross "$frame" 0 $sel &
    (( ++pending >= JOBS )) && { wait -n 2>/dev/null || wait; pending=$((pending-1)); }
  fi
done <<< "$CASES"
wait

pending=0
while IFS='|' read -r tag frame sel; do
  tag="$(echo "$tag" | tr -d ' \t')"; frame="$(echo "$frame" | tr -d ' \t')"
  [[ -z "$tag" || "${tag:0:1}" == "#" ]] && continue
  [[ -n "$FILTER" && "$tag" != *"$FILTER"* ]] && continue
  ( capture "$tag-cur"  "$OUT" vk "$frame" 1 $sel
    OPENPETE_PORTAL_APERTURE=0 capture "$tag-base" "$OUT" vk "$frame" 1 $sel ) &
  (( ++pending >= JOBS )) && { wait -n 2>/dev/null || wait; pending=$((pending-1)); }
done <<< "$CASES"
wait

python3 - "$OUT" "$CACHE" "$FILTER" <<'PY'
import sys, os, glob, re
import numpy as np
from PIL import Image
out, cache, filt = sys.argv[1], sys.argv[2], sys.argv[3]
L = lambda p: np.asarray(Image.open(p).convert('RGB')).astype(int)
print(f"\n{'case':14s}{'mask px':>9s}{'baseline':>10s}{'current':>10s}   verdict")
tot_b = tot_c = 0.0; n = 0
for f in sorted(glob.glob(os.path.join(out, '*-cur__*-vk.png'))):
    tag = re.match(r'(.+)-cur__(\d+)-vk\.png', os.path.basename(f))
    if not tag: continue
    name, frame = tag.group(1), tag.group(2)
    base = os.path.join(out, f'{name}-base__{frame}-vk.png')
    og   = os.path.join(cache, f'{name}__{frame}-og.png')
    if not (os.path.exists(base) and os.path.exists(og)):
        print(f'{name:14s}{"":>9s}{"":>10s}{"":>10s}   MISSING leg'); continue
    C, B, O = L(f), L(base), L(og)
    if not (C.shape == B.shape == O.shape):
        print(f'{name:14s}   SIZE MISMATCH — comparison invalid'); continue
    m = np.abs(B - C).sum(axis=2) > 8
    if not m.sum():
        print(f'{name:14s}{0:>9d}{"":>10s}{"":>10s}   no change'); continue
    db = np.abs(O - B).sum(axis=2)[m].mean(); dc = np.abs(O - C).sum(axis=2)[m].mean()
    tot_b += db; tot_c += dc; n += 1
    v = 'BETTER' if dc < db - 0.05 else ('worse' if dc > db + 0.05 else 'same')
    print(f'{name:14s}{int(m.sum()):>9d}{db:>10.1f}{dc:>10.1f}   {v}')
if n:
    print(f'\n{"MEAN":14s}{"":>9s}{tot_b/n:>10.1f}{tot_c/n:>10.1f}   '
          f'({"closer to PSX" if tot_c < tot_b else "further from PSX"})')
PY
exit 0
