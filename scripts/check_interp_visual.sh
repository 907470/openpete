#!/usr/bin/env bash
# check_interp_visual.sh — automated visual harness for the transform-
# interp plan (notes/plans/openpete-transform-interp.md).
#
# Captures K consecutive presents starting at sim frame N under three
# --render-fps modes (0/60/480), then computes a diff matrix:
#
#   1. CANONICAL REGRESSION (§10.5): every fps>0 canonical present must
#      be byte-identical to the fps=0 canonical at the same sim frame.
#      Any drift means the interp mechanism perturbs the *canonical*
#      output, which would violate PHASE-C's read-only claim.
#
#   2. INTERP-FIRING (§10.1, §10.2): at fps=60, INTERMEDIATE presents
#      should differ from canonical presents (interp delta reaches the
#      GL submit). Adjacent-present diff < canonical→canonical diff at
#      fps=0 (single sim frame vs two-sim-frame gap).
#
#   3. 480-DEGENERATION (§7.5-R3 PHASE-B open item): if fps=480
#      adjacent-diffs are pixel-identical to fps=60 adjacent-diffs, the
#      8× sim rate is being clamped somewhere (vsync, pacer, or swap
#      latency) — not interp itself, but the symptom users report as
#      "looks like 30fps."
#
# Deterministic attract-mode demo (no --bk2), windowed via WSLg or any
# X/Wayland display. Headless mode is not used because §7.5 documented
# `substitute_query` is gated `if lerp_alpha>0` and headless alpha≡0.
#
# Usage:
#   scripts/check_interp_visual.sh [START_FRAME [PRESENT_COUNT [OUTDIR]]]
#
# Defaults: START_FRAME=800, PRESENT_COUNT=8, OUTDIR=/tmp/interp_visual.

set -euo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/.."

START=${1:-800}
COUNT=${2:-8}
OUT=${3:-/tmp/interp_visual}
FRAME_LIMIT=$((START + COUNT + 100))

# Extra binary flags (e.g. "--aspect 16:9 --no-lod") so the canonical
# byte-identity gate can run under the exact presentation config a user
# plays with — the widescreen OFX paths are active only at non-4:3.
EXTRA_FLAGS=${OPENPETE_INTERP_VISUAL_EXTRA:-}

mkdir -p "$OUT"
rm -f "$OUT"/fps*.png

BIN=build/linux-release/games/spyro-1/openpete-spyro1
if [[ ! -x "$BIN" ]]; then
    echo "openpete binary missing at $BIN — run cmake --build --preset linux-release" >&2
    exit 1
fi

for fps in 0 60 480; do
    echo "[interp-visual] --render-fps $fps"
    timeout 90s "$BIN" \
        --disc games/spyro-1/spyro-1/build/SCUS-94228.cue --disc-cache 0 \
        --render-fps "$fps" \
        --frame-limit "$FRAME_LIMIT" \
        --screenshot-presents-start "$START" \
        --screenshot-presents-count "$COUNT" \
        --screenshot-output "$OUT/fps${fps}_p{p}_f{n}.png" \
        --mute on 2>&1 | grep "screenshot:" | head -"$COUNT" | sed 's/^/  /'
done

python3 - "$OUT" <<'PYEOF'
import sys, os, glob, re
from PIL import Image
out = sys.argv[1]

def load(p):
    return list(Image.open(p).convert('RGB').getdata())

def diff(a, b):
    A, B = load(a), load(b)
    n = sum(1 for x, y in zip(A, B) if x != y)
    return n, len(A)

print()
print("=== 1. Canonical regression — fps0 vs fps60/480 at same sim frame ===")
print("    EXPECTED: 0 pixels differ. Any drift = PHASE-C is NOT read-only.")
fps0 = { int(re.search(r'f(\d+)', f).group(1)): f
         for f in glob.glob(f'{out}/fps0_p*.png') }
ok = True
for fps in ['60', '480']:
    for f in sorted(glob.glob(f'{out}/fps{fps}_p*.png')):
        m = re.search(r'p(\d+)_f(\d+)', f)
        pi, fi = int(m.group(1)), int(m.group(2))
        # canonical presents at fps>0 are odd-indexed in the captured
        # sequence (intermediate runs first in the iter immediately
        # after sim ticked); validated empirically.
        if pi % 2 != 1: continue
        if fi not in fps0: continue
        d, _ = diff(fps0[fi], f)
        status = 'PASS' if d == 0 else 'FAIL'
        print(f"  sim f={fi:>4}: fps0 vs fps{fps} canonical → {d:>6} pixels differ  [{status}]")
        if d != 0: ok = False
print(f"  Result: {'PASS' if ok else 'FAIL'}")

print()
print("=== 2. Interp firing — fps60 adjacent-present diff ===")
print("    EXPECTED: intermediate→canonical and canonical→intermediate")
print("    differ ~< canonical→canonical at fps0 (single-sim-frame gap).")
files60 = sorted(glob.glob(f'{out}/fps60_p*.png'))
for i in range(len(files60)-1):
    d, t = diff(files60[i], files60[i+1])
    print(f"  {os.path.basename(files60[i])} → {os.path.basename(files60[i+1])}: "
          f"{100*d/t:5.2f}% differ")

print()
print("=== 3. 480 degeneration — fps480 vs fps60 byte-identity ===")
print("    BUG SIGNAL: if fps480 frames are byte-identical to fps60")
print("    frames at the same present index, the 8× sim rate is clamped")
print("    (vsync/pacer/swap-blocking), not interp itself.")
import hashlib
def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()[:16]
files480 = sorted(glob.glob(f'{out}/fps480_p*.png'))
identical_count = 0
for i, f480 in enumerate(files480):
    if i >= len(files60): break
    h480 = sha(f480)
    h60 = sha(files60[i])
    same = h480 == h60
    if same: identical_count += 1
    print(f"  p{i}: fps60 {h60}  fps480 {h480}  {'IDENTICAL' if same else 'differs'}")
ratio = 100 * identical_count / max(1, len(files480))
print(f"  fps480 byte-identical to fps60: {identical_count}/{len(files480)} ({ratio:.0f}%)")
if ratio >= 80:
    print("  VERDICT: BUG REPRODUCED — present cadence at --render-fps 480")
    print("           is wall-clock-clamped to the --render-fps 60 rate.")
elif ratio >= 30:
    print("  VERDICT: partial clamp — investigate the pacer/vsync interaction.")
else:
    print("  VERDICT: distinct cadence — 480 mode produces unique frames.")
PYEOF
