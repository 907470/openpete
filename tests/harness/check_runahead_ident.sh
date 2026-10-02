#!/usr/bin/env bash
# check_runahead_ident.sh - the runahead prediction contract, checked on pixels.
#
# --runahead N presents the game state N ticks (2N VBLs) ahead of the
# committed timeline: it saves the committed tick, speculates 2N VBLs with
# held input, lets the interp captures snapshot that led state, and rolls the
# guest back before anything else observes RAM. The contract that makes the
# feature correct is that the led present is the picture the run would have
# produced 2N VBLs later without runahead, exactly:
#
#   capture at VBL V under --runahead N  ==  capture at VBL V+2N without it
#
# Any canonical presentation latch written during a speculative tick breaks
# it. Such a latch holds one tick's bytes addressed off another tick's
# primitive buffer, and the sub-tick replay decodes the mismatch as geometry.
#
# The comparand was calibrated on the Spyro 1 control row: the led capture at
# VBL 1828 is pixel-identical to the unaccelerated capture at 1832, and to no
# other VBL in 1824..1834 (the neighbouring ticks differ by ten thousand
# pixels). Headless presents one canonical picture per tick, so both members
# of a tick's VBL pair carry the same image.
#
# Per row of games/<slug>/corpus/runahead-corpus.manifest (bk2 | vbl | depth), two
# headless replays of the movie:
#   (a) --runahead <depth>, native capture at <vbl>;
#   (b) no runahead, native capture at <vbl> + 2*<depth>.
# Statistic: the number of differing pixels and the worst channel difference.
#
# Pass criterion: 0 differing pixels. The contract is exact, so there is no
# tolerance; a row that does not meet it names a latch written under
# speculation.
#
# Engagement: the runahead leg must log `armed: leading presentation
# by` and must report a nonzero prediction-check count from
# OPENPETE_RA_PREDICT_CHECK=1, which counts led predictions retired against
# the committed timeline. A run that never speculated would meet the pixel
# criterion trivially, so it is VACUOUS instead of PASS. A row whose
# speculation was adopted into the real timeline (`crossed into a transition`)
# before its VBL is also VACUOUS: nothing led that capture.
#
# Verdict line, one per row:
#   [runahead-ident] game=<g> row=<bk2>@<vbl> depth=<n> diffpx=<n>
#                    maxabs=<n> checked=<n> verdict=PASS|FAIL|VACUOUS
#
# Usage: tests/harness/check_runahead_ident.sh [--game SLUG] [--filter SUBSTR]
#                                        [--build-dir=DIR] [--help]
# Exit 0 PASS, 1 FAIL/VACUOUS or a missing corpus manifest (the message names
# the path), 64 SKIP (binary, disc, bk2, python3 or PIL missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

FILTER=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        -h|--help) sed -n '2,50p' "$0"; exit 0;;
    esac
done
# --filter takes its value as the next argument.
prev=""
for a in "$@"; do
    [[ "$prev" == "--filter" ]] && FILTER="$a"
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

MANIFEST="$(pg_corpus_manifest "$GAME" runahead-corpus.manifest)"
pg_manifest "$MANIFEST" pipe
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] || { echo "runahead-ident: SKIP (binary or disc missing)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null || { echo "runahead-ident: SKIP (python3 or PIL missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/runahead-ident/$GAME"
mkdir -p "$OUT"

fail=0
rows=0
while IFS='|' read -r bk2 vbl depth; do
    bk2="${bk2// /}"; vbl="${vbl// /}"; depth="${depth// /}"
    [[ -z "$bk2" || "$bk2" == \#* ]] && continue
    [[ -n "$FILTER" && "$bk2" != *"$FILTER"* ]] && continue
    MOVIE="$PG_BK2_DIR/$bk2"
    if [[ ! -f "$MOVIE" ]]; then
        echo "[runahead-ident] game=$GAME row=$bk2@$vbl SKIP (no such bk2)"
        continue
    fi
    rows=$((rows + 1))
    led=$((vbl + 2 * depth))
    tag="${bk2%.bk2}"
    RA_LOG="$OUT/$tag-ra.log"; RA_PNG="$OUT/$tag-ra-$vbl-vk.png"
    CN_LOG="$OUT/$tag-canon.log"; CN_PNG="$OUT/$tag-canon-$led-vk.png"

    PG_CAPTURE_ENV=(OPENPETE_RA_PREDICT_CHECK=1)
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$RA_LOG" "$RA_PNG" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --vsync 0 \
        --bk2 "$MOVIE" --runahead "$depth" \
        --screenshot-native-only --screenshot-on-frame "$vbl" \
        --screenshot-output "$OUT/$tag-ra-{n}.png" \
        --frame-limit $((led + 30)) \
        || { echo "[runahead-ident] game=$GAME row=$bk2@$vbl FAIL (runahead run, see $RA_LOG)"; fail=1; continue; }

    PG_CAPTURE_ENV=()
    pg_capture "$CN_LOG" "$CN_PNG" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --vsync 0 \
        --bk2 "$MOVIE" \
        --screenshot-native-only --screenshot-on-frame "$led" \
        --screenshot-output "$OUT/$tag-canon-{n}.png" \
        --frame-limit $((led + 30)) \
        || { echo "[runahead-ident] game=$GAME row=$bk2@$vbl FAIL (canonical run, see $CN_LOG)"; fail=1; continue; }

    armed=0
    grep -q 'armed: leading presentation by' "$RA_LOG" && armed=1
    adopted=0
    grep -q 'crossed into a transition' "$RA_LOG" && adopted=1
    checked=$(grep -o '\[ra-predict\] checked=[0-9]*' "$RA_LOG" | tail -1)
    checked="${checked##*checked=}"
    [[ -n "$checked" ]] || checked=0

    read -r diffpx maxabs < <(python3 - "$CN_PNG" "$RA_PNG" <<'PYEOF'
import sys
from PIL import Image, ImageChops
A = Image.open(sys.argv[1]).convert("RGB")
B = Image.open(sys.argv[2]).convert("RGB")
if A.size != B.size:
    print(A.size[0] * A.size[1] + B.size[0] * B.size[1], 255)
    raise SystemExit
d = ImageChops.difference(A, B)
if d.getbbox() is None:
    print(0, 0)
    raise SystemExit
bands = d.split()
mx = max(b.getextrema()[1] for b in bands)
n = sum(1 for px in d.getdata() if px != (0, 0, 0))
print(n, mx)
PYEOF
    )

    verdict=PASS
    if [[ $armed -eq 0 || "$checked" -eq 0 || $adopted -eq 1 ]]; then
        verdict=VACUOUS; fail=1
    elif [[ "$diffpx" -ne 0 ]]; then
        verdict=FAIL; fail=1
    fi
    echo "[runahead-ident] game=$GAME row=$bk2@$vbl depth=$depth diffpx=$diffpx" \
         "maxabs=$maxabs checked=$checked verdict=$verdict"
    [[ "$verdict" == VACUOUS ]] && echo "[runahead-ident]   armed=$armed adopted=$adopted checked=$checked ($RA_LOG)"
    [[ "$verdict" == FAIL ]] && echo "[runahead-ident]   led $RA_PNG vs $CN_PNG"
done < "$MANIFEST"

if [[ $rows -eq 0 ]]; then
    echo "[runahead-ident] game=$GAME VACUOUS: no row ran (filter '$FILTER')"
    exit 1
fi
echo "[runahead-ident] game=$GAME rows=$rows verdict=$([[ $fail -eq 0 ]] && echo PASS || echo FAIL)"
exit $fail
