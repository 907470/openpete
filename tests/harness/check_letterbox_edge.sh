#!/usr/bin/env bash
# check_letterbox_edge.sh - no lit pixel outside the native view rect in a
# windowed live present whose window shape leaves letterbox or pillarbox bars.
#
# The native scene is scissored to the view rect and the background quad and
# the 2D bars are drawn to the same clip-space edges. A scissor that reaches
# one pixel past the quads leaves a lit line along the bar edge. Each row
# opens a Spyro 2 window on a private Xvfb display at a size whose edges fall
# on fractional pixels, captures the live present at one VBL, reads the view
# rect's window-pixel edges from the renderer's layout log line, and counts
# the lit pixels whose centre lies outside [l, r) x [t, b). Headless captures
# render to a target of the view's own shape and never reach this case.
#
# Rows (spyro-2 riptos_arena, VBL 1800):
#   641x600  AUTO  bars top and bottom (the line is row 516 when it fails)
#   1280x720 4:3   bars left and right (the line is column 1173 when it fails)
#
# Pass: every row has bars (VACUOUS otherwise), the PNG is the window size,
# and no lit pixel lies outside the view rect. --osd off, --version-osd off
# and --perf-stats off keep the window-anchored text out of the bars.
#
# Usage:
#   tests/harness/check_letterbox_edge.sh [--out DIR] [--row WxH:ASPECT]...
# Env: OPENPETE_BIN (default build/linux-release/openpete), OPENPETE_DISC.
#
# Exit codes: 0 PASS, 2 FAIL, 3 VACUOUS (no bars, no live-present capture,
# no layout line), 64 SKIP (xvfb-run, binary or disc missing).
set -uo pipefail

export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/sandbox.sh
source scripts/lib/sandbox.sh
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh

OUT="build/letterbox-edge"
ROWS=()
while (( $# )); do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --row) ROWS+=("$2"); shift 2 ;;
        *) echo "[letterbox-edge] unknown argument: $1" >&2; exit 64 ;;
    esac
done
(( ${#ROWS[@]} )) || ROWS=("641x600:auto" "1280x720:4:3")

BIN="${OPENPETE_BIN:-build/linux-release/openpete}"
DISC="${OPENPETE_DISC:-games/spyro-2/spyro-2/build/SCUS-94425.cue}"
VBL=1800

# The rows need a windowed present, on a private Xvfb display.
op_xvfb_prefix letterbox-edge 2048x1536
[[ -x "$BIN" ]] || { echo "[letterbox-edge] SKIP: no binary at $BIN" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "[letterbox-edge] SKIP: no disc at $DISC" >&2; exit 64; }


mkdir -p "$OUT"
fail=0 vacuous=0
for row in "${ROWS[@]}"; do
    size="${row%%:*}"; aspect="${row#*:}"
    tag="${size}_${aspect//:/x}"
    png="$OUT/${tag}__${VBL}-vk.png"; log="$OUT/${tag}.log"
    rm -f "${png:?}" "${log:?}"
    OPENPETE_LOG="native:debug,*:info" timeout 600 \
        "${OP_XVFB[@]}" \
        "$BIN" --game spyro-2 --no-config --disc "$DISC" --no-card --mute 1 \
            --input-lock --vibration 0 --renderer vk --lod original \
            --osd off --version-osd off --perf-stats off \
            --window "$size" --aspect "$aspect" --skip-to-level riptos_arena \
            --screenshot-native-only --screenshot-on-frame "$VBL" \
            --screenshot-output "$OUT/${tag}__{n}.png" --frame-limit $((VBL + 10)) \
            > "$log" 2>&1
    rc=$?
    if [[ ! -s "$png" ]]; then
        echo "[letterbox-edge] $tag: no capture (openpete rc=$rc, log $log)"
        vacuous=1
        continue
    fi
    python3 - "$png" "$log" "$size" "$tag" <<'EOF'
import re, sys
import numpy as np
from PIL import Image
png, log, size, tag = sys.argv[1:5]
W, H = map(int, size.split("x"))
text = open(log, errors="replace").read()
if "(live present) screenshot" not in text:
    print(f"[letterbox-edge] {tag}: VACUOUS: the capture is not a live-present readback"); sys.exit(3)
lay = re.findall(rf"layout: window {W}x{H} .* view=([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+) scissor=(\d+),(\d+),(\d+)x(\d+)", text)
if not lay:
    print(f"[letterbox-edge] {tag}: VACUOUS: no layout line for a {W}x{H} window"); sys.exit(3)
l, t, r, b = map(float, lay[-1][:4]); sc = tuple(map(int, lay[-1][4:]))
a = np.asarray(Image.open(png).convert("RGB")).astype(int).max(axis=2)
if a.shape != (H, W):
    print(f"[letterbox-edge] {tag}: VACUOUS: capture {a.shape[1]}x{a.shape[0]} is not the window"); sys.exit(3)
xc = np.arange(W) + 0.5; yc = np.arange(H) + 0.5
inx = (xc >= l) & (xc < r); iny = (yc >= t) & (yc < b)
inside = iny[:, None] & inx[None, :]
lit_out = (a > 0) & ~inside
bar_rows = int((~iny).sum()); bar_cols = int((~inx).sum())
x0, x1 = int(inx.argmax()), int(W - inx[::-1].argmax())
y0, y1 = int(iny.argmax()), int(H - iny[::-1].argmax())
lines = []
for name, n in (("row", y0 - 1), ("row", y1)):
    if 0 <= n < H: lines.append(f"{name} {n}: {int((a[n] > 0).sum())}")
for name, n in (("col", x0 - 1), ("col", x1)):
    if 0 <= n < W: lines.append(f"{name} {n}: {int((a[:, n] > 0).sum())}")
n = int(lit_out.sum())
print(f"[letterbox-edge] {tag}: view=({l:.3f},{t:.3f})-({r:.3f},{b:.3f}) covered px x[{x0},{x1}) y[{y0},{y1})"
      f" scissor={sc[0]},{sc[1]},{sc[2]}x{sc[3]} bar_rows={bar_rows} bar_cols={bar_cols}"
      f" edge-adjacent lit [{'; '.join(lines)}] lit_outside={n}")
if bar_rows + bar_cols == 0:
    print(f"[letterbox-edge] {tag}: VACUOUS: the window has no bars"); sys.exit(3)
sys.exit(2 if n else 0)
EOF
    case $? in
        0) ;;
        3) vacuous=1 ;;
        *) fail=1 ;;
    esac
done

if (( fail )); then echo "[letterbox-edge] verdict: FAIL"; exit 2; fi
if (( vacuous )); then echo "[letterbox-edge] verdict: VACUOUS"; exit 3; fi
echo "[letterbox-edge] verdict: PASS"
