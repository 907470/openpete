#!/usr/bin/env bash
# check_glitch_mark.sh - the glitch-mark rings as a standing check.
#
# The glitch mark keeps the last N presents and the last N terrain walks in
# two lock-free rings and writes them to a file on request, so a one-present
# artifact can be read back after it has gone. The reader
# tools/glitch_mark_read.py flags the three shapes such an artifact takes:
#
#   SWING       a walk whose terrain triangle count differs from BOTH
#               neighbours by more than the threshold while the neighbours
#               agree with each other - geometry that left and came back
#   STAMP-BACK  a present that composed an OLDER terrain build than the
#               present before it
#   GAP         a skipped serial or ordinal, so the rows either side of it
#               are not neighbours in time
#
# This runs that reader over one fixed replay, so a change that starts
# producing any of the three fails here instead of waiting to be noticed in a
# window. The window is a level-loaded stretch chosen to be clean on the
# current build: every flag it reports is a change of behaviour, not a
# property of the scene.
#
# A headless run composes no presents at all - render_frame_common's callers
# both early-return headless - so the present ring is empty here and only the
# walk ring carries rows. STAMP-BACK is a present-ring flag and therefore
# cannot fire in this mode; the walk ring's SWING and GAP are what this row
# actually measures. The run still needs ONE screenshot request: headless
# composes the native scene only for a tick something reads, and without it
# the terrain walk never runs and the ring stays empty.
#
# Engagement: the dump must carry a full DEPTH walk rows with none lost to
# ring recycling. Fewer rows means the mark fired before the ring filled, or
# the walk never ran, and the flag counts would then be a property of the
# short window rather than of the build - refused as VACUOUS.
#
# Verdict line:
#   [glitch-mark] game=<g> bk2=<b> at=w<n> depth=<n> walks=<n> presents=<n>
#                 lost_walks=<n> lost_presents=<n> flagged_walks=<n>
#                 flagged_presents=<n> verdict=PASS|FAIL
#
# Usage: tests/harness/check_glitch_mark.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary, disc, bk2, reader or
# python3 missing, or the game has no row).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# Per game: the bk2, the walk ordinal the mark fires on, the ring depth, and
# the VBL the screenshot request lands on. The ordinal names the END of the
# window: the dump holds walks MARK_ORD-DEPTH+1 .. MARK_ORD. For spyro-1 that
# is walks 701..1300 of artisans-bridge, VBLs 1781..2979 - wholly inside the
# loaded level, and clear of the three swings the earlier walks carry (two on
# the title screen, one at the load itself).
case "$GAME" in
  spyro-1) BK2="artisans-bridge.bk2"; MARK_ORD=1300; DEPTH=600; SHOT_VBL=2990;;
  *)       BK2="";;
esac
[[ -n "$BK2" ]] || { echo "glitch-mark: SKIP ($GAME has no glitch-mark row)"; exit 64; }

READER="tools/glitch_mark_read.py"
MOVIE="$PG_BK2_DIR/$BK2"
[[ -x "$PG_BIN" && -f "$PG_DISC" && -f "$MOVIE" ]] ||
    { echo "glitch-mark: SKIP (binary, disc or bk2 missing)"; exit 64; }
[[ -f "$READER" ]] || { echo "glitch-mark: SKIP ($READER missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "glitch-mark: SKIP (python3 missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/glitch-mark/$GAME"
mkdir -p "$OUT"
# The dump directory is the directory of the run's --log-file. Every mark file
# there goes before the run, not just the one this run names: the mark names
# its file after the ring ordinal, so a run that fired somewhere else would
# otherwise leave a file that reads as a result.
rm -f "$OUT/glitch-mark-"*.txt

LOG="$OUT/run.log"
SHOT="$OUT/$GAME.png"
# --screenshot-native-only names the file after the render path, so the image
# pg_capture asserts carries the -vk suffix the run writes.
SHOT_VK="$OUT/$GAME-vk.png"
DUMP="$OUT/glitch-mark-$MARK_ORD.txt"

PG_CAPTURE_ENV=(OPENPETE_GLITCH_MARK_N="$DEPTH" OPENPETE_GLITCH_MARK_AT="w$MARK_ORD")
PG_CAPTURE_TIMEOUT=900
pg_capture "$LOG" "$SHOT_VK,$DUMP" -- \
    --renderer vk --mute 1 --aspect 4:3 --lod original \
    --bk2 "$MOVIE" --terrain-cache 0 --vsync 0 \
    --screenshot-native-only --screenshot-on-frame "$SHOT_VBL" \
    --screenshot-output "$SHOT" \
    --log-file "$OUT/host.log" \
    --frame-limit $((SHOT_VBL + 20))
rc=$?
if (( rc != 0 )); then
    echo "glitch-mark: FAIL (run rc=$rc, or no mark at w$MARK_ORD; see $LOG)"
    exit 1
fi

python3 - "$GAME" "$BK2" "$MARK_ORD" "$DEPTH" "$DUMP" "$READER" <<'PYEOF'
import re
import subprocess
import sys

game, bk2, mark_ord, depth, dump, reader = sys.argv[1:7]
mark_ord, depth = int(mark_ord), int(depth)

text = open(dump, encoding="utf-8", errors="replace").read()


def count(pattern, default=-1):
    m = re.search(pattern, text, re.M)
    return int(m.group(1)) if m else default


walks = count(r"^walks n=(\d+)")
presents = count(r"^presents n=(\d+)")
lost_p = count(r"^lost presents=(\d+)")
lost_w = count(r"^lost presents=\d+ walks=(\d+)")

# The reader is the instrument: run the shipped tool rather than a second copy
# of its rules, so the row fails when the rules change under it.
r = subprocess.run([sys.executable, reader, dump, "--flags-only"],
                   capture_output=True, text=True)
out = r.stdout
sys.stdout.write(out)
sys.stdout.write(r.stderr)
m = re.search(r"^flagged: (\d+) present row\(s\), (\d+) walk row\(s\)", out, re.M)
fp, fw = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)

head = ("[glitch-mark] game=%s bk2=%s at=w%d depth=%d walks=%d presents=%d "
        "lost_walks=%d lost_presents=%d flagged_walks=%d flagged_presents=%d"
        % (game, bk2, mark_ord, depth, walks, presents, lost_w, lost_p, fw, fp))

if fp < 0:
    print(head + " FAIL: the reader printed no flag summary (rc=%d)" % r.returncode)
    sys.exit(1)
if walks < depth or lost_w != 0:
    print(head + " VACUOUS: floor walks>=%d with lost_walks=0" % depth)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if (fp or fw) else "PASS"))
sys.exit(1 if (fp or fw) else 0)
PYEOF
