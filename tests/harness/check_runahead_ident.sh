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
# Per row of games/<slug>/corpus/runahead-corpus.manifest
# (bk2 | vbl | depth | deferral | card, the last two optional), two
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
# speculation left gameplay (`speculation left gameplay`: rolled back or
# adopted, its led state is not captured) or was adopted into the real
# timeline (`adopted the led timeline`) before its VBL is also VACUOUS:
# nothing led that capture.
#
# Deferral: the runahead leg's `deferred to a final yield:` line is printed
# per row; its `runahead:` part counts the speculation snapshots owed to a
# later yield (the tick's draw first seen below an intermediate yield of its
# wait) and the rollbacks whose speculation ended below one. A run without
# the line is FAIL. A row whose deferral field is `>0` FAILs when both runahead
# counts are zero: its intermediate yields (a mode-2 tick's VSync(2), an
# emitted site, the spin tick) never met a snapshot request.
#
# Card: a row whose card field names a fixture under tests/golden/<slug>/
# runs both legs with a scratch copy of it in slot 1 and slot 2 empty (the
# game's --no-card boot flag dropped). Frame writes under speculation
# change the in-memory card and suppress only the disk write, so the card
# the runahead leg leaves on disk must be byte-identical to the canonical
# leg's (same frame limit), and differ from the fixture: a run that
# committed no card write is VACUOUS. The row prints the committed card
# bytes changed and the speculative writes suppressed.
#
# Verdict line, one per row:
#   [runahead-ident] game=<g> row=<bk2>@<vbl> depth=<n> diffpx=<n>
#                    maxabs=<n> checked=<n> ra_deferred=<owed>/<rollbacks>
#                    verdict=PASS|FAIL|VACUOUS
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
while IFS='|' read -r bk2 vbl depth want_def card; do
    bk2="${bk2// /}"; vbl="${vbl// /}"; depth="${depth// /}"; want_def="${want_def//[[:space:]]/}"
    card="${card//[[:space:]]/}"
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

    # A card row: each leg its own scratch copy of the fixture, deleted first.
    local_run_args=("${PG_RUN_ARGS[@]}")
    RA_CARD_ARGS=(); CN_CARD_ARGS=(); FX=""
    if [[ -n "$card" && "$card" != - ]]; then
        FX="tests/golden/$GAME/$card"
        [[ -f "$FX" ]] || { echo "[runahead-ident] game=$GAME row=$bk2@$vbl FAIL (no card fixture $FX)"; fail=1; continue; }
        local_run_args=()
        for t in "${PG_RUN_ARGS[@]}"; do [[ "$t" == --no-card ]] || local_run_args+=("$t"); done
        rm -f "${OUT:?}/$tag-ra.mcr" "${OUT:?}/$tag-ra.mcr.modset" \
              "${OUT:?}/$tag-canon.mcr" "${OUT:?}/$tag-canon.mcr.modset"
        cp "$FX" "$OUT/$tag-ra.mcr"; cp "$FX" "$OUT/$tag-canon.mcr"
        RA_CARD_ARGS=(--card1 "$OUT/$tag-ra.mcr" --no-card2)
        CN_CARD_ARGS=(--card1 "$OUT/$tag-canon.mcr" --no-card2)
    fi
    saved_run_args=("${PG_RUN_ARGS[@]}")
    PG_RUN_ARGS=("${local_run_args[@]}")

    PG_CAPTURE_ENV=(OPENPETE_RA_PREDICT_CHECK=1)
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$RA_LOG" "$RA_PNG" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --vsync 0 \
        --bk2 "$MOVIE" --runahead "$depth" "${RA_CARD_ARGS[@]}" \
        --screenshot-native-only --screenshot-on-frame "$vbl" \
        --screenshot-output "$OUT/$tag-ra-{n}.png" \
        --frame-limit $((led + 30)) \
        || { echo "[runahead-ident] game=$GAME row=$bk2@$vbl FAIL (runahead run, see $RA_LOG)"; fail=1; PG_RUN_ARGS=("${saved_run_args[@]}"); continue; }

    PG_CAPTURE_ENV=()
    pg_capture "$CN_LOG" "$CN_PNG" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --vsync 0 \
        --bk2 "$MOVIE" "${CN_CARD_ARGS[@]}" \
        --screenshot-native-only --screenshot-on-frame "$led" \
        --screenshot-output "$OUT/$tag-canon-{n}.png" \
        --frame-limit $((led + 30)) \
        || { echo "[runahead-ident] game=$GAME row=$bk2@$vbl FAIL (canonical run, see $CN_LOG)"; fail=1; PG_RUN_ARGS=("${saved_run_args[@]}"); continue; }
    PG_RUN_ARGS=("${saved_run_args[@]}")

    armed=0
    grep -q 'armed: leading presentation by' "$RA_LOG" && armed=1
    adopted=0
    grep -q 'speculation left gameplay\|adopted the led timeline' "$RA_LOG" && adopted=1
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

    def_line="$(grep -o 'deferred to a final yield: .*' "$RA_LOG" | tail -1)"
    ra_owed="$(sed -n 's/.*runahead: \([0-9]*\) snapshot.*/\1/p' <<<"$def_line")"
    ra_rb="$(sed -n 's/.*, \([0-9]*\) rollback.*/\1/p' <<<"$def_line")"
    echo "[runahead-ident] game=$GAME row=$bk2@$vbl ${def_line:-no deferral line}"

    card_ok=1; card_eng=1
    if [[ -n "$FX" ]]; then
        card_changed=$(cmp -l "$FX" "$OUT/$tag-canon.mcr" 2>/dev/null | wc -l)
        spec_wr=$(grep -c 'flush during speculative tick suppressed' "$RA_LOG")
        cmp -s "$OUT/$tag-ra.mcr" "$OUT/$tag-canon.mcr" || card_ok=0
        [[ "$card_changed" -gt 0 ]] || card_eng=0
        echo "[runahead-ident] game=$GAME row=$bk2@$vbl card=$card committed_bytes_changed=$card_changed" \
             "speculative_writes_suppressed=$spec_wr card_runahead_eq_canonical=$card_ok"
    fi

    verdict=PASS
    if [[ $armed -eq 0 || "$checked" -eq 0 || $adopted -eq 1 || $card_eng -eq 0 ]]; then
        verdict=VACUOUS; fail=1
    elif [[ "$diffpx" -ne 0 || $card_ok -eq 0 ]]; then
        verdict=FAIL; fail=1
    elif [[ -z "$ra_owed" || -z "$ra_rb" ]]; then
        verdict=FAIL; fail=1
        echo "[runahead-ident]   no runahead deferral count in $RA_LOG"
    elif [[ "$want_def" == ">0" && $((ra_owed + ra_rb)) -eq 0 ]]; then
        verdict=FAIL; fail=1
        echo "[runahead-ident]   deferral field >0, runahead deferred nothing"
    fi
    echo "[runahead-ident] game=$GAME row=$bk2@$vbl depth=$depth diffpx=$diffpx" \
         "maxabs=$maxabs checked=$checked ra_deferred=${ra_owed:-?}/${ra_rb:-?} verdict=$verdict"
    [[ "$verdict" == VACUOUS ]] && echo "[runahead-ident]   armed=$armed adopted=$adopted checked=$checked ($RA_LOG)"
    [[ "$verdict" == FAIL ]] && echo "[runahead-ident]   led $RA_PNG vs $CN_PNG"
done < "$MANIFEST"

if [[ $rows -eq 0 ]]; then
    echo "[runahead-ident] game=$GAME VACUOUS: no row ran (filter '$FILTER')"
    exit 1
fi
echo "[runahead-ident] game=$GAME rows=$rows verdict=$([[ $fail -eq 0 ]] && echo PASS || echo FAIL)"
exit $fail
