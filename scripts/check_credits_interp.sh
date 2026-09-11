#!/usr/bin/env bash
# check_credits_interp.sh — sub-tick interpolation of the credits-roll
# letters (--skip-to-level credits, GS_Credits with g_CreditsStage < 99).
#
# The roll draws two kinds of HUD-pool billboard moby at once: a settled
# line (static screen position, rotation 0) and a line flying in (position,
# depth and rotation all change every tick). Headless native-only captures
# at 4:3 with --render-paths moby isolate the letters on a flat backdrop.
#
# Checks, each with an engagement condition so a capture that drew nothing
# cannot pass:
#   identity  the intermediate re-extract at alpha 1 reproduces the canonical
#             dump of the same frame pixel for pixel.
#   settled   the settled line on the alpha-0.5 intermediate is pixel-
#             identical to one of its canonical neighbours (the game itself
#             moves the line by a pixel per tick, and the intermediate
#             re-extract carries the newer pose; a settled glyph paired with
#             a departed glyph's snapshot rotates and matches neither).
#   flying    the flying line's intermediate differs from BOTH neighbours,
#             and the rightmost glyph's centroid lies strictly between theirs
#             (a snapped position equals one neighbour).
#   keys      OPENPETE_INTERP_OBJS_KEYLOG: a HUD-pool key spawns only when a
#             line spawns — none in the window after the second line lands
#             (a key renamed on the polybuf flip spawns every other tick).
#
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary / disc / python+PIL missing).
set -u
export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/.."

BIN="${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
[[ -x "$BIN" && -f "$DISC" ]] || { echo "credits-interp: SKIP (binary or disc missing)"; exit 64; }
python3 -c 'import PIL, numpy' 2>/dev/null || { echo "credits-interp: SKIP (python3 PIL/numpy missing)"; exit 64; }

WORK="$(mktemp -d /tmp/openpete-credits-interp.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Frames: the second line ("Universal Interactive Studios:" settled, a
# name line flying in) is on screen from 4168; 4206-4210 sit mid-flight.
C0=4206; I0=4207; C1=4208; I1=4209; C2=4210
LIM=4212

run() {  # leg out-flag out-name frame-list [env...]
    local leg="$1" flag="$2" name="$3" frames="$4"; shift 4
    env "$@" timeout 300 "$BIN" --headless --no-config --disc "$DISC" --run-recomp 0x80012204 \
        --renderer vk --mute 1 --aspect 4:3 --skip-to-level credits --screenshot-native-only \
        --render-paths moby "$flag" "$frames" --screenshot-output "$WORK/${name}_{n}.png" \
        --frame-limit "$LIM" >"$WORK/$leg.log" 2>&1
}
run canon --screenshot-on-frame canon "$C0,$C1,$C2"
run inter --screenshot-intermediate-on-frame inter "$I0,$I1"
run ident --screenshot-intermediate-on-frame ident "$C1" OPENPETE_INTERMEDIATE_ALPHA=1
for f in canon_$C0 canon_$C1 canon_$C2 inter_$I0 inter_$I1 ident_$C1; do
    [[ -f "$WORK/$f-vk.png" ]] || { echo "credits-interp: FAIL capture missing $f"; exit 1; }
done

# Keying census over the whole second line: keylog lines carry the frame.
OPENPETE_INTERP_OBJS_KEYLOG=1 timeout 300 "$BIN" --headless --no-config --disc "$DISC" \
    --run-recomp 0x80012204 --renderer vk --mute 1 --aspect 4:3 --skip-to-level credits \
    --screenshot-native-only --render-paths moby --screenshot-intermediate-on-frame 4229 \
    --screenshot-output "$WORK/keys_{n}.png" --frame-limit 4230 >"$WORK/keys.log" 2>&1

python3 - "$WORK" "$C0" "$I0" "$C1" "$I1" "$C2" <<'PY'
import sys, re
import numpy as np
from PIL import Image
W, C0, I0, C1, I1, C2 = sys.argv[1], *map(int, sys.argv[2:])
def load(n): return np.array(Image.open(f"{W}/{n}-vk.png").convert("RGB")).astype(int)
c0, i0, c1, i1, c2, ident = (load(n) for n in (f"canon_{C0}", f"inter_{I0}", f"canon_{C1}",
                                                f"inter_{I1}", f"canon_{C2}", f"ident_{C1}"))
H = c0.shape[0]
# Bands (fractions of the 4:3 frame): the settled title line and the flying name line.
SET = slice(int(H * 0.28), int(H * 0.40)); FLY = slice(int(H * 0.42), int(H * 0.58))
def glyph_mask(a): return (a[..., 0] > 90) & (a[..., 1] > 90) & (a[..., 2] < 80)
def blobs(a):
    cols = glyph_mask(a[FLY]).any(axis=0); out = []; s = None
    for x, v in enumerate(cols):
        if v and s is None: s = x
        if not v and s is not None: out.append((s + x) / 2 if x - s > 3 else None); s = None
    return [b for b in out if b is not None]
fails = []
def check(name, ok, detail):
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {detail}")
    if not ok: fails.append(name)

# Engagement: letters drawn in both bands; the flying line moves.
n_set = int(glyph_mask(c1[SET]).sum()); n_fly = int(glyph_mask(c1[FLY]).sum())
check("engage.settled", n_set > 200, f"{n_set} glyph px in the settled band")
check("engage.flying", n_fly > 100, f"{n_fly} glyph px in the flying band")
d_fly_cc = int((np.abs(c1[FLY] - c2[FLY]).sum(axis=2) > 0).sum())
check("engage.motion", d_fly_cc > 100, f"{d_fly_cc} px differ between canonical {C1} and {C2} in the flying band")

# identity
d_id = int((np.abs(ident - c1).sum(axis=2) > 0).sum())
check("identity", d_id == 0, f"{d_id} px differ, alpha-1 intermediate vs canonical {C1}")

# settled: no glyph rotates on the intermediate
s_prev = int((np.abs(i1[SET] - c1[SET]).sum(axis=2) > 0).sum())
s_next = int((np.abs(i1[SET] - c2[SET]).sum(axis=2) > 0).sum())
check("settled", min(s_prev, s_next) == 0,
      f"settled band, intermediate {I1} vs canonical {C1}: {s_prev} px differ, vs {C2}: {s_next} px")

# flying: not a snap to either neighbour, centroid between
d_prev = int((np.abs(i1[FLY] - c1[FLY]).sum(axis=2) > 0).sum())
d_next = int((np.abs(i1[FLY] - c2[FLY]).sum(axis=2) > 0).sum())
check("flying.moved", d_prev > 100 and d_next > 100,
      f"intermediate {I1} differs from canonical {C1} by {d_prev} px and from {C2} by {d_next} px")
b1, bi, b2 = blobs(c1), blobs(i1), blobs(c2)
ok = bool(b1 and bi and b2) and (min(b1[-1], b2[-1]) < bi[-1] < max(b1[-1], b2[-1]))
check("flying.between", ok, f"rightmost glyph centroid x: canonical {C1}={b1[-1] if b1 else None}, "
      f"intermediate {I1}={bi[-1] if bi else None}, canonical {C2}={b2[-1] if b2 else None}")

# keys: pool-key spawns only at line spawns
log = open(f"{W}/keys.log", errors="replace").read()
spawns = [(int(f), k) for f, k in re.findall(r"\[interp-objs\] f=(\d+) spawn key=([0-9a-f]{8})", log)]
pool = [(f, k) for f, k in spawns if k[0] in "456"]
at_line = [k for f, k in pool if f == 4168]
after = [(f, k) for f, k in pool if 4168 < f <= 4230]
check("keys.engage", len(at_line) >= 5, f"{len(at_line)} pool keys spawned at the line spawn (4168)")
check("keys.stable", len(after) == 0, f"{len(after)} pool-key spawns in 4169..4230: {sorted(set(after))[:6]}")

if fails: print("credits-interp: FAIL", ", ".join(fails)); sys.exit(1)
print("credits-interp: PASS")
PY
