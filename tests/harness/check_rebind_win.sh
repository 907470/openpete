#!/usr/bin/env bash
# check_rebind_win.sh - the binding page's windowed leg on the Windows exe.
#
# A windowed run takes the thread split, so the pad samples on main's pump
# loop and a binding edit restamps PsyCross's maps there; the headless legs
# of check_rebind.sh run unsplit and restamp on the host loop. The exe is
# launched with --background (window created without activation),
# --input-lock (physical devices hidden; the virtual pad and the injection
# script pass), --pad-unfocused 1 and --vibration 0, no screenshot flags,
# and --bk2-record as the port-0 recorder.
#
# The script opens the settings menu, moves to ADVANCED and changes it
# (the overlay opens at its top), starts the capture the page's button
# posts (`rebind pad.cross 1`), presses pad east as the candidate, closes
# the overlay with Escape, then presses pad east again.
# Pass: "open at top" logged; the commit logged; the recording's records hold one run of Cross and no
# Circle (the post-close press; the capture press is held neutral); and
# main's restamp count > 0 in the exit summary. A neutral record during the
# windowed capture proves nothing about the capture term (the focused page
# already holds the pad); check_rebind.sh carries that proof.
#
# Usage (openpete checkout root, a current windows-mingw build):
#   tests/harness/check_rebind_win.sh [EXE]
# Exit: 0 PASS, 1 FAIL, 2 VACUOUS, 64 SKIP (no exe or disc).

set -u
cd "${OPENPETE_ROOT:-$(pwd)}"

EXE="${1:-build/windows-mingw/openpete.exe}"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"
OUT="build/rebind-win"
[ -x "$EXE" ] || { echo "[rebind-win] SKIP: no exe at $EXE"; exit 64; }
[ -f "$DISC" ] || { echo "[rebind-win] SKIP: no disc at $DISC"; exit 64; }
DISC_ARG="$(wslpath -w "$DISC" 2>/dev/null || echo "$DISC")"

# shellcheck source=scripts/lib/wslenv.sh
. scripts/lib/wslenv.sh
# shellcheck source=scripts/lib/scratch_cards.sh
. scripts/lib/scratch_cards.sh   # OPENPETE_CARD_DIR: scratch cards, forwarded by wslenv_forward
mkdir -p "$OUT"
rm -f build/rebind-win/win.log build/rebind-win/win.txt build/rebind-win/win.bk2 \
      build/rebind-win/win.bk2.args

{
    echo "1 pad south 0"
    echo "8000ms menu 1"
    t=9000
    # Up from row 0 wraps to EXIT GAME, the last row; ADVANCED is above it.
    for _ in $(seq 2); do
        echo "${t}ms keydown Up"; echo "$((t + 60))ms keyup Up"; t=$((t + 150))
    done
    echo "${t}ms keydown Right"; echo "$((t + 60))ms keyup Right"
    cat <<'EOF'
13000ms rebind pad.cross 1
13500ms pad east 1
13650ms pad east 0
15000ms keydown Escape
15100ms keyup Escape
16500ms pad east 1
16650ms pad east 0
EOF
} >"$OUT/win.txt"

wslenv_forward OPENPETE_VIBRATION=0 OPENPETE_INPUT_VGAMEPAD=1 \
    OPENPETE_MOUSE_INJECT="$OUT/win.txt"
env "${WSLENV_ARGS[@]}" "$EXE" --disc "$DISC_ARG" --background --input-lock \
    --vibration 0 --mute 1 --pad-unfocused 1 --no-config \
    --bk2-record "$OUT/win.bk2" --frame-limit 1300 >"$OUT/win.log" 2>&1
echo "[rebind-win] exit $?"

count() { grep -c -- "$1" "$OUT/win.log" 2>/dev/null || true; }
runs() { # runs COL: runs and records with digital column COL set
    python3 - "$OUT/win.bk2" "$1" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); col = int(sys.argv[2])
runs = n = 0; prev = False
for l in z.read("Input Log.txt").decode().splitlines():
    if not (l.startswith("|") and l.count("|") >= 3): continue
    b = l.split("|")[2].split(",")[-1]
    on = len(b) > col and b[col] != "."
    n += on; runs += on and not prev; prev = on
print(runs, n)
PY
}

opened=$(count "devui: open at top")
commits=$(count "rebind commit pad.cross pad:east")
main_restamps=$(sed -n 's/.*restamps main=\([0-9]*\).*/\1/p' "$OUT/win.log" | tail -1)
if [ ! -f "$OUT/win.bk2" ]; then
    echo "[rebind-win] VACUOUS: no recording; see $OUT/win.log"; exit 2
fi
read -r cross_runs cross_n < <(runs 9)
read -r circle_runs circle_n < <(runs 8)
detail="open_top=$opened commits=$commits restamps_main=${main_restamps:-?} cross_runs=$cross_runs circle_records=$circle_n"
if [ "$commits" = 0 ] || [ "$opened" = 0 ]; then
    echo "[rebind-win] VACUOUS $detail"; exit 2
fi
if [ "$cross_runs" = 1 ] && [ "$circle_n" = 0 ] &&
   [ "${main_restamps:-0}" -gt 0 ]; then
    echo "[rebind-win] PASS $detail"; exit 0
fi
echo "[rebind-win] FAIL $detail"; exit 1
